#!/usr/bin/env python3
"""One row per day per queue model: the passive maker on OKX's own archive, inside one UTC window.

The fill rule is the whole point of the table. `conservative` never removes a cancelled unit from the queue ahead
(`src/exchange/queue_model.h`), so a resting order fills only when trades consume everything in front of it; a cancel
that empties the level does not move it forward. `prorata` removes cancels in proportion. The gap between the two
columns is the optimism a touch-based fill would hide, and the reason both are printed rather than one.

Two things are refused rather than reported. A day whose trades and depth do not cover the same window -- which
`stale_trades` would show as prints with no book -- is cut from the depth file's own first timestamp, so the two agree
by construction. And a day whose depth cut is missing is built from the archive by
`tools/study/okx_book_window.py`, which is what makes a replay's book open on a snapshot.

Usage:
    python3 tools/sweep/okx_maker_baseline.py --days 2026-11-16,2026-11-17 --models conservative,prorata
    python3 tools/sweep/okx_maker_baseline.py --days ... --base-spread-grid 1,2,3 --refresh-grid 1,2,4

Two columns exist because the window's own P&L lies when a position is held: `gross_bps` marks the inventory and
`closed_bps` counts only the round trips, and on a thin book with a capped position the first can be twenty times the
second. `closed_bps` is the one to read against the fee, and the grid mode prints one row per cell so the two quote
flags can be asked together. A window cut left half-written by a concurrent run is detected and re-cut rather than
trusted, because a cut that starts mid-stream replays a book built from nothing.

Measured with it (2026-11-25, `UP-USDT-SWAP`, seven days at 13:30-15:30 UTC): the median net is -2.28 bps under
`conservative` and -2.49 under `prorata`, one day of seven positive either way, and the fee is 2.00 bps on every run
with no taker fill -- so what the window loses is gross, not fee. Tightening the inventory cap to three contracts and
closing out at the end of the window both made it worse or left it unchanged, which is why the tool takes `--inv-limit`
and `--extra` rather than a single fixed shape.
"""

import argparse
import csv
import subprocess
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BIN = ROOT / "out/build/linux-debug/toy_quant"
WINDOW_TOOL = ROOT / "tools/study/okx_book_window.py"
SLICE_TOOL = ROOT / "tools/fetch/slice.py"
# The archive and the trade tape live in `<root>/<SYMBOL>/` or in the flat root; one module owns both layouts.
sys.path.insert(0, str(ROOT / "tools" / "lib"))
from okx_data import depth_archive, trade_tape  # noqa: E402
COLUMNS = ["day", "model", "base", "refresh", "latency_us", "fills", "fill_rate", "fees_bps", "gross_bps", "closed_bps",
           "closed_per_fill_bps", "net_bps", "net_usd", "notional_usd", "breakeven_bps", "capture_ticks", "markout_ticks", "zero_queue",
           "max_inv", "net_pos", "hold_s", "settle_bps", "settled_bps", "stale"]


def number_list(text, cast=float):
    """`1,2,3` -> [1.0, 2.0, 3.0]; an empty string is one cell meaning "the binary's own default"."""
    parts = [part.strip() for part in (text or "").split(",") if part.strip()]
    return [cast(part) for part in parts] or [None]


def end_of_window(day_text, end):
    day = datetime.strptime(day_text, "%Y-%m-%d").replace(tzinfo=timezone.utc)
    hour, minute = (int(part) for part in end.split(":"))
    return int(day.replace(hour=hour, minute=minute).timestamp() * 1000)


def first_timestamp(path):
    """The cut's first record, which is the window's true start because the cut opens on a snapshot.

    None means the file is absent, empty, or was read while it was still being written -- a cut is made without a
    temporary name, so a concurrent reader can see a truncated first line. The caller re-cuts rather than trusting it.
    """
    try:
        with Path(path).open("rb") as source:
            line = source.readline()
    except OSError:
        return None
    marker = b'"ts":"'
    if marker not in line:
        return None
    start = line.index(marker) + len(marker)
    return int(line[start:start + 13])


def summary_fields(text):
    fields = {}
    for line in text.splitlines():
        if not line.startswith("["):
            continue
        for token in line.strip().strip("[]").split():
            if "=" in token:
                key, _, value = token.partition("=")
                fields.setdefault(key, value)
    return fields


def ensure_cut(archive, cuts, start, end, tick):
    """The day's depth cut, made by the window tool when it is absent; returns its path or None.

    The cut's name carries the instrument, because a cuts directory is shared: `okx_2026-11-16_1330.jsonl` was keyed by
    day, start and tick only, so the first measurement of a second instrument silently replayed the *first* one's book --
    measured 2026-11-25, the DNA run read `fills=?` on three days and the cut's own `instId` said `UP-USDT-SWAP`.
    """
    day_text = archive.name.split("-400lv-")[-1].split(".tar.gz")[0]
    symbol = archive.name.split("-L2orderbook")[0]
    cut = Path(cuts) / f"okx_{symbol}_{day_text}_{start.replace(':', '')}.jsonl"
    if cut.exists() and first_timestamp(cut) is not None:
        return cut
    if cut.exists():
        # A cut without a readable first line is a partial write from another run; re-cutting is the only way to
        # know the book it replays starts from a snapshot.
        cut.unlink()
    made = subprocess.run([sys.executable, str(WINDOW_TOOL), str(archive), "--start", start, "--end", end,
                           "--tick", str(tick), "--emit", str(cuts)], cwd=ROOT, capture_output=True, text=True)
    if made.returncode != 0 or first_timestamp(cut) is None:
        print(f"# {day_text}: no usable cut ({made.stderr.strip()[:160]})")
        return None
    return cut


def trade_source(day_text, symbol, start_ms):
    """The trade file whose UTC+8 day covers `start_ms`.

    OKX cuts its trade archive on midnight UTC+8 while the book archive is cut on UTC, so a window that starts at or
    after 16:00 UTC on day D lives in the file named D+1. Reading only the file named D made an evening window
    two hours of book against zero trades, which the engine answered with zero quotes -- measured 2026-11-25, and the
    reason `trades=0` appears in a run whose book was intact.
    """
    day = datetime.strptime(day_text, "%Y-%m-%d").replace(tzinfo=timezone.utc)
    sixteen = int(day.replace(hour=16).timestamp() * 1000)
    name = day_text if start_ms < sixteen else (day + timedelta(days=1)).strftime("%Y-%m-%d")
    # `<root>/<SYMBOL>/` or the flat root; `tools/lib/okx_data.py` is the one place that knows both.
    found = trade_tape(ROOT / "data/v2/crypto", symbol, name)
    return found if found is not None else ROOT / f"data/v2/crypto/okx_trades_{name}_{symbol}.csv"


def slice_trades(day_text, symbol, start_ms, end_ms, cuts):
    """The window's trades, which is what keeps `stale_trades` from appearing at all."""
    source = trade_source(day_text, symbol, start_ms)
    if not source.exists():
        return None
    # The slice's name carries the instrument for the same reason the cut's does: one cuts directory serves every symbol.
    target = Path(cuts) / f"trades_{symbol}_{day_text}.csv"
    run = subprocess.run([sys.executable, str(SLICE_TOOL), "window", "--input", str(source), "--column", "6",
                          "--from", str(start_ms), "--to", str(end_ms), "--output", str(target)],
                         cwd=ROOT, capture_output=True, text=True)
    return target if run.returncode == 0 else None


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--days", required=True, help="comma separated file days, as the archive names them")
    parser.add_argument("--symbol", default="UP-USDT-SWAP")
    parser.add_argument("--archives", default="data/v2/crypto", help="where the day archives live")
    parser.add_argument("--cuts", default="/tmp/tq/upwin", help="where the window cuts and trade slices go")
    parser.add_argument("--models", default="conservative,prorata")
    parser.add_argument("--start", default="13:30")
    parser.add_argument("--end", default="15:30")
    parser.add_argument("--tick", type=float, default=0.0001)
    parser.add_argument("--base-spread-grid", default="1",
                        help="quote half width in ticks, comma separated; empty leaves the strategy's own default")
    parser.add_argument("--refresh-grid", default="4",
                        help="how far the fair price must move to requote, in ticks, comma separated")
    parser.add_argument("--inv-limit", default="100000000", help="engine units of net position; one contract is ten million")
    parser.add_argument("--order-size", default="10000000")
    parser.add_argument("--latency-us", type=int, default=0,
                        help="order and cancel latency the replayer holds each leg for, in microseconds")
    parser.add_argument("--extra", default="", help="further l2_replay flags, space separated")
    parser.add_argument("--hold-time", action="store_true",
                        help="ask the run for --metrics, which is what makes it print the round trips' holding time")
    parser.add_argument("--settle-residual", action="store_true",
                        help="price the leftover position as a handover: the far touch, one tick past it, plus a taker "
                             "fee -- the exit a replay cannot fill")
    parser.add_argument("--settle-taker-bps", type=float, default=5.0,
                        help="the taker fee the handover pays, in basis points")
    parser.add_argument("--out", default="/tmp/tq/baseline.csv")
    args = parser.parse_args()

    # The flag is microseconds, and a sweep written in milliseconds passes 80 where it meant 80000. Measured on
    # 2026-11-25: a five-point grid labelled "0 to 80 ms" was five runs at 0 to 80 us, and every conclusion drawn from it
    # was a statement about zero latency. The number cannot be corrected for the caller, so it is named in both units.
    if 0 < args.latency_us < 1000:
        print(f"# warning: --latency-us={args.latency_us} is {args.latency_us / 1000.0:g} ms, which is not a round "
              f"trip; a millisecond value is 1000x this", file=sys.stderr)

    Path(args.cuts).mkdir(parents=True, exist_ok=True)
    bases = number_list(args.base_spread_grid)
    refreshes = number_list(args.refresh_grid, cast=int)
    rows = []
    for day_text in [part.strip() for part in args.days.split(",") if part.strip()]:
        archive = depth_archive(ROOT / args.archives, args.symbol, day_text)
        if archive is None:
            print(f"# {day_text}: no {args.symbol}-L2orderbook-400lv-{day_text}.tar.gz under {args.archives}")
            continue
        cut = ensure_cut(archive, args.cuts, args.start, args.end, args.tick)
        if cut is None:
            continue
        trades = slice_trades(day_text, args.symbol, first_timestamp(cut), end_of_window(day_text, args.end), args.cuts)
        if trades is None:
            print(f"# {day_text}: no trades CSV for {args.symbol}")
            continue
        for model in args.models.split(","):
            for base in bases:
                for refresh in refreshes:
                    shape = []
                    if base is not None:
                        shape.append(f"--base-spread-ticks={base:g}")
                    if refresh is not None:
                        shape.append(f"--refresh-price-ticks={refresh:g}")
                    if args.latency_us > 0:
                        shape.append(f"--order-latency-us={args.latency_us}")
                        shape.append(f"--cancel-latency-us={args.latency_us}")
                    if args.hold_time:
                        # One line the run prints only when asked, because the sampling is a per-event cost.
                        shape.append("--metrics")
                    if args.settle_residual:
                        shape.append("--settle-residual")
                        shape.append(f"--settle-taker-bps={args.settle_taker_bps:g}")
                        shape.append("--settle-slippage-ticks=1")
                    command = [str(BIN), "l2_replay", str(trades), str(cut), args.symbol, "0", "active_l2", "1", model,
                               "--fast-validation", f"--order-size={args.order_size}",
                               f"--inventory-limit={args.inv_limit}"] + shape + [p for p in args.extra.split() if p]
                    run = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
                    # A failed replay used to become a row of `?` and 0.0000, which reads as "measured, nothing
                    # happened" and hides the cause: measured 2026-11-25, OL/PROS/BLUR ran with no instrument
                    # specification, every cell came out `?`, and the reason -- `Error: no instrument specification
                    # for L2 replay symbol: OL-USDT-SWAP`, exit 1 -- was in a stderr nobody printed. A missing input
                    # is an error that names the file or the symbol, never a table of zeros.
                    if run.returncode != 0 or "trade_reports" not in summary_fields(run.stdout):
                        detail = run.stderr.strip().splitlines()
                        detail = detail[-1] if detail else run.stdout.strip()[:200]
                        print(f"# {day_text} {model} base={base} refresh={refresh}: replay failed, exit "
                              f"{run.returncode}: {detail}")
                        return 2
                    fields = summary_fields(run.stdout)
                    notional = float(fields.get("maker_notional_usd", 0.0) or 0.0)

                    def bps(name):
                        value = float(fields.get(name, 0.0) or 0.0)
                        return f"{value / notional * 1e4:.2f}" if notional else "n/a"

                    row = {"day": day_text, "model": model,
                           "base": "" if base is None else f"{base:g}",
                           "refresh": "" if refresh is None else f"{refresh:g}",
                           "latency_us": str(args.latency_us),
                           "fills": fields.get("trade_reports", "?"),
                           "fill_rate": fields.get("fill_rate", "?"), "fees_bps": bps("fees_paid"),
                           "gross_bps": bps("gross_pnl_usd"), "closed_bps": bps("gross_closed_pnl_usd"),
                           "closed_per_fill_bps": bps("gross_closed_per_fill_usd"),
                           "net_bps": bps("net_pnl_usd"),
                           # The two absolute figures the bps columns divide away. A size ladder is a question about
                           # _dollars_ -- a rate that falls while the notional rises can still pay more -- and bps
                           # alone cannot answer it.
                           "net_usd": f"{float(fields.get('net_pnl_usd', 0.0) or 0.0):.4f}",
                           "notional_usd": f"{float(fields.get('maker_notional_usd', 0.0) or 0.0):.2f}",
                           "breakeven_bps": f"{float(fields.get('breakeven_maker_rate', 0.0) or 0.0) * 1e4:.2f}",
                           "capture_ticks": fields.get("captured_edge_per_unit_ticks", "?"),
                           "markout_ticks": fields.get("markout_per_unit_ticks", "?"),
                           "zero_queue": fields.get("queue_zero_orders", "?"),
                           "max_inv": fields.get("max_abs_inventory", "?"),
                           "net_pos": fields.get("strategy_net_position", "?"),
                           # How long an inventory lived before it came back, from the [ROUND_TRIPS] line `--metrics`
                           # prints; "n/a" when the run did not ask or completed no round trip.
                           "hold_s": fields.get("average_holding_seconds", "n/a"),
                           # The handover screen: what the leftover position would cost to cross out of, and the
                           # marked result with that cost taken out. Both are the engine's own figures.
                           "settle_bps": fields.get("settle_cost_bps", "n/a"),
                           "settled_bps": fields.get("settled_net_bps", "n/a"),
                           "stale": fields.get("stale_trades", "0")}
                    rows.append(row)
                    print(f"# {day_text} {model} base={row['base']} refresh={row['refresh']}: "
                          f"fills={row['fills']} gross_bps={row['gross_bps']} net_bps={row['net_bps']}")

    if not rows:
        print("no rows: nothing measured")
        return 1
    with open(args.out, "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=COLUMNS)
        writer.writeheader()
        writer.writerows(rows)
    print(f"wrote {args.out} rows={len(rows)}")
    print("| " + " | ".join(COLUMNS) + " |")
    print("|" + "---|" * len(COLUMNS))
    for row in rows:
        print("| " + " | ".join(str(row[key]) for key in COLUMNS) + " |")
    if len(bases) * len(refreshes) * len(args.models.split(",")) > 1:
        print()
        print("| model | base | refresh | days | mean net bps | median net bps | positive | mean fills |")
        print("|---|---:|---:|---:|---:|---:|---:|---:|")
        cells = {}
        for row in rows:
            cells.setdefault((row["model"], row["base"], row["refresh"]), []).append(row)

        def as_float(text):
            try:
                return float(text)
            except (TypeError, ValueError):
                return None

        for (model, base, refresh), members in sorted(cells.items()):
            nets = [value for value in (as_float(member["net_bps"]) for member in members) if value is not None]
            fills = [value for value in (as_float(member["fills"]) for member in members) if value is not None]
            if not nets:
                continue
            nets.sort()
            median = nets[len(nets) // 2] if len(nets) % 2 else (nets[len(nets) // 2 - 1] + nets[len(nets) // 2]) / 2
            print(f"| {model} | {base or 'default'} | {refresh or 'default'} | {len(nets)} | "
                  f"{sum(nets) / len(nets):.2f} | {median:.2f} | {sum(1 for value in nets if value > 0)} | "
                  f"{sum(fills) / len(fills):.0f} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())

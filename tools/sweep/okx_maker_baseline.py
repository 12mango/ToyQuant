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
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BIN = ROOT / "out/build/linux-debug/toy_quant"
WINDOW_TOOL = ROOT / "tools/study/okx_book_window.py"
SLICE_TOOL = ROOT / "tools/fetch/slice.py"
COLUMNS = ["day", "model", "fills", "fill_rate", "fees_bps", "gross_bps", "net_bps", "breakeven_bps",
           "capture_ticks", "markout_ticks", "zero_queue", "max_inv", "net_pos", "stale"]


def end_of_window(day_text, end):
    day = datetime.strptime(day_text, "%Y-%m-%d").replace(tzinfo=timezone.utc)
    hour, minute = (int(part) for part in end.split(":"))
    return int(day.replace(hour=hour, minute=minute).timestamp() * 1000)


def first_timestamp(path):
    """The cut's first record, which is the window's true start because the cut opens on a snapshot."""
    with Path(path).open("rb") as source:
        line = source.readline()
    marker = b'"ts":"'
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
    """The day's depth cut, made by the window tool when it is absent; returns its path or None."""
    day_text = archive.name.split("-400lv-")[-1].split(".tar.gz")[0]
    cut = Path(cuts) / f"okx_{day_text}_{start.replace(':', '')}.jsonl"
    if cut.exists():
        return cut
    made = subprocess.run([sys.executable, str(WINDOW_TOOL), str(archive), "--start", start, "--end", end,
                           "--tick", str(tick), "--emit", str(cuts)], cwd=ROOT, capture_output=True, text=True)
    if made.returncode != 0 or not cut.exists():
        print(f"# {day_text}: no cut ({made.stderr.strip()[:160]})")
        return None
    return cut


def slice_trades(day_text, symbol, start_ms, end_ms, cuts):
    """The day's trades inside the same window, which is what keeps `stale_trades` from appearing at all."""
    source = ROOT / f"data/v2/crypto/okx_trades_{day_text}_{symbol}.csv"
    if not source.exists():
        return None
    target = Path(cuts) / f"trades_{day_text}.csv"
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
    parser.add_argument("--inv-limit", default="100000000", help="engine units of net position; one contract is ten million")
    parser.add_argument("--order-size", default="10000000")
    parser.add_argument("--extra", default="", help="further l2_replay flags, space separated")
    parser.add_argument("--out", default="/tmp/tq/baseline.csv")
    args = parser.parse_args()

    Path(args.cuts).mkdir(parents=True, exist_ok=True)
    rows = []
    for day_text in [part.strip() for part in args.days.split(",") if part.strip()]:
        archive = ROOT / args.archives / f"{args.symbol}-L2orderbook-400lv-{day_text}.tar.gz"
        if not archive.exists():
            print(f"# {day_text}: no archive at {archive}")
            continue
        cut = ensure_cut(archive, args.cuts, args.start, args.end, args.tick)
        if cut is None:
            continue
        trades = slice_trades(day_text, args.symbol, first_timestamp(cut), end_of_window(day_text, args.end), args.cuts)
        if trades is None:
            print(f"# {day_text}: no trades CSV for {args.symbol}")
            continue
        for model in args.models.split(","):
            command = [str(BIN), "l2_replay", str(trades), str(cut), args.symbol, "0", "active_l2", "1", model,
                       "--fast-validation", f"--order-size={args.order_size}", f"--inventory-limit={args.inv_limit}",
                       "--base-spread-ticks=1", "--refresh-price-ticks=4"] + [p for p in args.extra.split() if p]
            run = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
            fields = summary_fields(run.stdout)
            notional = float(fields.get("maker_notional_usd", 0.0) or 0.0)

            def bps(name):
                value = float(fields.get(name, 0.0) or 0.0)
                return f"{value / notional * 1e4:.2f}" if notional else "n/a"

            row = {"day": day_text, "model": model, "fills": fields.get("trade_reports", "?"),
                   "fill_rate": fields.get("fill_rate", "?"), "fees_bps": bps("fees_paid"),
                   "gross_bps": bps("gross_pnl_usd"), "net_bps": bps("net_pnl_usd"),
                   "breakeven_bps": f"{float(fields.get('breakeven_maker_rate', 0.0) or 0.0) * 1e4:.2f}",
                   "capture_ticks": fields.get("captured_edge_per_unit_ticks", "?"),
                   "markout_ticks": fields.get("markout_per_unit_ticks", "?"),
                   "zero_queue": fields.get("queue_zero_orders", "?"),
                   "max_inv": fields.get("max_abs_inventory", "?"),
                   "net_pos": fields.get("strategy_net_position", "?"),
                   "stale": fields.get("stale_trades", "0")}
            rows.append(row)
            print(f"# {day_text} {model}: fills={row['fills']} gross_bps={row['gross_bps']} net_bps={row['net_bps']}")

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
    return 0


if __name__ == "__main__":
    sys.exit(main())

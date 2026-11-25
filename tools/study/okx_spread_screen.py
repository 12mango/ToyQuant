#!/usr/bin/env python3
"""Screen every OKX swap by its *median* half-spread over repeated samples, not by one REST snapshot.

Why the sampling matters. A single ticker read is one moment of a book that tightens and widens all day, and the first
crypto screen in this repository was one such read: it put a symbol on the list on the strength of a half-spread that a
twenty-second-average book may not hold. A symbol whose true median is 2.5 to 4 bps can be missed by a snapshot taken in
a quiet hour, which is the error this tool exists to remove -- it reads the venue's tickers route every `--interval`
seconds for `--samples` reads and keeps the per-symbol **median** half-spread, so one tightened quote cannot promote a
symbol and one wide moment cannot promote it either.

Why the tick width is a column of its own. A half-spread in basis points is not the quantity a quoting rule can use: a
cheap instrument whose tick is worth 4.4 bps can quote a half-spread of 2.2 bps with the bid and the ask *one tick
apart*, and a rule that rests three ticks behind the touch -- the one that earns on UP-USDT-SWAP -- cannot be placed in
a book that is one tick wide at all. The tool therefore joins the venue's instrument table for `tickSz` and reports the
median spread in ticks beside the half-spread, and a candidate has to pass both: at least `--min-half-spread-bps` and at
least `--min-spread-ticks`. The second threshold is the one that decides whether a symbol is the same kind of animal.

The sample set is archived rather than kept in a temporary file. Reading it into `data/v2/crypto/` by default is
deliberate: this repository has lost scratch scripts and readings under `/tmp` twice, and a screen whose raw reads are
gone cannot be re-derived -- `--read` replays an archived set to the same table, which is what makes the screen a
measurement instead of a memory. The file is ignored by git (it is input, not output), so archiving it costs no
repository bytes.

The sampling window is the measurement, not a detail: 13:30-15:30 UTC is the US cash open and the window every other
screen in this repository uses, so a run outside it answers a different question. The tool prints the UTC time of its
first and last read so a table cannot be quoted without them.

Usage:
    python3 tools/study/okx_spread_screen.py --samples 30 --interval 20
    python3 tools/study/okx_spread_screen.py --read data/v2/crypto/okx_tickers_20261125T133000Z.json

The run prints the passers at the thresholds, the widest rows pass or fail, and the count that clears 1.2, 2.0 and 2.5 bps
under the same tick and volume floors -- three tiers rather than one verdict, because which tier a symbol lands in is the
first thing a reader asks and a table sorted by width does not answer it.
"""

import argparse
import json
import statistics
import sys
import time
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

TICKERS = "https://www.okx.com/api/v5/market/tickers?instType=SWAP"
INSTRUMENTS = "https://www.okx.com/api/v5/public/instruments?instType=SWAP"
# The venue answers a bare urllib agent with Cloudflare 1010, which reads as a venue outage rather than a header.
AGENT = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36"
DATA_DIR = Path(__file__).resolve().parents[2] / "data/v2/crypto"


def get_json(url, timeout=20.0):
    request = urllib.request.Request(url, headers={"User-Agent": AGENT, "Accept": "application/json"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        payload = json.loads(response.read().decode("utf-8"))
    if payload.get("code") not in ("0", 0):
        raise SystemExit(f"the venue refused {url}: code={payload.get('code')} msg={payload.get('msg')}")
    return payload.get("data", [])


def fetch_tick_sizes():
    """`instId -> tickSz`, from the venue's instrument table: the tickers route does not carry it.

    The tick width is what turns a half-spread in bps into the only number a quoting rule can act on -- how many ticks
    the book is wide -- and it is a per-instrument fact the ticker does not repeat.
    """
    sizes = {}
    for row in get_json(INSTRUMENTS):
        try:
            sizes[row["instId"]] = float(row["tickSz"])
        except (KeyError, TypeError, ValueError):
            continue
    return sizes


def read_tickers(timeout=20.0):
    """One ticker read, as the venue returns it: a list of dicts, one per swap."""
    return get_json(TICKERS, timeout)


def sample_once(samples, stamp, tick_sizes):
    """Fold one read into `samples`: half-spread in bps, spread in ticks, notional in USD, per instrument."""
    for row in read_tickers():
        symbol = row.get("instId")
        try:
            bid, ask = float(row["bidPx"]), float(row["askPx"])
            last = float(row.get("last") or 0.0)
            base_volume = float(row.get("volCcy24h") or 0.0)
            # Contracts traded in 24 hours. The ticker carries both the base-currency volume and the contract count, and
            # the count is the one that answers "how many times did anything happen": SSV trades 1.94 M USD a day and
            # 5,000 to 22,000 times, NES the same notional and 450,000 times, so a filter on dollars cannot separate them
            # and a filter on actions can.
            contracts = float(row.get("vol24h") or 0.0)
        except (KeyError, TypeError, ValueError):
            continue
        if bid <= 0.0 or ask <= 0.0 or ask <= bid:
            continue
        mid = 0.5 * (bid + ask)
        tick = tick_sizes.get(symbol)
        entry = samples.setdefault(symbol, {"spreads": [], "ticks": [], "notional": [], "contracts": [],
                                            "stamps": []})
        entry["spreads"].append((ask - bid) / 2.0 / mid * 10_000.0)
        entry.setdefault("ticks", []).append((ask - bid) / tick if tick else None)
        entry["notional"].append(base_volume * last)
        entry["contracts"].append(contracts)
        entry["stamps"].append(stamp)


def analyse(samples, min_spread, min_notional, min_ticks, quote_ticks, min_capture, min_contracts=0.0):
    """One row per instrument: the medians, the spread's own range, and the verdicts.

    `capture` is the column the screen exists for on this venue: a quote that rests `quote_ticks` behind the touch is
    filled only by a sweep that reaches it, and what it earns a fill is that distance in basis points. It is computed from
    the two medians -- tick width = 2 x half-spread / spread-in-ticks -- because those are the two columns the venue's
    ticker actually reports, and it is compared against the maker fee rather than against an opinion.
    """
    rows = []
    for symbol, entry in samples.items():
        spreads = entry["spreads"]
        if not spreads:
            continue
        spreads.sort()
        median = statistics.median(spreads)
        p10 = spreads[max(0, int(0.10 * (len(spreads) - 1)))]
        p90 = spreads[min(len(spreads) - 1, int(0.90 * (len(spreads) - 1)))]
        ticks = [value for value in entry.get("ticks", []) if value is not None]
        median_ticks = statistics.median(ticks) if ticks else None
        notional = statistics.median(entry["notional"]) if entry["notional"] else 0.0
        # Contracts a day, or None for an archive written before the column existed: an old sample must still re-derive,
        # so a missing count passes the filter instead of failing every row.
        contracts = statistics.median(entry["contracts"]) if entry.get("contracts") else None
        # One tick in bps: the half spread is `median`, the full spread is `median_ticks` ticks, so a tick is
        # 2 x half-spread / full-spread-in-ticks. None when the tick width could not be read.
        tick_bps = 2.0 * median / median_ticks if median_ticks else None
        capture = quote_ticks * tick_bps if tick_bps is not None else None
        rows.append({"symbol": symbol, "samples": len(spreads), "median": median, "p10": p10, "p90": p90,
                     "ticks": median_ticks, "notional": notional, "contracts": contracts,
                     "tick_bps": tick_bps, "capture": capture,
                     "spread_ok": median >= min_spread,
                     "ticks_ok": median_ticks is not None and median_ticks >= min_ticks,
                     "volume_ok": notional >= min_notional and
                                  (contracts is None or contracts >= min_contracts),
                     "capture_ok": capture is not None and capture >= min_capture})
    rows.sort(key=lambda row: row["median"], reverse=True)
    return rows


def print_table(rows, title, quote_ticks):
    print(f"### {title}")
    print()
    print(f"| instrument | reads | median half spread bps | median spread ticks | {quote_ticks:g}-tick capture bps |"
          " p10 | p90 | median 24h notional USD | median 24h contracts | spread | ticks | volume |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---|---|---|")
    for row in rows:
        ticks = "n/a" if row["ticks"] is None else f"{row['ticks']:.2f}"
        capture = "n/a" if row["capture"] is None else f"{row['capture']:.2f}"
        contracts = "n/a" if row.get("contracts") is None else f"{row['contracts']:,.0f}"
        print(f"| {row['symbol']} | {row['samples']} | {row['median']:.2f} | {ticks} | {capture} | "
              f"{row['p10']:.2f} | {row['p90']:.2f} | {row['notional']:,.0f} | {contracts} | "
              f"{'pass' if row['spread_ok'] else 'fail'} | "
              f"{'pass' if row['ticks_ok'] else 'fail'} | {'pass' if row['volume_ok'] else 'fail'} |")
    print()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--samples", type=int, default=30, help="reads to take (30 at 20 s is ten minutes)")
    parser.add_argument("--interval", type=float, default=20.0, help="seconds between reads")
    parser.add_argument("--min-half-spread-bps", type=float, default=1.2)
    parser.add_argument("--min-spread-ticks", type=float, default=2.0,
                        help="the book's median width in ticks; one tick is not a book a deep quote can rest in")
    parser.add_argument("--min-volume-usd", type=float, default=500_000.0)
    parser.add_argument("--min-contracts", type=float, default=0.0,
                        help="contracts a day, the count rather than the dollars: SSV and NES trade the same notional "
                             "and 5,000 against 450,000 times, so only a count separates a wide book from a busy one")
    parser.add_argument("--quote-ticks", type=float, default=3.0,
                        help="how far behind the touch the quoting rule rests; its capture a fill is that many ticks")
    parser.add_argument("--min-capture-bps", type=float, default=2.0,
                        help="the maker fee the capture has to clear, in basis points")
    parser.add_argument("--top", type=int, default=25, help="rows printed in the ranked table")
    parser.add_argument("--write", help="where to archive the raw reads; defaults to data/v2/crypto/okx_tickers_<utc>.json")
    parser.add_argument("--read", help="analyse an archived sample set instead of polling the venue")
    args = parser.parse_args()

    tick_sizes = fetch_tick_sizes()
    samples = {}
    target = None
    if args.read:
        stored = json.loads(Path(args.read).read_text(encoding="utf-8"))
        samples = {symbol: {key: list(value) if isinstance(value, list) else value
                            for key, value in entry.items()} for symbol, entry in stored.items()}
        stamps = [stamp for entry in samples.values() for stamp in entry["stamps"]]
        first, last = (min(stamps), max(stamps)) if stamps else ("?", "?")
    else:
        print(f"polling {TICKERS} every {args.interval:g} s for {args.samples} reads", file=sys.stderr)
        for index in range(args.samples):
            stamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
            sample_once(samples, stamp, tick_sizes)
            print(f"read {index + 1}/{args.samples} at {stamp}: {len(samples)} two-sided instruments", file=sys.stderr)
            if index + 1 < args.samples:
                time.sleep(args.interval)
        stamps = [stamp for entry in samples.values() for stamp in entry["stamps"]]
        first, last = (min(stamps), max(stamps)) if stamps else ("?", "?")
        # Archived by default, and into the repository's data directory: a screen whose raw reads sit in /tmp is a
        # table nobody can re-derive after the next reboot, which is how two scratch scripts were lost here.
        if args.write:
            target = Path(args.write)
        elif first != "?":
            target = DATA_DIR / f"okx_tickers_{first.replace('-', '').replace(':', '')}.json"
        if target is not None:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(json.dumps(samples), encoding="utf-8")
            print(f"archived the reads to {target} ({target.stat().st_size} bytes)")

    rows = analyse(samples, args.min_half_spread_bps, args.min_volume_usd, args.min_spread_ticks,
                   args.quote_ticks, args.min_capture_bps, args.min_contracts)
    passers = [row for row in rows if row["spread_ok"] and row["ticks_ok"] and row["volume_ok"]]
    print(f"reads from {first} to {last}, {len(samples)} instruments with a two-sided book at least once")
    print(f"screen: median half spread >= {args.min_half_spread_bps:g} bps, median spread >= "
          f"{args.min_spread_ticks:g} ticks, median 24h notional >= {args.min_volume_usd:,.0f} USD and "
          f"median 24h contracts >= {args.min_contracts:,.0f}")
    # The tiers are printed as counts as well as rows: a reader wants to know how many instruments clear 1.2, 2.0 and
    # 2.5 bps under the same tick and volume floors, and a table sorted by width does not answer that at a glance.
    tiers = [1.2, 2.0, 2.5]
    counts = {tier: sum(1 for row in rows if row["median"] >= tier and row["ticks_ok"] and row["volume_ok"])
              for tier in tiers}
    print("tiers (all with median spread >= {:g} ticks and notional >= {:.0f} USD): ".format(
        args.min_spread_ticks, args.min_volume_usd) +
        ", ".join(f"{tier:g} bps: {counts[tier]}" for tier in tiers))
    print()
    print_table(passers, f"the screen's passers ({len(passers)} of {len(rows)})", args.quote_ticks)
    print_table(rows[:args.top], f"the {min(args.top, len(rows))} widest, pass or fail", args.quote_ticks)
    # The question the width tiers do not answer: what a quote at our own distance earns a fill, against the fee. A wide
    # book measured in ticks is not the same as a wide book measured in bps -- a 30-tick spread can be worth 1.4 bps, and
    # three ticks behind it is worth 0.14 -- so this is the column the mechanism actually lives or dies on.
    by_capture = sorted((row for row in rows if row["capture"] is not None),
                        key=lambda row: row["capture"], reverse=True)
    clearing = [row for row in by_capture if row["capture_ok"] and row["ticks_ok"] and row["volume_ok"]]
    print(f"a quote {args.quote_ticks:g} ticks behind the touch captures {args.min_capture_bps:g} bps or more on "
          f"{len(clearing)} of {len(rows)} instruments (with the tick and volume floors); the maker fee is "
          f"{args.min_capture_bps:g} bps")
    print()
    print_table(clearing[:args.top], f"the {min(args.top, len(clearing))} widest captures that clear the fee",
                args.quote_ticks)
    return 0


if __name__ == "__main__":
    sys.exit(main())

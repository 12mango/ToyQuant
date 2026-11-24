#!/usr/bin/env python3
"""Screen every OKX swap by its *median* half-spread over repeated samples, not by one REST snapshot.

Why the sampling matters. A single ticker read is one moment of a book that tightens and widens all day, and the first
crypto screen in this repository was one such read: it put a symbol on the list on the strength of a half-spread that a
twenty-second-average book may not hold. A symbol whose true median is 2.5 to 4 bps can be missed by a snapshot taken in
a quiet hour, which is the error this tool exists to remove -- it reads the venue's tickers route every `--interval`
seconds for `--samples` reads and keeps the per-symbol **median** half-spread, so one tightened quote cannot promote a
symbol and one wide moment cannot promote it either.

The sampling window is the measurement, not a detail: 13:30-15:30 UTC is the US cash open and the window every other
screen in this repository uses, so a run outside it answers a different question. The tool prints the UTC time of its
first and last read so a table cannot be quoted without them.

Volume is the other half. A half-spread that would pay a fee is worth nothing if nothing trades, so the screen applies a
floor to the 24-hour notional and prints it beside the spread; `volCcy24h` is the base-currency volume a swap's ticker
carries, and the notional is that times the last price.

Usage:
    python3 tools/study/okx_spread_screen.py --samples 30 --interval 20 --write /tmp/tq/okx_tickers_1330.jsonl
    python3 tools/study/okx_spread_screen.py --read /tmp/tq/okx_tickers_1330.jsonl

`--read` re-analyses a saved sample set, so a table can be re-derived from the reads it was computed from rather than
re-polled, which is what makes a screen a measurement instead of a memory.
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
# The venue answers a bare urllib agent with Cloudflare 1010, which reads as a venue outage rather than a header.
AGENT = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36"


def read_tickers(timeout=20.0):
    """One ticker read, as the venue returns it: a list of dicts, one per swap."""
    request = urllib.request.Request(TICKERS, headers={"User-Agent": AGENT, "Accept": "application/json"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        payload = json.loads(response.read().decode("utf-8"))
    if payload.get("code") not in ("0", 0):
        raise SystemExit(f"the venue refused the tickers read: code={payload.get('code')} msg={payload.get('msg')}")
    return payload.get("data", [])


def sample_once(samples, stamp):
    """Fold one read into `samples`: a list of half-spreads in bps and a notional in USD, per instrument."""
    for row in read_tickers():
        symbol = row.get("instId")
        try:
            bid, ask = float(row["bidPx"]), float(row["askPx"])
            last = float(row.get("last") or 0.0)
            base_volume = float(row.get("volCcy24h") or 0.0)
        except (KeyError, TypeError, ValueError):
            continue
        if bid <= 0.0 or ask <= 0.0 or ask <= bid:
            continue
        mid = 0.5 * (bid + ask)
        entry = samples.setdefault(symbol, {"spreads": [], "notional": [], "stamps": []})
        entry["spreads"].append((ask - bid) / 2.0 / mid * 10_000.0)
        entry["notional"].append(base_volume * last)
        entry["stamps"].append(stamp)


def analyse(samples, min_spread, min_notional):
    """One row per instrument: the medians, the spread's own range, and the two verdicts."""
    rows = []
    for symbol, entry in samples.items():
        spreads = entry["spreads"]
        if not spreads:
            continue
        spreads.sort()
        median = statistics.median(spreads)
        p10 = spreads[max(0, int(0.10 * (len(spreads) - 1)))]
        p90 = spreads[min(len(spreads) - 1, int(0.90 * (len(spreads) - 1)))]
        notional = statistics.median(entry["notional"]) if entry["notional"] else 0.0
        rows.append({"symbol": symbol, "samples": len(spreads), "median": median, "p10": p10, "p90": p90,
                     "notional": notional, "spread_ok": median >= min_spread, "volume_ok": notional >= min_notional})
    rows.sort(key=lambda row: row["median"], reverse=True)
    return rows


def print_table(rows, title):
    print(f"### {title}")
    print()
    print("| instrument | reads | median half spread bps | p10 | p90 | median 24h notional USD | spread | volume |")
    print("|---|---:|---:|---:|---:|---:|---|---|")
    for row in rows:
        print(f"| {row['symbol']} | {row['samples']} | {row['median']:.2f} | {row['p10']:.2f} | {row['p90']:.2f} | "
              f"{row['notional']:,.0f} | {'pass' if row['spread_ok'] else 'fail'} | "
              f"{'pass' if row['volume_ok'] else 'fail'} |")
    print()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--samples", type=int, default=30, help="reads to take (30 at 20 s is ten minutes)")
    parser.add_argument("--interval", type=float, default=20.0, help="seconds between reads")
    parser.add_argument("--min-half-spread-bps", type=float, default=1.2)
    parser.add_argument("--min-volume-usd", type=float, default=500_000.0)
    parser.add_argument("--top", type=int, default=25, help="rows printed in the ranked table")
    parser.add_argument("--write", help="keep the raw reads here, one JSON object a line, so --read can re-derive this")
    parser.add_argument("--read", help="analyse a saved sample set instead of polling the venue")
    args = parser.parse_args()

    samples = {}
    if args.read:
        stored = json.loads(Path(args.read).read_text(encoding="utf-8"))
        samples = {symbol: {key: list(value) for key, value in entry.items()} for symbol, entry in stored.items()}
        stamps = [stamp for entry in samples.values() for stamp in entry["stamps"]]
        first, last = (min(stamps), max(stamps)) if stamps else ("?", "?")
    else:
        if args.write:
            Path(args.write).parent.mkdir(parents=True, exist_ok=True)
        print(f"polling {TICKERS} every {args.interval:g} s for {args.samples} reads", file=sys.stderr)
        for index in range(args.samples):
            stamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
            sample_once(samples, stamp)
            print(f"read {index + 1}/{args.samples} at {stamp}: {len(samples)} two-sided instruments", file=sys.stderr)
            if index + 1 < args.samples:
                time.sleep(args.interval)
        stamps = [stamp for entry in samples.values() for stamp in entry["stamps"]]
        first, last = (min(stamps), max(stamps)) if stamps else ("?", "?")
        if args.write:
            Path(args.write).write_text(json.dumps(samples), encoding="utf-8")

    rows = analyse(samples, args.min_half_spread_bps, args.min_volume_usd)
    passers = [row for row in rows if row["spread_ok"] and row["volume_ok"]]
    print(f"reads from {first} to {last}, {len(samples)} instruments with a two-sided book at least once")
    print(f"screen: median half spread >= {args.min_half_spread_bps:g} bps and median 24h notional >= "
          f"{args.min_volume_usd:,.0f} USD")
    print()
    print_table(passers, f"the screen's passers ({len(passers)} of {len(rows)})")
    print_table(rows[:args.top], f"the {min(args.top, len(rows))} widest, pass or fail")
    return 0


if __name__ == "__main__":
    sys.exit(main())

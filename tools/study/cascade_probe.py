#!/usr/bin/env python3
"""Probe the last unmeasured family: does a forced-flow burst reverse far enough to pay a taker round trip?

The rule was registered before the run, on the data already on disk, and it is written here rather than in a document so
that the number and its rule cannot part company:

* population: every trade of one day on one Deribit perpetual, from `data/v2/crypto/`;
* a burst is a one-second bucket whose |net aggressive volume| (buy amount minus sell amount) is at or above the
  percentile asked for of that day's own bucket distribution, and the burst's direction is the sign of that net;
* the trade is contrarian by default -- buy the burst's opposite side -- at the first trade at or after the bucket's end,
  and exit at the first trade at or after each horizon;
* the fee is the venue's taker rate a side, so twice it round trip; the table also prints the gross move, which separates
  the direction from the cost;
* events closer together than the cooldown are dropped after the first, so two entries do not share one reversion;
* an entry or an exit with no trade inside its tolerance is dropped rather than filled at a stale price.

`--momentum` runs the same events the other way, because a negative gross for the contrarian entry is a positive gross
for the continuation and the fee decides between them.

One day and one instrument is a probe, not a conclusion: it says whether a family is worth a multi-day run, and this
repository's rule is that a single window is not evidence for a line.

Usage:
    python3 tools/study/cascade_probe.py --instrument BTC-PERPETUAL
    python3 tools/study/cascade_probe.py --instrument ETH-PERPETUAL --percentile 99.5 --momentum
"""

import argparse
import csv
import gzip
import statistics
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DAY = "2020-04-01"


def load(symbol, day):
    """(timestamp, side, price, amount) in file order, which is the venue's own order."""
    path = ROOT / "data/v2/crypto" / f"deribit_trades_{day}_{symbol}.csv.gz"
    if not path.exists():
        raise SystemExit(f"missing input: {path}")
    trades = []
    with gzip.open(path, "rt", newline="") as handle:
        for row in csv.DictReader(handle):
            try:
                trades.append((int(row["timestamp"]), row["side"], float(row["price"]), float(row["amount"])))
            except (KeyError, TypeError, ValueError):
                continue
    return trades


def percentile_of(values, share):
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(round(share / 100.0 * (len(ordered) - 1)))))
    return ordered[index]


def first_price_at_or_after(trades, target, tolerance):
    low, high, best = 0, len(trades) - 1, None
    while low <= high:
        middle = (low + high) // 2
        if trades[middle][0] >= target:
            best, high = trades[middle], middle - 1
        else:
            low = middle + 1
    if best is None or best[0] - target > tolerance:
        return None
    return best



def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--instrument", required=True)
    parser.add_argument("--day", default=DAY)
    parser.add_argument("--percentile", type=float, default=99.0)
    parser.add_argument("--horizons", default="1,5,30,60", help="seconds")
    parser.add_argument("--entry-tolerance-us", type=int, default=5_000_000)
    parser.add_argument("--exit-tolerance-us", type=int, default=30_000_000)
    parser.add_argument("--cooldown-us", type=int, default=30_000_000)
    parser.add_argument("--taker-bps", type=float, default=5.0)
    parser.add_argument("--momentum", action="store_true", help="follow the burst instead of fading it")
    # `check_docs.py` appends `--binary <path>` to a `tool:` line, so a tool a document quotes accepts it. This one reads
    # trades only, so the path is taken and unused rather than a second argument name that the gate would not pass.
    parser.add_argument("--binary", default="", help="accepted for the document gate, which asserts it; unused here")
    args = parser.parse_args()

    trades = load(args.instrument, args.day)
    if not trades:
        raise SystemExit(f"no trades in {args.instrument} on {args.day}")

    buckets = {}
    for ts, side, _price, amount in trades:
        second = ts // 1_000_000
        buckets[second] = buckets.get(second, 0.0) + (amount if side == "buy" else -amount)
    threshold = percentile_of([abs(net) for net in buckets.values()], args.percentile)

    events = []
    last_entry = -(10**18)
    for second in sorted(buckets):
        net = buckets[second]
        if threshold <= 0.0 or abs(net) < threshold:
            continue
        entry = first_price_at_or_after(trades, (second + 1) * 1_000_000, args.entry_tolerance_us)
        if entry is None or entry[0] - last_entry < args.cooldown_us:
            continue
        last_entry = entry[0]
        # Contrarian: after a sell burst, buy. Momentum is the same events with the sign reversed.
        direction = 1.0 if net < 0 else -1.0
        events.append((entry, -direction if args.momentum else direction))

    label = "momentum" if args.momentum else "contrarian"
    print(f"### {args.instrument} {args.day}, {label} after a P{args.percentile:.0f} one-second burst: "
          f"{len(trades)} trades, {len(buckets)} buckets, threshold {threshold:.1f} contracts, {len(events)} bursts")
    print()
    print("| horizon s | fills | mean bps | median bps | share positive | mean at 0 bps |")
    print("|---:|---:|---:|---:|---:|---:|")
    best = None
    for horizon in [float(part) for part in args.horizons.split(",") if part.strip()]:
        nets, gross = [], []
        for entry, direction in events:
            exit_trade = first_price_at_or_after(trades, entry[0] + int(horizon * 1_000_000), args.exit_tolerance_us)
            if exit_trade is None:
                continue
            move = (exit_trade[2] / entry[2] - 1.0) * 10_000.0 * direction
            gross.append(move)
            nets.append(move - 2.0 * args.taker_bps)
        if not nets:
            print(f"| {horizon:g} | 0 | -- | -- | -- | -- |")
            continue
        mean = statistics.mean(nets)
        if best is None or mean > best[0]:
            best = (mean, horizon, statistics.mean(gross), len(nets))
        print(f"| {horizon:g} | {len(nets)} | {mean:.2f} | {statistics.median(nets):.2f} | "
              f"{sum(1 for value in nets if value > 0) / len(nets):.2f} | {statistics.mean(gross):.2f} |")
    print()
    if best is None:
        print(f"[CASCADE] instrument={args.instrument} mode={label} bursts={len(events)} best_net_bps=n/a")
        return 0
    print(f"[CASCADE] instrument={args.instrument} mode={label} bursts={len(events)} "
          f"best_net_bps={best[0]:.4f} best_horizon_s={best[1]:g} gross_at_best_bps={best[2]:.4f} fills={best[3]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

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
import bisect
import csv
import gzip
import heapq
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


def depth_file(symbol, day):
    """The incremental book for the same day, plain or gzipped, or an error naming the path."""
    for suffix in (".csv", ".csv.gz"):
        path = ROOT / "data/v2/crypto" / f"deribit_incremental_book_L2_{day}_{symbol}{suffix}"
        if path.exists():
            return path
    raise SystemExit(f"missing input: data/v2/crypto/deribit_incremental_book_L2_{day}_{symbol}.csv[.gz]")


def top_of_book_by_second(path):
    """(bid size, ask size, mid) at the end of every second the book existed for.

    The file carries one level change a row, so the book is a dict a side and the best price comes off a heap with lazy
    deletion: an amount of zero is a removal, and a heap entry whose price is no longer in the dict is stale. Only the
    second's last state is kept, because the condition below is a property of a second rather than of a row.
    """
    open_file = gzip.open if str(path).endswith(".gz") else open
    books = {"bid": {}, "ask": {}}
    heaps = {"bid": [], "ask": []}
    series = {}
    with open_file(path, "rt", newline="") as handle:
        reader = csv.reader(handle)
        header = next(reader, None)
        if not header:
            return series
        column = {name: index for index, name in enumerate(header)}
        i_ts, i_side, i_px, i_amt = column["timestamp"], column["side"], column["price"], column["amount"]
        current = None
        for row in reader:
            try:
                timestamp, side, price, amount = row[i_ts], row[i_side], float(row[i_px]), float(row[i_amt])
            except (IndexError, TypeError, ValueError):
                continue
            second = int(timestamp) // 1_000_000
            if current is not None and second != current:
                # The second is over: its state is what the book held at its last row, which is where we are now.
                record(series, books, heaps, current)
                current = second
            elif current is None:
                current = second
            if side not in books:
                continue
            if amount <= 0.0:
                books[side].pop(price, None)
            else:
                books[side][price] = amount
                heapq.heappush(heaps[side], -price if side == "bid" else price)
        if current is not None:
            record(series, books, heaps, current)
    return series


def record(series, books, heaps, second):
    """One second's last top of book, if both sides are there and uncrossed."""
    bid_px = best_price(books["bid"], heaps["bid"], highest=True)
    ask_px = best_price(books["ask"], heaps["ask"], highest=False)
    if bid_px is None or ask_px is None or ask_px <= bid_px:
        return
    series[second] = (books["bid"][bid_px], books["ask"][ask_px], 0.5 * (bid_px + ask_px))


def best_price(book, heap, highest):
    """The top of one side, popping heap entries whose level has left the book."""
    while heap:
        candidate = -heap[0] if highest else heap[0]
        if candidate in book:
            return candidate
        heapq.heappop(heap)
    return None


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


def first_through(trades, times, start_ts, limit_price, side, tick, patience_us):
    """Where a resting order at `limit_price` fills, or None.

    The conservative rule, and the reason this mode exists: a passive order is not filled because the price touched it.
    It fills when the market trades *through* it by at least one tick -- a buy needs a print at or below
    `limit_price - tick`, a sell one at or above `limit_price + tick` -- and the fill is at the limit, which is what a
    resting order gets. A model that fills on the touch is the one that overstates P_fill, and on this tape a print at
    the touch is the common case rather than an edge.
    """
    threshold = limit_price - side * tick
    index = bisect.bisect_left(times, start_ts)
    end_ts = start_ts + patience_us
    while index < len(trades) and trades[index][0] <= end_ts:
        price = trades[index][2]
        if (side > 0 and price <= threshold) or (side < 0 and price >= threshold):
            return limit_price, trades[index][0]
        index += 1
    return None


def last_price_within(trades, times, start_ts, patience_us):
    """The last print inside the patience window, which is where an exit that never filled is marked."""
    index = bisect.bisect_left(times, start_ts)
    end_ts = start_ts + patience_us
    last = None
    while index < len(trades) and trades[index][0] <= end_ts:
        last = trades[index][2]
        index += 1
    return last



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
    parser.add_argument("--maker-bps", type=float, default=2.0, help="the fee a passive leg pays")
    parser.add_argument("--entry-mode", choices=("taker", "passive"), default="taker")
    parser.add_argument("--exit-mode", choices=("taker", "passive"), default="taker")
    parser.add_argument("--tick", type=float, default=0.5, help="the instrument's price step, for the through rule")
    parser.add_argument("--patience-s", type=float, default=900.0,
                        help="how long a passive leg waits for a print one tick through it")
    parser.add_argument("--momentum", action="store_true", help="follow the burst instead of fading it")
    parser.add_argument("--condition", choices=("trades", "depth"), default="trades",
                        help="trades: a burst of aggressive volume; depth: the top of book thins out")
    parser.add_argument("--vacuum-fraction", type=float, default=0.2,
                        help="depth condition: top size at or below this share of its trailing median")
    parser.add_argument("--trailing-seconds", type=int, default=300,
                        help="depth condition: the window the median is taken over")
    parser.add_argument("--depth-file", default="",
                        help="the incremental book to read; default is the day's file for this instrument")
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
    burst_seconds = {second for second, net in buckets.items() if threshold > 0.0 and abs(net) >= threshold}

    vacuum_seconds = set()
    overlap = 0
    if args.condition == "depth":
        series = top_of_book_by_second(Path(args.depth_file) if args.depth_file else depth_file(args.instrument, args.day))
        if not series:
            raise SystemExit(f"no book in {args.instrument} on {args.day}")
        for second in sorted(series):
            window = [series[other][0] + series[other][1]
                      for other in range(second - args.trailing_seconds, second) if other in series]
            if len(window) < args.trailing_seconds // 3:
                continue
            if series[second][0] + series[second][1] <= args.vacuum_fraction * statistics.median(window):
                vacuum_seconds.add(second)
        overlap = len(vacuum_seconds & burst_seconds)
        candidates = [(second, net) for second, net in sorted(buckets.items()) if second in vacuum_seconds]
    else:
        candidates = [(second, buckets[second]) for second in sorted(buckets) if second in burst_seconds]

    events = []
    last_entry = -(10**18)
    for second, net in candidates:
        entry = first_price_at_or_after(trades, (second + 1) * 1_000_000, args.entry_tolerance_us)
        if entry is None or entry[0] - last_entry < args.cooldown_us:
            continue
        last_entry = entry[0]
        # Contrarian: after a sell burst, buy. Momentum is the same events with the sign reversed.
        direction = 1.0 if net < 0 else -1.0
        events.append((entry, -direction if args.momentum else direction))

    label = "momentum" if args.momentum else "contrarian"
    if args.condition == "depth":
        print(f"### {args.instrument} {args.day}, {label} after a depth vacuum: {len(trades)} trades, "
              f"{len(vacuum_seconds)} vacuum seconds (top size at or below "
              f"{args.vacuum_fraction:.2f} x its {args.trailing_seconds} s median, {overlap} of them also P99 bursts), "
              f"{len(events)} events")
    else:
        print(f"### {args.instrument} {args.day}, {label} after a P{args.percentile:.0f} one-second burst: "
              f"{len(trades)} trades, {len(buckets)} buckets, threshold {threshold:.1f} contracts, {len(events)} bursts")
    print()
    passive = args.entry_mode == "passive" or args.exit_mode == "passive"
    print("| horizon s | fills | mean bps | median bps | share positive | mean at 0 bps |" +
          (" unfilled |" if passive else ""))
    print("|---:|---:|---:|---:|---:|---:|" + ("---:|" if passive else ""))
    times = [trade[0] for trade in trades]
    patience_us = int(args.patience_s * 1_000_000)
    best = None
    for horizon in [float(part) for part in args.horizons.split(",") if part.strip()]:
        nets, gross = [], []
        unfilled = 0
        for entry, direction in events:
            entry_price, entry_ts, entry_fee = entry[2], entry[0], args.taker_bps
            if args.entry_mode == "passive":
                # Rest at the price the burst printed at and wait for the market to come through it.
                filled = first_through(trades, times, entry[0], entry[2], direction, args.tick, patience_us)
                if filled is None:
                    unfilled += 1
                    continue
                entry_price, entry_ts = filled
                entry_fee = args.maker_bps
            exit_trade = first_price_at_or_after(trades, entry_ts + int(horizon * 1_000_000),
                                                 args.exit_tolerance_us)
            if exit_trade is None:
                continue
            exit_price, exit_fee = exit_trade[2], args.taker_bps
            if args.exit_mode == "passive":
                # The exit is the opposite side of the position, and it is also charged the through rule.
                filled = first_through(trades, times, exit_trade[0], exit_trade[2], -direction, args.tick,
                                       patience_us)
                if filled is None:
                    # Nothing came through: the position is marked at the last print it could have been closed at
                    # and pays the taker fee, which is what an unfilled exit costs in practice.
                    unfilled += 1
                    mark = last_price_within(trades, times, exit_trade[0], patience_us)
                    if mark is None:
                        continue
                    exit_price, exit_fee = mark, args.taker_bps
                else:
                    exit_price = filled[0]
                    exit_fee = args.maker_bps
            move = (exit_price / entry_price - 1.0) * 10_000.0 * direction
            gross.append(move)
            nets.append(move - entry_fee - exit_fee)
        if not nets:
            print(f"| {horizon:g} | 0 | -- | -- | -- | -- |" + (" -- |" if passive else ""))
            continue
        mean = statistics.mean(nets)
        if best is None or mean > best[0]:
            best = (mean, horizon, statistics.mean(gross), len(nets))
        suffix = f" {unfilled} |" if passive else ""
        print(f"| {horizon:g} | {len(nets)} | {mean:.2f} | {statistics.median(nets):.2f} | "
              f"{sum(1 for value in nets if value > 0) / len(nets):.2f} | {statistics.mean(gross):.2f} |{suffix}")
    print()
    if best is None:
        print(f"[CASCADE] instrument={args.instrument} mode={label} condition={args.condition} "
              f"events={len(events)} best_net_bps=n/a")
        return 0
    extra = f" vacuum_seconds={len(vacuum_seconds)} overlap_p99={overlap}" if args.condition == "depth" else ""
    print(f"[CASCADE] instrument={args.instrument} mode={label} condition={args.condition} "
          f"bursts={len(events)} best_net_bps={best[0]:.4f} best_horizon_s={best[1]:g} "
          f"gross_at_best_bps={best[2]:.4f} fills={best[3]}{extra}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

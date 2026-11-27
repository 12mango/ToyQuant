#!/usr/bin/env python3
"""A day's physical profile in half-hour buckets: the half-spread the book held and the rate the tape printed.

Why this exists. The gates a maker carries read the book -- a quote needs an edge over the fee, and the touch has to be
wide enough to stand behind -- and neither of them says whether anybody is trading. A quiet hour can show a perfectly wide
spread that no print ever reaches, and a strategy that quotes it fills only when the flow that does arrive is the informed
kind. Measured 2026-11-25, widening a two-hour window to eight lowered the closed median from +12.40 to -1.89 bps, and this
tool is how the cause was looked at without touching the strategy: two physical quantities per bucket, no P&L, no window
chosen for being positive.

    python3 tools/study/okx_halfhour_profile.py --symbol UP-USDT-SWAP \\
        --days 2026-11-16,2026-11-17,...,2026-11-22

The spread is duration-weighted inside each bucket -- a book that was two ticks wide for fifty minutes and ten for ten is
not described by the median of its records -- and the trade rate is the count in the bucket over its length. The table's
cells are the *median across days*, so one busy day cannot carry a bucket.

The trade archive is cut on midnight UTC+8 and the book archive on UTC, so a UTC day reads the second half of one trade file
and the first half of the next. Both halves are read and bucketed by the record's own timestamp; a profile built from one
file would show an afternoon with no trades at all.
"""

import argparse
import csv
import json
import statistics
import sys
import tarfile
from datetime import datetime, timedelta, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "lib"))
from okx_data import depth_archive, trade_tape  # noqa: E402
MICROSECONDS_PER_DAY = 86_400_000_000


def buckets_per_day(bucket_us):
    """How many buckets one UTC day has at this width -- the width is a flag, so this cannot be a constant.

    The width here is **microseconds**, which is the unit the book's stamps are converted to (see `spread_profile`):
    measured 2026-11-28, `--bucket-minutes 10` folded a day into 48 buckets -- an eight-hour cycle -- because the count
    was a constant sized for 30, so every row after 08:00 mixed three different parts of the day. The default 30 was the
    only width the old constant was right for, which is why no recorded number moved when it became a function.
    """
    return MICROSECONDS_PER_DAY // bucket_us


def bucket_of(ts_us, bucket_us):
    return (ts_us // bucket_us) % buckets_per_day(bucket_us)


def trade_rates(symbol, day_text, bucket_ms, trades_dir):
    """Trades a second per bucket, from the two UTC+8 files a UTC day spans."""
    day = datetime.strptime(day_text, "%Y-%m-%d").replace(tzinfo=timezone.utc)
    start_ms = int(day.timestamp() * 1000)
    end_ms = start_ms + 24 * 3600 * 1000
    counts = [0] * buckets_per_day(bucket_ms * 1000)
    # The file named X covers [X-1 16:00 UTC, X 16:00 UTC), so the UTC day D is the head of D's file and the
    # afternoon of D+1's. Reading D-1 and D instead left every bucket after 16:00 with no trades -- measured
    # 2026-11-25, the first version of this table showed 0.000 trades a second for the whole evening.
    for name in (day.strftime("%Y-%m-%d"), (day + timedelta(days=1)).strftime("%Y-%m-%d")):
        path = trade_tape(trades_dir, symbol, name)
        if path is None:
            continue
        with path.open(newline="") as handle:
            reader = csv.reader(handle)
            header = next(reader, None)
            if not header:
                continue
            try:
                column = header.index("created_time")
            except ValueError:
                continue
            for row in reader:
                try:
                    ts = int(row[column])
                except (IndexError, TypeError, ValueError):
                    continue
                if start_ms <= ts < end_ms:
                    counts[bucket_of(ts * 1000, bucket_ms * 1000)] += 1
    return [count / (bucket_ms / 1000.0) for count in counts]


def weighted_median(items):
    """The value that splits the duration in half, not the count."""
    if not items:
        return None
    items = sorted(items, key=lambda item: item[1])
    total = sum(duration for duration, _ in items)
    running = 0.0
    for duration, value in items:
        running += duration
        if running * 2 >= total:
            return value
    return items[-1][1]


def spread_profile(archive, bucket_ms):
    """(duration, half spread bps) per bucket, from one pass over the archive."""
    samples = [[] for _ in range(buckets_per_day(bucket_ms * 1000))]
    book = {"bid": {}, "ask": {}}
    best = {"bid": None, "ask": None}
    last_ts = None
    current = None
    with tarfile.open(archive) as handle:
        member = next((item for item in handle.getmembers() if item.name.endswith(".data")), None)
        if member is None:
            raise SystemExit(f"{archive}: no .data member")
        for line in handle.extractfile(member):
            if not line.startswith(b"{"):
                continue
            record = json.loads(line)
            # The archive's ts is milliseconds; the durations below are microseconds so that a 30-minute bucket is
            # 1.8e9 of them rather than a float.
            ts = int(record["ts"]) * 1000
            bucket = bucket_of(ts, bucket_ms * 1000)
            if last_ts is not None and ts > last_ts and current is not None:
                bid, ask = best["bid"], best["ask"]
                if bid is not None and ask is not None and bid < ask:
                    mid = 0.5 * (bid + ask)
                    # The half-spread is what a maker earns and what the fee is compared against; the full
                    # spread is twice it, and the first version of this table did not divide.
                    samples[current].append((ts - last_ts, (ask - bid) / 2.0 / mid * 1e4))
            last_ts = ts
            current = bucket
            touched = {"bid": [], "ask": []}
            for key, side in (("bids", "bid"), ("asks", "ask")):
                for price, size, *_ in record.get(key, []):
                    price, size = float(price), float(size)
                    if size == 0.0:
                        book[side].pop(price, None)
                    else:
                        book[side][price] = size
                    touched[side].append((price, size))
            # The best is maintained rather than recomputed: a full sort per record is what made the first version of
            # this pass take minutes a day. A level that leaves the book is the only case that needs the scan.
            if not book["bid"]:
                best["bid"] = None
            elif best["bid"] is None or best["bid"] not in book["bid"]:
                best["bid"] = max(book["bid"])
            else:
                for price, size in touched["bid"]:
                    if size > 0.0 and price > best["bid"]:
                        best["bid"] = price
            if not book["ask"]:
                best["ask"] = None
            elif best["ask"] is None or best["ask"] not in book["ask"]:
                best["ask"] = min(book["ask"])
            else:
                for price, size in touched["ask"]:
                    if size > 0.0 and price < best["ask"]:
                        best["ask"] = price
    return [weighted_median(items) for items in samples]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--days", required=True, help="comma separated UTC days, as the archive names them")
    parser.add_argument("--symbol", default="UP-USDT-SWAP")
    parser.add_argument("--archives", default="data/v2/crypto")
    parser.add_argument("--trades", default="data/v2/crypto")
    parser.add_argument("--bucket-minutes", type=int, default=30)
    parser.add_argument("--gate-rate", type=float, default=2.5,
                        help="trades a second the table counts each bucket against, which is the density gate's number")
    args = parser.parse_args()

    bucket_ms = args.bucket_minutes * 60 * 1000
    spreads, rates = [], []
    for day_text in [part.strip() for part in args.days.split(",") if part.strip()]:
        archive = depth_archive(ROOT / args.archives, args.symbol, day_text)
        if archive is None:
            print(f"# {day_text}: no {args.symbol}-L2orderbook-400lv-{day_text}.tar.gz under {args.archives}")
            continue
        spreads.append(spread_profile(archive, bucket_ms))
        rates.append(trade_rates(args.symbol, day_text, bucket_ms, ROOT / args.trades))
        print(f"# {day_text}: profiled", flush=True)

    if not spreads:
        print("no rows: nothing measured")
        return 1
    print(f"| bucket UTC | median half spread bps | median trades/s | min trades/s | days at or above "
          f"{args.gate_rate:g}/s | days |")
    print("|---|---:|---:|---:|---:|---:|")
    for index in range(buckets_per_day(bucket_ms * 1000)):
        column = [row[index] for row in spreads if row[index] is not None]
        rate_column = [row[index] for row in rates]
        hours = index * args.bucket_minutes // 60
        minutes = index * args.bucket_minutes % 60
        spread_text = f"{statistics.median(column):.2f}" if column else "n/a"
        rate_text = f"{statistics.median(rate_column):.3f}" if rate_column else "n/a"
        min_text = f"{min(rate_column):.3f}" if rate_column else "n/a"
        above = sum(1 for value in rate_column if value >= args.gate_rate)
        print(f"| {hours:02d}:{minutes:02d} | {spread_text} | {rate_text} | {min_text} | {above} of "
              f"{len(rate_column)} | {len(rate_column)} |")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

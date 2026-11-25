#!/usr/bin/env python3
"""The tape's own toxicity per half hour: where a print was, and where the mid went after it.

The half-spread profile answered whether the book is wide and whether anybody trades; it cannot say whether the flow that
does arrive is informed. This measures that, from the market's prints alone -- no strategy, no fills of ours, no P&L --
so the number is a property of the flow rather than of where we happened to quote.

    python3 tools/study/okx_tape_toxicity.py --days 2026-11-16,...,2026-11-22 --horizon-ms 1000

Per print: the signed move of the mid over `--horizon-ms`, with the sign of the aggressor, reported as the *maker's*
markout -- so a positive cell means the mid came back towards the passive side and a negative one means the print was
followed by a move in its own direction, which is the fill a passive quote would regret. Buckets are UTC half hours; the
table's cells are the median across days, with the print count beside them because a median over three prints is not a
measurement.

The mid is sampled every `--sample-ms` from the archive and looked up by bisect, because holding one sample per record
would cost gigabytes for a number whose resolution is a second anyway.
"""

import argparse
import bisect
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
BUCKETS_PER_DAY = 48


def bucket_of(ts_ms, bucket_ms):
    return (ts_ms // bucket_ms) % BUCKETS_PER_DAY


def mid_series(archive, sample_ms):
    """[(ts_ms, mid)] sampled at most every `sample_ms`, from one pass over the archive.

    The book walk is the same shape as the half-hour profile's: a dict a side and the best maintained rather than
    recomputed, since a level leaving the book is the only case that needs the scan.
    """
    series = []
    book = {"bid": {}, "ask": {}}
    best = {"bid": None, "ask": None}
    last_sample = None
    with tarfile.open(archive) as handle:
        member = next((item for item in handle.getmembers() if item.name.endswith(".data")), None)
        if member is None:
            raise SystemExit(f"{archive}: no .data member")
        for line in handle.extractfile(member):
            if not line.startswith(b"{"):
                continue
            record = json.loads(line)
            ts = int(record["ts"])
            touched = {"bid": [], "ask": []}
            for key, side in (("bids", "bid"), ("asks", "ask")):
                for price, size, *_ in record.get(key, []):
                    price, size = float(price), float(size)
                    if size == 0.0:
                        book[side].pop(price, None)
                    else:
                        book[side][price] = size
                    touched[side].append((price, size))
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
            if last_sample is not None and ts - last_sample < sample_ms:
                continue
            bid, ask = best["bid"], best["ask"]
            if bid is None or ask is None or bid >= ask:
                continue
            series.append((ts, 0.5 * (bid + ask)))
            last_sample = ts
    return series


def prints_of_day(symbol, day_text, trades_dir):
    """(ts_ms, aggressor side, price) for the UTC day, from the two UTC+8 files it spans."""
    day = datetime.strptime(day_text, "%Y-%m-%d").replace(tzinfo=timezone.utc)
    start_ms = int(day.timestamp() * 1000)
    end_ms = start_ms + 24 * 3600 * 1000
    rows = []
    # The file named X covers [X-1 16:00 UTC, X 16:00 UTC): the UTC day D is D's head and D+1's afternoon.
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
                i_ts, i_side, i_price = (header.index("created_time"), header.index("side"),
                                         header.index("price"))
            except ValueError:
                continue
            for row in reader:
                try:
                    ts = int(row[i_ts])
                    price = float(row[i_price])
                except (IndexError, TypeError, ValueError):
                    continue
                if start_ms <= ts < end_ms:
                    rows.append((ts, row[i_side], price))
    rows.sort(key=lambda row: row[0])
    return rows


def toxicity_for_day(symbol, day_text, archive, horizon_ms, sample_ms, bucket_ms, trades_dir,
                     min_distance_ticks=0.0, tick=0.0001):
    """Per bucket: median and mean maker markout, the share of prints the mid moved on, and the count.

    A median alone is degenerate here: one tick on this instrument is 5.2 bps (0.0001 on a 0.19 price) and the mid often
    does not move a tick at all within the horizon, so zero is the mode in every bucket (measured 2026-11-25 at 1 s and
    5 s: every cell read +0.000). The mean carries the adverse selection the median hides, and the moved share says how
    much of it there is to average.

    `min_distance_ticks` keeps only the prints that were at least that far through the mid when they happened -- the
    subset a quote standing behind the touch is filled by, rather than the prints that only reach the touch.
    """
    series = mid_series(archive, sample_ms)
    if not series:
        return [None] * BUCKETS_PER_DAY, [None] * BUCKETS_PER_DAY, [0.0] * BUCKETS_PER_DAY, [0] * BUCKETS_PER_DAY
    times = [ts for ts, _ in series]
    mids = [mid for _, mid in series]
    floor = min_distance_ticks * tick
    values = [[] for _ in range(BUCKETS_PER_DAY)]
    for ts, side, price in prints_of_day(symbol, day_text, trades_dir):
        before = bisect.bisect_left(times, ts)
        after = bisect.bisect_left(times, ts + horizon_ms)
        if before >= len(times) or after >= len(times) or after <= before:
            continue
        mid = mids[before]
        if mid <= 0.0 or price <= 0.0:
            continue
        if floor > 0.0 and abs(price - mid) < floor:
            continue
        # The aggressor's signed move, negated: what the passive side made of the print.
        direction = 1.0 if side == "buy" else -1.0
        maker_bps = -(mids[after] - mid) * direction / mid * 1e4
        values[bucket_of(ts, bucket_ms)].append(maker_bps)
    medians = [statistics.median(v) if v else None for v in values]
    means = [statistics.mean(v) if v else None for v in values]
    moved = [sum(1 for x in v if x != 0.0) / len(v) if v else 0.0 for v in values]
    return medians, means, moved, [len(v) for v in values]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--days", required=True, help="comma separated UTC days, as the archive names them")
    parser.add_argument("--symbol", default="UP-USDT-SWAP")
    parser.add_argument("--archives", default="data/v2/crypto")
    parser.add_argument("--trades", default="data/v2/crypto")
    parser.add_argument("--horizon-ms", type=int, default=1000)
    parser.add_argument("--sample-ms", type=int, default=100)
    parser.add_argument("--bucket-minutes", type=int, default=30)
    parser.add_argument("--tick", type=float, default=0.0001)
    parser.add_argument("--min-distance-ticks", type=float, default=0.0,
                        help="keep only prints at least this far through the mid, the subset a deeper quote is filled by")
    args = parser.parse_args()

    bucket_ms = args.bucket_minutes * 60 * 1000
    days_medians, days_means, days_moved, days_counts = [], [], [], []
    for day_text in [part.strip() for part in args.days.split(",") if part.strip()]:
        archive = depth_archive(ROOT / args.archives, args.symbol, day_text)
        if archive is None:
            print(f"# {day_text}: no {args.symbol}-L2orderbook-400lv-{day_text}.tar.gz under {args.archives}")
            continue
        medians, means, moved, counts = toxicity_for_day(args.symbol, day_text, archive, args.horizon_ms,
                                                         args.sample_ms, bucket_ms, ROOT / args.trades,
                                                         args.min_distance_ticks, args.tick)
        days_medians.append(medians)
        days_means.append(means)
        days_moved.append(moved)
        days_counts.append(counts)
        print(f"# {day_text}: profiled", flush=True)
    if not days_medians:
        print("no rows: nothing measured")
        return 1

    print(f"| bucket UTC | maker markout bps at +{args.horizon_ms} ms, mean | median | moved | prints |")
    print("|---|---:|---:|---:|---:|")
    for index in range(BUCKETS_PER_DAY):
        mean_column = [row[index] for row in days_means if row[index] is not None]
        median_column = [row[index] for row in days_medians if row[index] is not None]
        moved_column = [row[index] for row in days_moved]
        count_column = [row[index] for row in days_counts]
        hours = index * args.bucket_minutes // 60
        minutes = index * args.bucket_minutes % 60
        mean = f"{statistics.mean(mean_column):+.3f}" if mean_column else "n/a"
        median = f"{statistics.median(median_column):+.3f}" if median_column else "n/a"
        moved = f"{statistics.mean(moved_column) * 100.0:.0f}%" if moved_column else "n/a"
        count = f"{statistics.median(count_column):.0f}" if count_column else "n/a"
        print(f"| {hours:02d}:{minutes:02d} | {mean} | {median} | {moved} | {count} |")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

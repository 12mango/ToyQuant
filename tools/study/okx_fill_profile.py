#!/usr/bin/env python3
"""Our own fills per half hour, from a run's own trade stream: capture, markout, and how much of it opened a position.

The tape profile (`okx_tape_toxicity.py`) measures the market's flow; this measures the subset our quotes actually got,
which is the quantity that separated the profitable two hours from the night. It reads the file every replay already
writes -- `data/runtime/trades.csv`, `ts,symbol,side,price,quantity,order_id,liquidity_role,fee` -- rather than
`--record-orders`, because that flag used to wrap the venue in `RecordingEngine` and change the run: measured 2026-11-25,
the same day and window read 123 fills without it and 207 with it. That defect is fixed -- the decorator forwarded
`process_bbo` and not `process_l2_top`, so L2 updates arrived as BBO ones and the engine's queue accounting never ran --
and the A/B now reads 123 fills on both sides. The trade stream stays the source here anyway, because this tool counts
fills and that is the file a fill is written to.

    python3 tools/study/okx_fill_profile.py --fills /tmp/tq/fp/trades_2026-11-16.csv,... \\
        --days 2026-11-16,...

Per fill: the capture against the midpoint at the fill (`side`-signed, so a buy below the mid is positive), the markout
at two horizons (`+5 s` and `+30 s`, the mid's move in our favour), and whether the fill increased or reduced the
position -- a bucket whose fills mostly increase it is the one-sided inventory the four-block comparison could not see.
Buckets are UTC half hours; cells are medians across days, with the day count beside the fill count.
"""

import argparse
import bisect
import csv
import json
import statistics
import sys
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "lib"))
from okx_data import depth_archive  # noqa: E402
BUCKETS_PER_DAY = 48
QUANTITY_SCALE = 10_000_000  # engine units of one contract, as the sweep's `--order-size` uses


def bucket_of(ts_ms, bucket_ms):
    return (ts_ms // bucket_ms) % BUCKETS_PER_DAY


def mid_series(archive, sample_ms):
    """[(ts_ms, mid)] sampled at most every `sample_ms`, one pass over the day's archive."""
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
            for side, pick in (("bid", max), ("ask", min)):
                if not book[side]:
                    best[side] = None
                elif best[side] is None or best[side] not in book[side]:
                    best[side] = pick(book[side])
                else:
                    for price, size in touched[side]:
                        better = price > best[side] if side == "bid" else price < best[side]
                        if size > 0.0 and better:
                            best[side] = price
            if last_sample is not None and ts - last_sample < sample_ms:
                continue
            bid, ask = best["bid"], best["ask"]
            if bid is None or ask is None or bid >= ask:
                continue
            series.append((ts, 0.5 * (bid + ask)))
            last_sample = ts
    return series


def fills_of_file(path):
    """(ts, side, price, quantity) from a run's own trade stream."""
    rows = []
    with Path(path).open(newline="") as handle:
        lines = [line for line in handle if not line.startswith("#")]
    reader = csv.DictReader(lines)
    for row in reader:
        try:
            rows.append((int(row["ts"]), row["side"], float(row["price"]), int(row["quantity"])))
        except (KeyError, TypeError, ValueError):
            continue
    rows.sort(key=lambda row: row[0])
    return rows


def profile_day(fills_path, archive, day_text, sample_ms, bucket_ms, horizons_ms):
    """Per bucket: counts, net contracts, the share that opened, capture and markout medians."""
    series = mid_series(archive, sample_ms)
    if not series:
        return None
    times = [ts for ts, _ in series]
    mids = [mid for _, mid in series]
    opened = [0] * BUCKETS_PER_DAY
    closed = [0] * BUCKETS_PER_DAY
    counts = [0] * BUCKETS_PER_DAY
    net_units = [0] * BUCKETS_PER_DAY
    capture = [[] for _ in range(BUCKETS_PER_DAY)]
    markout = {h: [[] for _ in range(BUCKETS_PER_DAY)] for h in horizons_ms}
    position = 0
    for ts, side, price, quantity in fills_of_file(fills_path):
        index = bucket_of(ts, bucket_ms)
        direction = 1 if side == "B" else -1
        before = abs(position)
        position += direction * quantity
        after = abs(position)
        counts[index] += 1
        net_units[index] += direction * quantity
        # A fill that grew the position opened it; one that shrank it closed it. A flip counts as both, which is what
        # it is: one side closed and the other opened in the same print.
        if after > before:
            opened[index] += 1
        if after < before or (before > 0 and position * direction < 0):
            closed[index] += 1
        at = bisect.bisect_left(times, ts)
        if at >= len(times):
            continue
        mid = mids[at]
        if mid <= 0.0 or price <= 0.0:
            continue
        capture[index].append(direction * (mid - price) / mid * 1e4)
        for horizon in horizons_ms:
            later = bisect.bisect_left(times, ts + horizon)
            if later < len(times):
                markout[horizon][index].append(direction * (mids[later] - mid) / mid * 1e4)
    return {"counts": counts, "opened": opened, "closed": closed, "net_units": net_units,
            "capture": capture, "markout": markout}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--fills", required=True, help="comma separated trade streams, one per day, in --days order")
    parser.add_argument("--days", required=True, help="comma separated UTC days the fills belong to")
    parser.add_argument("--symbol", default="UP-USDT-SWAP")
    parser.add_argument("--archives", default="data/v2/crypto")
    parser.add_argument("--sample-ms", type=int, default=100)
    parser.add_argument("--bucket-minutes", type=int, default=30)
    parser.add_argument("--horizons-ms", default="5000,30000")
    args = parser.parse_args()

    fills = [part.strip() for part in args.fills.split(",") if part.strip()]
    days = [part.strip() for part in args.days.split(",") if part.strip()]
    if len(fills) != len(days):
        raise SystemExit("--fills and --days must have one entry each, in the same order")
    horizons = [int(part) for part in args.horizons_ms.split(",") if part.strip()]
    bucket_ms = args.bucket_minutes * 60 * 1000

    profiles = []
    for fills_path, day_text in zip(fills, days):
        archive = depth_archive(ROOT / args.archives, args.symbol, day_text)
        if archive is None or not Path(fills_path).exists():
            print(f"# {day_text}: no fills or no archive, skipped")
            continue
        profile = profile_day(fills_path, archive, day_text, args.sample_ms, bucket_ms, horizons)
        if profile is not None:
            profiles.append(profile)
        print(f"# {day_text}: profiled", flush=True)
    if not profiles:
        print("no rows: nothing measured")
        return 1

    header = "| bucket UTC | fills a day | net contracts | opened share | capture bps |"
    for horizon in horizons:
        header += f" markout bps at +{horizon} ms |"
    print(header)
    print("|---:|---:|---:|---:|---:|" + "---:|" * len(horizons))
    for index in range(BUCKETS_PER_DAY):
        counts = [profile["counts"][index] for profile in profiles]
        if not any(counts):
            continue
        active = [profile for profile in profiles if profile["counts"][index]]
        opens = sum(profile["opened"][index] for profile in active)
        total = sum(profile["counts"][index] for profile in active)
        nets = [profile["net_units"][index] / QUANTITY_SCALE for profile in active]
        capture_column = [statistics.median(profile["capture"][index]) for profile in active
                          if profile["capture"][index]]
        hours, minutes = index * args.bucket_minutes // 60, index * args.bucket_minutes % 60
        cells = [f"| {hours:02d}:{minutes:02d}", f"{statistics.median(counts):.0f}",
                 f"{statistics.median(nets):+.2f}", f"{opens / total * 100.0:.0f}%",
                 f"{statistics.median(capture_column):+.2f}" if capture_column else "n/a"]
        for horizon in horizons:
            column = [statistics.median(profile["markout"][horizon][index]) for profile in active
                      if profile["markout"][horizon][index]]
            cells.append(f"{statistics.median(column):+.3f}" if column else "n/a")
        print(" | ".join(cells) + " |")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

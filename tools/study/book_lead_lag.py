#!/usr/bin/env python3
"""Does the leader's book move before the follower's, and by how much -- the measurement the lead-lag row waits on.

The frontier row (`docs/RETROSPECTIVE.md`, "cross-book lead-lag") is bound to one number it does not have: **the lead on
this venue**. What it has instead is our own round trip -- 77 to 118 ms a placement and 76 to 78 ms a cancel, measured
2026-11-25 -- and the literature's 10 to 100 ms, which is *shorter than the reaction it would have to beat*. So the row
is bought or killed by this number: a lead below our cancel's 76 ms cannot be acted on, one well above our placement's
118 ms can.

Method: two mid-price series at a fixed cadence from the two 400-level archives, returns in bps between consecutive
samples, and the cross-correlation of the follower's return against the leader's at every lag. The peak's lag is the
lead; the correlation at zero is what a same-clock reader would see.

    python3 tools/study/book_lead_lag.py --leader BTC-USDT-SWAP --follower UP-USDT-SWAP \\
        --days 2026-11-16,2026-11-17 --start 13:30 --end 15:30
"""
import argparse
import json
import sys
import tarfile
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "lib"))
from okx_data import depth_archive  # noqa: E402


def mids(archive, start_ms, end_ms):
    """(timestamp ms, mid) each time the touch changed, for records inside the window.

    The book is maintained from the file's first record so the window opens on a populated touch, and only the series is
    clipped to the window.
    """
    book = {"bid": {}, "ask": {}}
    best = {"bid": None, "ask": None}
    series = []
    last_mid = None
    with tarfile.open(archive) as handle:
        member = next((item for item in handle.getmembers() if item.name.endswith(".data")), None)
        if member is None:
            raise SystemExit(f"{archive}: no .data member")
        for line in handle.extractfile(member):
            if not line.startswith(b"{"):
                continue
            record = json.loads(line)
            ts = int(record["ts"])
            if ts >= end_ms:
                break
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
            if best["bid"] is None or best["ask"] is None:
                continue
            mid = 0.5 * (best["bid"] + best["ask"])
            if mid != last_mid:
                # The pre-window changes are kept in the series so `resample` has a value to fill forward from; the
                # window itself starts on a populated touch rather than on a run of `None` (measured 2026-11-28: the
                # first grid point of both instruments read `None`).
                series.append((ts, mid))
                last_mid = mid
    return series


def resample(series, start_ms, cadence_ms, count):
    """Forward-filled mid on a fixed grid, so two instruments' returns are on one clock."""
    grid = []
    index = 0
    current = None
    while index < len(series) and series[index][0] <= start_ms:
        current = series[index][1]
        index += 1
    for step in range(count):
        stamp = start_ms + step * cadence_ms
        while index < len(series) and series[index][0] <= stamp:
            current = series[index][1]
            index += 1
        grid.append(current)
    return grid


def returns_bps(grid):
    out = []
    for previous, current in zip(grid, grid[1:]):
        if previous is None or current is None or previous <= 0:
            out.append(0.0)
        else:
            out.append((current / previous - 1.0) * 1e4)
    return out


def correlate(follower, leader, lag):
    """corr(follower[t], leader[t - lag]); a positive lag means the leader's return came first."""
    if lag > 0:
        a, b = follower[lag:], leader[:-lag]
    elif lag < 0:
        a, b = follower[:lag], leader[-lag:]
    else:
        a, b = follower, leader
    n = min(len(a), len(b))
    if n < 10:
        return None
    a, b = a[:n], b[:n]
    mean_a = sum(a) / n
    mean_b = sum(b) / n
    cov = sum((x - mean_a) * (y - mean_b) for x, y in zip(a, b))
    var_a = sum((x - mean_a) ** 2 for x in a)
    var_b = sum((y - mean_b) ** 2 for y in b)
    if var_a <= 0 or var_b <= 0:
        return None
    return cov / (var_a * var_b) ** 0.5


def window_bounds(day, start, end):
    base = datetime.strptime(day, "%Y-%m-%d").replace(tzinfo=timezone.utc)
    start_dt = base.replace(hour=int(start[:2]), minute=int(start[3:5]))
    end_dt = base.replace(hour=int(end[:2]), minute=int(end[3:5]))
    return int(start_dt.timestamp() * 1000), int(end_dt.timestamp() * 1000)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--leader", default="BTC-USDT-SWAP")
    parser.add_argument("--follower", default="UP-USDT-SWAP")
    parser.add_argument("--days", required=True, help="comma separated days both archives exist for")
    parser.add_argument("--start", default="13:30")
    parser.add_argument("--end", default="15:30")
    parser.add_argument("--cadence-ms", type=int, default=100)
    parser.add_argument("--max-lag-ms", type=int, default=2000)
    parser.add_argument("--archives", default="data/v2/crypto")
    args = parser.parse_args()

    steps = args.max_lag_ms // args.cadence_ms
    lags = list(range(-steps, steps + 1))
    peaks = []
    for day in [part.strip() for part in args.days.split(",") if part.strip()]:
        start_ms, end_ms = window_bounds(day, args.start, args.end)
        leader_archive = depth_archive(ROOT / args.archives, args.leader, day)
        follower_archive = depth_archive(ROOT / args.archives, args.follower, day)
        if leader_archive is None or follower_archive is None:
            print(f"# {day}: a missing archive, skipped")
            continue
        count = (end_ms - start_ms) // args.cadence_ms
        leader = returns_bps(resample(mids(leader_archive, start_ms, end_ms), start_ms, args.cadence_ms, count))
        follower = returns_bps(resample(mids(follower_archive, start_ms, end_ms), start_ms, args.cadence_ms, count))
        usable = {lag: value for lag in lags
                  for value in [correlate(follower, leader, lag)] if value is not None}
        if not usable:
            print(f"# {day}: no correlation measurable")
            continue
        best = max(usable, key=usable.get)
        peaks.append(best * args.cadence_ms)
        at_78 = usable.get(max(1, round(78 / args.cadence_ms)), float("nan"))
        at_118 = usable.get(max(1, round(118 / args.cadence_ms)), float("nan"))
        print(f"# {day}: peak lag {best * args.cadence_ms:+5d} ms corr {usable[best]:+.3f} | at 0 ms "
              f"{usable.get(0, float('nan')):+.3f} | at +78 ms {at_78:+.3f} | at +118 ms {at_118:+.3f}", flush=True)
    if not peaks:
        print("no rows: nothing measured")
        return 1
    peaks.sort()
    print(f"\npeak lag over {len(peaks)} day(s): median {peaks[len(peaks) // 2]:+d} ms, "
          f"min {peaks[0]:+d}, max {peaks[-1]:+d}; our cancel round trip is 76-78 ms, a placement 77-118 ms")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""What a state gate is still worth once the cancel has to travel: the same counterfactual, with latency in it.

A fill-level counterfactual that drops every fill taken while the leader was moving assumes the cancel was already there.
It was not: the measured round trip is 77 to 118 ms, so a state that turns inside that window cannot act on the fill, and
counting it as avoided is the optimism this tool prices.

For each maker fill it reads the leader's 3 s return twice -- at the fill (`now`) and one latency earlier (`before`) -- and
sorts the fill into three kinds:

  * flat          -- `|now|` under the threshold, so no gate fires either way;
  * avoidable     -- moving `now` **and** moving `before`: flagged at least one latency ahead, so a cancel could have landed;
  * unavoidable   -- moving `now` but flat `before`: the state turned inside the latency, the fill a gate cannot reach.

Three sets follow, and the verdict the review asked for is the last two compared:

  no gate        -- every fill;
  gate, 0 us     -- drops all moving fills, the counterfactual as usually written;
  gate, N us     -- drops only the avoidable ones.

    python3 tools/study/gate_latency_counterfactual.py --days 2026-11-05,2026-11-26 --in-flight-us 75000
"""
import argparse
import csv
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fill_markout_by_state as fm  # noqa: E402  (the mid reader, the helper and the archive path, one implementation)

ROOT = fm.ROOT
CUTS = Path("out/cuts")


def state_at(series, stamp, window_ms):
    """The leader's return, in bps, over the `window_ms` that end at `stamp`."""
    now = fm.mid_at(series, stamp)
    before = fm.mid_at(series, stamp - window_ms)
    if now is None or before is None or before <= 0:
        return None
    return (now - before) / before * 1e4


def fills_for_day(day, follower, leader, in_flight_ms, window_ms, records):
    record = Path(records) / f"rec_{day}_orders.csv"
    cut = CUTS / f"okx_{follower}_{day}_1330.jsonl"
    if not record.exists() or not cut.exists():
        return None, "no record or no cut"
    start_ms, end_ms = fm.bl.window_bounds(day, "13:30", "15:30")
    follower_mids = fm.mids_from_jsonl(str(cut), start_ms, end_ms)
    leader_mids = fm.bl.mids(fm.bl.depth_archive(ROOT / "data/v2/crypto", leader, day), start_ms, end_ms)
    rows = []
    with open(record, newline="") as handle:
        for row in csv.DictReader(handle):
            if row["kind"] != "report" or row["exec_type"] != "trade":
                continue
            if row.get("liquidity_role") != "maker":
                continue
            stamp, price = int(row["ts"]), float(row["price"])
            future = fm.mid_at(follower_mids, stamp + 1000)
            now = state_at(leader_mids, stamp, window_ms)
            before = state_at(leader_mids, stamp - in_flight_ms, window_ms)
            if future is None or now is None or before is None or price <= 0:
                continue
            sign = 1.0 if row["side"] == "B" else -1.0
            rows.append((now, before, sign * (future - price) / price * 1e4))
    return rows, ""


def summarise(name, marks):
    if not marks:
        return f"{name}: no fill"
    return (f"{name}: {len(marks):>4} fills, mean markout {statistics.mean(marks):+.2f}, "
            f"median {statistics.median(marks):+.2f} bps")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--days", required=True, help="comma separated days that have a record and a cut")
    parser.add_argument("--leader", default="BTC-USDT-SWAP")
    parser.add_argument("--follower", default="UP-USDT-SWAP")
    parser.add_argument("--in-flight-us", type=int, default=75000,
                        help="the cancel round trip in microseconds (measured 77000 to 118000)")
    parser.add_argument("--window-ms", type=int, default=3000, help="the leader state's own window")
    parser.add_argument("--threshold-bps", type=float, default=2.0, help="what counts as 'the leader is moving'")
    parser.add_argument("--records", default="/tmp/tq/fill_state")
    args = parser.parse_args()

    in_flight_ms = args.in_flight_us / 1000.0
    days = [part.strip() for part in args.days.split(",") if part.strip()]
    flat, avoidable, unavoidable, all_marks = [], [], [], []
    for day in days:
        rows, why = fills_for_day(day, args.follower, args.leader, in_flight_ms, args.window_ms, args.records)
        if rows is None:
            print(f"# {day}: {why}", flush=True)
            continue
        moving = 0
        for now, before, markout in rows:
            all_marks.append(markout)
            if abs(now) < args.threshold_bps:
                flat.append(markout)
                continue
            moving += 1
            (avoidable if abs(before) >= args.threshold_bps else unavoidable).append(markout)
        print(f"# {day}: {len(rows)} fills, {moving} with the leader moving", flush=True)

    print()
    print(summarise("no gate        ", all_marks))
    print(summarise("gate, 0 us     ", flat + unavoidable))
    print(summarise(f"gate, {args.in_flight_us} us", flat + unavoidable + avoidable))
    if avoidable:
        print(f"\nthe latency keeps {len(avoidable)} of the {len(avoidable) + len(unavoidable)} moving fills, "
              f"and the gated set reads {statistics.mean(flat + unavoidable + avoidable):+.2f} bps against "
              f"{statistics.mean(flat + unavoidable):+.2f} as if the cancel were free")
    else:
        print("\nno fill is avoidable inside the latency: every moving fill turned inside it")
    if all_marks and (flat + unavoidable + avoidable):
        print(f"the gate with {args.in_flight_us} us of latency reads "
              f"{statistics.mean(flat + unavoidable + avoidable):+.2f} against the pooled {statistics.mean(all_marks):+.2f} bps")
    return 0


if __name__ == "__main__":
    sys.exit(main())

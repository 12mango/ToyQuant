#!/usr/bin/env python3
"""Join each maker fill to the venue's index and ask whether the basis leans the fills.

Tier 2 #5 registered the question and the bar before the data existed: *the basis's extreme-negative decile must show a
fill asymmetry whose sign is stable across the two months, before it is worth wiring.* This tool produces the decile table
from three things already on disk:

  * the replay's own record, `rec_<day>_orders.csv` (`fill_markout_by_state.py --record-orders`), whose `report`/`trade`
    rows with `liquidity_role=maker` are our fills, with their stamps and prices;
  * the window cut, `okx_<symbol>_<day>_1330.jsonl`, for the follower's mid series (the same reader the state-gate study
    uses, imported rather than re-implemented);
  * the index bars, `data/v2/crypto/index/<INDEX>.1m.<day>.csv`, from `tools/fetch/fetch_okx_index.py`.

The basis is `(mid - index) / index * 1e4` at the fill's own stamp, the index bar being the one whose open time floors the
stamp -- measured 2026-11-28 on 2026-11-26 13:30:00: mid 0.198000 against the index's 0.198500, so -25.19 bps. Fills are
bucketed into deciles of that basis and each bucket reports its fills, its buy share and its mean one-second markout: a
downward-leaning basis that fills us on the bid more often than on the offer is the inventory bias the row asks about.

Usage:
    python3 tools/study/fill_basis_asymmetry.py --index UP-USDT --days 2026-10-30,2026-10-31
    python3 tools/study/fill_basis_asymmetry.py --index UP-USDT --days <a month> --split 2026-11-16
"""
import argparse
import csv
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fill_markout_by_state as fm  # noqa: E402  (the mid reader and its helper, one implementation)

ROOT = fm.ROOT
CUTS = Path("out/cuts")
INDEX_DIR = ROOT / "data/v2/crypto/index"
DECILES = 10


def load_index(day, inst_id):
    path = INDEX_DIR / f"{inst_id}.1m.{day}.csv"
    if not path.exists():
        return None
    with open(path, newline="") as handle:
        return {int(row["ts"]): float(row["close"]) for row in csv.DictReader(handle) if float(row["close"]) > 0}


def basis_at(index_bars, stamp):
    """The index close of the bar whose open time floors the stamp, or None when that minute has no bar."""
    return index_bars.get(stamp - (stamp % 60_000))


def fills_for_day(day, follower, index_bars):
    """(basis bps, markout bps, is_buy) for each maker fill that has a mid, an index bar and a future mid."""
    record = Path("/tmp/tq/fill_state") / f"rec_{day}_orders.csv"
    cut = CUTS / f"okx_{follower}_{day}_1330.jsonl"
    if not record.exists() or not cut.exists():
        return None
    start_ms, end_ms = fm.bl.window_bounds(day, "13:30", "15:30")
    mids = fm.mids_from_jsonl(str(cut), start_ms, end_ms)
    out = []
    with open(record, newline="") as handle:
        for row in csv.DictReader(handle):
            if row["kind"] != "report" or row["exec_type"] != "trade":
                continue
            if row.get("liquidity_role") != "maker":
                continue
            stamp, price, side = int(row["ts"]), float(row["price"]), row["side"]
            mid, future = fm.mid_at(mids, stamp), fm.mid_at(mids, stamp + 1000)
            index = basis_at(index_bars, stamp)
            if mid is None or future is None or index is None or price <= 0:
                continue
            sign = 1.0 if side == "B" else -1.0
            out.append(((mid - index) / index * 1e4, sign * (future - price) / price * 1e4, side == "B"))
    return out


def report(name, rows):
    """The decile table, and the bottom decile's buy lean as the number the bar is read on."""
    if not rows:
        print(f"{name}: no fill could be joined")
        return None
    rows.sort()
    size = max(1, len(rows) // DECILES)
    print(f"\n{name}: {len(rows)} fills, basis {rows[0][0]:+.1f} to {rows[-1][0]:+.1f} bps")
    print(f"{'decile':>7} {'basis from':>11} {'fills':>6} {'buy share':>10} {'mean markout':>13}")
    for index in range(DECILES):
        chunk = rows[index * size:(index + 1) * size] if index < DECILES - 1 else rows[index * size:]
        if not chunk:
            continue
        buys = sum(1 for _, _, buy in chunk if buy)
        marks = [mark for _, mark, _ in chunk]
        print(f"{index + 1:>7} {chunk[0][0]:>+11.1f} {len(chunk):>6} {buys / len(chunk):>9.0%} "
              f"{statistics.mean(marks):>+13.2f}")
    bottom = rows[:size]
    buys = sum(1 for _, _, buy in bottom if buy)
    return buys / len(bottom) - 0.5, statistics.mean([mark for _, mark, _ in bottom])


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--index", required=True, help="the index instrument, e.g. UP-USDT")
    parser.add_argument("--follower", default="UP-USDT-SWAP", help="the swap whose record and cut are read")
    parser.add_argument("--days", required=True, help="comma separated days that have a record and a cut")
    parser.add_argument("--split", help="an optional day: rows before it and from it form two groups, for the sign check")
    args = parser.parse_args()

    days = [part.strip() for part in args.days.split(",") if part.strip()]
    groups = {"all": []} if not args.split else {"before": [], "from": []}
    for day in days:
        index_bars = load_index(day, args.index)
        if index_bars is None:
            print(f"# {day}: no index file for {args.index} -- skipped", flush=True)
            continue
        rows = fills_for_day(day, args.follower, index_bars)
        if not rows:
            print(f"# {day}: no record or no joinable fill -- skipped", flush=True)
            continue
        print(f"# {day}: {len(rows)} fills joined", flush=True)
        if args.split:
            groups["before" if day < args.split else "from"].extend(rows)
        else:
            groups["all"].extend(rows)

    results = {name: report(name, rows) for name, rows in groups.items()}
    if args.split and all(results.values()):
        lean = {"before": results["before"][0], "from": results["from"][0]}
        same = (lean["before"] > 0) == (lean["from"] > 0)
        print(f"\nsign of the bottom decile's buy lean: before {lean['before']:+.1%}, from {lean['from']:+.1%} -- "
              f"{'stable' if same else 'NOT stable'}")
        print("The bar also asks for the markout in that decile: "
              f"before {results['before'][1]:+.2f}, from {results['from'][1]:+.2f} bps.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Split a replay window's P&L into spread capture, adverse drift and fees -- and make them add up.

The ledger quotes one number per window (`net_pnl_usd`) and the reader cannot tell whether it was earned at the price or
lost after it. This tool writes the identity down and checks it against the number the replay itself reports:

    net = spread + drift - fees

  * **spread** -- `sign * (mid_at_fill - price) * qty`, summed: what the fill earned at the instant it happened, against the
    mid of the book it arrived into. A maker that is always filled below the mid earns here.
  * **drift** -- `sign * (mid_at_end - mid_at_fill) * qty`: what that fill then lost or gained as the book moved to the
    window's close. This is the adverse-selection term, and it is the one a state gate is supposed to shrink.
  * **fees** -- the record's own `fee` column, in USD (2 bps maker on the cell this is measured on).

Both sums are taken on the fills the replay marked `liquidity_role=maker`, because the venue's record carries the
counterparty's mirror rows too; the end mid is the window cut's last, so `spread + drift` is the position marked at the
close and the comparison is `net_usd` from the sweep CSV, not `settled_bps` (which prices the residual's settlement
separately).

**It does not reconcile on most days, and the accounting pass says why it is not the scale.** An earlier reading blamed the
instrument's multiplier (10 on UP); the measurement rejects it -- the average-cost pass over the record's maker rows lands
within a cent of the replay's `net_usd` on **3 of 21 days at 1x and 0 of 21 at 10x**, and those three land to the digit
(2026-11-03 -3.5836, 11-13 +0.9822, 11-23 -7.3281). So the replay's basis is the notional's own -- the one the fee is charged
on -- and the multiplier is inert there. The days that miss are the days the replay counts more fills than the record's
maker rows carry (`trade_reports=60` against 58 on 2026-11-25), and reading every trade row rather than the maker rows makes
it worse (0 of 21) because the venue's taker rows are the counterparty's mirrors, not the engine's own fills. What stays
open is the fill set, and until it is answered this tool prints its split and refuses to call it reconciled.

    python3 tools/study/fill_decomposition.py --days 2026-11-05,2026-11-26 --sweep /tmp/tq/db40000/f1_control.csv
"""
import argparse
import csv
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fill_markout_by_state as fm  # noqa: E402  (the mid reader and its helpers, one implementation)

QUANTITY_SCALE = 1e6  # engine units per coin, the same figure every recorded cell uses


def window_end_mid(mids):
    """The last mid the cut holds, which is the price a position is marked at when the window closes."""
    if not mids:
        return None
    for _, mid in reversed(mids):
        return mid
    return None


def decompose(day, follower, records):
    record = Path(records) / f"rec_{day}_orders.csv"
    cut = Path("out/cuts") / f"okx_{follower}_{day}_1330.jsonl"
    if not record.exists() or not cut.exists():
        return None, "no record or no cut"
    start_ms, end_ms = fm.bl.window_bounds(day, "13:30", "15:30")
    mids = fm.mids_from_jsonl(str(cut), start_ms, end_ms)
    end_mid = window_end_mid(mids)
    if end_mid is None:
        return None, "the cut holds no mid"
    spread = drift = fees = 0.0
    fills = 0
    with open(record, newline="") as handle:
        for row in csv.DictReader(handle):
            if row["kind"] != "report" or row["exec_type"] != "trade":
                continue
            if row.get("liquidity_role") != "maker":
                continue
            stamp, price = int(row["ts"]), float(row["price"])
            coins = float(row["qty"]) / QUANTITY_SCALE
            mid = fm.mid_at(mids, stamp)
            if mid is None or price <= 0:
                continue
            sign = 1.0 if row["side"] == "B" else -1.0
            spread += sign * (mid - price) * coins
            drift += sign * (end_mid - mid) * coins
            fees += float(row["fee"] or 0.0)
            fills += 1
    if not fills:
        return None, "no joinable fill"
    return (fills, spread, drift, fees, spread + drift - fees), ""


def average_cost(day, follower, records, roles="maker"):
    """The portfolio's own accounting: average cost, realized on the closing part of each fill, unrealized on the rest.

    `src/accounting/portfolio.cpp` books a close as `units * (exit - entry) * unit_value`, where `unit_value` is
    `price * multiplier` when the instrument's `unit_notional_usd` is zero (`unit_notional_at`), and then subtracts
    `report.fee` -- a figure in plain USD -- from the same number. So the P&L term carries the instrument's multiplier while
    the fee does not, and which of the two the replay used is not something to argue about: both are computed here and the
    replay's own `net_usd` decides.
    """
    record = Path(records) / f"rec_{day}_orders.csv"
    if not record.exists():
        return None, "no record"
    rows = []
    with open(record, newline="") as handle:
        for row in csv.DictReader(handle):
            if row["kind"] != "report" or row["exec_type"] != "trade":
                continue
            if roles != "all" and row.get("liquidity_role") != "maker":
                continue
            rows.append(row)
    if not rows:
        return None, "no maker fill"
    rows.sort(key=lambda r: int(r["ts"]))
    position = 0.0  # coins, signed
    average = 0.0
    realized = fees = 0.0
    for row in rows:
        coins = float(row["qty"]) / QUANTITY_SCALE
        price = float(row["price"])
        sign = 1.0 if row["side"] == "B" else -1.0
        fees += float(row["fee"] or 0.0)
        if position == 0.0 or (position > 0.0) == (sign > 0.0):
            # Opening or extending: the average moves by the traded quantity and nothing is realized.
            average = (average * abs(position) + price * coins) / (abs(position) + coins)
            position += sign * coins
            continue
        closing = min(coins, abs(position))
        realized += closing * (price - average) * (1.0 if position > 0.0 else -1.0)
        position += sign * closing
        if abs(position) < 1e-12:
            position, average = 0.0, 0.0
        remaining = coins - closing
        if remaining > 1e-12:
            position, average = sign * remaining, price
    cut = Path("out/cuts") / f"okx_{follower}_{day}_1330.jsonl"
    end_mid = None
    if cut.exists():
        start_ms, end_ms = fm.bl.window_bounds(day, "13:30", "15:30")
        end_mid = window_end_mid(fm.mids_from_jsonl(str(cut), start_ms, end_ms))
    unrealized = position * (end_mid - average) if (position and end_mid) else 0.0
    return (len(rows), realized, unrealized, fees, position, end_mid), ""


def sweep_net_usd(path):
    """The replay's own net per day, from the sweep CSV the cell wrote."""
    net = {}
    with open(path, newline="") as handle:
        for row in csv.DictReader(handle):
            value = row.get("net_usd", "")
            if value and value != "n/a":
                net[row["day"]] = float(value)
    return net


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--days", required=True, help="comma separated days that have a record and a cut")
    parser.add_argument("--follower", default="UP-USDT-SWAP")
    parser.add_argument("--records", default="/tmp/tq/fill_state")
    parser.add_argument("--sweep", help="the sweep CSV whose net_usd this reconciles against")
    parser.add_argument("--multiplier", type=float, default=10.0,
                        help="the instrument's multiplier from InstrumentSpec, so both bases are checked (10 on UP)")
    parser.add_argument("--roles", default="maker", choices=("maker", "all"),
                        help="which trade rows the accounting pass reads: the maker fills alone, or every row the venue "
                             "wrote (a day where the replay counts more fills than the maker rows has the difference here)")
    args = parser.parse_args()

    days = [part.strip() for part in args.days.split(",") if part.strip()]
    net = sweep_net_usd(args.sweep) if args.sweep else {}
    print(f"{'day':>12} {'fills':>6} {'spread':>9} {'drift':>9} {'fees':>8} {'net_of_the_three':>16} "
          f"{'replay':>9} {'gap':>8}")
    totals = [0.0, 0.0, 0.0, 0.0]
    worst = (0.0, "")
    for day in days:
        result, why = decompose(day, args.follower, args.records)
        if result is None:
            print(f"{day:>12} {why}")
            continue
        fills, spread, drift, fees, implied = result
        replay = net.get(day)
        gap = (implied - replay) if replay is not None else None
        if gap is not None and abs(gap) > abs(worst[0]):
            worst = (gap, day)
        for index, value in enumerate((spread, drift, fees, implied)):
            totals[index] += value
        replay_cell = f"{replay:>+9.4f}" if replay is not None else f"{'n/a':>9}"
        gap_cell = f"{gap:>+8.4f}" if gap is not None else f"{'n/a':>8}"
        print(f"{day:>12} {fills:>6} {spread:>+9.4f} {drift:>+9.4f} {fees:>8.4f} {implied:>+16.4f} "
              f"{replay_cell} {gap_cell}")
    # The accounting view: the same fills through `portfolio.cpp`'s average cost, at 1x and at the instrument's multiplier,
    # so which basis the replay's own `net_usd` sits on is a measurement rather than an argument.
    print(f"\n{'day':>12} {'fills':>6} {'realized':>10} {'unrealized':>11} {'fees':>8} {'total_1x':>10} "
          f"{'total_mx':>10} {'replay':>9} {'gap_1x':>9} {'gap_mx':>9}")
    basis_hits = {1.0: 0, args.multiplier: 0}
    basis_days = 0
    for day in days:
        result, why = average_cost(day, args.follower, args.records, args.roles)
        if result is None:
            print(f"{day:>12} {why}")
            continue
        fills, realized, unrealized, fees, position, end_mid = result
        total_1x = realized + unrealized - fees
        total_mx = (realized + unrealized) * args.multiplier - fees
        replay = net.get(day)
        gap_1x = (total_1x - replay) if replay is not None else None
        gap_mx = (total_mx - replay) if replay is not None else None
        if replay is not None:
            basis_days += 1
            for basis, gap in ((1.0, gap_1x), (args.multiplier, gap_mx)):
                if abs(gap) <= 0.01:
                    basis_hits[basis] += 1
        print(f"{day:>12} {fills:>6} {realized:>+10.4f} {unrealized:>+11.4f} {fees:>8.4f} {total_1x:>+10.4f} "
              f"{total_mx:>+10.4f} "
              f"{(replay if replay is not None else 0.0):>+9.4f} "
              f"{(gap_1x if gap_1x is not None else 0.0):>+9.4f} "
              f"{(gap_mx if gap_mx is not None else 0.0):>+9.4f}")

    print(f"\ntotals: spread {totals[0]:+.4f}, drift {totals[1]:+.4f}, fees {totals[2]:.4f}, "
          f"the three together {totals[3]:+.4f} USD")
    if net:
        print(f"largest gap against the replay's net_usd: {worst[0]:+.4f} USD on {worst[1] or 'no day'}")
        if abs(worst[0]) <= 0.01:
            print("the identity reconciles: spread + drift - fees == net_usd, within a cent on every day")
        else:
            print("the identity does NOT reconcile -- do not quote the split until the gap is explained")
    if net and basis_days:
        at_1x, at_mx = basis_hits[1.0], basis_hits[args.multiplier]
        print(f"\nthe accounting view lands within a cent on {at_1x} of {basis_days} days at 1x and "
              f"{at_mx} of {basis_days} at the instrument's {args.multiplier:g}x")
        if at_mx > at_1x:
            print("so the replay's P&L carries the multiplier while the notional, the fee and the exposure printed beside")
            print("it do not: the absolute dollars are the multiplier's worth of the consistent figure, and every relative")
            print("statement -- medians, tails, rankings -- stands because a uniform scale cannot move it")
        elif at_1x > at_mx:
            print("so the replay's P&L is on the notional's own basis and the multiplier in the spec is inert on this run")
        else:
            print("neither basis accounts for the replay's net, so the gap is not the multiplier alone -- quote no split")
    return 0


if __name__ == "__main__":
    sys.exit(main())

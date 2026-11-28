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

**It does not reconcile, and on 2026-11-28 the gap was traced to the engine rather than to this tool.** The replay's own
summary on 2026-11-25 reads `realized_pnl=4.18189`, `gross_pnl_usd=4.8715`, `maker_notional_usd=473.237` and
`fees_paid=0.0946474`; this tool computes 0.2480 of spread, 1.5885 of drift and the same 0.0945 of fees over the same 58
maker fills, i.e. 1.7420 USD against a reported 4.7768. The P&L terms are valued `units * price * multiplier` with
`multiplier=10.0` for this instrument (`src/common/instrument_spec.h`), while the notional, the fee and the `[EXPOSURE]`
line use `qty/1e6 * price` -- so the reported P&L is ten times the basis of everything printed beside it. The tool is
therefore right to refuse: until the basis is settled one way, a split it prints would be a factor of ten out and would
look like a measurement.

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
    print(f"\ntotals: spread {totals[0]:+.4f}, drift {totals[1]:+.4f}, fees {totals[2]:.4f}, "
          f"the three together {totals[3]:+.4f} USD")
    if net:
        print(f"largest gap against the replay's net_usd: {worst[0]:+.4f} USD on {worst[1] or 'no day'}")
        if abs(worst[0]) <= 0.01:
            print("the identity reconciles: spread + drift - fees == net_usd, within a cent on every day")
        else:
            print("the identity does NOT reconcile -- do not quote the split until the gap is explained")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Do market-regime labels explain the cell's per-window result, and can any of them be known before the window?

The regime idea (A: regular chop, B: wide two-way tug-of-war, C: one-sided shock) is only useful if a label separates the
cell's outcomes, and only buildable if the label can be computed *before* the window it names. This joins each day's
market features -- from the tape alone, never from the PnL -- with the control arm's row for the same day, buckets the
days, and then describes the same buckets by their half hour before 13:30.

Measured 2026-11-28 over 2026-10-30..2026-11-26 (control arm, ten contracts):

    regime                days   closed median   net median   worst net   fills   markout
    A  |move| <= 100         9           +19.6        +24.3        +5.5      45     -0.01
    B  100 < |move| <= 200   6            +9.5         +1.2       -12.6      44     +0.13
    C  |move| > 200         13            +6.4        +13.8      -250.5      50     +0.73

**The label separates the tail and nothing else.** A never lost a day (+5.5 is its worst), C lost 250 bps on its worst --
a 45-fold difference in the worst day -- while the *median* falls as the move grows (+19.6, +9.5, +6.4) because a wide day
carries inventory risk rather than extra capture. So C is not "unprocessable": its median day still earns +6.4 bps.

**And the month has no one-way days at all**: efficiency (net move over path travelled) is at or below 0.5 on all thirteen
C-days, so the bucket is "wide two-way range", not "single-direction shock". The damage is range, not trendiness.

**Nothing before the window predicts the bucket.** The same three buckets, described by 13:00-13:30:

    regime    days   pre |move| bps   pre realized   pre trades
    A            9             108            243        1,698
    B            6              82            308        5,822
    C           13              52            242        2,705

C's pre-window move is the *smallest* of the three and its pre-window realized volatility is the same as A's, so a regime
gate at 13:30 has no feature to stand on -- which is the same finding as the lead-lag measurement from the other side: what
happens in the two hours is not foreshadowed by anything measured before them. That leaves *in-window* state (the leader's
volatility, the fill markout conditioning of `fill_markout_by_state.py`) as the only form of gate this data supports.

**Reproduced on fifty-nine days, and one of the three findings was a nine-day artifact.** The same buckets over
2026-09-28..2026-11-26 (the sixty-day control arm, `--rows out/p6/f1_control.csv`; 2026-10-17 is `n/a` and is skipped,
never counted as a zero):

    regime                days   closed median   net median   worst net   fills   markout
    A  |move| <= 100        20           +19.5        +20.2       -28.6      32     +0.28
    B  100 < |move| <= 200  13           +10.7         +4.8       -17.0      41     -0.13
    C  |move| > 200         26            +8.4        +14.3      -250.5      44     +0.54

The ordering and the size survive (+19.6/+9.5/+6.4 against +19.5/+10.7/+8.4, so within two basis points), efficiency is
still at or below 0.5 on **all twenty-six** C-days, and the half hour before 13:30 still fails to separate the buckets
(C's pre-move 46 bps against A's 98). What does not survive is "A never loses a day": on twenty days its worst is
**-28.6**, so the +5.5 of the smaller sample was the sample. And the worst day of C is the *same* -250.5 in both windows --
one event (2026-11-22) rather than a property of a regime, which is the honest way to read the tail column.

`--rows` names the arm's per-day CSV and `--folder` its tape folder, so the same three buckets on a second month are one
command against that month's rows (`out/p6/...`), which is what makes the finding falsifiable rather than a description.
"""
import argparse
import csv
import math
import statistics
from datetime import datetime, timedelta, timezone

FOLDER = "data/v2/crypto/UP-USDT-SWAP"
ROWS = "out/p2/f1_control.csv"


def features(day, folder):
    path = f"{folder}/okx_trades_{day}_UP-USDT-SWAP.csv"
    start = datetime.strptime(day, "%Y-%m-%d").replace(hour=13, minute=30, tzinfo=timezone.utc)
    lo, hi = int(start.timestamp() * 1000), int((start + timedelta(hours=2)).timestamp() * 1000)
    prices = []
    with open(path, newline="", encoding="utf-8") as handle:
        for row in csv.reader(handle):
            if len(row) < 6:
                continue
            try:
                stamp = int(row[5])
            except ValueError:
                continue
            if lo <= stamp < hi:
                prices.append(float(row[3]))
    if len(prices) < 50:
        return None
    first, last = prices[0], prices[-1]
    steps = [abs(b / a - 1.0) * 1e4 for a, b in zip(prices, prices[1:])]
    travelled = sum(steps)
    move = (last / first - 1.0) * 1e4
    return {"day": day, "move": move, "abs_move": abs(move), "range": (max(prices) / min(prices) - 1.0) * 1e4,
            "travelled": travelled, "efficiency": abs(move) / travelled if travelled else 0.0,
            "realized": math.sqrt(sum(step * step for step in steps)), "trades": len(prices)}


def pre_window(day, folder):
    """What was knowable at 13:30: the half hour before the window, from the same tape."""
    path = f"{folder}/okx_trades_{day}_UP-USDT-SWAP.csv"
    start = datetime.strptime(day, "%Y-%m-%d").replace(hour=13, minute=0, tzinfo=timezone.utc)
    lo, hi = int(start.timestamp() * 1000), int((start + timedelta(minutes=30)).timestamp() * 1000)
    prices = []
    with open(path, newline="", encoding="utf-8") as handle:
        for row in csv.reader(handle):
            if len(row) < 6:
                continue
            try:
                stamp = int(row[5])
            except ValueError:
                continue
            if lo <= stamp < hi:
                prices.append(float(row[3]))
    if len(prices) < 20:
        return None
    steps = [abs(b / a - 1.0) * 1e4 for a, b in zip(prices, prices[1:])]
    return {"pre_move": abs((prices[-1] / prices[0] - 1.0) * 1e4),
            "pre_realized": math.sqrt(sum(step * step for step in steps)),
            "pre_trades": len(prices)}

def number(row, key):
    """A cell that a run could not fill is `n/a`, not a zero -- so it makes the day absent rather than poor."""
    value = row.get(key, "")
    return None if value in ("", "n/a") else float(value)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Bucket the windows by their own physics and join them with one arm's rows.")
    parser.add_argument("--rows", default=ROWS,
                        help="the arm's per-day CSV (default out/p2/f1_control.csv; needs day, closed_bps, net_bps, fills "
                             "and markout_ticks columns, which up_ten_contract_cell.sh writes)")
    parser.add_argument("--folder", default=FOLDER, help=f"the instrument's tape folder (default {FOLDER})")
    args = parser.parse_args(argv)

    rows = {row["day"]: row for row in csv.DictReader(open(args.rows))}
    days = sorted(rows)
    table = [f for f in (features(day, args.folder) for day in days) if f]
    for entry in table:
        row = rows[entry["day"]]
        entry["closed"] = number(row, "closed_bps")
        entry["net"] = number(row, "net_bps")
        entry["fills"] = number(row, "fills") or 0.0
        entry["markout"] = number(row, "markout_ticks")
    absent = [entry["day"] for entry in table if entry["closed"] is None or entry["net"] is None]
    table = [entry for entry in table if entry["closed"] is not None and entry["net"] is not None]
    print(f"{len(table)} day(s) with a tape and a filled row"
          + (f"; {len(absent)} skipped as n/a: {', '.join(absent[:6])}" if absent else ""))
    print(f"{'day':>10} {'|move|':>7} {'range':>6} {'effic':>6} {'trades':>7} {'closed':>8} {'net':>8} {'fills':>6} "
          f"{'markout':>8}")
    for entry in table:
        markout_text = "" if entry["markout"] is None else f"{entry['markout']:+.2f}"
        print(f"{entry['day']:>10} {entry['abs_move']:>7.0f} {entry['range']:>6.0f} {entry['efficiency']:>6.2f} "
              f"{entry['trades']:>7} {entry['closed']:>+8.1f} {entry['net']:>+8.1f} {entry['fills']:>6.0f} "
              f"{markout_text:>8}")

    buckets = [("A  |move| <= 100", lambda e: e["abs_move"] <= 100),
               ("B  100 < |move| <= 200", lambda e: 100 < e["abs_move"] <= 200),
               ("C  |move| > 200", lambda e: e["abs_move"] > 200)]
    print(f"\n{'regime':>22} {'days':>5} {'closed median':>14} {'net median':>11} {'worst net':>10} "
          f"{'fills':>6} {'markout':>8}")
    for name, keep in buckets:
        subset = [entry for entry in table if keep(entry)]
        if not subset:
            print(f"{name:>22} {0:>5}")
            continue
        markouts = [entry["markout"] for entry in subset if entry["markout"] is not None]
        print(f"{name:>22} {len(subset):>5} {statistics.median(e['closed'] for e in subset):>+14.1f} "
              f"{statistics.median(e['net'] for e in subset):>+11.1f} {min(e['net'] for e in subset):>+10.1f} "
              f"{statistics.median(e['fills'] for e in subset):>6.0f} "
              f"{(statistics.median(markouts) if markouts else float('nan')):>+8.2f}")
    # And the split that the labels ignore: does the *efficiency* (trend against chop) move the result at a fixed move?
    print("\nwithin C, by efficiency (0 = chop, 1 = one-way):")
    wide = [entry for entry in table if entry["abs_move"] > 200]
    for label, keep in (("chop  <=0.5", lambda e: e["efficiency"] <= 0.5), ("trend >0.5", lambda e: e["efficiency"] > 0.5)):
        subset = [entry for entry in wide if keep(entry)]
        if subset:
            print(f"  {label:>12} days={len(subset):>2} closed median={statistics.median(e['closed'] for e in subset):+.1f} "
                  f"net median={statistics.median(e['net'] for e in subset):+.1f}")

    # The ex-ante question: at 13:30 the window's own move is not known, so a label computed from it cannot gate anything.
    # The half hour before it, however, is knowable -- does it separate the buckets?
    print("\nthe same buckets, described by the half hour *before* the window (knowable at 13:30):")
    print(f"{'regime':>22} {'days':>5} {'pre |move|':>11} {'pre realized':>13} {'pre trades':>11}")
    for name, keep in buckets:
        subset = []
        for entry in table:
            if not keep(entry):
                continue
            before = pre_window(entry["day"], args.folder)
            if before:
                subset.append(before)
        if not subset:
            print(f"{name:>22} {0:>5}")
            continue
        print(f"{name:>22} {len(subset):>5} {statistics.median(e['pre_move'] for e in subset):>11.0f} "
              f"{statistics.median(e['pre_realized'] for e in subset):>13.0f} "
              f"{statistics.median(e['pre_trades'] for e in subset):>11.0f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

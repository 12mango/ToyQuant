#!/usr/bin/env python3
"""Two seats merged into one account: the joint day series, the dispersion gate on it, and the margin it needs.

    python3 tools/study/portfolio_stress.py --up /tmp/tq/rs_up10.csv --nes /tmp/tq/rs4_nes5.csv

The two seats are measured one at a time, and a real account pays for both out of one margin pool. This tool answers the
three questions that only exist once they are merged:

  * do their bad days land on the same day? -- the per-day table plus the correlation, with the worst day of each seat
    named (at seven days a coefficient is a rank observation, not a statistic, and it is printed as one)
  * does the merged series clear `sweep_summary.py`'s gate? -- the same rule (leave-one-out median above zero, worst day
    within 3x the median), applied to USD because a portfolio of two instruments has no single basis-point denominator
  * how much margin does both seats fully loaded need? -- peak position in coins from `max_inv / quantity_scale`, its
    notional at the day's own print, the gross multiple of equity, the margin at each leverage and the adverse move that
    would liquidate it

The prices are the archive's own last print for the day, not a remembered quote: a notional that is priced from memory
is a notional nobody can re-derive.
"""
import argparse
import csv
import math
import statistics

QUANTITY_SCALE = 1e6  # engine units per coin, the same figure every recorded cell uses


def load(path, price):
    """The seat's rows keyed by day, with the three derived columns: closed USD, mark bps, mark USD, peak position USD."""
    rows = {}
    for row in csv.DictReader(open(path)):
        notional = float(row["notional_usd"])
        closed_bps = float(row["closed_bps"])
        mark_bps = float(row["net_bps"]) - closed_bps
        coins = float(row["max_inv"]) / QUANTITY_SCALE
        rows[row["day"]] = dict(closed_bps=closed_bps, closed_usd=closed_bps * notional / 1e4,
                                mark_bps=mark_bps, mark_usd=mark_bps * notional / 1e4,
                                net_usd=float(row["net_usd"]), notional=notional,
                                peak_coins=coins, peak_usd=coins * price)
    return rows


def gate(values):
    """`sweep_summary.py`'s two conditions on a daily series: leave-one-out median > 0 and worst within 3x."""
    ordered = sorted(values)
    median = statistics.median(ordered)
    rest = list(ordered)
    rest.remove(max(ordered))
    leave_one_out = statistics.median(rest)
    multiple = abs(min(ordered)) / median if median > 0 else float("inf")
    return dict(median=median, leave_one_out=leave_one_out, worst=min(ordered), multiple=multiple,
                positive=sum(1 for value in values if value > 0), passed=leave_one_out > 0 and multiple <= 3.0)


def pearson(a, b):
    ma, mb = statistics.mean(a), statistics.mean(b)
    numerator = sum((x - ma) * (y - mb) for x, y in zip(a, b))
    denominator = math.sqrt(sum((x - ma) ** 2 for x in a) * sum((y - mb) ** 2 for y in b))
    return numerator / denominator if denominator else float("nan")


def spearman(a, b):
    """Rank correlation without scipy: the ranks are the sorted position of each value."""
    ra = {value: index for index, value in enumerate(sorted(a))}
    rb = {value: index for index, value in enumerate(sorted(b))}
    return pearson([ra[v] for v in a], [rb[v] for v in b])


def report(up, nes, equity, leverages, maintenance):
    days = sorted(set(up) & set(nes))
    if len(days) < len(up) or len(days) < len(nes):
        missing = (set(up) ^ set(nes))
        raise SystemExit(f"the two seats do not cover the same days: {sorted(missing)}")
    up_net = [up[d]["net_usd"] for d in days]
    nes_net = [nes[d]["net_usd"] for d in days]
    port_net = [u + n for u, n in zip(up_net, nes_net)]
    port_closed = [up[d]["closed_usd"] + nes[d]["closed_usd"] for d in days]
    port_mark = [up[d]["mark_usd"] + nes[d]["mark_usd"] for d in days]

    lines = ["day         UP net   NES net | port net | UP closed NES closed | port closed | UP mark  NES mark | port mark"]
    for d, u, n, pn, pc, pm in zip(days, up_net, nes_net, port_net, port_closed, port_mark):
        lines.append(f"{d}  {u:>7.3f} {n:>8.3f} | {pn:>8.3f} | {up[d]['closed_usd']:>8.3f} {nes[d]['closed_usd']:>9.3f} | "
                     f"{pc:>10.3f} | {up[d]['mark_usd']:>7.3f} {nes[d]['mark_usd']:>8.3f} | {pm:>8.3f}")
    lines.append("")
    for name, series in (("UP net", up_net), ("NES net", nes_net), ("portfolio net", port_net)):
        g = gate(series)
        lines.append(f"{name:14} sum={sum(series):>7.2f} median={g['median']:>7.3f} loo={g['leave_one_out']:>7.3f} "
                     f"worst={g['worst']:>7.3f} worst/median={g['multiple']:>5.2f}x positive={g['positive']}/{len(days)}")
    for name, series in (("UP closed", [up[d]["closed_usd"] for d in days]),
                         ("NES closed", [nes[d]["closed_usd"] for d in days]), ("port closed", port_closed)):
        g = gate(series)
        lines.append(f"{name:14} sum={sum(series):>7.2f} median={g['median']:>7.3f} loo={g['leave_one_out']:>7.3f} "
                     f"worst={g['worst']:>7.3f} worst/median={g['multiple']:>5.2f}x gate={'PASS' if g['passed'] else 'FAIL'}")
    lines.append(f"\ncorrelation on the daily net: r={pearson(up_net, nes_net):>6.3f} rho={spearman(up_net, nes_net):>6.3f}")
    lines.append(f"worst day: UP {days[up_net.index(min(up_net))]} ({min(up_net):>7.3f}), "
                 f"NES {days[nes_net.index(min(nes_net))]} ({min(nes_net):>7.3f})")
    lines.append(f"portfolio mark: worst={min(port_mark):>7.3f} on {days[port_mark.index(min(port_mark))]}, sum={sum(port_mark):>7.3f}")

    peak_up = max(up[d]["peak_usd"] for d in days)
    peak_nes = max(nes[d]["peak_usd"] for d in days)
    gross = peak_up + peak_nes
    lines.append(f"\npeak position: UP {max(up[d]['peak_coins'] for d in days):.0f} coins = {peak_up:.1f} USD, "
                 f"NES {max(nes[d]['peak_coins'] for d in days):.0f} coins = {peak_nes:.1f} USD, pair {gross:.1f} USD")
    lines.append(f"equity {equity:.0f}: gross leverage {gross / equity:.2f}x")
    for leverage in leverages:
        margin = gross / leverage
        verdict = "  -- more than the account has" if margin > equity else ""
        lines.append(f"  at {leverage}x: margin {margin:>6.1f} USD = {margin / equity:>5.0%} of equity{verdict}")
    for rate in maintenance:
        lines.append(f"  liquidation distance at {rate:.1%} maintenance: "
                     f"{(equity - rate * gross) / gross:>5.1%} adverse move on both books together")
    return "\n".join(lines) + "\n", gate(port_closed), gross


def self_test():
    """The gate's two conditions and the margin arithmetic, on series whose answers are known by hand."""
    assert gate([1.0, 2.0, 3.0, 4.0, -6.0])["multiple"] == 3.0, gate([1.0, 2.0, 3.0, 4.0, -6.0])
    assert gate([1.0, 2.0, 3.0, 4.0, -6.0])["passed"], "worst exactly at the 3x line is inside it"
    refused = gate([1.0, 2.0, 3.0, 4.0, -6.5])
    assert not refused["passed"] and refused["multiple"] > 3.0, refused
    # A series carried by its best day: the leave-one-out median is the condition that catches it.
    carried = gate([-5.0, -4.0, -3.0, -2.0, -1.0, 100.0])
    assert carried["leave_one_out"] < 0 and not carried["passed"], carried
    assert abs(pearson([1, 2, 3], [1, 2, 3]) - 1.0) < 1e-12 and abs(pearson([1, 2, 3], [3, 2, 1]) + 1.0) < 1e-12
    assert abs(spearman([1, 2, 3], [10, 20, 30]) - 1.0) < 1e-12
    print("portfolio_stress self-test OK")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--up", help="the first seat's sweep CSV (UP at ten contracts)")
    parser.add_argument("--nes", help="the second seat's sweep CSV (NES at five contracts)")
    parser.add_argument("--price-up", type=float, default=0.2358, help="UP's price for the peak-position notional")
    parser.add_argument("--price-nes", type=float, default=0.2988, help="NES's price, same day")
    parser.add_argument("--equity", type=float, default=100.0, help="the account's equity in USD (default 100)")
    parser.add_argument("--leverage", default="3,5", help="leverage levels to price the margin at")
    parser.add_argument("--maintenance", default="0.01,0.005", help="maintenance-margin rates to price liquidation at")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.up or not args.nes:
        parser.error("both --up and --nes are required: a portfolio has two seats")
    text, _, _ = report(load(args.up, args.price_up), load(args.nes, args.price_nes), args.equity,
                        [float(value) for value in args.leverage.split(",")],
                        [float(value) for value in args.maintenance.split(",")])
    print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

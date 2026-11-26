#!/usr/bin/env python3
"""Summarise a sweep's CSV: the medians a cell is quoted by, and the spread that decides whether to trust them.

`okx_maker_baseline.py` writes one row a day; every research table in `docs/` quotes a median from a row set like that, and
the median alone is what hid the finding that ended the BLUR size ladder (2026-11-26): three hundred contracts read a
closed median of +60.91 while **one day carried +941 bps on 48 fills**, and a median without its spread made the cell look
like a curve. This tool prints the spread beside the median, so a row that is carried by one day says so.

Usage:
    python3 tools/study/sweep_summary.py --read /tmp/tq/rs2_blur300.csv
    python3 tools/study/sweep_summary.py --read a.csv b.csv
"""

import argparse
import csv
import statistics
import sys


def number(row, field):
    """A column as a float, or None for `n/a` -- which is a measurement the run could not take, not a zero."""
    value = (row.get(field) or "").strip()
    if value in ("", "n/a"):
        return None
    try:
        return float(value)
    except ValueError:
        return None


def summarise(rows, worst_multiple=3.0):
    """One summary per (strategy, model), which is what a cell is: the same run under one queue model."""
    groups = {}
    for row in rows:
        key = (row.get("strategy", "?"), row.get("model", "?"))
        groups.setdefault(key, []).append(row)
    out = []
    for (strategy, model), group in sorted(groups.items()):
        closed = [v for v in (number(r, "closed_bps") for r in group) if v is not None]
        net = [v for v in (number(r, "net_bps") for r in group) if v is not None]
        usd = [v for v in (number(r, "net_usd") for r in group) if v is not None]
        fills = [v for v in (number(r, "fills") for r in group) if v is not None]
        fees = [v for v in (number(r, "fees_bps") for r in group) if v is not None]
        pos = [abs(v) for v in (number(r, "net_pos") for r in group) if v is not None]
        summary = {
            "strategy": strategy,
            "model": model,
            "days": len(group),
            "measured": len(closed),
            # The three numbers a cell is judged by, and the spread that says whether the median is a fact about the
            # run or about one day inside it.
            "closed_median": statistics.median(closed) if closed else None,
            "closed_min": min(closed) if closed else None,
            "closed_max": max(closed) if closed else None,
            "closed_positive": sum(1 for v in closed if v > 0),
            "net_median": statistics.median(net) if net else None,
            "usd_median": statistics.median(usd) if usd else None,
            "fills_median": statistics.median(fills) if fills else None,
            "fee_min": min(fees) if fees else None,
            "fee_max": max(fees) if fees else None,
            "position_median": statistics.median(pos) if pos else None,
        }
        # The dispersion the median hides. `leave_one_out` is the median with the best day removed -- the owner's first
        # condition; `best_share` and `best_over_second` say how much of the cell is one day; `worst_multiple_ratio` is the
        # worst day against the median, the second condition.
        if len(closed) >= 2:
            best = max(closed)
            rest = list(closed)
            rest.remove(best)
            ordered = sorted(closed, reverse=True)
            summary["leave_one_out_median"] = statistics.median(rest)
            summary["best_share"] = best / sum(closed) if sum(closed) != 0 else None
            summary["best_over_second"] = best / ordered[1] if ordered[1] > 0 else None
            median = summary["closed_median"]
            summary["worst_multiple_ratio"] = (abs(min(closed)) / median) if median and median > 0 else None
            problems = []
            if summary["leave_one_out_median"] <= 0:
                problems.append("leave_one_out_median<=0")
            if summary["worst_multiple_ratio"] is not None and summary["worst_multiple_ratio"] > worst_multiple:
                problems.append(f"worst_day>{worst_multiple:g}x_median")
            summary["dispersion_problems"] = problems
        else:
            summary["leave_one_out_median"] = None
            summary["best_share"] = None
            summary["best_over_second"] = None
            summary["worst_multiple_ratio"] = None
            summary["dispersion_problems"] = ["fewer_than_two_measured_days"]
        out.append(summary)
    return out


def format_summary(summary):
    def fmt(value, digits=2):
        return "n/a" if value is None else f"{value:.{digits}f}"

    return (f"{summary['strategy']}/{summary['model']}: days={summary['days']} "
            f"measured={summary['measured']} fills={fmt(summary['fills_median'], 0)} "
            f"closed={fmt(summary['closed_median'])} [{fmt(summary['closed_min'])}, {fmt(summary['closed_max'])}] "
            f"positive={summary['closed_positive']}of{summary['measured']} "
            f"net={fmt(summary['net_median'])} usd={fmt(summary['usd_median'], 4)} "
            f"fee={fmt(summary['fee_min'])}-{fmt(summary['fee_max'])} "
            f"pos={fmt(summary['position_median'], 0)} "
            # The dispersion, on the same line: a reader who quotes the median without these has quoted half the row.
            f"loo={fmt(summary.get('leave_one_out_median'))} "
            f"best_share={fmt(summary.get('best_share'), 3)} "
            f"best_over_second={fmt(summary.get('best_over_second'))} "
            f"worst_x={fmt(summary.get('worst_multiple_ratio'))}")


def gate_verdict(summary):
    problems = summary.get("dispersion_problems") or []
    return "pass" if not problems else "fail(" + ",".join(problems) + ")"


def self_test():
    sample = [
        {"strategy": "active_l2", "model": "prorata", "closed_bps": "10", "net_bps": "5", "net_usd": "0.1",
         "fills": "20", "fees_bps": "2", "net_pos": "-1000"},
        {"strategy": "active_l2", "model": "prorata", "closed_bps": "-4", "net_bps": "1", "net_usd": "-0.05",
         "fills": "40", "fees_bps": "4", "net_pos": "1000"},
        {"strategy": "active_l2", "model": "prorata", "closed_bps": "900", "net_bps": "1", "net_usd": "0",
         "fills": "60", "fees_bps": "6", "net_pos": "0"},
        # A day the run could not measure: `n/a` is not a zero and must not be counted as one.
        {"strategy": "active_l2", "model": "prorata", "closed_bps": "n/a", "net_bps": "n/a", "net_usd": "n/a",
         "fills": "1", "fees_bps": "n/a", "net_pos": ""},
        {"strategy": "execution_l2", "model": "conservative", "closed_bps": "0", "net_bps": "0", "net_usd": "0",
         "fills": "2", "fees_bps": "5", "net_pos": "0"},
    ]
    summaries = {f"{s['strategy']}/{s['model']}": s for s in summarise(sample)}
    maker = summaries["active_l2/prorata"]
    assert maker["days"] == 4 and maker["measured"] == 3, maker
    # The median of ten, minus four and nine hundred is ten -- and the spread is what this tool exists to show.
    assert maker["closed_median"] == 10, maker
    assert (maker["closed_min"], maker["closed_max"]) == (-4, 900), maker
    assert maker["closed_positive"] == 2, maker
    # Four days, so a median is the mean of the middle two: fills 20, 40, 60 and 1 give 30.
    assert maker["fills_median"] == 30, maker
    assert (maker["fee_min"], maker["fee_max"]) == (2, 6), maker
    # The absolute position, because a short tail is as much a residual as a long one: 0, 0, 1000 and 1000 give 1000.
    assert maker["position_median"] == 1000, maker
    # An all-`n/a` group is measured=0 rather than a median of zeros.
    flat = summarise([{"strategy": "x", "model": "y", "closed_bps": "n/a", "net_bps": "n/a", "net_usd": "n/a",
                       "fills": "0", "fees_bps": "n/a", "net_pos": "0"}])[0]
    assert flat["measured"] == 0 and flat["closed_median"] is None, flat
    assert flat["dispersion_problems"] == ["fewer_than_two_measured_days"], flat

    # The gate's first condition: a cell whose sign lives in its best day. Five days are small, one is a hundred, and the
    # median is positive -- but remove that day and the median is not.
    flip = summarise([{"strategy": "s", "model": "m", "closed_bps": str(v), "net_bps": "0", "net_usd": "0",
                       "fills": "1", "fees_bps": "2", "net_pos": "0"} for v in (-5, -4, -3, 100, 2, 3, 4)])[0]
    assert flip["closed_median"] == 2, flip
    assert flip["leave_one_out_median"] < 0, flip
    assert "leave_one_out_median<=0" in flip["dispersion_problems"], flip
    assert gate_verdict(flip).startswith("fail"), flip

    # The gate's second condition: a worst day more than three times the median is a tail, whatever the median says.
    tail = summarise([{"strategy": "s", "model": "m", "closed_bps": str(v), "net_bps": "0", "net_usd": "0",
                       "fills": "1", "fees_bps": "2", "net_pos": "0"} for v in (10, 1, 1, 1, 1, 1, -40)])[0]
    assert tail["worst_multiple_ratio"] == 40, tail
    assert any(p.startswith("worst_day>") for p in tail["dispersion_problems"]), tail

    # And the cell the gate was written for: BLUR at three hundred contracts passes both conditions while **one day is 88
    # per cent of the cell and nine times its second best** -- which is why the dominance is printed rather than gated.
    blur = summarise([{"strategy": "s", "model": "m", "closed_bps": str(v), "net_bps": "0", "net_usd": "0",
                       "fills": "1", "fees_bps": "2", "net_pos": "0"}
                      for v in (32.75, 941.21, 102.00, 89.07, -21.87, -69.57)])[0]
    assert gate_verdict(blur) == "pass", blur
    assert 0.85 < blur["best_share"] < 0.90, blur
    assert 9.0 < blur["best_over_second"] < 9.3, blur
    print("self-test OK")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--read", nargs="+", metavar="CSV", help="sweep CSV files, as okx_maker_baseline.py writes them")
    parser.add_argument("--gate", action="store_true",
                        help="print the dispersion verdict per cell: the median with its best day removed must stay "
                             "positive, and the worst day may not exceed --worst-multiple times the median")
    parser.add_argument("--worst-multiple", type=float, default=3.0,
                        help="how many times the median the worst day may be before the gate refuses the cell (default 3)")
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()
    if not arguments.read:
        parser.error("name --read CSV")
    status = 0
    for path in arguments.read:
        try:
            with open(path, newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
        except OSError as error:
            # A missing file is an error that names it, never a summary of nothing.
            print(f"Error: {error}", file=sys.stderr)
            status = 1
            continue
        if not rows:
            print(f"Error: {path} has no rows", file=sys.stderr)
            status = 1
            continue
        print(path)
        for summary in summarise(rows, worst_multiple=arguments.worst_multiple):
            print("  " + format_summary(summary))
            if arguments.gate:
                print(f"    dispersion gate: {gate_verdict(summary)}")
    return status


if __name__ == "__main__":
    sys.exit(main())

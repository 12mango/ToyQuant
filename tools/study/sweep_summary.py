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


def summarise(rows):
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
        out.append(
            {
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
        )
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
            f"pos={fmt(summary['position_median'], 0)}")


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
    print("self-test OK")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--read", nargs="+", metavar="CSV", help="sweep CSV files, as okx_maker_baseline.py writes them")
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
        for summary in summarise(rows):
            print("  " + format_summary(summary))
    return status


if __name__ == "__main__":
    sys.exit(main())

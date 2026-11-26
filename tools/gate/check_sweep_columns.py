#!/usr/bin/env python3
"""The sweep's CSV is a contract between a C++ run and a Python table, and this checks the two columns that prove it.

Two things have gone wrong in this column set and both were invisible in the table. `fees_bps` divides by the *maker*
notional, so a taker-only execution reads `n/a` there and a reader takes it for a missing measurement. And the
`[EXECUTION_ALGO]` block -- the shortfall a close-out is judged on -- was for one session not parsed at all, so its
columns did not exist; when they did exist the margin was five times the parent because a different defect hid behind
`filled_ratio`.

So the gate runs two one-minute sweeps and this checks: the row names the strategy it was produced by, a maker row carries
no execution shortfall, an execution row carries both a filled ratio and a shortfall, and **the shortfall adds up** --
`is_total_bps` is `is_filled_bps + is_fees_bps`, which is the same reconciliation rule the rest of this project holds its
numbers to.

Usage:
    python3 tools/gate/check_sweep_columns.py maker.csv execution.csv
"""

import csv
import sys


def rows_of(path):
    with open(path, newline="") as handle:
        return list(csv.DictReader(handle))


def main(argv):
    if len(argv) != 3:
        print("usage: check_sweep_columns.py <maker.csv> <execution.csv>")
        return 2
    problems = []
    maker = rows_of(argv[1])
    execution = rows_of(argv[2])
    for name, rows in (("maker", maker), ("execution", execution)):
        if not rows:
            problems.append(f"{name}: no row was written")
    if maker:
        row = maker[0]
        if row.get("strategy") != "active_l2":
            problems.append(f"maker row says strategy={row.get('strategy')!r}")
        if str(row.get("stale")) != "0":
            problems.append(f"maker row says stale={row.get('stale')!r}, so its trades and book do not cover one window")
        for column in ("filled_ratio", "is_filled_bps", "is_fees_bps", "is_total_bps", "maker_share", "child_orders"):
            if row.get(column) != "n/a":
                problems.append(f"maker row carries {column}={row.get(column)!r}, which is an execution figure")
    if execution:
        row = execution[0]
        if row.get("strategy") != "execution_l2":
            problems.append(f"execution row says strategy={row.get('strategy')!r}")
        if row.get("filled_ratio") == "n/a":
            problems.append("execution row has no filled_ratio, so the [EXECUTION_ALGO] block was not parsed")
        if row.get("is_total_bps") == "n/a":
            problems.append("execution row has no is_total_bps: the parent's decomposition is missing")
        else:
            try:
                total = float(row["is_total_bps"])
                parts = float(row["is_filled_bps"]) + float(row["is_fees_bps"])
            except (KeyError, TypeError, ValueError):
                problems.append("execution row's shortfall is not three numbers")
            else:
                if abs(total - parts) > 0.01:
                    problems.append(f"execution shortfall does not add up: total {total:.2f} against "
                                    f"filled+fees {parts:.2f}")
        if row.get("filled_ratio") not in ("n/a", None):
            try:
                if float(row["filled_ratio"]) <= 0.0:
                    problems.append(f"execution row filled {row['filled_ratio']} of its parent, so nothing was measured")
            except ValueError:
                problems.append(f"execution row's filled_ratio is not a number: {row['filled_ratio']!r}")
    for problem in problems:
        print(f"  sweep columns: {problem}")
    print("sweep columns: " + ("ok" if not problems else f"{len(problems)} problem(s)"))
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))

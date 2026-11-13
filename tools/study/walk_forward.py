#!/usr/bin/env python3
"""Expanding walk-forward: choose the parameter on the past, score it on the next window.

The repository's sweeps pick a best cell and then quote it, and the studies page already says why that is not a
freeze: `trend_l2`'s 27-cell grid named 5/30 with a 3-tick entry as its best, and the sentence next to it reads
"that is a statement about this window, not a parameter to freeze". A parameter chosen on the window it is quoted
from has already seen the answer. This tool does the other half: it fits on every window up to the fold, applies
the chosen cell to the fold's window alone, and prints both numbers, so a reader can see whether the choice
travelled or whether it was the window.

What it consumes is a table, not a binary. The sweeps already write one row per run with the parameters and the
metrics as columns (`tools/sweep/trend_sweep.sh` writes `fast,slow,entry_ticks,...,net_pnl_usd,...`), and a tool
that re-ran the grid itself would duplicate that loop and disagree with it the first time a flag was added. So this
reads those CSVs, one per window, in the order they are given -- which is the chronological order, and the reason
`--run` is named rather than positional: a set of windows is not a folder to glob, it is a sequence.

The selection rule is stated rather than implied: the parameter tuple with the highest **mean** of the metric over
the training windows wins, ties go to whichever tuple appears first in the file, and the fold's own row for that
tuple is the out-of-sample number. Nothing is refitted inside the fold, and no window is dropped for being
uncomfortable. `replicated` is yes only when every fold's out-of-sample number is positive; a tool that decided
otherwise would be choosing which folds count.

Usage:
    # two windows: one fold, chosen on April, scored on June
    python3 tools/study/walk_forward.py --run 2020-04-01=/tmp/wf_04.csv \\
        --run 2020-06-01=/tmp/wf_06.csv --param fast,slow,entry_ticks --metric net_pnl_usd

    # three or more windows: one fold per window after the first, each printed on its own line
    python3 tools/study/walk_forward.py --run d1=a.csv --run d2=b.csv --run d3=c.csv \\
        --param quote_ticks --metric net_per_fill_usd

    python3 tools/study/walk_forward.py --self-test

A cell whose metric is missing or unreadable (`?` is what the sweeps print when a run did not report a field) is
left out of the fit and counted as `missing`; it never scores zero, because zero is a result and a missing number
is not. Exit status is 0 when the folds ran, 1 when an input could not be read or a self-test case failed.
"""

import argparse
import contextlib
import csv
import io
import sys
import tempfile
from pathlib import Path

# Written once and printed, because a document that quotes a fold is quoting this rule rather than a mood.
SELECTION_RULE = "highest mean over the training windows, ties to the first tuple in the file"


def read_table(path):
    """Every row of a sweep CSV as a dict of strings; the header is checked where it is used."""
    with open(path, newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            raise ValueError(f"{path} has no header row")
        return [dict(row) for row in reader]


def number(text):
    """The metric as a float, or None when the run printed no usable value."""
    try:
        return float(text)
    except (TypeError, ValueError):
        return None


def tuple_of(row, params):
    return tuple(str(row.get(name, "")).strip() for name in params)


def tuples_in_order(rows, params):
    """The tuples in the order the file lists them, which is what a tie is broken by."""
    seen = []
    for row in rows:
        key = tuple_of(row, params)
        if key not in seen:
            seen.append(key)
    return seen


def fit(rows, params, metric):
    """The training set's scores, its winner, and that winner's mean."""
    scores = {key: [] for key in tuples_in_order(rows, params)}
    for row in rows:
        value = number(row.get(metric))
        if value is None:
            continue
        scores[tuple_of(row, params)].append(value)
    means = {key: sum(values) / len(values) for key, values in scores.items() if values}
    if not means:
        return scores, None, None
    # `max` over a dict keeps the first key on a tie, and the dict was built in file order, so the tie rule is
    # the iteration order rather than a second sort that could disagree with the comment above it.
    winner = max(means, key=means.get)
    return scores, winner, means[winner]


def apply_to(rows, params, metric, chosen):
    """The chosen tuple's own row in a window, or None when that cell is absent or unreadable."""
    for row in rows:
        if tuple_of(row, params) != chosen:
            continue
        return number(row.get(metric))
    return None


def walk_forward(runs, params, metric):
    """One fold per window after the first: fit on everything before it, score it alone."""
    folds = []
    for index in range(1, len(runs)):
        training = [row for _, rows in runs[:index] for row in rows]
        _, chosen, in_sample = fit(training, params, metric)
        out_of_sample = apply_to(runs[index][1], params, metric, chosen) if chosen else None
        folds.append({"fold": index, "window": runs[index][0],
                      "train": "+".join(name for name, _ in runs[:index]),
                      "chosen": chosen, "in_sample": in_sample, "out_of_sample": out_of_sample})
    return folds


def summarise(folds):
    """The counts a document quotes, computed here rather than read off the fold lines."""
    scores = [fold["out_of_sample"] for fold in folds if fold["out_of_sample"] is not None]
    choices = {fold["chosen"] for fold in folds if fold["chosen"] is not None}
    return {
        "folds": len(folds),
        "scored": len(scores),
        "missing": len(folds) - len(scores),
        "oos_positive": sum(1 for value in scores if value > 0),
        "oos_mean": (sum(scores) / len(scores)) if scores else None,
        # The one bit that answers the question this tool exists for: did the choice travel?
        "replicated": "yes" if scores and all(value > 0 for value in scores) else "no",
        "distinct_choices": len(choices),
    }


def show(folds, params, metric):
    for fold in folds:
        chosen = ",".join(fold["chosen"]) if fold["chosen"] else "none"
        in_sample = "missing" if fold["in_sample"] is None else f"{fold['in_sample']:g}"
        out_of_sample = "missing" if fold["out_of_sample"] is None else f"{fold['out_of_sample']:g}"
        print(f"[WALK-FORWARD] fold={fold['fold']} window={fold['window']} train={fold['train']} "
              f"chosen={chosen} in_sample={in_sample} out_of_sample={out_of_sample}")
    summary = summarise(folds)
    chosen = ",".join(folds[-1]["chosen"]) if folds and folds[-1]["chosen"] else "none"
    mean = "missing" if summary["oos_mean"] is None else f"{summary['oos_mean']:g}"
    print(f"[WALK-FORWARD] folds={summary['folds']} scored={summary['scored']} "
          f"missing={summary['missing']} metric={metric} params={','.join(params)} "
          f"last_chosen={chosen} oos_positive={summary['oos_positive']} oos_mean={mean} "
          f"distinct_choices={summary['distinct_choices']} replicated={summary['replicated']}")
    print(f"Selection rule: {SELECTION_RULE}. A fold is out of sample by construction; a choice that changed "
          f"between folds is an unstable fit, which is not the same thing as the windows disagreeing.")
    return summary


def run(specs, params, metric):
    """Read the windows, refuse anything that cannot be a fold, and walk them."""
    if len(specs) < 2:
        raise ValueError(f"a walk-forward needs at least two windows and has {len(specs)}; "
                         f"one window has nothing to be out of sample")
    runs = []
    for name, path in specs:
        rows = read_table(path)
        if not rows:
            raise ValueError(f"{path} holds no data row, so window '{name}' has nothing to fit or score")
        missing = [column for column in list(params) + [metric] if column not in rows[0]]
        if missing:
            raise ValueError(f"{path} has no column(s) {', '.join(missing)}; its header is "
                             f"{', '.join(rows[0])}")
        runs.append((name, rows))
    return show(walk_forward(runs, params, metric), params, metric)


def self_test():
    """Hand-checked folds: a choice that travels, one that does not, a missing cell, and two refusals."""
    with tempfile.TemporaryDirectory() as room:
        folder = Path(room)

        def table(name, rows):
            path = folder / name
            path.write_text("\n".join([",".join(rows[0])] + [",".join(map(str, row)) for row in rows[1:]]) + "\n")
            return path

        def silent(specs, params, metric):
            """The run, with its lines captured: a self-test in the gate should print one line, not a report."""
            buffer = io.StringIO()
            with contextlib.redirect_stdout(buffer):
                summary = run(specs, params, metric)
            return summary, buffer.getvalue()

        # Window 1 likes A and dislikes B; window 2 the other way round. Fitting on window 1 must pick A and then
        # score A's own window-2 number, which is negative: this is the case the tool exists for.
        first = table("w1.csv", [["cell", "net"], [5, 9], [20, -1]])
        second = table("w2.csv", [["cell", "net"], [5, -4], [20, 7]])
        summary, printed = silent([("2020-01", first), ("2020-02", second)], ["cell"], "net")
        assert summary["folds"] == 1 and summary["scored"] == 1, summary
        assert summary["oos_positive"] == 0 and summary["replicated"] == "no", summary
        assert summary["oos_mean"] == -4, summary
        # The out-of-sample number is the chosen cell's, not the best cell's: 5 on window 2 is -4, not 7.
        assert "chosen=5 in_sample=9 out_of_sample=-4" in printed, printed
        assert "replicated=no" in printed and "distinct_choices=1" in printed, printed

        # The same cell wins every training set and is positive in both folds: the choice travelled.
        first = table("t1.csv", [["cell", "net"], [5, 9], [20, 1]])
        second = table("t2.csv", [["cell", "net"], [5, 8], [20, 2]])
        third = table("t3.csv", [["cell", "net"], [5, 7], [20, 3]])
        summary, _ = silent([("2020-01", first), ("2020-02", second), ("2020-03", third)], ["cell"], "net")
        assert summary["folds"] == 2 and summary["replicated"] == "yes", summary
        assert summary["oos_mean"] == 7.5 and summary["distinct_choices"] == 1, summary

        # A missing metric is dropped from the fit and reported, rather than read as a zero.
        first = table("m1.csv", [["cell", "net"], [5, 9], [20, "?"]])
        second = table("m2.csv", [["cell", "net"], [5, "?"], [20, 4]])
        summary, _ = silent([("2020-01", first), ("2020-02", second)], ["cell"], "net")
        assert summary["folds"] == 1 and summary["missing"] == 1 and summary["scored"] == 0, summary
        assert summary["replicated"] == "no", summary

        # A column the file does not have is named, not guessed at.
        try:
            run([("a", first), ("b", second)], ["cell", "quote_ticks"], "net")
        except ValueError as error:
            assert "quote_ticks" in str(error) and "cell" in str(error), str(error)
        else:
            raise AssertionError("a missing parameter column was accepted")

        # One window is not a walk-forward.
        try:
            run([("only", first)], ["cell"], "net")
        except ValueError as error:
            assert "at least two windows" in str(error), str(error)
        else:
            raise AssertionError("a single window was accepted")

    print("self-test OK: the fold chosen on the past, the choice that travelled, the missing cell, "
          "the missing column and the single window")
    return 0


def parse_run(text):
    """`name=path`, because the window's name is what a reader quotes and the path is not."""
    name, separator, path = text.partition("=")
    if not separator or not name or not path:
        raise argparse.ArgumentTypeError(f"'{text}' is not name=path")
    return name, path


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--run", action="append", type=parse_run, default=[], metavar="NAME=CSV",
                        help="one sweep CSV per window, in the order the windows happened")
    parser.add_argument("--param", action="append", default=[], metavar="COLUMN[,COLUMN...]",
                        help="the column(s) that identify a parameter tuple; repeatable")
    parser.add_argument("--metric", default="net_pnl_usd",
                        help="the column scored and maximised; the sweeps write net_pnl_usd")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    if not args.run:
        parser.error("--run NAME=CSV is required: a walk-forward is a sequence of windows")
    params = []
    for group in args.param:
        params.extend(column.strip() for column in group.split(",") if column.strip())
    if not params:
        parser.error("--param is required: without one there is no choice to walk forward")
    try:
        run(args.run, params, args.metric)
    except (OSError, ValueError) as error:
        print(f"walk_forward: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

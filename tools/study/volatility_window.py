#!/usr/bin/env python3
"""A windowed true range, and the two-bucket join that makes it a measurement instead of a knob.

`src/strategy/active_l2_market_maker.h` carries a volatility state and the note that it cannot do its job: the
halt compares an EWMA of |change in mid| per second against a threshold, and the same file records that every
threshold from 1.0 to 50.0 lands in the same place on this feed, because a speed built from tick-to-tick moves
measures the gap distribution rather than the market. The note names what would mean something -- "a tick range
over a fixed time window" -- and leaves it as a known gap. This tool is that range, computed from data already on
disk, so the question can be asked before the engine is changed: *are the quiet minutes the losing ones?*

Two modes, because they are two jobs and joining them would hide which one produced a number:

    --trades FILE        one file in, the per-window range series out (and a `[VOLATILITY]` line)
    --bucket             that series in, a table of per-window results in, the two buckets' means out

The range is the window's own high minus low, in ticks, and the average of those ranges is printed as
`atr_ticks`. That is deliberately not the textbook ATR, which carries the previous close into each window: a
window boundary is a cut this tool chose, and a term that reached across it would make the number depend on the
cut. Two averages are printed, `atr_ticks` over every window and `atr_multi_ticks` over the windows with at least
two prints, because a window with one print has a true range of zero and would otherwise dilute the average by
however many quiet windows the file happens to contain. `singles=` says how many those were, so the difference
between the two columns is visible rather than inferred.

The bucket mode takes no verdict. It prints each bucket's window count and mean and stops there: what counts as
too few windows, and whether a difference is a result, are the document's questions, and a tool that answered them
would be the knob the repository keeps refusing to install. Windows whose value is missing are counted as
`skipped` rather than read as zero.

Usage:
    # the range series of one window, in ticks, cut every sixty seconds
    python3 tools/study/volatility_window.py \\
        --trades data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz \\
        --tick-size 0.5 --window-seconds 60 --series /tmp/vol_04.csv

    # the same for the second window, then the buckets over both
    python3 tools/study/volatility_window.py \\
        --trades data/v2/crypto/deribit_trades_2020-06-01_BTC-PERPETUAL.csv.gz \\
        --tick-size 0.5 --window-seconds 60 --series /tmp/vol_06.csv
    python3 tools/study/volatility_window.py --bucket /tmp/vol_04.csv --values /tmp/per_window.csv \\
        --window-field name --value-field net_per_fill_usd

    python3 tools/study/volatility_window.py --self-test

Exit status is 0 when a series or a pair of buckets was printed, 1 when an input was missing or unreadable or a
self-test case failed.
"""

import argparse
import csv
import gzip
import statistics
import sys
import tempfile
from pathlib import Path

# The two columns every trade file in `data/` carries, and the ones this tool needs. The replay shape is what
# `--record` writes and what every reader reads; the venue archives share its first three columns.
TRADE_PRICE_COLUMN = "price"
TRADE_TIMESTAMP_COLUMN = "timestamp"


def read_rows(path):
    """Every data row of a trade CSV, gzip or not, as dicts; the header is read rather than assumed."""
    opener = gzip.open if path.endswith(".gz") else open
    with opener(path, "rt", newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            raise ValueError(f"{path} has no header row")
        for column in (TRADE_TIMESTAMP_COLUMN, TRADE_PRICE_COLUMN):
            if column not in reader.fieldnames:
                raise ValueError(f"{path} has no '{column}' column; its header is "
                                 f"{', '.join(reader.fieldnames)}")
        return [row for row in reader]


def series(rows, tick_size, window_seconds):
    """One entry a window: its start, its high minus low in ticks, and how many prints it held.

    Timestamps are microseconds, which is what every trade file in `data/` carries and what the replay CSV's
    `timestamp` column is. A file in seconds would produce one enormous window rather than an error, so the span
    is printed and a reader can see it.
    """
    if window_seconds <= 0 or tick_size <= 0:
        raise ValueError("--window-seconds and --tick-size have to be positive")
    width = int(window_seconds * 1000000)
    buckets = {}
    for row in rows:
        try:
            timestamp = int(str(row[TRADE_TIMESTAMP_COLUMN]).strip())
            price = float(str(row[TRADE_PRICE_COLUMN]).strip())
        except (TypeError, ValueError):
            continue
        start = timestamp // width * width
        bucket = buckets.setdefault(start, {"high": price, "low": price, "prints": 0})
        bucket["high"] = max(bucket["high"], price)
        bucket["low"] = min(bucket["low"], price)
        bucket["prints"] += 1
    entries = []
    for start in sorted(buckets):
        bucket = buckets[start]
        entries.append({"window_start": start,
                        "range_ticks": (bucket["high"] - bucket["low"]) / tick_size,
                        "trades": bucket["prints"]})
    return entries


def summarise(entries):
    """The line a document quotes: both averages, the spread around them, and how many windows had one print."""
    ranges = [entry["range_ticks"] for entry in entries]
    multi = [entry["range_ticks"] for entry in entries if entry["trades"] > 1]
    if not ranges:
        raise ValueError("no trade carried a usable timestamp and price, so there is no series")
    ordered = sorted(ranges)
    # The 95th percentile by nearest rank: with a handful of windows an interpolated percentile would be a number
    # no window had, and every other statistic here is a window's own value.
    index = min(len(ordered) - 1, max(0, int(round(0.95 * len(ordered))) - 1))
    return {
        "windows": len(ranges),
        "multi": len(multi),
        "singles": len(ranges) - len(multi),
        "atr_ticks": sum(ranges) / len(ranges),
        "atr_multi_ticks": (sum(multi) / len(multi)) if multi else None,
        "median_ticks": statistics.median(ranges),
        "p95_ticks": ordered[index],
        "max_ticks": ordered[-1],
    }


def print_series(entries, summary, window_seconds):
    span = (entries[-1]["window_start"] - entries[0]["window_start"]) / 1000000.0 + window_seconds
    multi = "missing" if summary["atr_multi_ticks"] is None else f"{summary['atr_multi_ticks']:.6g}"
    print(f"[VOLATILITY] windows={summary['windows']} multi_windows={summary['multi']} "
          f"singles={summary['singles']} window_seconds={window_seconds:g} span_seconds={span:.6g} "
          f"atr_ticks={summary['atr_ticks']:.6g} atr_multi_ticks={multi} "
          f"median_ticks={summary['median_ticks']:.6g} p95_ticks={summary['p95_ticks']:.6g} "
          f"max_ticks={summary['max_ticks']:.6g}")
    print("The range is the window's own high minus low in ticks. A window with one print has a range of zero, "
          "which is why atr_ticks and atr_multi_ticks are both printed.")


def write_series(path, entries):
    with open(path, "w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["window_start", "range_ticks", "trades"])
        for entry in entries:
            writer.writerow([entry["window_start"], f"{entry['range_ticks']:.6g}", entry["trades"]])


def read_csv(path):
    with open(path, newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            raise ValueError(f"{path} has no header row")
        return [dict(row) for row in reader]


def buckets(series_rows, value_rows, window_field, value_field):
    """The windows split at their own median range, with each side's mean of the value column.

    The split is the median rather than a threshold somebody chose, and it is printed, so a reader can see where
    the division fell. No window is dropped for its value being extreme: dropping the tail is how a bucket
    comparison turns into a result.
    """
    for column in ("window_start", "range_ticks"):
        if series_rows and column not in series_rows[0]:
            raise ValueError(f"the series has no '{column}' column; write it with --series from this tool")
    if value_rows and value_field not in value_rows[0]:
        raise ValueError(f"{value_field} is not a column of the values table; its header is "
                         f"{', '.join(value_rows[0])}")
    values = {}
    for row in value_rows:
        try:
            values[str(row[window_field]).strip()] = float(str(row[value_field]).strip())
        except (KeyError, TypeError, ValueError):
            continue
    pairs = []
    skipped = 0
    for row in series_rows:
        key = str(row.get("window_start", "")).strip()
        if key not in values:
            skipped += 1
            continue
        pairs.append((float(row["range_ticks"]), values[key]))
    if not pairs:
        raise ValueError("no window of the series has a value in the values table; check --window-field")
    split = statistics.median([span for span, _ in pairs])
    low = [value for span, value in pairs if span <= split]
    high = [value for span, value in pairs if span > split]
    return {
        "paired": len(pairs),
        "skipped": skipped,
        "split_ticks": split,
        "low_windows": len(low),
        "low_mean": (sum(low) / len(low)) if low else None,
        "high_windows": len(high),
        "high_mean": (sum(high) / len(high)) if high else None,
    }


def print_buckets(summary, value_field):
    low = "missing" if summary["low_mean"] is None else f"{summary['low_mean']:.6g}"
    high = "missing" if summary["high_mean"] is None else f"{summary['high_mean']:.6g}"
    print(f"[VOLATILITY] buckets=2 split_ticks={summary['split_ticks']:.6g} paired={summary['paired']} "
          f"skipped={summary['skipped']} low_windows={summary['low_windows']} low_mean={low} "
          f"high_windows={summary['high_windows']} high_mean={high} field={value_field}")
    print("No verdict is printed: whether a difference between the two means is a result depends on how many "
          "windows each side holds and what unit they are in, which is the document's question.")


def self_test():
    """Hand-checked ranges, the series round trip, the bucket join, and the two refusals."""
    with tempfile.TemporaryDirectory() as room:
        folder = Path(room)
        trades = folder / "trades.csv"
        # Four windows of sixty seconds at a half-point tick: ranges of 2, 2, 0 and 4 ticks. The third window has
        # one print, so its true range is zero and it must show up in `singles` rather than be dropped.
        trades.write_text(
            "exchange,symbol,timestamp,id,side,price,amount\n"
            "deribit,BTC-PERPETUAL,0,1,buy,100.0,1\n"
            "deribit,BTC-PERPETUAL,30000000,2,buy,101.0,1\n"
            "deribit,BTC-PERPETUAL,61000000,3,sell,99.0,1\n"
            "deribit,BTC-PERPETUAL,90000000,4,buy,100.0,1\n"
            "deribit,BTC-PERPETUAL,121000000,5,buy,100.0,1\n"
            "deribit,BTC-PERPETUAL,181000000,6,buy,95.0,1\n"
            "deribit,BTC-PERPETUAL,200000000,7,buy,97.0,1\n")
        entries = series(read_rows(str(trades)), tick_size=0.5, window_seconds=60)
        assert [entry["range_ticks"] for entry in entries] == [2.0, 2.0, 0.0, 4.0], entries
        assert [entry["trades"] for entry in entries] == [2, 2, 1, 2], entries
        summary = summarise(entries)
        assert summary["windows"] == 4 and summary["multi"] == 3 and summary["singles"] == 1, summary
        assert abs(summary["atr_ticks"] - 2.0) < 1e-12, summary
        assert abs(summary["atr_multi_ticks"] - (2.0 + 2.0 + 4.0) / 3) < 1e-12, summary
        assert summary["median_ticks"] == 2.0 and summary["max_ticks"] == 4.0, summary

        # The series written and read back is the series computed: the file is the interface between the two modes.
        written = folder / "series.csv"
        write_series(str(written), entries)
        reloaded = read_csv(str(written))
        assert [float(row["range_ticks"]) for row in reloaded] == [2.0, 2.0, 0.0, 4.0], reloaded

        # The bucket join: the split is the median range of 2 ticks, and a window equal to the median falls in the
        # low bucket, so the two counts are 3 and 1 rather than 2 and 2 -- the tool prints the counts for exactly
        # this reason, and the self-test pins the tie rule rather than a nicer-looking split.
        values = folder / "values.csv"
        values.write_text(
            "window_start,net_per_fill_usd\n"
            "0,-0.5\n"
            "60000000,0.5\n"
            "120000000,-1.0\n"
            "180000000,1.5\n"
            "999,7.0\n")  # a window the series does not have: not paired, and not counted as one
        joined = buckets(reloaded, read_csv(str(values)), "window_start", "net_per_fill_usd")
        assert joined["paired"] == 4 and joined["split_ticks"] == 2.0, joined
        assert (joined["low_windows"], joined["high_windows"]) == (3, 1), joined
        assert abs(joined["low_mean"] - (-0.5 + 0.5 - 1.0) / 3) < 1e-12, joined
        assert joined["high_mean"] == 1.5, joined

        # A values table without the field is named, not silently paired with something else.
        try:
            buckets(reloaded, read_csv(str(values)), "window_start", "net_pnl_usd")
        except ValueError as error:
            assert "net_pnl_usd" in str(error), str(error)
        else:
            raise AssertionError("a missing value column was accepted")

        # A trade file without a price column is refused with the file and the column named.
        empty = folder / "empty.csv"
        empty.write_text("exchange,symbol,timestamp,id,side,amount\n")
        try:
            read_rows(str(empty))
        except ValueError as error:
            assert "price" in str(error) and str(empty) in str(error), str(error)
        else:
            raise AssertionError("a trade file with no price column was accepted")

    print("self-test OK: four hand-checked ranges, the series round trip, the median split, "
          "and the two refusals")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--trades", help="a trade CSV (or .gz) to reduce to a per-window range series")
    parser.add_argument("--tick-size", type=float, default=0.5, help="the instrument's tick, in price units")
    parser.add_argument("--window-seconds", type=float, default=60.0, help="the window the range is taken over")
    parser.add_argument("--series", help="write the per-window series here, for a later --bucket")
    parser.add_argument("--bucket", help="a series written by --series, to split into two buckets")
    parser.add_argument("--values", help="the per-window results to bucket, one row a window")
    parser.add_argument("--window-field", default="window_start",
                        help="the column of --values that names the window")
    parser.add_argument("--value-field", default="net_per_fill_usd",
                        help="the column of --values that is averaged inside each bucket")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    try:
        if args.bucket:
            if not args.values:
                parser.error("--bucket needs --values: a series has no results to bucket on its own")
            summary = buckets(read_csv(args.bucket), read_csv(args.values),
                              args.window_field, args.value_field)
            print_buckets(summary, args.value_field)
            return 0
        if not args.trades:
            parser.error("--trades FILE is required, or --bucket with --values")
        entries = series(read_rows(args.trades), args.tick_size, args.window_seconds)
        summary = summarise(entries)
        if args.series:
            write_series(args.series, entries)
        print_series(entries, summary, args.window_seconds)
        return 0
    except (OSError, ValueError) as error:
        print(f"volatility_window: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

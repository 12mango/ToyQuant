#!/usr/bin/env python3
"""Cut a slice out of a market data file, in either of the two shapes this project needs.

A slice is how every measurement here is bounded: the gate does not replay a whole day when a window will do, and
the documents quote windows rather than days. The two shapes are different contracts, which is why they are two
subcommands rather than one flag-heavy script:

    window     rows whose integer in a chosen 1-based column falls inside a closed range. The column is named by
               position because the files it is used on share no header -- an aggregator dump and a book ticker
               dump number their timestamp columns differently, and the funding window needs the sixth of both.
    snapshot   rows of an L2 snapshot file inside a timestamp range, with a stride and a row cap, keyed on the
               `timestamp` column by name because that file's contract says it is there.

Both stream, both choose gzip by suffix, and both refuse a row they cannot read instead of dropping it: a filter
that silently skips what it cannot parse is how a slice ends up quietly short of the settlement it was cut for.
`window` is what `tools/gate/verify_l2.sh` cuts its funding window with; `snapshot` is the documented way to take a
bounded sample of a snapshot day.

Usage:
    python3 tools/fetch/slice.py window --input data/v2/crypto/BTCUSDT-bookTicker-2024-03-28.csv --column 6 \\
        --from 1711612500000 --to 1711613100000 --output /tmp/btc_bbo_settle.csv
    python3 tools/fetch/slice.py snapshot data/v2/crypto/deribit_book_snapshot_25_2020-04-01_BTC-PERPETUAL.csv \\
        /tmp/l2_window.csv --max-rows 200000
"""

import argparse
import csv
import gzip
from pathlib import Path


def open_text(path):
    if str(path).endswith(".gz"):
        return gzip.open(path, "rt", newline="")
    return open(path, newline="")


def slice_window(parser, args):
    """The rows whose integer in `args.column` (1-based) fall in `[start, end]`, header included."""
    if args.column < 1:
        parser.error("--column is 1-based")
    if args.end < args.start:
        parser.error("--to must not be before --from")

    index = args.column - 1
    rows_read = 0
    rows_written = 0
    first_kept = None
    last_kept = None
    with open_text(args.input) as source:
        reader = csv.reader(source)
        try:
            header = next(reader)
        except StopIteration:
            parser.error(f"{args.input} is empty")
        if len(header) < args.column:
            parser.error(f"{args.input} has {len(header)} columns, so column {args.column} does not exist")

        out = None
        writer = None
        if args.output is not None:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            out = open(args.output, "w", newline="")
            writer = csv.writer(out, lineterminator="\n")
            writer.writerow(header)
        try:
            for row in reader:
                rows_read += 1
                if len(row) < args.column:
                    parser.error(f"{args.input} row {rows_read + 1} is short of column {args.column}")
                try:
                    value = int(row[index])
                except ValueError:
                    parser.error(f"{args.input} row {rows_read + 1} column {args.column} is not an integer: "
                                 f"{row[index]!r}")
                if args.start <= value <= args.end:
                    rows_written += 1
                    first_kept = value if first_kept is None else first_kept
                    last_kept = value
                    if writer is not None:
                        writer.writerow(row)
        finally:
            if out is not None:
                out.close()
    print(f"rows_read={rows_read} rows_written={rows_written}")
    if first_kept is not None:
        print(f"first_kept={first_kept} last_kept={last_kept}")
    if args.output is not None:
        print(f"output={args.output}")
    return 0



def slice_snapshot(input_path, output_path, start_ts, end_ts, max_rows, every):
    """The bounded sample, returning what it saw, matched and wrote plus the range it kept."""
    if input_path.resolve() == output_path.resolve():
        raise ValueError("input and output paths must be different")

    output_path.parent.mkdir(parents=True, exist_ok=True)
    rows_seen = 0
    rows_matched = 0
    rows_written = 0
    first_ts = None
    last_ts = None

    opener = gzip.open if input_path.suffix == ".gz" else Path.open
    with opener(input_path, "rt", newline="", encoding="utf-8-sig", errors="replace") as source:
        reader = csv.DictReader(source)
        if not reader.fieldnames:
            raise ValueError("input CSV is missing a header")
        if "timestamp" not in reader.fieldnames:
            raise ValueError("input CSV is missing the timestamp column")

        with output_path.open("w", newline="", encoding="utf-8") as target:
            writer = csv.DictWriter(target, fieldnames=reader.fieldnames)
            writer.writeheader()

            for row in reader:
                rows_seen += 1
                try:
                    timestamp = int(row["timestamp"])
                except (TypeError, ValueError) as error:
                    raise ValueError(f"invalid timestamp at input row {rows_seen + 1}") from error

                if start_ts is not None and timestamp < start_ts:
                    continue
                if end_ts is not None and timestamp > end_ts:
                    continue

                rows_matched += 1
                if (rows_matched - 1) % every != 0:
                    continue

                writer.writerow(row)
                rows_written += 1
                first_ts = timestamp if first_ts is None else first_ts
                last_ts = timestamp
                if max_rows is not None and rows_written >= max_rows:
                    break

    return rows_seen, rows_matched, rows_written, first_ts, last_ts


def run_snapshot(parser, args):
    if args.start_ts is not None and args.end_ts is not None and args.start_ts > args.end_ts:
        parser.error("--start-ts must not be greater than --end-ts")
    if args.max_rows is not None and args.max_rows <= 0:
        parser.error("--max-rows must be positive")
    if args.every <= 0:
        parser.error("--every must be positive")

    try:
        stats = slice_snapshot(args.input, args.output, args.start_ts, args.end_ts, args.max_rows, args.every)
    except (OSError, ValueError) as error:
        parser.error(str(error))

    rows_seen, rows_matched, rows_written, first_ts, last_ts = stats
    print(f"source={args.input}")
    print(f"output={args.output}")
    print(f"rows_seen={rows_seen} rows_matched={rows_matched} rows_written={rows_written}")
    print(f"timestamp_range={first_ts}..{last_ts}")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    subcommands = parser.add_subparsers(dest="shape", required=True)

    window = subcommands.add_parser("window", help="rows whose integer column falls in a range")
    window.add_argument("--input", type=Path, required=True)
    window.add_argument("--column", type=int, required=True,
                        help="1-based column holding the integer to compare")
    window.add_argument("--from", dest="start", type=int, required=True)
    window.add_argument("--to", dest="end", type=int, required=True)
    window.add_argument("--output", type=Path, help="omit to count the window without writing it")

    snapshot = subcommands.add_parser("snapshot", help="a bounded sample of an L2 snapshot file")
    snapshot.add_argument("input", type=Path)
    snapshot.add_argument("output", type=Path)
    snapshot.add_argument("--start-ts", type=int, help="inclusive timestamp in source units")
    snapshot.add_argument("--end-ts", type=int, help="inclusive timestamp in source units")
    snapshot.add_argument("--max-rows", type=int, help="maximum number of output data rows")
    snapshot.add_argument("--every", type=int, default=1, help="keep every Nth matching row")

    args = parser.parse_args()
    if args.shape == "window":
        return slice_window(window, args)
    return run_snapshot(snapshot, args)


if __name__ == "__main__":
    raise SystemExit(main())

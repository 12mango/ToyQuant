#!/usr/bin/env python3
"""Run the same maker on consecutive slices of a window, and write the two tables a bucket join needs.

`tools/study/volatility_window.py` can compute a range per window and split a series at its median, but until
something produces a *result* per window at the same granularity, its bucket mode has nothing to join. This is
that producer: it cuts one depth file and one trade file into slices of a fixed length, replays the same maker on
each slice, and writes the per-slice result and the per-slice range keyed by the same `window_start`, so the two
files join without either tool knowing about the other's internals.

The cost is the reason it is written this way. A day of Deribit snapshots is about five million rows, and a
thirty-minute slice keeps every one of them (a snapshot row carries fifty levels, so a slice is about 130 MB).
Cutting each slice with a separate pass over the file would be one scan per slice -- twenty minutes a day -- so
this scans the depth file **once**, cuts a slice, replays it, deletes it, and moves on: peak disk is one slice and
a day costs one pass plus one replay a slice. Trades are small enough to group in memory first, which is also
what makes the range per slice free.

Nothing here decides anything. Whether the quiet slices earned less is the bucket tool's table and the
document's question; this tool's job is to make the two columns exist, and to say `recon=bad` rather than quietly
keep a slice whose books did not reconcile.

Usage:
    python3 tools/study/slice_pnl.py \\
        --day 2020-04-01=data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz:\\
data/v2/crypto/deribit_book_snapshot_25_2020-04-01_BTC-PERPETUAL.csv \\
        --day 2020-06-01=data/v2/crypto/deribit_trades_2020-06-01_BTC-PERPETUAL.csv.gz:\\
data/v2/crypto/deribit_book_snapshot_25_2020-06-01_BTC-PERPETUAL.csv.gz \\
        --slice-seconds 1800 --symbol BTC-PERPETUAL --binary out/build/linux-debug/toy_quant \\
        --values /tmp/slices.csv --series /tmp/vol_30m.csv

    # then the question itself
    python3 tools/study/volatility_window.py --bucket /tmp/vol_30m.csv --values /tmp/slices.csv

    python3 tools/study/slice_pnl.py --self-test

A slice with no trade is not run and is counted as `empty`, because a replay with no market message is not a
result. A slice that runs but does not reconcile is kept and marked, because dropping it would be the tool
choosing which windows count. Exit status is 0 when every slice ran, 1 when an input was missing, a replay
failed hard, or a self-test case failed.
"""

import argparse
import csv
import gzip
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# The sibling tool owns what a window's range is; this file must not grow a second definition of it.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from volatility_window import series as window_series  # noqa: E402
from volatility_window import write_series  # noqa: E402

# The same argument tuple `tools/study/pnl_interval.py` replays its windows with, plus `--paper-broker`: the broker
# is what prints a `[RECON]` line, and a slice whose books do not reconcile is not a result this tool should hide.
# The gate's own brokered-versus-direct check is what says the broker does not change the run.
REPLAY = ("0", "active_l2", "1", "prorata", "--fast-validation",
          "--base-spread-ticks=1", "--refresh-price-ticks=4", "--maker-fee-rate=0.0002", "--paper-broker")

FIELD = re.compile(r"\b([a-z_]+)=([^\s]+)")


def open_text(path):
    return gzip.open(path, "rt", newline="", encoding="utf-8") if str(path).endswith(".gz") \
        else open(path, "rt", newline="", encoding="utf-8")


def parse_day(text):
    """`name=TRADES:DEPTH`, because a day is two files and the name is what the table quotes."""
    name, separator, paths = text.partition("=")
    trades, colon, depth = paths.partition(":")
    if not separator or not name or not colon or not trades or not depth:
        raise argparse.ArgumentTypeError(f"'{text}' is not name=TRADES:DEPTH")
    return name, trades, depth


def load_trades(path, width):
    """{slice start: [rows]} for one trade file, and the rows in the order they appeared.

    Timestamps are microseconds, grouped by `timestamp // width * width`, which is the same key
    `volatility_window.py` computes, so the two files join on the clock and not on a label.
    """
    rows = []
    with open_text(path) as source:
        reader = csv.DictReader(source)
        if reader.fieldnames is None or "timestamp" not in reader.fieldnames:
            raise ValueError(f"{path} has no header with a timestamp column")
        for row in reader:
            try:
                row["_start"] = int(str(row["timestamp"]).strip()) // width * width
            except (KeyError, TypeError, ValueError):
                continue
            rows.append(row)
    return rows


def write_trades(path, rows):
    """One slice's trades, header included, with the grouping key kept out of the file."""
    names = [name for name in rows[0] if name != "_start"]
    with open(path, "w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=names, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def run_slice(binary, trades_path, depth_path, symbol):
    """One replay: the numbers are the run's own `[PNL]` line, never recomputed here."""
    command = [str(binary), "l2_replay", str(trades_path), str(depth_path), symbol] + list(REPLAY)
    done = subprocess.run(command, capture_output=True, text=True)
    if done.returncode != 0:
        tail = (done.stderr.strip() or done.stdout.strip()).splitlines()[-1:] or [""]
        raise RuntimeError(f"the replay of {trades_path} exited {done.returncode}: {tail[0]}")
    fields = dict(FIELD.findall(done.stdout))
    recon = "ok" if re.search(r"\[RECON\][^\n]*status=ok", done.stdout) else "bad"
    return {"fills": fields.get("trade_reports", ""), "net_pnl_usd": fields.get("net_pnl_usd", ""),
            "fees_paid": fields.get("fees_paid", ""),
            "net_per_fill_usd": fields.get("net_per_fill_usd", ""), "recon": recon}


def slice_day(day, width, work, binary, symbol, tick_size, keep, verbose):
    """One pass over the depth file; a slice is cut, replayed and deleted as the clock leaves it."""
    name, trades_path, depth_path = day
    groups = {}
    for row in load_trades(trades_path, width):
        groups.setdefault(row["_start"], []).append(row)
    if not groups:
        raise ValueError(f"{trades_path} holds no trade with a usable timestamp, so day '{name}' has no slice")

    results = []
    entries = []
    seen = []
    empty = 0

    def finish(start, depth_rows):
        nonlocal empty
        rows = groups.get(start)
        depth_file = work / f"depth_{start}.csv"
        if not rows:
            # A replay with no market message is not a result, so the slice is counted rather than run.
            empty += 1
            if not keep:
                depth_file.unlink(missing_ok=True)
            return
        trades_file = work / f"trades_{start}.csv"
        write_trades(trades_file, rows)
        try:
            outcome = run_slice(binary, trades_file, depth_file, symbol)
        finally:
            if not keep:
                depth_file.unlink(missing_ok=True)
                trades_file.unlink(missing_ok=True)
        results.append({"window_start": start, "trades_rows": len(rows), "depth_rows": depth_rows,
                        **outcome})
        # The range of the same slice, from the same rows, through the tool that owns that definition. That tool
        # takes the window in seconds and this driver carries microseconds, so the unit is converted here rather
        # than in two places; a key that did not match is why the two files would not join.
        entries.extend(window_series(rows, tick_size, width / 1000000.0))
        if verbose:
            print(f"[SLICE] window={start} trades={len(rows)} depth={depth_rows} "
                  f"fills={outcome['fills']} net={outcome['net_pnl_usd']} recon={outcome['recon']}",
                  flush=True)

    with open_text(depth_path) as source:
        reader = csv.reader(source)
        try:
            header = next(reader)
        except StopIteration as error:
            raise ValueError(f"{depth_path} is empty, so day '{name}' has no book") from error
        if "timestamp" not in header:
            raise ValueError(f"{depth_path} has no timestamp column; its header is {', '.join(header)}")
        index = header.index("timestamp")
        current = None
        depth_file = None
        writer = None
        depth_rows = 0
        for row in reader:
            try:
                stamp = int(row[index])
            except (IndexError, ValueError):
                continue
            start = stamp // width * width
            if start != current:
                # The previous slice is closed and finished from `current`, not from the writer: a slice with no
                # trade has no writer, and reading "is there a file open" as "is there a slice" lost it.
                if current is not None:
                    if writer is not None:
                        depth_file.close()
                    finish(current, depth_rows)
                current, depth_rows = start, 0
                seen.append(start)
                # Opened lazily: a slice with no trade is never written, so an empty half hour costs a count
                # rather than a hundred and thirty megabytes of disk and a delete.
                writer = None
            if start in groups and writer is None:
                depth_file = open(work / f"depth_{start}.csv", "w", newline="", encoding="utf-8")
                writer = csv.writer(depth_file, lineterminator="\n")
                writer.writerow(header)
            if writer is not None:
                writer.writerow(row)
            depth_rows += 1
        if current is not None:
            if writer is not None:
                depth_file.close()
            finish(current, depth_rows)
    # A slice with trades but no book row is not an empty slice and not a result either: it is a hole in the
    # depth file, and it is counted so that the sum of the slices cannot quietly be smaller than the day.
    no_depth = [start for start in groups if start not in seen]
    return results, entries, empty, no_depth


def run(days, width, work, binary, symbol, tick_size, keep, verbose):
    """Every day in the order given, with the rows concatenated so the two CSVs cover all of them."""
    results, entries, empty, no_depth = [], [], 0, []
    for day in days:
        day_results, day_entries, day_empty, day_no_depth = slice_day(day, width, work, binary, symbol,
                                                                     tick_size, keep, verbose)
        results.extend(day_results)
        entries.extend(day_entries)
        empty += day_empty
        no_depth.extend(day_no_depth)
    return results, entries, empty, no_depth


def write_values(path, results):
    with open(path, "w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["window_start", "trades_rows", "depth_rows", "fills",
                         "net_pnl_usd", "fees_paid", "net_per_fill_usd", "recon"])
        for row in sorted(results, key=lambda entry: entry["window_start"]):
            writer.writerow([row["window_start"], row["trades_rows"], row["depth_rows"], row["fills"],
                             row["net_pnl_usd"], row["fees_paid"], row["net_per_fill_usd"], row["recon"]])


def whole(results, entries, empty, no_depth):
    """The counts a run prints, computed from the rows rather than from a running total."""
    def numberish(text):
        try:
            return float(text)
        except (TypeError, ValueError):
            return None

    fills = sum(int(row["fills"]) for row in results if str(row["fills"]).isdigit())
    net = sum(value for value in (numberish(row["net_pnl_usd"]) for row in results) if value is not None)
    return {"slices": len(results) + empty + len(no_depth), "ran": len(results), "empty": empty,
            "no_depth": len(no_depth),
            "recon_bad": sum(1 for row in results if row["recon"] != "ok"),
            "fills": fills, "net_usd": net, "series_windows": len(entries)}


def show(summary, values_path, series_path):
    print(f"[SLICES] slices={summary['slices']} ran={summary['ran']} empty={summary['empty']} "
          f"no_depth={summary['no_depth']} recon_bad={summary['recon_bad']} fills={summary['fills']} "
          f"net_usd={summary['net_usd']:.6g} series_windows={summary['series_windows']} "
          f"values={values_path} series={series_path}")
    print("A slice is cut on the same clock the range series uses, so the two files join on window_start. "
          "recon=bad is kept and counted, because dropping it would be the tool choosing which windows count.")


def self_test():
    """A fake binary and three slices: the run's fields, the range of the same slice, and the empty slice."""
    with tempfile.TemporaryDirectory() as room:
        folder = Path(room)
        work = folder / "work"
        work.mkdir()

        fake = folder / "fake_toy_quant"
        fake.write_text("#!/bin/sh\n"
                        "echo '[PNL] trade_reports=4 net_pnl_usd=-2 fees_paid=1.2 net_per_fill_usd=-0.5'\n"
                        "echo '[RECON] status=ok reports=4'\n"
                        "exit 0\n")
        fake.chmod(0o755)

        trades = folder / "trades.csv"
        trades.write_text("exchange,symbol,timestamp,id,side,price,amount\n"
                          "deribit,BTC-PERPETUAL,0,1,buy,100.0,1\n"
                          "deribit,BTC-PERPETUAL,30000000,2,buy,101.0,1\n"
                          "deribit,BTC-PERPETUAL,60000000,3,sell,99.0,1\n"
                          "deribit,BTC-PERPETUAL,180000000,4,buy,98.0,1\n")
        depth = folder / "depth.csv"
        depth.write_text("exchange,symbol,timestamp,asks[0].price,asks[0].amount\n"
                         "deribit,BTC-PERPETUAL,0,100.5,10\n"
                         "deribit,BTC-PERPETUAL,30000000,100.5,10\n"
                         "deribit,BTC-PERPETUAL,60000000,99.5,10\n"
                         "deribit,BTC-PERPETUAL,120000000,99.5,10\n")

        results, entries, empty, no_depth = slice_day(("day", str(trades), str(depth)), 60000000, work,
                                                      str(fake), "BTC-PERPETUAL", 0.5, False, False)
        # Two slices have trades; one holds only book rows and is counted rather than replayed; and the trade at
        # 180 s has no book row at all, which is a hole in the depth file and is counted as one.
        assert len(results) == 2 and empty == 1, (results, empty)
        assert no_depth == [180000000], no_depth
        assert [row["window_start"] for row in results] == [0, 60000000], results
        for row in results:
            assert (row["fills"], row["net_pnl_usd"], row["fees_paid"], row["net_per_fill_usd"],
                    row["recon"]) == ("4", "-2", "1.2", "-0.5", "ok"), row
        # The range comes from the same rows, through the sibling tool: 100 to 101 is two half-point ticks, and the
        # key is the slice's own start -- a range with the wrong key would not join to its result, which is the
        # failure this assertion exists for.
        assert [entry["range_ticks"] for entry in entries] == [2.0, 0.0], entries
        assert [entry["window_start"] for entry in entries] == [0, 60000000], entries
        # The per-slice ranges equal one pass over the whole file, for the slices that ran: the trade with no book
        # row is in the file and is not a slice, which is the distinction the `no_depth` count keeps.
        whole_series = window_series(load_trades(str(trades), 60000000), 0.5, 60.0)
        assert [entry for entry in whole_series if entry["window_start"] in {0, 60000000}] == entries, \
            whole_series
        # Nothing is left behind when the run is allowed to clean up.
        assert not list(work.iterdir()), list(work.iterdir())
        # With the slices kept, the empty half hour has no depth file at all: the lazy writer is the saving, and
        # this is the assertion that would fail if it went back to writing a slice it never runs.
        kept = folder / "kept"
        kept.mkdir()
        slice_day(("day", str(trades), str(depth)), 60000000, kept, str(fake), "BTC-PERPETUAL", 0.5,
                  True, False)
        names = sorted(path.name for path in kept.iterdir())
        assert names == ["depth_0.csv", "depth_60000000.csv", "trades_0.csv", "trades_60000000.csv"], names

        values = folder / "values.csv"
        series = folder / "series.csv"
        write_values(str(values), results)
        write_series(str(series), entries)
        assert values.read_text().splitlines()[0] == ("window_start,trades_rows,depth_rows,fills,"
                                                      "net_pnl_usd,fees_paid,net_per_fill_usd,recon"), \
            values.read_text()
        assert series.read_text().splitlines()[0] == "window_start,range_ticks,trades", series.read_text()

        # A replay that exits non-zero is an error naming the run, not a row of zeros.
        broken = folder / "broken_toy_quant"
        broken.write_text("#!/bin/sh\necho 'the book is empty' >&2\nexit 3\n")
        broken.chmod(0o755)
        try:
            run_slice(str(broken), str(trades), str(depth), "BTC-PERPETUAL")
        except RuntimeError as error:
            assert "exited 3" in str(error) and "empty" in str(error), str(error)
        else:
            raise AssertionError("a replay that exited 3 was accepted")

        # A depth file that is not there is named rather than treated as an empty book.
        try:
            slice_day(("day", str(trades), str(folder / "missing.csv")), 60000000, work, str(fake),
                      "BTC-PERPETUAL", 0.5, False, False)
        except OSError as error:
            assert "missing.csv" in str(error), str(error)
        else:
            raise AssertionError("a missing depth file was accepted")

    print("self-test OK: two slices replayed and one counted empty, the range from the same rows, "
          "the two CSV headers, and the two refusals")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--day", action="append", type=parse_day, default=[],
                        metavar="NAME=TRADES:DEPTH",
                        help="one window's files; repeatable, and the order is the order of the days")
    parser.add_argument("--slice-seconds", type=float, default=1800.0)
    parser.add_argument("--symbol", default="BTC-PERPETUAL")
    parser.add_argument("--tick-size", type=float, default=0.5)
    parser.add_argument("--binary", default="out/build/linux-debug/toy_quant")
    parser.add_argument("--values", default="/tmp/slices.csv",
                        help="the per-slice results, for volatility_window.py --values")
    parser.add_argument("--series", default="/tmp/vol_slices.csv",
                        help="the per-slice range series, for volatility_window.py --bucket")
    parser.add_argument("--work", help="where the cut slices live; a temporary directory by default")
    parser.add_argument("--keep", action="store_true", help="keep the cut slices instead of deleting them")
    parser.add_argument("--verbose", action="store_true", help="print one line per slice")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if not args.day:
        parser.error("--day NAME=TRADES:DEPTH is required")
    width = int(args.slice_seconds * 1000000)
    if width <= 0:
        parser.error("--slice-seconds has to be positive")
    work = Path(args.work) if args.work else Path(tempfile.mkdtemp(prefix="toyquant_slices_"))
    work.mkdir(parents=True, exist_ok=True)
    try:
        results, entries, empty, no_depth = run(args.day, width, work, args.binary, args.symbol,
                                               args.tick_size, args.keep, args.verbose)
        if not results:
            raise ValueError("no slice had a trade, so there is nothing to write")
        write_values(args.values, results)
        write_series(args.series, entries)
        show(whole(results, entries, empty, no_depth), args.values, args.series)
        return 0
    except (OSError, ValueError, RuntimeError) as error:
        print(f"slice_pnl: {error}", file=sys.stderr)
        return 1
    finally:
        if args.work is None and not args.keep:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

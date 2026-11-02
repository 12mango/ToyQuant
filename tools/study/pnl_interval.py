#!/usr/bin/env python3
"""What a mean is worth: the interval it carries, printed beside it.

Almost every claim in `docs/PERFORMANCE_HISTORY.md` that is not arithmetic is a mean over a handful of events,
and the rule that keeps those claims honest -- a fill count below thirty is not a conclusion -- has been applied
by hand, one row at a time, by whoever was writing the sentence. A rule applied by hand is applied by mood, and
the document says so: the maker break-even gap between the two Deribit perpetuals is quoted as -5.65 against
+1.26 bps from one window each, while the same measurement's standard error was 2.7 bps when it was recomputed
across four days. This tool puts the interval next to the mean instead, and says when the sample is too thin for
the interval to mean anything either.

Why a bootstrap. The series here are not normal: per-fill PnL of a market maker is a small captured edge with an
occasional adverse selection in the tail, and a window's break-even rate is a property of the window's volatility
as much as of the strategy. A t-interval assumes a shape these numbers do not have; resampling assumes only that
the sample is the population it came from, which is the same assumption an interval makes anyway and the weaker
of the two. It is also twenty lines of code, so nothing here is trusted to a library's default.

Why the interval is not a verdict. Six windows of one instrument under one configuration do not differ by noise:
they differ by *which month it was*, and the interval around their mean comes out narrower than the spread
between them, which is exactly the failure a mean invites. The tool therefore prints the median beside the mean,
the spread beside both, and a `sign_consistent` flag, and a document is expected to read the spread and not only
the interval. An interval answers "how sure is this mean"; it cannot answer "is this the same population as that
one", and the second question is the one most of these claims are actually asking.

Usage:
    python3 tools/study/pnl_interval.py --values 1.2 -3.4 0.5
    python3 tools/study/pnl_interval.py --field breakeven_maker_rate run.log --scale 1e4 --unit bps
    python3 tools/study/pnl_interval.py --column gross_closed_per_fill_usd a.csv b.csv
    python3 tools/study/pnl_interval.py --window btc --binary out/build/linux-debug/toy_quant
    python3 tools/study/pnl_interval.py --window btc-eth --binary out/build/linux-debug/toy_quant
    python3 tools/study/pnl_interval.py --list
    python3 tools/study/pnl_interval.py --self-test

The output is one `[INTERVAL]` line of `name=value` fields and a sentence after it, so a document can quote the
line in a marked block that `tools/gate/check_docs.py` re-derives. `--window` replays the windows itself rather than
reading a log, because a tool that needs a run to have been saved first is a tool nobody re-runs; the binary is
named the way the gate names it and the depth slices are cut under the names `tools/gate/verify_l2.sh` uses, so the
two do not keep two copies of the same seven hundred megabytes in /tmp.

Exit status is 0 when a series was summarised, 1 when it could not be, and 1 from the self-test when a
hand-checked case fails.
"""

import argparse
import gzip
import math
import random
import re
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

# The rule this tool enforces, written once. It is the document's own rule -- "a fill count below thirty is not a
# conclusion" -- and it is a flag on the line rather than a refusal, because a thin sample is still evidence and a
# tool that refused to print one would be argued around instead of read.
THIN_SAMPLE = 30

# The configuration the windows are replayed under, and the reason there is one: two rows measured under two
# configurations are not two draws from one distribution, so a series that mixed them would be measuring the
# configuration. `active_l2` with a one-tick quote refreshed every four and the venue's own maker fee is the
# configuration `tools/sweep/fee_sweep.sh` and the fee table in `docs/PERFORMANCE_HISTORY.md` already use, which is why
# the first three windows below reproduce the numbers recorded there to the last decimal. The subcommand and the
# two file arguments are added per window, so this tuple starts where they end.
REPLAY = ("0", "active_l2", "1", "prorata", "--fast-validation",
          "--base-spread-ticks=1", "--refresh-price-ticks=4", "--maker-fee-rate=0.0002")



class Window:
    """One replay that produces one break-even rate.

    `rows` is part of the identity rather than a footnote, because it changes the answer: the same day's same
    file at 200k and at 2M rows gives two different rates, and a series that mixed the lengths would be mixing a
    short window with a long one.
    """

    def __init__(self, symbol, day, kind, rows, source):
        self.symbol = symbol
        self.day = day
        self.kind = kind
        self.rows = rows
        self.source = source

    @property
    def key(self):
        """What two windows have to share to be compared as a pair: the day, the file type and the length."""
        return (self.day, self.kind, self.rows)

    @property
    def name(self):
        return f"{self.symbol.split('-')[0]}/{self.day}/{self.kind}/{self.rows}"

    def slice_path(self):
        """The cached slice, under the name `tools/gate/verify_l2.sh` already cuts it as.

        Sharing the names is not tidiness: the gate cuts the same slices for its own blocks, so a second set of
        names would mean two copies of several hundred megabytes in /tmp and two chances to cut one of them wrong.
        """
        incremental = self.symbol == "BTC-PERPETUAL"
        if self.kind == "incremental" and self.rows == 200001:
            return Path("/tmp/depth_200k_base.csv" if incremental else "/tmp/eth_depth_200k.csv")
        if self.kind == "incremental" and self.rows == 2000001:
            return Path("/tmp/depth_2m.csv" if incremental else "/tmp/eth_depth_2m.csv")
        if self.kind == "snapshot" and self.rows == 200001:
            if self.day == "2020-05-01":
                # The gate names this one after the day, so it keeps that name here rather than a second copy.
                return Path("/tmp/snap_0501_200k.csv")
            return Path(f"/tmp/snap_{self.day.replace('-', '')}_200k.csv")
        raise SystemExit(f"no cached slice name for {self.name}; teach Window.slice_path about it")

    def ensure_slice(self):
        """Cut the slice if it is not there, the way the gate cuts it, and say which file it came from."""
        path = self.slice_path()
        if path.exists():
            return path
        source = Path(self.source)
        if not source.is_absolute():
            source = ROOT / source
        if not source.exists():
            raise SystemExit(f"{source} is not on disk, so {self.name} cannot be replayed")
        opener = gzip.open if source.suffix == ".gz" else open
        with opener(source, "rt", encoding="utf-8") as handle, path.open("w", encoding="utf-8") as out:
            for count, line in enumerate(handle):
                if count >= self.rows:
                    break
                out.write(line)
        return path


# Every window the free sample days allow, under one configuration. The BTC list is the whole of that instrument's
# archive here -- 2020-04-01 as incremental depth and as snapshots, the first of each following month as snapshots,
# and the 2M-row slice of the first day -- and the ETH list is everything the archive holds for the second
# instrument. A window that does not exist is not invented: `--list` prints what does.
WINDOWS = (
    Window("BTC-PERPETUAL", "2020-04-01", "incremental", 200001,
           "data/v2/crypto/deribit_incremental_book_L2_2020-04-01_BTC-PERPETUAL.csv"),
    Window("BTC-PERPETUAL", "2020-04-01", "incremental", 2000001,
           "data/v2/crypto/deribit_incremental_book_L2_2020-04-01_BTC-PERPETUAL.csv"),
    Window("BTC-PERPETUAL", "2020-04-01", "snapshot", 200001,
           "data/v2/crypto/deribit_book_snapshot_25_2020-04-01_BTC-PERPETUAL.csv"),
    Window("BTC-PERPETUAL", "2020-05-01", "snapshot", 200001,
           "data/v2/crypto/deribit_book_snapshot_25_2020-05-01_BTC-PERPETUAL.csv.gz"),
    Window("BTC-PERPETUAL", "2020-06-01", "snapshot", 200001,
           "data/v2/crypto/deribit_book_snapshot_25_2020-06-01_BTC-PERPETUAL.csv.gz"),
    Window("BTC-PERPETUAL", "2020-07-01", "snapshot", 200001,
           "data/v2/crypto/deribit_book_snapshot_25_2020-07-01_BTC-PERPETUAL.csv.gz"),
    Window("BTC-PERPETUAL", "2020-08-01", "snapshot", 200001,
           "data/v2/crypto/deribit_book_snapshot_25_2020-08-01_BTC-PERPETUAL.csv.gz"),
    Window("ETH-PERPETUAL", "2020-04-01", "incremental", 200001,
           "data/v2/crypto/deribit_incremental_book_L2_2020-04-01_ETH-PERPETUAL.csv.gz"),
    Window("ETH-PERPETUAL", "2020-04-01", "incremental", 2000001,
           "data/v2/crypto/deribit_incremental_book_L2_2020-04-01_ETH-PERPETUAL.csv.gz"),
)

# The trade file names follow the archive's own pattern rather than being listed twice: the day and the symbol are
# the whole of the difference between the two instruments' runs.
#
# Paths are resolved from the repository root rather than from the working directory, because this tool lives two
# directories down and `tools/gate/check_docs.py` runs it from wherever the gate was started. A tool whose data paths
# depended on the caller's cwd would work in the gate and fail in the shell, which is the failure that reads as "the
# data is missing" when it is the path that is.
ROOT = Path(__file__).resolve().parents[2]
TRADES = str(ROOT / "data/v2/crypto/deribit_trades_{day}_{symbol}.csv.gz")


def percentile(sorted_values, quantile):
    """The quantile of an already sorted list, interpolating between neighbours.

    Interpolating rather than taking the nearest rank because with the five or six points a window series has,
    nearest-rank bounds jump by whole samples and the interval then swings by more than the effect being read.
    """
    position = (len(sorted_values) - 1) * quantile
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return sorted_values[int(position)]
    weight = position - lower
    return sorted_values[lower] * (1.0 - weight) + sorted_values[upper] * weight


def bootstrap_ci(values, resamples, level, seed):
    """The percentile interval of the mean, resampling the sample it was given.

    A fixed seed and a fixed order, because a document quotes this line and `tools/gate/check_docs.py` compares it to
    the last digit: a bootstrap that answered differently on every run would be unquotable, and seeding it is the
    whole reason a resampling method can be written into a table at all.
    """
    rng = random.Random(seed)
    size = len(values)
    means = []
    for _ in range(resamples):
        total = 0.0
        for _ in range(size):
            total += values[rng.randrange(size)]
        means.append(total / size)
    means.sort()
    tail = (1.0 - level) / 2.0
    return percentile(means, tail), percentile(means, 1.0 - tail)


def summarise(values, level, resamples, seed, unit):
    """Everything the line prints, from the sample alone.

    `sd` is the sample standard deviation -- divided by n-1, because these samples are the whole of what exists
    rather than a draw from something larger -- and `spread` is the range, printed because for a series of windows
    the range is the honest headline: the interval around the mean says how well the mean is pinned, and the range
    says how much one window can differ from another, which is usually the question.
    """
    if not values:
        raise SystemExit("no values to summarise: an interval over nothing is not a weak claim, it is no claim")
    size = len(values)
    mean = statistics.fmean(values)
    sd = statistics.stdev(values) if size > 1 else 0.0
    se = sd / math.sqrt(size)
    lo, hi = bootstrap_ci(values, resamples, level, seed)
    negatives = sum(1 for value in values if value < 0.0)
    return {
        "n": size,
        "mean": mean,
        "median": statistics.median(values),
        "sd": sd,
        "se": se,
        "ci95_lo": lo,
        "ci95_hi": hi,
        "spread": max(values) - min(values),
        "includes_zero": 1 if lo <= 0.0 <= hi else 0,
        "sign_consistent": 1 if negatives in (0, size) else 0,
        "thin_sample": 1 if size < THIN_SAMPLE else 0,
        "unit": unit,
    }


def format_line(stats):
    """The `[INTERVAL]` line: `name=value` pairs, six significant figures, so a block can compare it exactly."""
    order = ("n", "mean", "median", "sd", "se", "ci95_lo", "ci95_hi", "spread", "includes_zero",
             "sign_consistent", "thin_sample")
    fields = [f"{name}={stats[name]:.6g}" for name in order]
    return "[INTERVAL] " + " ".join(fields) + f" unit={stats['unit']}"


def report(values, args, label):
    """Print the line and the sentence that keeps it from being read as a verdict."""
    stats = summarise(values, args.level, args.resamples, args.seed, args.unit)
    print(format_line(stats))
    print(f"{label}: {stats['n']} draws, mean {stats['mean']:.4g} {stats['unit']} with a "
          f"{args.level * 100:.0f}% interval of {stats['ci95_lo']:.4g} to {stats['ci95_hi']:.4g}, "
          f"median {stats['median']:.4g}, and a range of {stats['spread']:.4g} across the draws.")
    if stats["thin_sample"]:
        print(f"  {stats['n']} is below the {THIN_SAMPLE} this repository requires before a mean is a conclusion, "
              f"so the interval is what this sample can say and not what the market can.")
    if not stats["sign_consistent"]:
        print("  The draws do not all fall on one side of zero: the sign of this number is not settled either.")


FIELD = re.compile(r"(?<![\w.])([A-Za-z_][A-Za-z0-9_]*)=(-?[0-9]+(?:\.[0-9]+)?(?:[eE][-+]?[0-9]+)?)")


def extract_field(paths, field):
    """`name=value` fields out of run logs, one value per file, in the order the files were named.

    One value per file rather than every match, because the point of a series is one draw per run and a log that
    printed a field twice would otherwise contribute two votes.
    """
    values = []
    for path in paths:
        text = Path(path).read_text(errors="replace")
        for name, value in FIELD.findall(text):
            if name == field:
                values.append(float(value))
                break
        else:
            raise SystemExit(f"{path} has no {field}= field: was it written by this tool's expected binary?")
    return values


def extract_column(paths, column):
    """One numeric column out of one or more CSVs, in row order, skipping the comment lines a run writes."""
    values = []
    for path in paths:
        rows = 0
        with Path(path).open(encoding="utf-8") as handle:
            header = None
            for line in handle:
                if line.startswith("#") or not line.strip():
                    continue
                cells = line.rstrip("\n").split(",")
                if header is None:
                    header = cells
                    if column not in header:
                        raise SystemExit(f"{path} has no {column} column, only {', '.join(header)}")
                    continue
                values.append(float(cells[header.index(column)]))
                rows += 1
        if rows == 0:
            raise SystemExit(f"{path} has a {column} column and no rows under it")
    return values


def replay(window, binary):
    """Run one window and return its break-even maker rate in basis points, with its fill count.

    The rate comes from `[PNL]`, which reports it as a fraction, so it is scaled here rather than in the table:
    every other number in this file is in basis points, and a table that mixed the two units would be unreadable
    to everyone including the person who wrote it.
    """
    depth = window.ensure_slice()
    command = ([str(binary), "l2_replay", TRADES.format(day=window.day, symbol=window.symbol), str(depth),
                window.symbol, *REPLAY])
    completed = subprocess.run(command, capture_output=True, text=True)
    if completed.returncode != 0:
        raise SystemExit(f"{window.name} exited {completed.returncode}: "
                         f"{completed.stderr.strip().splitlines()[:1]}")
    fields = {name: value for name, value in FIELD.findall(completed.stdout + completed.stderr)}
    if "breakeven_maker_rate" not in fields:
        raise SystemExit(f"{window.name} printed no breakeven_maker_rate=: the window produced no [PNL] line")
    return float(fields["breakeven_maker_rate"]) * 1e4, int(float(fields.get("trade_reports", "0")))


def window_series(spec, binary):
    """The break-even rates of a chosen set of windows, or the paired differences between the two instruments.

    `btc-months` is the series a claim should be made on, and it is not the largest one available: one snapshot
    slice a month, same venue, same file type, same length, so the draws differ by the month and by nothing the
    measurement chose. `btc` adds the incremental slices and the 2M one, which is a longer window and a different
    file type, and a series that put those beside the month series would be describing its own selection.

    A pair is the comparison that controls for the day: two instruments on the same day, the same file type and
    the same length differ by the tick and by the instrument, not by the month. It has the smaller n and the
    stronger claim, and both are printed rather than one being chosen.
    """
    if spec == "btc-eth":
        left = {window.key: replay(window, binary) for window in WINDOWS if window.symbol == "BTC-PERPETUAL"}
        right = {window.key: replay(window, binary) for window in WINDOWS if window.symbol == "ETH-PERPETUAL"}
        shared = [key for key in left if key in right]
        if not shared:
            raise SystemExit("no window exists on both instruments, so there is no pair to difference")
        values = []
        for key in shared:
            value = left[key][0] - right[key][0]
            print(f"  pair {key[0]} {key[1]} {key[2]} rows: BTC {left[key][0]:.4f} minus ETH "
                  f"{right[key][0]:.4f} = {value:.4f} bps")
            values.append(value)
        return values, "bps_btc_minus_eth"
    if spec == "btc-months":
        chosen = [window for window in WINDOWS if window.symbol == "BTC-PERPETUAL" and window.kind == "snapshot"]
    else:
        symbol = {"btc": "BTC-PERPETUAL", "eth": "ETH-PERPETUAL"}[spec]
        chosen = [window for window in WINDOWS if window.symbol == symbol]
    if not chosen:
        raise SystemExit(f"no window for {spec}")
    values = []
    for window in chosen:
        rate, fills = replay(window, binary)
        print(f"  {window.name}: {rate:.4f} bps over {fills} fills")
        values.append(rate)
    return values, "bps"


def self_test():
    """Hand arithmetic first, then the three properties a resampling interval has to have.

    Nothing here touches the binary or the archive: a self-test that needs seven hundred megabytes is a self-test
    the gate would not run, and the gate is the only reader this file is certain to have.
    """
    # [1, 2, 3, 4]: mean 2.5, deviations -1.5, -0.5, 0.5, 1.5, sum of squares 5 over n-1 = 3.
    stats = summarise([1.0, 2.0, 3.0, 4.0], 0.95, 2000, 11, "value")
    assert stats["n"] == 4 and stats["mean"] == 2.5 and stats["median"] == 2.5, stats
    assert abs(stats["sd"] - math.sqrt(5.0 / 3.0)) < 1e-12, stats["sd"]
    assert abs(stats["se"] - math.sqrt(5.0 / 3.0) / 2.0) < 1e-12, stats["se"]
    assert stats["spread"] == 3.0 and stats["thin_sample"] == 1, stats
    assert stats["ci95_lo"] <= 2.5 <= stats["ci95_hi"], stats

    # A constant sample has no width at all, so the interval is the constant and says so.
    flat = summarise([2.0] * 5, 0.95, 500, 11, "value")
    assert flat["sd"] == 0.0 and flat["ci95_lo"] == 2.0 and flat["ci95_hi"] == 2.0, flat
    assert flat["includes_zero"] == 0 and flat["sign_consistent"] == 1, flat

    # A sample that straddles zero gets an interval that straddles it, and one clear of zero does not.
    straddle = summarise([-1.0, 1.0] * 10, 0.95, 2000, 11, "value")
    assert straddle["includes_zero"] == 1 and straddle["sign_consistent"] == 0, straddle
    # Thirty-one draws well clear of zero: thick enough for the rule, and a sign that holds.
    solid = summarise([2.0 + 0.1 * (index % 3) for index in range(31)], 0.95, 2000, 11, "value")
    assert solid["includes_zero"] == 0 and solid["thin_sample"] == 0, solid

    # The width falls with the square root of the sample: four times the draws is about half the interval.
    rng = random.Random(7)
    population = [rng.gauss(0.0, 1.0) for _ in range(400)]
    small = summarise(population[:100], 0.95, 2000, 11, "value")
    large = summarise(population, 0.95, 2000, 11, "value")
    ratio = (small["ci95_hi"] - small["ci95_lo"]) / (large["ci95_hi"] - large["ci95_lo"])
    assert 1.4 < ratio < 2.6, ratio

    # The seed decides the line, and the same seed twice is the same line, which is what makes it quotable.
    first = summarise(population[:50], 0.95, 1000, 4242, "value")
    again = summarise(population[:50], 0.95, 1000, 4242, "value")
    assert format_line(first) == format_line(again), (first, again)
    # ... and a different seed moves the bounds without moving what they bracket.
    other = summarise(population[:50], 0.95, 1000, 99, "value")
    assert other["ci95_lo"] != first["ci95_lo"], (other, first)
    assert other["ci95_lo"] <= other["mean"] <= other["ci95_hi"], other

    # One outlier drags the mean and leaves the median: both are printed because of this case and no other.
    with_outlier = summarise([0.1] * 20 + [50.0], 0.95, 2000, 11, "value")
    assert with_outlier["median"] == 0.1 and with_outlier["mean"] > 1.0, with_outlier

    # One draw is not a sample: the interval collapses onto it and the rule flags it.
    single = summarise([3.5], 0.95, 500, 11, "value")
    assert single["n"] == 1 and single["ci95_lo"] == 3.5 == single["ci95_hi"] and single["thin_sample"] == 1

    # Nothing at all is an error rather than a wide interval.
    try:
        summarise([], 0.95, 500, 11, "value")
    except SystemExit:
        pass
    else:
        raise AssertionError("an empty series was summarised")

    # Quantile interpolation: the quarter point of [0, 10] is 2.5, not 0 and not 10.
    assert percentile([0.0, 10.0], 0.25) == 2.5 and percentile([0.0, 10.0], 0.5) == 5.0

    # The extractors, on the two shapes they read: a log line and a CSV with the comment a run writes first.
    with tempfile.TemporaryDirectory() as directory:
        log = Path(directory) / "run.log"
        log.write_text("[PNL] trade_reports=21 breakeven_maker_rate=-0.000389584 maker_notional_usd=210\n")
        assert extract_field([log], "breakeven_maker_rate") == [-0.000389584]
        assert extract_field([log], "trade_reports") == [21.0]
        try:
            extract_field([log], "no_such_field")
        except SystemExit:
            pass
        else:
            raise AssertionError("a missing field was not reported")
        table = Path(directory) / "trades.csv"
        table.write_text("# source_l2_market_data=binance\n"
                         "ts,symbol,fee\n"
                         "1,BTC-PERPETUAL,0.002\n"
                         "2,ETH-PERPETUAL,0.0035\n")
        assert extract_column([table], "fee") == [0.002, 0.0035]
        try:
            extract_column([table], "pnl_bps")
        except SystemExit:
            pass
        else:
            raise AssertionError("a missing column was not reported")
    print("self-test OK: the mean, the deviation, the interval's width against the sample size, the seed, the "
          "outlier, the single draw, the empty series and both extractors all match hand arithmetic")



def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--values", nargs="+", type=float, help="the series, on the command line")
    source.add_argument("--field", help="pull this name=value field out of each file named")
    source.add_argument("--column", help="pull this CSV column out of each file named")
    source.add_argument("--window", choices=["btc", "btc-months", "eth", "btc-eth"],
                        help="replay the windows on disk rather than read a series")
    parser.add_argument("files", nargs="*", help="logs for --field, CSVs for --column")
    parser.add_argument("--binary", type=Path, default=ROOT / "out/build/linux-debug/toy_quant",
                        help="the binary --window replays with; tools/gate/check_docs.py passes its own")
    parser.add_argument("--scale", type=float, default=1.0,
                        help="multiply every value, e.g. 1e4 to read a rate as basis points; the window mode "
                             "ignores it, because what it replays is already in basis points")
    parser.add_argument("--unit", default="value", help="what the numbers are, for the printed line")
    parser.add_argument("--level", type=float, default=0.95)
    parser.add_argument("--resamples", type=int, default=2000)
    parser.add_argument("--seed", type=int, default=20261101)
    parser.add_argument("--list", action="store_true", help="print the windows and exit")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        self_test()
        return 0
    if args.list:
        for window in WINDOWS:
            print(f"{window.name}  source={window.source}  slice={window.slice_path()}")
        return 0
    if args.resamples < 100:
        raise SystemExit("--resamples wants at least 100: fewer than that resamples the noise")
    if not 0.0 < args.level < 1.0:
        raise SystemExit("--level is a fraction between 0 and 1, such as 0.95")

    if args.window:
        if args.files:
            raise SystemExit("--window replays the windows itself and takes no files")
        if not args.binary.exists():
            raise SystemExit(f"--window needs a binary and {args.binary} is not there")
        values, unit = window_series(args.window, args.binary)
        # The window mode reports the unit the replay is in whatever the caller said, because that is what the
        # table in the document is in and a series that redefined its own units could not be quoted beside it.
        args.unit = unit
        report(values, args, f"{args.window} windows")
        return 0

    if args.values:
        values = [value * args.scale for value in args.values]
        label = "the command line"
    elif args.field:
        if not args.files:
            raise SystemExit(f"--field {args.field} needs the files to read it out of")
        values = [value * args.scale for value in extract_field(args.files, args.field)]
        label = f"{args.field} from {len(args.files)} file(s)"
    elif args.column:
        if not args.files:
            raise SystemExit(f"--column {args.column} needs the files to read it out of")
        values = [value * args.scale for value in extract_column(args.files, args.column)]
        label = f"{args.column} from {len(args.files)} file(s)"
    else:
        raise SystemExit("nothing to summarise: name a series with --values, --field, --column or --window")
    report(values, args, label)
    return 0


if __name__ == "__main__":
    sys.exit(main())


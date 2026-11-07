# Reading the output

*Part of the [User Guide](../USER_GUIDE.md).*

What each line reports, and how sure a number is.

## Inspect Results

Each run updates:

```text
data/runtime/orders.csv
data/runtime/trades.csv
```

Legacy CSV/UDP runtime files begin with `# source_ticks=...`; reuse that same Tick file for
`backtest_main`. Replay runtime files begin with `# source_market_data=...` and are not currently
the input format for the legacy backtest analyzer.

The terminal shows tick and summary information. Backtest reports are written under `logs/`:

```bash
./out/build/linux-debug/backtest_main \
  data/scenarios/sample_ticks.csv \
  data/runtime/orders.csv \
  data/runtime/trades.csv \
  0 0 backtest logs/backtest.log
```

`backtest_main` currently expects the legacy Tick file for mark prices. Replay mode records
`source_market_data` metadata, but full BBO-based portfolio marking is a separate future step.

## How Sure a Number Is

Every number this project quotes that is not arithmetic is a mean over a few events, and a mean without its
interval is a habit. `tools/study/pnl_interval.py` prints the interval instead:

```bash
# a series on the command line, in whatever unit you name
python3 tools/study/pnl_interval.py --values 1.2 -3.4 0.5 2.0 --unit=bps

# one field out of runs you have already saved
python3 tools/study/pnl_interval.py --field breakeven_maker_rate data/logs/*.log --scale 1e4 --unit=bps

# one column of a CSV, which is how a per-fill series would be read if the run wrote one
python3 tools/study/pnl_interval.py --column gross_closed_per_fill_usd data/runtime/trades.csv

# the windows on disk, replayed under one configuration rather than read from a log
python3 tools/study/pnl_interval.py --window btc-months
python3 tools/study/pnl_interval.py --window btc-months-span   # the same days cut by the clock, not by the row count
python3 tools/study/pnl_interval.py --window btc-eth      # the same-day pairs, differenced
```

The line it prints is `[INTERVAL] n=… mean=… median=… sd=… se=… ci95_lo=… ci95_hi=… spread=… includes_zero=…
sign_consistent=… thin_sample=… unit=…`, so it can be quoted in a marked block and re-derived by
`tools/gate/check_docs.py`; the sentence under it says the same thing in words.

Two habits are worth taking from the tool rather than reading past:

- **Read the range before the interval.** An interval says how well the mean is pinned; the range says how far
  apart the draws are. Five months of one instrument give a mean of -1.87 bps inside an interval of -2.84 to
  -0.90 while the draws themselves run from -3.19 to -0.44 -- the mean is tight and the population is not one
  thing, which is exactly what a single window cannot tell you.
- **A thin sample is flagged, not hidden.** `thin_sample=1` is the repository's thirty-fill rule applied by the
  tool instead of by memory, and a tool that refused to print a small sample would be argued around rather than
  read. What the flag means is that the interval is what this sample supports, not what the market does.

The floor numbers need none of this, and the contrast is the point: 5.39 bps a crossing costs is fee plus half a
tick, so running it twice gives the same digits and its interval has zero width. Intervals belong to estimates.


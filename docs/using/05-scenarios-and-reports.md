# Scenarios and reports

*Part of the [User Guide](../USER_GUIDE.md).*

Two kinds of input, and one picture at the end of either. The first is for a fast, repeatable check and the second is for a
measurement; neither is a market.

## Scenarios

| class | what it is | who uses it |
|---|---|---|
| a deterministic script | 1,000 synthetic ticks, one price a row | a format check and a regression run |
| a real archive | a venue-day of trades and depth | every number this repository quotes |

A scenario has one price a row rather than a book, so a queue model over it is a story about the script and not about a
venue; a real archive is what makes the queue the thing being measured.

## The report, in four views

| view | what it shows |
|---|---|
| Quotes & Executions | the reference price, the quote bands and the execution markers |
| Quote Distance to Reference Mid | how far the quote sits, in ticks |
| Net Inventory Profile | signed inventory against the configured bound |
| Equity (PnL) Path | the account marked every tick; its worst drop is the report's drawdown |

The summary panel beside them carries order count, executions, traded quantity, fill rate, initial capital, current
equity, max drawdown, inventory peak and unexecuted orders.

> `Reference Mid` uses the tick price as its visible reference, because a scenario row has no bid and ask. The report is a
> visual diagnostic rather than a full PnL report, and `data/runtime/*.csv` is overwritten by the next run.

/// details | Running a scenario and drawing the report

The scenarios are deterministic: the same seed recreates the same ticks, which is what makes them a regression input
rather than a sample. `sample_ticks.csv` is deliberately small for a format check.

```bash
python3 tools/legacy/gen_ticks.py all --count 1000 --seed 42 --output-dir data/scenarios
./out/build/linux-debug/toy_quant csv data/scenarios/sample_ticks.csv 0 optimized
python3 -m venv .venv && .venv/bin/python -m pip install -r requirements.txt
.venv/bin/python tools/study/plot_report.py \
  --ticks data/scenarios/sample_ticks.csv \
  --orders data/runtime/orders.csv \
  --trades data/runtime/trades.csv \
  --output reports/sample_ticks_optimized.png
```

The shipped files are `sample_ticks.csv` (small), `synthetic_ticks.csv` (mixed), `flat_ticks.csv`, `uptrend_ticks.csv`,
`downtrend_ticks.csv`, `shock_ticks.csv` and `random_ticks.csv`. Keep the file and the seed fixed when comparing two
strategies, and change one thing at a time.

///


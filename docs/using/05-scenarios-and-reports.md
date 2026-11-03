# Scenarios and reports

*Part of the [User Guide](../USER_GUIDE.md).*

The synthetic tick files, and the static report with a plot.

## Scenarios

Scenarios are CSV files containing synthetic market ticks. They make it easy to run the same
strategy against different market conditions. Built-in files live in `data/scenarios/`:

| File | Market behavior |
|---|---|
| `sample_ticks.csv` | Small input for a quick run |
| `synthetic_ticks.csv` | General-purpose mixed sample |
| `flat_ticks.csv` | Mostly stable prices |
| `uptrend_ticks.csv` | Rising prices |
| `downtrend_ticks.csv` | Falling prices |
| `shock_ticks.csv` | Sudden price movement |
| `random_ticks.csv` | Noisy price movement |

The generated scenarios contain 1,000 ticks each; `sample_ticks.csv` is intentionally small for
quick format checks. Keep the scenario file fixed when comparing `naive` and `optimized`, and
remember that runtime CSV files are overwritten by the next simulation.

Generate reproducible scenarios with a fixed seed:

```bash
python3 tools/legacy/gen_ticks.py all --count 1000 --seed 42 --output-dir data/scenarios
```

Using the same seed recreates the same ticks. For a controlled experiment, keep the scenario and
seed fixed, change only the strategy or one parameter, and compare the orders and trades. These
scenarios are for learning and regression experiments; they do not represent real market data or
production trading performance.

## Generate a Visual Report

After running a scenario, create a static report with the four main views:

- `Quotes & Executions`: reference tick price, buy/sell quote bands, and execution markers.
- `Quote Distance to Reference Mid`: quote displacement from the reference price in ticks.
- `Net Inventory Profile`: signed inventory and the configured `+/-1000` inventory boundary.
- `Equity (PnL) Path`: the portfolio marked to market at every tick, with the starting capital as the
  reference line. It is the path whose end is the summary's `Current Equity` and whose worst drop is
  its `Max Drawdown`.

The report also includes a right-side summary panel with order count, executions, traded quantity,
fill rate, initial capital, current equity, max drawdown, inventory peak, and unexecuted orders.

Set up the optional plotting environment and generate the report with:

```bash
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
.venv/bin/python tools/study/plot_report.py \
  --ticks data/scenarios/sample_ticks.csv \
  --orders data/runtime/orders.csv \
  --trades data/runtime/trades.csv \
  --output reports/sample_ticks_optimized.png
```

The report is a visual diagnostic, not a full PnL report. The input files contain one price per tick rather than complete bid/ask snapshots, so `Reference Mid` uses the tick price as its visible reference. Runtime CSV files are overwritten by the next simulation.


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

## Which fields appear, and when

Five fields decide whether a run of this mode was any good, and all five are `[EXECUTION]`/`[PNL]` fields whose
definition is in the [Learning Path field reference](../LEARNING.md#field-reference): `fill_rate`, `cancel_rate`,
`queue_ahead_consumed`, `net_pnl_usd`, and `breakeven_maker_rate`. The last is the one to read first — it is the maker fee
that would have made the window flat, so a negative value says no fee schedule can save it.

The rest of the summary is conditional, and this is the part worth knowing when running *this* mode, because each group
appears only when a flag asks for it:

- `min_commission_orders`, `min_commission_paid` — orders charged a schedule's floor under one order's commission and
  what it added, both zero unless `--fee-schedule=min=...` names a floor;
- `max_exposure_limit_usd`, `hard_limit_rejections` — the exposure cap and the orders it refused, both zero unless
  `--max-exposure-usd=...` names one;
- `kill_switch_loss_usd`, `kill_switch_triggered`, `kill_switch_ts`, `kill_switch_rejected_orders`, plus
  `kill_switch_closed_quantity` and `kill_switch_close_bps` when the switch actually closed something — all zero or
  false unless `--kill-switch-loss-usd=...` names a loss;
- `fat_finger_rejections`, `rate_limited_orders` — printed in a `[RISK]` line that appears only when `--fat-finger-bps`
  or `--max-orders-per-second` is named;
- `audited_rejected_orders`, `invalid_order_transitions`, `orphan_order_reports`,
  `cancelled_after_fill_orders`/`cancelled_before_fill_orders`, `orders_that_filled`, `filled_orders_by_size` — the order
  lifecycle and state-machine health counters, appended to `[EXECUTION]` only when non-zero. `filled_orders_by_size` is a
  histogram as `count/quantity` per bucket with the buckets `<=50, 51-99, 100-199, 200-499, 500-999, 1000+`: the bounds at
  100 and 200 are where the two share schedules this repository has read stop charging a minimum per order, so a
  commission under either convention is computable from it exactly;
- `total_order_lifetime_cycles` — quote-cycle lifetime sum, not elapsed microseconds.

The same question for a whole line -- what `[EXECUTION]` prints and what each name means -- is in the
[field reference in the learning path](../LEARNING.md#field-reference) and, for the arithmetic, in
[the glossary](../GLOSSARY.md).

## Two things a run's numbers do not include

**The warm-up.** The mainline does not quote until it has seen 16 trades and 2 valid views (`warmup_trades` and
`warmup_views` in `src/strategy/active_l2_market_maker.h`), and `trend_l2` waits for its slow average before it will act.
The *book* has no cold period to match: the incremental reader's first batch is the day's snapshot, so the first decision
is taken on a two-sided book. On the recorded BTC window the warm-up is shorter than a second -- 16 trades at that
window's own density of about 4,100 a minute is 0.23 s, against a 0.49 h window -- and on the two-hour SPY window about
five seconds. Those are arithmetic on the windows' trade counts, not measurements of the warm-up.

What it costs is not a biased mean but **a shorter opportunity set**: a strategy that warms up misses the fills at the
start, so two strategies with different warm-ups are compared over slightly different windows, and nothing in
`[EXECUTION]` reports how much of a window was warmed up. The baselines do not warm up at all, which is correct for them:
they need no history to quote. The one place the effect can bite is a window with so few trades that 16 of them take
minutes -- and the thin months of the interval series are exactly those windows.

**The exit.** A run that ends holding a position marks it at the last midpoint, and `[PNL]`'s `gross` and `net` count that
mark as if it were money. `--close-out` closes the position at the touch instead and `[WIND-DOWN]` prices the difference.
How large that difference is depends on the venue, not on the strategy:

| venue | exit cost | why |
|---|---|---|
| a US share | 0.18 bps | the half spread crossed and the commission are both fractions of a one-cent tick |
| a perpetual | 5.39 bps | a taker order crosses a 5 bps fee plus half a tick that is worth real basis points |

The pinned example is [the SPY window](../performance/history-03-studies.md#what-the-two-symbols-do-in-the-engine): the mark
reads 262.51 and the account after the exit 257.11, with 38 of the 438 shares left unclosed because the touch showed no
size. Every `net USD` column in these documents that carries a position is a mark until someone runs it with `--close-out`.


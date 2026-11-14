# Glossary: the arithmetic behind the words

*Part of the reference tier. The map of the documents is [`MAP.md`](MAP.md).*

Every number this project quotes has a definition, and most of those definitions are one line of arithmetic. This page
owns the **concepts**: what a term means, the formula behind it, and the file that computes it. It does not own the
field list -- [the learning path](LEARNING.md) explains every field a run prints, and
[the architecture](architecture/06-accounting-and-the-summary.md) explains which line computes which. Read this page when
a term in a table is doing work you cannot see.

Conventions below: `mid` is the midpoint of the top of book at the moment named; `tick` is the instrument's price
increment; `qty` is the quantity of a fill in the instrument's own units; **`side` refers to our own fill**, not to the
aggressor.

## Prices and the book

| term | what it means, and the formula | computed in |
|---|---|---|
| tick | the instrument's minimum price increment; every price in the engine is an integer count of ticks | `src/common/instrument_spec.h` |
| top of book (BBO) | the best bid and best offer, with sizes | `src/orderbook/top_of_book.h` |
| the touch | the side of the book a quote would join at the best price: the bid for a buy, the ask for a sell | `docs/LEARNING.md` |
| mid | `(best_bid + best_ask) / 2` | `L2MarketView::mid` |
| half-spread | `(best_ask - best_bid) / 2`, in ticks | derived |
| crossed book | `best_bid >= best_ask`; the view reports nothing and every consumer skips it, so a run counts these instead | `crossed_book_batches` on `[L2 DATA]` |
| stale print | a market trade with no midpoint to compare against, because the book was one-sided or older than a second | `stale_trades` on `[L2 DATA]` |
| dislocated print | a market trade more than five basis points from a *current* midpoint, which is a sweep rather than a bad row | `dislocated_trades` on `[L2 DATA]` |

## What a fill earns and what it costs

| term | what it means, and the formula | computed in |
|---|---|---|
| capture | the fill against the mid at the fill, in the direction we earned it: `buy ? (mid - price) : (price - mid)`. **Positive is good.** The reported `captured_edge_per_unit_ticks` is the quantity-weighted average of this divided by the tick | `src/app/fill_accounting.cpp` |
| markout | the fill against the mid a horizon later, in the direction that hurts us: `buy ? (price - mid_later) : (mid_later - price)`. **Positive is adverse** -- the mid moved away from the position -- which is why it accumulates into a field called `adverse_selection` | `src/app/run_accounting.cpp` |
| markout horizon | how many quote cycles the markout waits before it resolves; five by default, `--markout-horizon=N` to change it | `--markout-horizon` |
| quoting margin | `capture - markout - every fee`, the number a quote has to keep positive to be a business | `docs/RETROSPECTIVE.md` |
| fee | what the venue or broker charges a fill: a maker/taker rate on notional for a perpetual, a per-share commission plus a minimum per order for a US equity | `src/exchange/fee_schedule.h` |
| fee floor | the minimum commission per order. On a small clip it binds before the rate does, and it is counted as `min_commission_orders` | `min_commission_paid` on `[EXECUTION]` |
| `fee_ratio` | `fees_paid / abs(gross_pnl)`, with a 1e-12 floor on the denominator | `src/app/strategy_benchmark.cpp` |
| `breakeven_maker_rate` | `gross_pnl_usd / maker_notional_usd` -- the maker rate at which the window would be flat. **Negative means no fee tier could have saved it**; positive means the venue could have charged this much on top of the commission and the window would still break even | `src/app/run_summary.h` |
| net per fill | `net_pnl_usd / trade_reports` | `[PNL]` |
| gross, closed, net | gross is the whole run marked to the last mid; closed is the part opened *and* closed inside the window; net is gross less fees and commission. The spread between gross and closed is the position left open. When the closing mark is missing, the three mark-dependent figures print `n/a` and `marks_complete=0` says why | `[PORTFOLIO]`, `[PNL]` |

## Queue and flow

| term | what it means, and the formula | computed in |
|---|---|---|
| queue ahead | the quantity resting in front of our order at its price when it joined | `queue_profile_orders` |
| queue consumed | `queue_ahead_consumed`: the quantity that traded through that queue over the run -- the measure of whether a quote was close to filling or nowhere near it | `[EXECUTION]` |
| queue fraction | the share of the queue in front of an order that was still there at its death: median and tenth percentile, as a fraction of what it joined behind. A model that gives a resting quote no queue reports zeros | `queue_min_fraction_p50`, `queue_min_fraction_p10` |
| fill rate | `trade_reports / submitted_orders` | `[EXECUTION]` |
| cancel rate | `cancel_requests / submitted_orders`; a quoter that refreshes on every price move has one near 1 | `[EXECUTION]` |
| quote distance | how far from the mid a quote sat, in ticks; the gate buckets fills into near (within a tick) and far | `total_quote_distance_ticks` |


## The order leg, and a venue that answers

The one place where a run leaves the process. The terms are small but they are the ones a first sandbox run gets wrong:

| term | what it means | comes from |
|---|---|---|
| the order leg | a run's own orders leaving the process: a socket to the relay, which signs the venue's REST calls, with the venue's private `orders` channel coming back as state | `src/lines/live_venue.h` |
| `owner` | the client id the adapter stamps on every order, so a venue report can be matched to the order that caused it | `ExecutionReport::owner` |
| `liquidity_role` | what the **venue** says the fill was, `maker` or `taker`, which picks the fee rate; the engine's own queue model does not set it | `ExecutionReport::liquidity_role` |
| `ctVal` | a contract's size in the underlying -- `0.01` BTC for `BTC-USDT-SWAP` -- so a venue-unit quantity is not a coin quantity | the venue's instrument list |
| reconciliation | the ledger's statement against the venue's own reports; `status=break` means the two disagree, and no number from that run is quoted | `[RECON]` |

## Statistics, when a set of runs is quoted

`tools/study/pnl_interval.py` resamples a series and prints one line; these are its fields.

| term | what it means |
|---|---|
| `mean`, `median` | the mean is a point estimate; the median is beside it because one outlier or a trend moves the mean and not the middle |
| `ci95_lo`, `ci95_hi` | a 95% resampling interval around the mean: how well the mean is pinned, **not** how far the draws spread |
| `spread` | the range of the draws themselves, which on a series of windows is usually the larger and the more interesting number |
| `thin_sample` | the repository's rule that a fill count below thirty is not a conclusion, applied rather than remembered |
| `sign_consistent` | whether every draw falls on one side of zero; 0 means the direction is not settled, whatever the mean says |

## The quotient that closes a market

A line is closed when `capture / sigma(position)` is too small to separate at any sample this account can buy, where
`sigma(position)` is the standard deviation of the position term the policy builds rather than of a one-unit price move.
The tool, the eight recorded verdicts and the arithmetic are in [the tools](TOOLS.md) and
[the retrospective](RETROSPECTIVE.md); the number is re-derived on every gate run by `tools/study/viability.py`.

## A sign convention two documents read differently

The formulas above are what the code computes, and `note_fill()` passes **our own side** (`src/app/fill_accounting.cpp`),
so a positive markout is adverse. Two sentences in the history read the sign the other way. Stating it once here means a
reader does not have to guess:

- [the L2 history](performance/history-02-l2.md) describes an `adverse_selection` of -0.5 as edge "given back", which is
  the opposite of what the code's sign means for that run;
- [the studies](performance/history-03-studies.md) calls a positive markout evidence that "the fills were not picked
  off", which is also inverted.

Neither changes the conclusion it supports -- in the second case the argument is about the position's mark, not the
fills -- but a reader who follows the prose rather than the formula gets the sign backwards. The code is the authority,
and both sentences carry a note where they stand, because the notebooks are a record of what was observed and are not
rewritten to match a later reading.

## Where to go next

- what a run prints, field by field: [the learning path](LEARNING.md)
- which line computes which number: [accounting and the summary](architecture/06-accounting-and-the-summary.md)
- how a number is re-derived: the marked block next to it, and [the tools](TOOLS.md)

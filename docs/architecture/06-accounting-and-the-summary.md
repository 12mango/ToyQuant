# Accounting and the summary

*Section 7 of the module reference. The index is [`../ARCHITECTURE.md`](../ARCHITECTURE.md).*

What a fill does to cash and inventory, and what every field of every report
line means and who computes it.

## 7. Backtest and Performance Metrics

**Files:** `src/backtest/backtest_driver.h`, `backtest_driver.cpp`

`BacktestDriver` is an offline analysis component. It reads the Tick file and recorded trades,
replays positions with slippage, and writes a report. It does not participate in live matching.

```cpp
double exec_price = trade.price +
    (trade.side == Side::Buy ? slippage_ : -slippage_);
double fee = trade.quantity * exec_price * fee_rate_;

auto& pos = positions[trade.symbol];
```

The PnL logic uses a **net-position** model: positive quantity is long, negative quantity is short,
and zero is flat. A buy first closes a short; a sell first closes a long; any remainder extends the
opposite position.

`Position` stores the average entry price. Closing quantity contributes to `realized_pnl`; remaining
quantity contributes unrealized PnL at the latest Tick price. The backtest uses an initial capital
baseline, records equity while replaying ticks and trades in timestamp order, and then computes
`max_drawdown` from that equity curve. The `orders_file` argument is retained for CLI compatibility
but is not read.

**Files:** `src/backtest/performance.*`, `tests/performance_test.cpp`

The reusable `metrics` module keeps formulas independent from the live pipeline and `BacktestDriver`.
`BacktestDriver` computes `max_drawdown` from the tick path's equity curve; the pipeline samples the marked
equity once per 60-second bucket behind `--metrics` and derives the same drawdown plus its duration, the
annualized volatility and the Sharpe, Sortino and Calmar ratios from it, so the two paths are one
implementation rather than two. The same flag turns on the round-trip distribution and the taker
measurements, each on its own line, and none of those lines exists in a run that did not ask for it.

The pre-trade controls are no longer branches inside `submit_strategy_actions`. `src/app/risk_pipeline.*` holds
them as an ordered list -- kill switch, fat-finger band, token bucket, exposure cap, position gate -- and returns
the first reason that refuses an order together with its own counter. A rejected order consumes neither a rate
token nor a pending reservation, and a control at zero is off, so a run that names none of them follows the same
path and prints the same summary it did before the class existed. The order is the policy: the price band is
checked before the bucket so a bad price cannot spend an order's rate, and the exposure cap is checked before the
position gate so neither folds the other's pending quantity into its own projection. A reducing order is allowed
through the bucket, the exposure cap and the position gate: those controls refuse new risk, not its removal, so a
position that is already too large can be worked down. A refusal is reported to the strategy through
`Strategy::on_order_rejected`, which is what lets `trend_l2` back off instead of retrying on the next view;
`Pipeline::handle_rejected_order` uses the same callback for a venue or broker refusal at submission, in
addition to the cancelled report that removes the order from every strategy's open set.

| Metric | Meaning | Current consumer |
|---|---|---|
| `fill_rate` | Filled quantity divided by submitted quantity | `Pipeline` execution summary |
| `cancel_rate` | Cancel requests divided by submitted orders | `Pipeline` execution summary |
| `inventory_exposure` | Absolute value of net position | `RunSummary` portfolio metrics |
| `max_drawdown` | Largest decline from an equity peak | `BacktestDriver` on the tick path, and `[PERFORMANCE]` on the marked equity curve |
| `fees_paid` | Fees charged on recorded MarketMaker fills | Runtime and offline summaries |
| `adverse_selection` | Delayed fill markout accumulated after a quote horizon | L1 runtime summary |
| `average_abs_inventory` | Average absolute effective inventory at quote decisions | L1 runtime summary |
| `quote_lifetime` | Number of BBO decision cycles before fill or cancel | L1 runtime summary |
| `queue_ahead_consumed` | L2 market volume absorbed ahead of local maker orders | L2 runtime and benchmark summary |
| `buy_queue_ahead_levels_cleared`, `sell_queue_ahead_levels_cleared` | Number of price levels whose external queue reached zero | L2 runtime and benchmark summary |
| `audited_orders`, `audited_filled_orders`, `audited_cancelled_orders` | Order lifecycle terminal-state counts | Runtime summary |
| `audited_rejected_orders` | Orders that ended in a venue, broker or timeout rejection, appended to `[EXECUTION]` only when non-zero. A rejection is terminal like a cancel but is not one the strategy asked for, which is why it has its own count and its own `RejectReason` | Runtime summary |
| `invalid_order_transitions`, `orphan_order_reports` | A report that would move an open order backwards through Pending -> Resting -> PartiallyFilled -> terminal, and a MarketMaker report for an order whose audit is already closed. Both are state-machine health counts, zero in a normal run and appended to `[EXECUTION]` only when non-zero | Runtime summary |
| `cancelled_after_fill_orders`, `cancelled_before_fill_orders` | Cancelled orders split by whether they had a fill | Runtime summary |
| `orders_that_filled`, `filled_orders_by_size` | Filled order instances (a modification or a cancel and replace makes a new one) and their histogram in `count/quantity` buckets of `<=50, 51-99, 100-199, 200-499, 500-999, 1000+` shares, which is what a broker's minimum per order is charged on | Runtime summary |
| `min_commission_orders`, `min_commission_paid` | Orders charged a schedule's floor under one order's commission, and what that floor added. Zero unless `--fee-schedule=min=...` names one; the charge itself is booked to `fees_paid` and `cash` when the order ends, because that is when the order's total is known | Runtime summary |
| `max_exposure_limit_usd`, `hard_limit_rejections` | The pre-trade exposure cap in USD and the orders it refused. The cap marks the projected position at the order's own price, the conservative side of the mark for an order that adds to the position, and it runs before the position gate so that neither gate folds the other's pending quantity into its own check | Runtime summary |
| `kill_switch_loss_usd`, `kill_switch_triggered`, `kill_switch_ts`, `kill_switch_rejected_orders`, `kill_switch_closed_quantity`, `kill_switch_close_bps` | The loss the switch was set at, whether it tripped, the timestamp, the orders the strategy asked for after it did and did not get, and what the halt itself did: the quantity it sent out and what that exit cost in basis points. It is a latch, checked once a cycle by `RiskPipeline::begin_cycle`, which `submit_strategy_actions` calls before anything is submitted, so a strategy cannot reason its way past it. A latched switch closes the position it was holding at the touch and then refuses every order that is not an exit, and it is marked each cycle while armed -- the first version only stopped quoting, which left the risk the cap exists to remove, and it also refused the exit order, which is why the two defects are recorded together | Runtime summary |
| `fat_finger_rejections`, `rate_limited_orders` | The two controls the non-market-making line added, printed in a `[RISK]` line only when either was named: an order priced too far from the current midpoint, and the token bucket refusing a submission. Neither consumes a token or a pending reservation; the first is checked before the bucket and both before the exposure and position gates | Runtime summary |
| `total_order_lifetime_cycles` | Sum of quote-cycle lifetimes for audited orders | Runtime summary |
| `[L2 DATA]`: `events`, `trades`, `depth_snapshots`, `incremental_batches`, `incremental_snapshot_batches` | How much input arrived, by kind. Counted by `MarketDataValidator` as it validates, so the line reports what the feed delivered rather than what the strategy chose to use. A run whose depth input delivered nothing is an error rather than a line of zeros | `[L2 DATA]`, from the feed's validation summary |
| `[L2 DATA]`: `crossed_book_batches`, `stale_trades`, `dislocated_trades` | Three counters that appear only when non-zero, so a clean window keeps its recorded line: incremental batches that left the book crossed, prints whose reference midpoint was missing or more than a second old, and prints more than five basis points from a *current* midpoint. The last of them is `Pipeline::process_l2_market_trade` scoring each print against the book, because the depth path has no best bid and offer to score it against | `[L2 DATA]` suffix, from the book and the pipeline |
| `span_seconds`, `samples`, `periods_per_year`, `max_drawdown_duration_seconds` | The equity series behind `[PERFORMANCE]`: the marked account sampled once per 60-second bucket, the window it covers, the sample count, the annualization factor the rates were scaled by, and the longest time from a peak until the curve won it back -- a deep-but-short decline and a shallow-but-permanent one are not the same risk. Printed only under `--metrics` | Runtime `[PERFORMANCE]` |
| `sharpe`, `sortino`, `calmar`, `ann_vol` | `mean(r)/std(r)·sqrt(N)`, the same with the downside deviation taken about zero, annualized return over the maximum drawdown, and `std(r)·sqrt(N)`, with `r_t = E_t/E_{t-1} - 1`. `N` is the number of 60-second buckets a 24/7 year holds and is printed on the line, because a ten-minute window's ten buckets are not its annualization. A constant series, a series with no loss and a run with no drawdown report zero rather than a division by zero | Runtime `[PERFORMANCE]` |
| `win_rate`, `profit_factor`, `expectancy_usd`, `average_win_usd`, `average_loss_usd`, `average_holding_seconds` | The distribution of the round trips the account completed, net of both fee halves: a winning round trip is one whose net PnL is above zero, the factor is the winning sum over the absolute losing one, and the holding time runs from the fill that opened the position. The sum of the trips plus the entry fees still carried by an open position is the run's realized PnL, which is what makes the distribution a decomposition rather than a second opinion | Runtime `[ROUND_TRIPS]` |
| `fees_usd`, `fees_per_round_trip_usd`, `fees_bps_of_turnover` | What the round trips paid to be made: the fees booked to them when they closed, their mean, and those fees over the notional the run traded. `expectancy_usd` is already net of them, so the pair a reader compares is `fees_per_round_trip_usd` against the operation's gross edge -- when the first exceeds the second there is no fee schedule the run can survive, which is the shape most of this repository's closed lines took. The basis-point figure divides by the run's own notional rather than by `quantity * price`, because the latter is not a notional at all on a fixed-face-value contract | Runtime `[ROUND_TRIPS]` |
| `taker_notional_usd`, `total_notional_usd`, `turnover` | What the aggressive and the quoted fills traded, and their sum over the collateral the run started with. The maker half is what the breakeven maker rate divides by, and the total is the capacity question a participation limit answers | Runtime `[ROUND_TRIPS]` |
| `side=long`, `side=short` round-trip rows | The same distribution over only the round trips whose position was opened on that side, printed as one `[ROUND_TRIPS_SIDE]` line per side that completed a trade. A maker's long and short fills are different populations -- one is hit by sellers, the other by buyers -- so the split is what shows a side that never wins, which the aggregate hides. The account-level notional and turnover stay on the aggregate line, because a fill belongs to the account rather than to the side of the position it happened to close | Runtime `[ROUND_TRIPS_SIDE]` |
| `filled_ratio`, `is_filled_bps`, `is_fees_bps`, `maker_share`, `maker_fees_bps`, `taker_fees_bps`, `is_opportunity_bps`, `is_total_bps` | Implementation shortfall of one parent order, in basis points of the notional it decided at: the filled part against the arrival midpoint, the fees, and what the quantity it never got would have cost at the deadline. The three mix fields say **which** of the venue's two fee rates those fees paid: the share of the filled quantity that rested, and the fee charged on each role, whose sum is `is_fees_bps`. Resting is worth 10 to 20 bps a round trip on the venues measured here, so a fee without its mix cannot say whether a parent took the cheap role or paid for the expensive one. Printed as `[EXECUTION_ALGO]` only when `--parent` named an order, and the opportunity term is what stops a schedule that trades little from looking cheap | Runtime `[EXECUTION_ALGO]` |
| `twap_mid`, `filled_vs_twap_bps`, `opportunity_vs_twap_bps`, `is_vs_twap_bps` | The same order against the time-weighted midpoint of its own window instead of the price it arrived at. A parent that buys into a rising midpoint looks expensive against the arrival however it is worked, so this second benchmark is the one that says whether a schedule tracked the market it was measured against: `twap` lands within three basis points of it, and a single crossing's distance from it is the window's drift with the sign flipped | Runtime `[EXECUTION_ALGO]` |
| `arrival_slippage_ticks`, `arrival_slippage_bps`, `arrival_slippage_usd` | The price a crossing fill paid against the midpoint its order decided at, quantity weighted and signed as a cost, so a buy above the decision price and a sell below it are both money paid away | Runtime `[TAKER]` |
| `taker_edge_5_ticks`, `taker_edge_20_ticks`, `taker_edge_60_ticks` | The drift from a crossing fill to the midpoint 5, 20 and 60 quote cycles later, positive when the move covered the spread. The sign is the fill's own, the opposite of `adverse_selection`, and it is deliberately not called `captured_edge`: a taker's spread payment is a cost rather than a capture, and the three buckets are the C++ side of the two the Python benchmark measures | Runtime `[TAKER]` |
| `shortfall_filled_usd`, `shortfall_fees_usd`, `shortfall_opportunity_usd`, `shortfall_usd`, `shortfall_bps` | Implementation shortfall of the IOC/FOK orders: the filled quantity against the decision price, the fees, and what the unfilled remainder would have earned by the horizon -- the term an IOC run is most likely to lose and the breakout line's main cost. The basis-point figure divides by the decision notional, so an order that never filled still contributes its denominator. A quote the run cancelled itself is not measured: cancelling declines a fill rather than missing one | Runtime `[TAKER]` |

The metric tests cover zero denominators, positive and negative inventory, empty and single-point equity
curves, a known peak-to-trough drawdown, the compounding annualization of a two-point series, a constant
series whose ratios are zero, a drawdown's duration with and without a recovery, the two half-fees a round
trip carries, the win/loss split of a three-trade distribution, and the three terms of an implementation
shortfall. This separation lets future metrics be added without expanding `backtest_driver.h` or duplicating
formulas in `main.cpp`.

**Files:** `src/accounting/portfolio.*`, `tests/portfolio_test.cpp`

`Portfolio` books execution reports and values cash, realized PnL and equity. Until the carry line it
knew one income stream, the spread a fill captures, and one cost, the fee.
`accrue_interval_flow` adds the flow a *position* earns or pays for as long as it is held: funding on
a perpetual, the basis a future pays at roll, a dividend, or the financing a position is charged. One
method for all four, because the ledger side is identical in each and only the source of the rate
differs:

```cpp
double flow = portfolio.accrue_interval_flow(symbol, interval_rate, mark_price);
```

`interval_rate` is the rate one USD of notional earns per interval to a long position, so the
position's own sign decides the direction of the payment and one rate serves both sides of a market:
a short pays exactly what a long receives. A published funding rate is the rate longs pay and enters
as `-funding_rate`; a dividend is positive and financing negative. The notional is the USD value the
class already assigns the position at `mark_price` through `unit_notional_at`, so a fixed-USD-face
contract accrues on its face value and a price-quoted unit on `quantity * mark_price`; the entry price
and unrealized PnL play no part. `accrue_interval_flows` applies one interval across the whole book
from a rate map and a mark map, which is the shape a rate series has and the shape two legs will need,
and a position that either map does not name accrues nothing rather than being guessed at.

Cash, realized PnL and equity move together by the amount returned. An accrued flow has been paid
rather than marked, so it does not wait for a closing trade to become realized, and equity tracks cash
so the identity `mark_to_market` produced still holds between marks. `tests/portfolio_test.cpp` states
the cash, the equity and the realized PnL after N intervals for a price-quoted position, a fixed-face
contract and a short, pins the notional basis and the sign rule, and checks that a flat position, an
unknown symbol and an unpriced price-quoted instrument all accrue nothing. There is deliberately no
data plumbing yet: the mechanism is checkable before a rate series exists, the way the queue model was
checked before live fills were discussed. The order of the remaining steps is scoped in
[Roadmap](../archive/studies.md#the-carry-line-financing-flows-before-carry-strategies)

The rate then arrives from a file, which is `src/market/funding_rate.*`. Two shapes are recognised by
their own header: an OKX daily funding archive, which holds every swap on the venue in one file, and a
Binance Vision monthly archive, which holds one symbol. `FundingRateSeries` keeps one instrument's
settlements in order and consumes them forward, so a settlement is applied exactly once, at the first
event at or after its timestamp, on the position the previous event left and at the last price the run
has seen. The settlement instant itself is not in the event stream, so that mark is the nearest causal
one available; the position is exact, because no event between the settlement and the event that
follows it could have changed it. The columns are read as published and the polarity is flipped once,
here, because a published funding rate is what longs pay while `accrue_interval_flow` takes the rate a
long earns. The run summary prints a `[FUNDING]` line with the file's own arithmetic (points, span,
summed rate, annualized rate) next to what the replay did with it (settlements reached, USD moved), and
only when `--funding-rates` supplied a series, which is why a run without one still produces exactly
the output it produced before this input existed. A settlement reached before the account holds a
position is counted and moves nothing: the line therefore reports two different things, and a window
that begins after a settlement is the case where they disagree.

The two legs are then the carry mode itself, `src/lines/carry_ledger.*`. A leg is a stated USD
notional rather than a contract count, so one quantity unit is one dollar: the venue's taker rate
applies to the number the command states, the funding accrues on the same number, and neither needs a
price feed. What that leaves out is stated rather than hidden — a fixed-contract position's notional
drifts with the mark, which is a second-order term in carry against fees, and the mode does not model
it. Both legs are opened and closed as taker fills at the published rates through the same
`fee_on_fill` the matching engine charges with, so a fill booked outside a book cannot be priced
differently from one booked inside it.

Two numbers describe the resulting book and they are not interchangeable. `carry_gross_notional_usd`
is both legs and `carry_net_notional_usd` is their difference, which is the account's own exposure: a
hedged carry holds two notional and is exposed to none of it, and a report offering only one of the two
would make the position look either twice its size or half its return. The acceptance number is the
annualized carry divided by the fees the position paid, which is the form the roadmap states, printed
next to `carry_breakeven_hours`, which is the same comparison in the form a decision needs: a fee is
paid once while a carry accrues every interval, so the two only meet through a holding period, and a
ratio above one can coexist with a window in which the position did not quite pay for itself.


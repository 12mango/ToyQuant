*Part of the [User Guide](../../USER_GUIDE.md), part 2 of 3. The index is [`../03-l2-replay.md`](../03-l2-replay.md).*

## The broker and the reconciliation

`--paper-broker` puts a broker between the strategy and the engine, which is a layer a live path has and a
replay does not: it refuses orders for reasons a strategy cannot see in the price, fills in pieces, loses an
execution report, and goes away for a while. It decorates the engine rather than replacing it, so the
strategy, the book and the accounting do not know it is there, and with no faults configured it forwards
every call and every report — `bash tools/gate/verify_l2.sh fast` asserts that a brokered run reports exactly the
numbers of the same run without it — and then prints two lines of its own:

```bash
./out/build/linux-debug/toy_quant l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz \
  /tmp/depth_200k_base.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --paper-broker
```

```text
[BROKER] orders_submitted=381 orders_rejected=0 fills_seen=3 fills_split=0 fills_dropped=0 outages=0 orders_held=0 orders_released=0 cancels_while_held=0
[RECON] symbols_checked=1 quantity_breaks=0 max_quantity_break=0 net_quantity_break=0 gross_quantity_break=0 report_count_difference=0 fee_difference=0.000000 fees_match=1 status=ok
```

The faults are counts of orders or fills rather than drawn probabilities, so a faulted run is reproducible
from its command line alone. `--broker-reject-every=N` refuses every Nth order and counts each refusal,
because a refusal emits no report — the same reason the engine carries a reject count for the pipeline to
watch across a submission. `--broker-partial-every=N` delivers every Nth of the strategy's fills as two
prints with the fee split the same way, so the position and the fee are what one print would have been and
only the report count changes: on the 200k slice with `--order-size=10` it split three fills, the run
reported six trade reports for the same three executions, and the reconciliation stayed `status=ok` with a
fee difference in the last bits only. `--broker-drop-every=N` withholds every Nth fill from the strategy
while the venue's statement still records it executed. And `--broker-outage-after=N` with
`--broker-outage-holds=M` takes the venue away once N orders have been accepted, holds the next M, and
delivers them in submission order when the order after them arrives; a hold of zero is a venue that never
comes back. A fault flag without `--paper-broker` is refused rather than ignored, because a fault
the run cannot show would leave a faulted session looking exactly like a clean one.

`[RECON]` is the check the whole thing exists for. The statement is built from the reports the venue sent and
the portfolio from the reports the strategy received, so a fill the strategy never heard about is a position
that disagrees with a statement saying it happened. Only this strategy's own fills are compared, because two
populations would break on every print that was somebody else's, and cash is deliberately not compared: a
broker's USD arithmetic would be a second implementation of the portfolio's instrument model, and a copy
that agrees because it was copied is not a check. The columns are the net position per symbol, the gross
quantity traded, the fills the strategy was sent against the fills it booked, and the fees. Gross is there
because a loss on each side leaves the position exactly where it was: on the 200k slice with every fill
withheld, every position column read zero breaks and the gross column caught both fills —

```text
[RECON] symbols_checked=1 quantity_breaks=0 max_quantity_break=0 net_quantity_break=0 gross_quantity_break=2 report_count_difference=0 fee_difference=0.004000 fees_match=0 status=break
```

A fee difference is read with a tolerance, because splitting one fill into two prints adds the parts in a
different order from the total they came from, and `fees_match` is the verdict on it rather than the number
being exactly zero.

Two things the broker has to do that are easy to leave out, both of which the faults found by being run
rather than by being reasoned about. A cancel that arrives for an order still held — one the venue has never
been given — is answered with a `Cancelled` report, because that order is gone for good and the strategy is
owed the confirmation; resolving it silently left a strategy that keys on cancel acknowledgements with a
working order for the rest of the window, and it stopped quoting 51 orders into a 381 order run. And a fill
is split only when it is more than one unit, since two prints of zero are not a split.

## The order gateway

`--order-gateway` puts the client's side of the order path outside the broker. It is the layer that knows
the difference between an order that was sent and an order the venue answered, and it keeps its own table
of every order in between, printed as `[GATEWAY]`:

```bash
./out/build/linux-debug/toy_quant l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz \
  /tmp/depth_200k_base.csv BTC-PERPETUAL 0 active_l2 1 conservative --fast-validation \
  --order-latency-us=100000 --order-gateway --gateway-ack-timeout-us=50000 --gateway-retry-timeouts
```

```text
[GATEWAY] submissions=6 duplicates_suppressed=0 acks=6 ack_timeouts=6 unacknowledged=0 timeouts_queried=6 timeouts_working=0 timeouts_unknown=6 retries=6 retry_rejections=0 venue_rejects=0 cancels=6 cancels_after_fill=0
```

A timeout is not a terminal state. The gateway queries the venue with `OrderQuery` and reports the
reconciliation separately: `unacknowledged` is the number still waiting for an answer, `timeouts_working`
means the venue still holds the order, and `timeouts_unknown` means it does not — neither result is treated
as a fill or a cancel. The ledger keeps the order `Pending` until a real report arrives.

The client order id is the strategy's order id, entered in the table before the venue is called. A second
submission under an id already in the table is absorbed rather than forwarded, so a retry cannot become a
second position. `--gateway-ack-timeout-us=N` counts an order the venue has not said anything about within
N microseconds of market time, and `--gateway-retry-timeouts` sends it again under the same id. The venue
already refuses a duplicate id, so the second copy is a refusal rather than a fill: on the 200k slice with
each order held for 100 ms and a timeout of 50 ms, the gateway timed out and re-sent all six orders, the
engine refused six duplicates on stderr, and the run's trades, P&L, fees and inventory were exactly those
of the same run with the latency and no retry. A cancel for an order that has already finished is counted
in `cancels_after_fill` instead of being sent to a venue with nothing to cancel. With every knob at zero
the gateway forwards every call and every report, and a run with it reports the numbers of the same run
without it.

The symbol decides the instrument, through `instrument_spec_of`: `BTC-PERPETUAL`, `BTC-USDT-SWAP`, `SPY` and
`MESZ6` are the entries today, and a symbol with no entry is an error rather than a default, because a default
would quietly price the wrong tick with the wrong fee. A listed future also states its contract multiplier,
which is what makes one micro E-mini S&P 500 unit worth five times the index price, and its quarter point tick
worth 1.25 USD rather than 0.25, so a commission of about sixty cents a contract is half a tick a side:

```bash
./build/toy_quant l2_replay data/v2/futures/glbx_MESZ6_trades.csv.gz data/v2/futures/glbx_MESZ6_depth.csv.gz \
  MESZ6 0 active_l2 1 conservative --order-size=5 --inventory-limit=20 --base-spread-ticks=1
```

The collateral fields stay a cash-only figure, which is the documented model and not a description of a margined
future: such a run reports exposure over collateral and should be read for its capture, fee and markout columns.

`--live-pacing[=SPEED]` delivers the replay against the wall clock instead of as fast as the file reads, which is
what turns a measurement into a rehearsal: a strategy that watches its own quote age, or cancels on a timer, only
behaves like itself when the market arrives at its real speed. The fifth positional delay still works and now
means "pace this run at real time", because a fixed sleep after every event neither reproduced the file's timing
nor scaled. Pacing changes no decision — only when the decision is taken — so a paced run reports the same
`[PNL]` fields as the unpaced one, and the line it adds is the feed's own report card:

```text
[LIVE] paced_events=3582893 late_events=3551778 lateness_mean_us=6827 lateness_p50_us=1024
       lateness_p99_us=65536 lateness_max_us=79282 market_window_s=7199 wall_s=11 speed=600
```

`lateness_*` is how late each delivery was against the schedule the file's own timestamps define, so it is the
sum of everything between the schedule and the delivery, and it is the one number here that a live session would
watch: at 600 times real time the process above is at its limit and says so, and at real time the same window is
six hundred times less work than that.

When the benchmark is given an incremental file, the tool reconstructs a compact top-five timeline
from the grouped updates and reports the same output fields. Python `markout_5/20` means the
mid-price difference at the fifth/twentieth subsequent sampled depth record; it is not the same
as the C++ runtime markout measured in quote cycles, and results with different `depth_every`
values are not directly comparable. The reconstructed timeline is a comparison aid; it is not an
order-level queue reconstruction.

For incremental feeds, active L2 quote age advances only when the external best-price midpoint
moves by at least one tick. Depth-only quantity updates do not consume quote lifetime. In four
valid direct 15-minute windows on the 2020-04-01 incremental file, active L2 reached `-172.15` net
PnL versus `-218.82` for `flow_aware_l2`; this is a relative improvement, not a profitability claim.

When the queue ahead is actively being consumed and fair price has not changed, active L2 also
preserves the quote through the normal age limit instead of cancelling and losing queue priority.
On four queue-aware incremental windows, this version measured `-133.55` versus `-148.21` for
`flow_aware_l2`; both remain negative after fees.

Five fields decide whether a run of this mode was any good, and all five are `[EXECUTION]`/`[PNL]` fields whose
definition is in the [Learning Path field reference](../../LEARNING.md#field-reference): `fill_rate`, `cancel_rate`,
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

For the benchmark binary's CSV columns (the `buy_`/`sell_` split, per-quote queue consumption and the fill probabilities),
the column list is `tools/study/benchmark_l2_strategies.py --help`; those are columns of a file, not fields of a line.

A candidate must remain competitive across at least two moderate windows; a one-window PnL win is
not sufficient. Use `analyze_l2_windows.py` for repeated 15-minute windows:

```bash
python3 tools/study/analyze_l2_windows.py \
  --binary build/toy_quant \
  --trades data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz \
  --depth data/v2/crypto/deribit_book_snapshot_25_2020-04-01_BTC-PERPETUAL.csv \
  --symbol BTC-PERPETUAL \
  --start-ts 1585699200000000 \
  --end-ts 1585715400000000 \
  --window-minutes 15 \
  --depth-every 1 \
  --csv reports/l2_windows.csv
```

The Deribit benchmark uses `0.02%` maker and `0.05%` taker fee assumptions. Results are
educational replay measurements, not profitability claims. L2 replay uses a fixed one-market-event
cancellation delay; it is not a millisecond latency model. Queue model defaults to `prorata`, the model the measured cancel share supports;
`prorata` and `optimistic` are sensitivity analyses only, with `prorata` chosen because
`tools/study/calibrate_queue_model.py` measures 99.96% of the size removed from a book level as cancellation
rather than trade. Pass `--queue-model` to `analyze_l2_windows.py` when comparing these assumptions.

The replay also reports `max_abs_exposure_usd` next to `starting_cash_usd`, the cash the run began with.
The model is cash only and no margin, so exposure above that ceiling is a modelling violation, and
`exposure_over_collateral_fills` counts how often a fill pushed the marked position past it. Pass
`--max-position=N` to enforce the ceiling as a pre-trade gate in quantity units; it counts the quantity
already resting on the side an order adds to, so two quotes that arrive in the same cycle cannot jump
it. `strategy_position_mismatches` compares the strategy's own position against the pipeline's on every
report, which is how a dropped callback would show up instead of as an unexplained PnL difference.

The replay has policy levers and model levers, and every one of them is additive to the recorded
defaults, so a run without them reproduces the recorded numbers byte for byte:

| Flag | Effect | Default |
|---|---|---|
| `--order-size=N` | Contracts per quote | Derived from the instrument quantity scale |
| `--inventory-limit=N` | Position at which the strategy stops adding | Derived from the instrument |
| `--max-position=N` | Pre-trade gate the pipeline enforces | Off, reported only |
| `--base-spread-ticks=N` | Quote placement; 1 puts the quote at the venue touch | 2 |
| `--refresh-price-ticks=N` | How far the fair price must move before a requote | 2 for the flow strategies |
| `--arrival-share=X` | Share of the displayed size a new quote starts behind | 1.0, which is FIFO |
| `--queue-chunks=N` | Whole orders per level decrease, for `--queue-model=lumpy` | 1 |
| `--queue-seed=N` | Makes a lumpy run reproducible | Fixed |
| `--order-latency-us=N` | One-way latency between sending an order and the exchange having it | 0, a synchronous engine |
| `--cancel-latency-us=N` | One-way latency between sending a cancel and the exchange applying it | 0, a synchronous engine |
| `--trace-order=N` | Trace the queue in front of order N, reported as `[QUEUE_TRACE]` lines | 0, no trace |
| `--funding-rates=FILE` | Apply an instrument's funding history as the replay passes each settlement | Off, and the summary then has no `[FUNDING]` line |
| `--maker-fee-rate=R` | Replace the venue's maker rate, for the fee question; the run echoes it in its `[Mode]` line and may be negative for a rebate | Off, the venue's own rate |
| `--fee-schedule=KEY=VALUE[,...]` | Price a run under a broker's own schedule instead of the venue's. Keys: `min` (a floor under one order's commission, USD), `regulatory` (a fraction of the proceeds of a sale), `clearing` and `audit` (USD a share on every execution), `rebate` (USD a share of credit for adding liquidity). All five are 0 unless named and the `[Mode]` line reports the five in force | Off: the venue's own schedule, which is what every recorded row uses |
| `--max-exposure-usd=X` | Refuse any order whose projected marked exposure would pass X dollars. It is in dollars rather than in shares because the same share count is three times the exposure on a thousand dollar share, which is the difference between MU and AAPL in the recorded rows. A reducing order is allowed even above the cap, so a position can be worked down. With a second book the cap is the **account's**: what the other book already holds counts against the same X, so two books cannot each carry the full amount | Off: no cap, which is what every recorded row uses |
| `--kill-switch-loss-usd=X` | Stop trading once realised plus unrealised reaches a loss of X dollars, and close what the book is holding. It is a latch: checked once a cycle before anything is submitted, and once tripped it **flattens once at the touch** and then refuses every order that would add risk — a reducing order still passes, because a switch that trapped the position it was meant to protect would be the opposite of a control. The exit is priced in basis points on the same line (`kill_switch_closed_quantity`, `kill_switch_close_bps`). Measured on the 200k window: a limit below the run's own loss closes the single contract it held for **5.39 bps**, and `[EXPOSURE]` then reports `symbols=0` instead of one leg. The account is marked each cycle while this is armed, because an unmarked account carries its loss inside the position and the switch would never see it | Off: the run quotes to the end of its input and carries whatever it is holding into the final mark |
| `--fat-finger-bps=X` | Refuse an order whose price is more than X basis points from the current midpoint. It runs before the rate bucket and before the exposure and position gates, so a bad price spends neither a token nor a reservation | Off: no price band |
| `--max-orders-per-second=X` | Cap submissions with a token bucket at X orders a second. A rejected order consumes no token, a reducing order passes without one, and the bucket runs after the price band and before the exposure gate | Off: no rate cap |
| `--order-burst=N` | The bucket's size in orders; only meaningful with `--max-orders-per-second` | 1 |
| `--participation-limit=X` | Cap one aggressive fill against the external touch at X of the displayed best size. The rest of the order is left to rest (GTC) or to its time in force. The execution price remains the touch, so this is a capacity cap rather than a price-impact model | 1.0, which takes the whole displayed size |
| `--time-in-force=MODE` | Apply one time in force to every strategy order: `gtc`, `ioc` or `fok`. IOC takes the immediately available quantity and cancels the remainder; FOK checks the external touch and the internal book first and either fills completely or cancels without trading. Both end with a `Cancelled` report whose reason is `ioc` or `fok`, and neither leaves a `Resting` order | `gtc`, the historical behaviour |
| `--post-only` | The venue's side of the same question: an order that would trade on arrival is **cancelled** instead, with reason `post_only`, and counted as `post_only_cancels`. It is the difference between paying the maker rate and paying the taker rate for a quote that arrived after the market went through it. Measured: `trend_l2` clips at the touch, so all 55 of its fills on the 200k window were taker fills (taker fees 0.275 USD, maker fees 0); with the flag it makes 1,814 crossing attempts, all cancelled, and trades nothing. `active_l2` is unchanged by it on that window, because none of its quotes arrived crossed | Off: the venue trades a crossing quote and charges the taker rate |
| `--close-out` | Flatten whatever the run is holding when the input ends, at the touch, and print `[WIND-DOWN]` with the half spread and the taker fee the exit cost in basis points. It is the difference between *marking* a position at the last midpoint and *closing* it: every `net_pnl_usd` that carries a position is optimistic by this amount on that position's notional. Measured on the 200k window: **5.39 bps** (5.00 of taker fee + 0.39 of half tick), which is the same floor the crossing measurements found, arriving from the other end of the trade. A close the touch cannot fill says so (`no_liquidity=1` with the position as `residual`) rather than reporting a flat account | Off: the position is carried into the final mark, which is what every recorded number describes |
| `--trend-params=FAST,SLOW,ENTRY_TICKS,EXIT_TICKS,STOP_TICKS[,COOLDOWN_VIEWS[,HOLDING_VIEWS]]` | The non-market-making signal: the two EWMA view counts, the entry, exit and stop bands in ticks, and optionally the post-stop cooldown and the time stop. Only `trend_l2` reads it | The strategy's own defaults: 10, 60, 2, 0.5, 6, 60, 2000 |
| `--inventory-shift-ticks=X` | How far the inventory-aware strategy moves its quote centre at a full inventory, in ticks: the reservation price of Avellaneda and Stoikov in the form this engine can use, where the shift is proportional to the position rather than solved from a risk aversion. The unit matters as much as the number: the shift is in ticks against a quote spacing set by `--base-spread-ticks`, so at a one-tick spacing a shift of six is six times the half spread, which is far more aggressive than the model's optimal and is what the recorded sweep found | Off: the strategy's own 2.0 |
| `--markout-cover` | Quote only where the capture also covers what passive fills at that distance have cost, measured from our own resolved fills | Off |
| `--metrics` | Sample the marked equity once per 60-second bucket and report `[PERFORMANCE]` (span, samples, the annualization factor, Sharpe, Sortino, Calmar, annualized volatility, the maximum drawdown and how long it took to recover), `[ROUND_TRIPS]` (trades, win rate, profit factor, expectancy, the average win and loss, the average holding time, the aggressive and total notional, and turnover) with one `[ROUND_TRIPS_SIDE]` line per side that traded, and `[TAKER]` (arrival slippage in ticks, basis points and dollars, the drift after a crossing fill at 5, 20 and 60 quote cycles, and the implementation shortfall of the IOC/FOK orders) | Off |
| `--markout-horizon=N` | Resolve the fill markouts and the implementation shortfall's opportunity term at N quote cycles instead of 5; the taker drift keeps its own three fixed horizons | 5, the count every recorded run uses |
| `--parent=SIDE:QUANTITY:SECONDS` | Work one parent order instead of quoting: the execution line's benchmark is the midpoint when the order arrives, and `[EXECUTION_ALGO]` reports the implementation shortfall in basis points against that price **and** against the window's time-weighted midpoint, which is the comparison that separates a schedule from the market's drift. Needs `execution_l2`, and the strategy needs it. Works in `l2_replay` and `ws`; in `replay` only `--algo=market` is accepted, because that mode's touch callback carries no clock for a schedule to space its slices on | Off: no parent, no execution run |
| `--algo=market\|twap:N\|pov:X` | The schedule that works the parent: one crossing child, `N` equal children across the horizon, or children sized to `X` of the volume that has traded | `market`, the single crossing the other two are measured against |
| `--algo-entry=cross\|post:SECONDS` | How a slice is sent: `cross` takes the far touch with an IOC and pays the spread and the taker fee; `post` rests at the near touch and crosses whatever the slice has not filled after `SECONDS` | `cross`, which is what the recorded execution runs use |
| `--flatten-band=N` | Reduce a position above N units with a taker order at the touch instead of carrying it | Off |
| `--live-pacing[=SPEED]` | Deliver the replay against the wall clock at this speed and report how late it ran as `[LIVE]`; the fifth positional argument on this mode means the same at real time | Off, which reads the file as fast as it can |
| `--record=PREFIX` | Write the delivered stream to `PREFIX_trades.csv` and `PREFIX_depth.csv` in the replay shapes, reported as a `[REC]` line | Off |
| `--feed-thread` | Read on a second thread and hand each event to the strategy thread over a ring, reported as a `[LATENCY]` line | Off, the feed reads in front of the strategy |
| `--ring-capacity=N` | Slots in that ring, rounded up to a power of two | 4096 |
| `--ws-target=/path` | The HTTP path the live `ws` handshake asks for | `/` |
| `--ws-seconds=N` | Stop a live `ws` session after N wall-clock seconds and drain what arrived, for a capture whose peer never closes the socket | Off: run until the peer closes |
| `--ws-ring-capacity=N` | Slots in the live feed's one input ring; 0 uses 131,072, sized from the measured depth rate | 0 |
| `--idle-wait-us=N` | Microseconds a live reader sleeps on an empty ring; 0 spins, which is the lowest-latency answer | 0 |
| `--paper-broker` | Put a broker between the strategy and the engine that keeps its own statement of what the venue executed, reported as `[BROKER]` and compared with the portfolio as `[RECON]` | Off, the plain engine |
| `--broker-reject-every=N` | Refuse every Nth order at the broker, counted | 0, refuse none |
| `--broker-partial-every=N` | Deliver every Nth of the strategy's fills as two prints, with the fee split the same way | 0, split none |
| `--broker-drop-every=N` | Withhold every Nth fill from the strategy while the venue's statement records it executed | 0, drop none |
| `--broker-outage-after=N` | Take the venue away once N orders have been accepted | 0, never down |
| `--broker-outage-holds=M` | Orders held while it is away, delivered in submission order when it returns | 0, a venue that never comes back |
| `--order-gateway` | Put the client's side of the order path outside the broker: keep a table of orders from submission to answer, absorb a retry under the same client id, and report `[GATEWAY]` | Off, the plain venue |
| `--gateway-ack-timeout-us=N` | Count an order the venue has not answered within N microseconds of market time | 0, count none |
| `--gateway-retry-timeouts` | Re-send a timed-out order under the same client id, which the venue refuses as a duplicate | Off |

An immediate order is terminal even when it does not fill. An IOC that takes part of the displayed size emits
its fills and then a `Cancelled` for the remainder; a FOK that cannot fill the whole quantity emits no trade at
all, just the cancel. The `participation-limit` is applied before either decision, so a FOK can fail because the
cap leaves too little displayed capacity even when the raw BBO size would have been enough. `--time-in-force`
overrides every strategy order, which is useful with the taker line (`trend_l2`) and deliberately hostile to a
passive market maker: an IOC quote that does not cross is cancelled in the same event, so use the default `gtc`
for the quoting strategies.


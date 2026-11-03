# The L2 replay

*Part of the [User Guide](../USER_GUIDE.md).*

The mainline: trades plus incremental depth, every flag that changes it, and what
each one does to the run.

## Replay Trades and BBO

Replay the Binance samples in `data/v2` with:

```bash
./out/build/linux-debug/toy_quant replay \
  data/v2/fixtures/test_aggTrades_5k.csv \
  data/v2/fixtures/test_bookTicker_5k.csv \
  BTCUSDT 0 optimized 1000000
```

The command format is:

```text
toy_quant replay <trades_csv> <bbo_csv> <symbol> [delay_ms] [strategy] [quantity_scale]
```

The replay feed streams and merges both files by transaction timestamp. BBO events replace the
external best bid and ask and trigger strategy decisions. Aggregate trades drive fills; Binance's
`is_buyer_maker=true` means that the seller was the aggressor.

Each event is validated before dispatch. Invalid structural data stops the run. The final `[DATA]`
summary reports `status=ok` or `status=warning`; warning means the replay contained soft alignment
issues such as a missing BBO, stale BBO, or a trade far from the BBO midpoint.

`quantity_scale` converts decimal exchange quantities to the engine's integer units. The default
is `1000000`, so `0.001 BTC` becomes `1000` internal units. Strategy order sizes and generated
runtime quantities use those same units. The replay looks its instrument up by symbol -- `BTCUSDT`, `XRPUSDT` and `DOGEUSDT` are registered -- and each
specification carries that symbol's price tick, its lot step as the quantity unit and the venue's 2/5 bps fee
schedule, so `BTCUSDT` quotes in `0.10`, `XRPUSDT` in `0.0001` and `DOGEUSDT` in `0.00001`. A positional
`quantity_scale` overrides the specification's own, which is what makes the same unit mean the same thing on every
symbol: one unit is `0.001 BTC`, `0.1 XRP` or one DOGE by default. The existing `csv` and `udp` modes retain
their legacy defaults.

Use `l1` when you want the BBO-aware strategy:

```bash
./out/build/linux-debug/toy_quant replay \
  data/v2/fixtures/test_aggTrades_5k.csv \
  data/v2/fixtures/test_bookTicker_5k.csv \
  BTCUSDT 0 l1 1000000
```

### L2 strategies and validation

Use Deribit trades and reconstructed top-25 depth snapshots with the L2 replay mode. The available
strategy names are:

| Name | Role |
|---|---|
| `passive_l2` | Depth-aware reference baseline |
| `inventory_aware_l2` | Inventory-control comparison |
| `flow_aware_l2` | Flow/depth comparison |
| `active_l2` | Comprehensive L2 mainline |
| `adaptive_l2` | Compatibility alias for `active_l2` |
| `trend_l2` | Non-market-making line: an EWMA cross sets a target position, clips at the touch build it, and a stop, a time stop or the exit band closes it |
| `execution_l2` | Execution line: works one parent order from `--parent` with `--algo=market\|twap:N\|pov:X`, takes no view on direction, and reports `[EXECUTION_ALGO]` |

`trend_l2` uses `--inventory-limit` as the size of a full signal and `--order-size` as the clip it is built in. The derived default for both is the instrument's own minimum, so a run that wants to watch a position build should name both, for example `--order-size=10 --inventory-limit=100`. `--trend-params` sets the signal itself; `bash tools/sweep/trend_sweep.sh` replays a grid of them on the 200k slice and writes the fills, PnL, fees and risk pauses to a CSV. A refused order is retried with an exponential backoff (60 views, doubling to 3,840), and a reducing order bypasses the rate bucket, the exposure cap and the position gate. A pre-trade refusal uses `on_order_rejected`; a venue or broker refusal at submission arrives as a terminal `Rejected` report with a `reject_reason`, which clears the open set and backs off the same way.

The pre-trade controls are measured rather than asserted: `bash tools/sweep/risk_check.sh` runs four 200k-row replays, requires the no-flag run to print no `[RISK]` line, requires a permissive setting to leave `[EXECUTION]` exactly as it was, and requires the 1 bp band and a very low rate to refuse orders while every run still reconciles.

Run the standard benchmark with a fixed window and `depth_every=1`:

Use the same benchmark configuration for every candidate strategy:

```bash
python3 tools/study/benchmark_l2_strategies.py \
  --binary build/toy_quant \
  --trades data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz \
  --depth data/v2/crypto/deribit_book_snapshot_25_2020-04-01_BTC-PERPETUAL.csv \
  --symbol BTC-PERPETUAL \
  --start-ts 1585699200000000 \
  --end-ts 1585699500000000 \
  --depth-every 1
```

To replay the connected incremental L2 file, pass it as the depth argument:

```bash
./build/toy_quant l2_replay \
  data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz \
  data/v2/crypto/deribit_incremental_book_L2_2020-04-01_BTC-PERPETUAL.csv \
  BTC-PERPETUAL 0 active_l2 1
```

The reader groups rows by `local_timestamp` and emits one decision per update batch. Snapshot
depth files remain supported through the original full-snapshot reader.

Two optional flags override the sizing that is otherwise derived from the instrument's quantity scale
(`order_size = quantity_scale/1000`, `inventory_limit = quantity_scale/10`). For an instrument whose
quantities are already contract counts that derivation is wrong: `BTC-PERPETUAL` declares
`quantity_scale = 1` and therefore ran with an order size of one contract and an inventory limit of one,
which makes every PnL a few cents and the inventory logic unreachable.

```bash
./build/toy_quant l2_replay \
  data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz \
  data/v2/crypto/deribit_incremental_book_L2_2020-04-01_BTC-PERPETUAL.csv \
  BTC-PERPETUAL 0 active_l2 1 conservative --order-size=10 --inventory-limit=100
```

The remaining `l2_replay` flags are `--no-output`, `--fast-validation`, `--profile-stages[=N]`,
`--live-pacing[=SPEED]`, `--record=PREFIX`, `--feed-thread`, `--ring-capacity=N`, `--paper-broker`, and the
broker's faults `--broker-reject-every=N`, `--broker-partial-every=N`, `--broker-drop-every=N`,
`--broker-outage-after=N` and `--broker-outage-holds=M`.

A second book is three flags, and a replay that names them trades two instruments into one account:

```bash
./build/toy_quant l2_replay \
  data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_200k_base.csv BTC-PERPETUAL \
  0 active_l2 1 prorata --fast-validation \
  --symbol-b=ETH-PERPETUAL \
  --trades-b=data/v2/crypto/deribit_trades_2020-04-01_ETH-PERPETUAL.csv.gz \
  --depth-b=/tmp/eth_depth_200k.csv
```

Each symbol gets its own book, its own decision layer, its own venue and its own pipeline -- the pipeline is
one engine and one strategy by construction -- and the two share one account. The four readers are merged by
timestamp before the feed sees them, so the event order is the venue's clock with a tie going to the first
stream listed. Three things change in what is printed: one `[EXECUTION]` line per book, carrying
`symbol=`; one `[BOOK]` line per book with how many events it was given; and one `[EXPOSURE]` line for the
account, where each leg is valued with its own instrument's face value, so a ten dollar contract and a one
dollar one are not the same exposure. `[PORTFOLIO]` is printed once, and each book prints its own
`[PNL] symbol=..`: the account's line cannot say which leg lost the money, and a book's line can — the split is
taken where the account's totals are written, so the legs add up to the account exactly. The broker, the gateway,
`--metrics`, `--parent`, `--record` and
`--funding-rates` are refused with a second book, each for its own reason, rather than silently applied to
only one of the two.

`--record=PREFIX` taps the stream a run is fed and writes it back out in the replay's own shapes,
`PREFIX_trades.csv` and `PREFIX_depth.csv`, so a paced session or a live feed becomes a file the same
binary replays:

```bash
./build/toy_quant l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz \
  data/v2/crypto/deribit_incremental_book_L2_2020-04-01_BTC-PERPETUAL.csv BTC-PERPETUAL 0 active_l2 1 \
  --fast-validation --record=out/lat/recording
./build/toy_quant l2_replay out/lat/recording_trades.csv out/lat/recording_depth.csv \
  BTC-PERPETUAL 0 active_l2 1 --fast-validation
```

The second command is the first one with a different input, because the recorder's claim is that the
events came back unchanged, and `bash tools/gate/verify_l2.sh fast` asserts exactly that: it records the
200k slice, replays the recording, and requires every reported number to match. Three limits are
worth knowing before pointing it at a new feed. A price and a quantity are written so they read back
as the values that arrived, but the readers' own running ids for a snapshot and a batch are not,
because the replay formats have no column for them. A stream that is both a snapshot and a batch —
the OKX archive, whose first line is a snapshot and the rest are updates — has no single replay file,
so recording it is refused. And so is any event the replay formats cannot express, such as a trade
with no aggressor side, because a file with a hole where an event was reads back as a book that is
quietly wrong.

A session can be written down and taken over by another process. `--snapshot-out=FILE` writes what the run
knows when it ends: the account (cash, realized PnL, fees, both trade counts), every position with its
entry price and the fee still riding on it, the last midpoint each symbol showed, the orders the ledger
still holds, and the clock and next order id.

```bash
./build/toy_quant l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz \
  /tmp/depth_200k_base.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation \
  --snapshot-out=/tmp/session
./build/toy_quant l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz \
  /tmp/depth_200k_base.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation \
  --snapshot-in=/tmp/session
```

The second run prints `[RESUME]` with what it took over and what it found, then replays the file from its
first line — the book has to be the one the snapshot was taken against — and holds its own orders until the
snapshot's clock, so the market is rebuilt and the decisions are not repeated. Two things are worth knowing.
Every order the snapshot listed is **asked about at the venue**: one the venue still holds is cancelled,
because a queue position a new process did not stand in is not one it can keep, and one the venue has
forgotten is counted rather than assumed to be filled or cancelled — the venue's query cannot tell those
apart, and a fill that happened while nobody was listening is what `reconcile_positions` is for. And the
**strategy is not restored**: the new process starts with the account it was given and no memory of its own
quotes, which the first fill after a resume reports as a `strategy_position_mismatches` count instead of
hiding. A snapshot holds one record per book, so a two-symbol run resumes both legs and resolves each one's orders
at its own venue; `--snapshot-in` refuses `--order-gateway`, whose own table is client state a resumed process does
not have.

`--feed-thread` moves the reading to its own thread and hands each event to the strategy thread over a
bounded single-producer ring, which is the shape a live path has: the feed's own work — block refill,
`memchr`, field split, number conversion — then runs beside the strategy's instead of in front of it.
The run reports what the hand-off cost as a `[LATENCY]` line: the ring wait, the processing (the
decision plus whatever orders the engine accepted inside it), and the total from the feed handing an
event over to the strategy finishing with it, each as a mean, a p50, a p99 and a maximum, followed by
how deep the ring got and how often the feed found it full:

```bash
./build/toy_quant l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz \
  data/v2/crypto/deribit_incremental_book_L2_2020-04-01_BTC-PERPETUAL.csv BTC-PERPETUAL 0 active_l2 1 \
  --fast-validation --feed-thread --ring-capacity=8192
```

Read that line with two things in mind. The figures are microseconds, and a mean below one prints as `0`,
which is what the processing column of a window this size does. And an unpaced replay reads faster than
the strategy works, so the ring fills and the wait column is backpressure rather than the hand-off:
`ring_depth_max` one above the ring's capacity says exactly that, `feed_waits` counts how often the feed
had to wait for room, and the paced case is the one that shows what the hand-off costs when the feed is
not running ahead.

A full ring is backpressure rather than a lost event: the feed waits for room, the wait is counted, and
on a paced replay it also shows up in the `[LIVE]` line as a feed that fell behind its own schedule,
which is what having both lines is for. Two combinations are refused rather than approximated:
`--feed-thread` with `--profile-stages`, because the profiler accumulates on whichever thread calls it
and this path calls it from two, and `--feed-thread` on the L1 replay, whose second stream is a BBO
quote. The threaded path reports the same numbers as the direct one — `bash tools/gate/verify_l2.sh fast`
asserts exactly that — because the ring is FIFO and the strategy sees the same sequence either way.

### The broker and the reconciliation

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

### The order gateway

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

### Funding as an input

A perpetual pays funding for as long as a position is held, and that is a flow rather than a price, so it
arrives as a file instead of in the event stream:

```bash
./out/build/linux-debug/toy_quant replay \
  data/v2/fixtures/test_aggTrades_5k.csv data/v2/fixtures/test_bookTicker_5k.csv BTCUSDT 0 l1 1000000 \
  --funding-rates=data/v2/funding/BTCUSDT-fundingRate-2024-03.csv
```

Two free formats are recognised by their header: an OKX daily funding archive
(`instrument_name,funding_rate,funding_time`, every swap on the venue in one file) and a Binance Vision
monthly `fundingRate` archive (`calc_time,funding_interval_hours,last_funding_rate`, one symbol).
`tools/fetch/fetch_funding_rates.py` fetches either for one instrument and `data/README.md` records what the two
files this checkout holds are.

The rate in both is the rate longs pay. The pipeline applies each settlement at the first event at or after
it, on the positions as they stand and at the last price the run has seen, which is the nearest causal mark
available because the settlement instant is not in the event stream. The run then reports:

```text
[FUNDING] funding_points=93 funding_span_hours=736 funding_sum_rate=0.0314019
          funding_annualized_rate=0.373751 funding_settlements=88 funding_flow_usd=0
```

`funding_points`, `funding_span_hours`, `funding_sum_rate` and `funding_annualized_rate` describe the file —
what a long holding one unit of notional paid over its span, and that sum continued over a year — while
`funding_settlements` counts the settlements the window reached and `funding_flow_usd` is what they moved in
cash. The two differ by design: a window that begins after a settlement applies it to an account that holds
nothing yet, so it is counted and worth zero, which is what the line above shows for a five minute replay
that starts inside a month of history. The run above and a ten minute window that does hold a position at a
settlement are both recorded with their commands in
[Performance History](../performance/history-03-studies.md#funding-rates-as-an-input) A file with no settlement for the
instrument is an error rather than an empty input, because a wrong symbol would otherwise look like an
instrument that never paid funding. Without the flag the summary is unchanged byte for byte, which is what
keeps every recorded run reproducible.

To read one order's life instead of the run's totals, name it and filter the summary for the trace block.
Order ids are assigned in submission order and start at 1, so a small id is an early quote:

```bash
./build/toy_quant l2_replay data/trades.csv.gz data/depth.csv BTC-PERPETUAL 0 active_l2 1 prorata \
  --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --trace-order=3 | grep QUEUE_TRACE
```

Each row is one event that changed the queue standing in front of that order, and the last row is the fill,
the cancel or the completion that ended it. The events are `submit` for the strategy's intent, `rest` for
the queue the order then joined behind, `view` and `trade` for the market events that moved that queue, and
`fill`, `fully_filled` or `cancel` for the report that ended the order. `docs/LEARNING.md` reads one of
these traces line by line.

An order id is not something a reader knows in advance, so a traced run also prints `[TRACE_HINTS]`: the id
of the first order that traded, the id of the filled order that waited longest, and the id of the still
resting order that has waited longest. Quoting the first few ids is usually the wrong way to find a good
example, because the earliest orders are the ones that die in the same millisecond they arrive:

```bash
./build/toy_quant l2_replay data/trades.csv.gz data/depth.csv BTC-PERPETUAL 0 active_l2 1 prorata \
  --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --trace-order=1 | grep TRACE_HINTS
```

Of these, the requote threshold moves the fill count the most of anything measured so far, and the
arrival share the least; `docs/PERFORMANCE_HISTORY.md` records the measured ordering.

### Replay strategy choices

The replay command continues to accept these frozen L1 strategy names for compatibility and
historical result reproduction. They are not an active development target; the current strategy
mainline is `active_l2`.

| Name | Use when you want to demonstrate |
|---|---|
| `passive_l1` | A simple passive two-sided L1 baseline |
| `inventory_aware_l1` | Inventory-sensitive prices and quantities |
| `flow_aware_l1` | Recent trade-flow and BBO imbalance response |
| `l1` | The complete replay-oriented strategy and default mainline comparison |

For a side-by-side replay comparison, use the benchmark target. It takes named options, and
`--help` lists them with the defaults they carry:

```bash
./out/build/linux-debug/strategy_benchmark \
  --trades=data/v2/fixtures/test_aggTrades_5k.csv \
  --quotes=data/v2/fixtures/test_bookTicker_5k.csv --symbol=BTCUSDT --quantity-scale=1000000
```

To keep one continuous strategy state while recording hourly checkpoints, pass `--timeline=PATH`:

```bash
./out/build/linux-debug/strategy_benchmark \
  --trades=data/v2/crypto/BTCUSDT-aggTrades-2024-03-30.csv \
  --quotes=data/v2/crypto/BTCUSDT-bookTicker-2024-03-30.csv --symbol=BTCUSDT \
  --quantity-scale=1000000 --timeline=reports/strategy_timeline.csv
```

The timeline keeps the replay state continuous and records cumulative hourly orders, fills,
position, gross/net PnL, fees, filtered trades, markout, and inventory metrics. The existing
window tool resets state per window and remains useful for comparing market phases; it is not a
replacement for this continuous timeline.

### Current demo status

The replay demo retains historical L1 and current L2 roles:

- `l1`: frozen L1 replay reference with inventory, flow, volatility, and fee-aware spread controls;
- `active_l1`: separate higher-activity experiment focused on inventory rotation;
- `flow_aware_l1`: bounded order-flow experiment and comparison baseline;
- `passive_l1` and `inventory_aware_l1`: simple reference baselines;
- `active_l2`: current L2 mainline for depth-aware replay;
- `passive_l2`, `inventory_aware_l2`, and `flow_aware_l2`: L2 comparison baselines;
- `trend_l2`: the non-market-making line, where the signal sets a target position instead of a quote.

Replay trades without a usable BBO, or with a price more than 5 bps from the latest BBO midpoint,
are counted as `filtered` and are not sent to matching. This keeps data-alignment problems from
creating synthetic fills. A high filtered count is still a data-quality warning: it does not mean
the source file has been repaired.

For final comparisons, read `gross_pnl`, `fees`, and `net_pnl` together with `markout`, inventory,
and filtered-trade counts. The result is an educational replay measurement, not a claim of live
profitability.

The benchmark reports submitted orders, filled quantity, final position, gross PnL, fees, net
PnL, and fee ratio. It also records unified execution-quality diagnostics for every replay strategy:
captured edge, adverse selection, inventory, markouts, and quote lifetime. `net_pnl` is the primary
result for cost-aware comparisons;
`gross_pnl` and `fees` explain why it changed.

Every row carries `fills_are_conclusive`: a strategy with fewer than `MINIMUM_FILLS` fills has too
little data for a PnL or per-fill edge statement, and the benchmark prints that warning rather than
leaving the reader to notice the count. On the recorded Deribit window no strategy clears it, which is
the honest reason the project reports a range across queue models instead of one number.

The replay strategies are intentionally different teaching baselines, not production algorithms.
L1 implementations are frozen; `flow_aware_l1` is an experimental transition strategy
whose useful bounded behavior has partly been incorporated into `l1`. `active_l1` is a separate
experiment that produces more observable activity and inventory rotation while accepting more
adverse-selection risk. After a fill it rebuilds quotes around the new inventory on the next BBO
cycle, making the inventory-reversion behavior visible in the benchmark diagnostics.


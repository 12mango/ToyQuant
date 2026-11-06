*Part of the [User Guide](../../USER_GUIDE.md), part 1 of 3. The index is [`../03-l2-replay.md`](../03-l2-replay.md).*

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


# Modes

*Part of the [User Guide](../USER_GUIDE.md).*

The four input modes, what each is for, and the legacy CSV and UDP commands.

## Current Modes

```text
csv         -> legacy Tick scenario
udp         -> legacy Tick stream
replay      -> v2 Binance Trade+BBO MarketEvent replay
l2_replay   -> Deribit L2 trade + depth replay, the mainline path
ws          -> the same L2 delivery path, reading the same replay shapes over a websocket
carry       -> two leg carry measured over two funding histories
```

`l2_replay` is the mainline path and the source of every market-making number the documents quote: it is the only
path that models the queue standing in front of a quote, and a fill depends on that queue rather than on the price
alone. `replay` (Binance Trade+BBO) and the legacy CSV and UDP inputs are retained for compatibility and for
the pinned regression invariants, and their fill counts answer a different question. `carry` measures the funding
line instead and is described at the end of this guide. The detail is in the L1
note in `docs/ARCHITECTURE.md`.

## Run the Binance Trade+BBO replay

```bash
./out/build/linux-debug/toy_quant replay \
  data/v2/fixtures/test_aggTrades_5k.csv \
  data/v2/fixtures/test_bookTicker_5k.csv \
  BTCUSDT 0 optimized 1000000
```

The format is `replay <trades_csv> <bbo_csv> <symbol> [delay_ms] [strategy] [quantity_scale]`. The feed merges both files
by transaction timestamp: BBO events replace the external best bid and ask and trigger decisions, aggregate trades drive
fills, and `is_buyer_maker=true` means the seller was the aggressor. `quantity_scale` converts decimal exchange quantities
into the engine's integer units -- the default `1000000` makes `0.001 BTC` a thousand units -- and a registered symbol
supplies its own tick, lot step and fee schedule.

Use `l1` instead of `optimized` when you want the BBO-aware strategy. A print without a usable BBO, or one more than five
basis points from the latest BBO midpoint, is **counted as filtered and not sent to matching**: a data-alignment problem
must not create synthetic fills, and the count is the warning rather than a repair. The L2 path applies the same
threshold as a *count* instead of a filter, because a book is a better reference than a BBO; both fields are defined in
[the glossary](../GLOSSARY.md).

## Run with Legacy CSV

```bash
./out/build/linux-debug/toy_quant csv data/scenarios/sample_ticks.csv 0 optimized
```

The command format is:

```bash
./out/build/linux-debug/toy_quant csv [tick_file] [delay_ms] [strategy]
```

`delay_ms` is normally `0`; use a positive value to slow down the tick stream. The available strategies are `naive`, `optimized`, and `l1`, with `optimized` as the default.

## Run with Legacy UDP

Start the simulator:

```bash
./out/build/linux-debug/toy_quant udp 9000 optimized
```

In another terminal, send a scenario:

```bash
python3 tools/legacy/udp_sender.py 127.0.0.1 9000 data/scenarios/sample_ticks.csv 0
```

CSV mode is easier for repeatable experiments; UDP mode is useful for observing a streaming feed.
Both modes use the legacy `Tick` contract and are not equivalent to the Binance Trade+BBO replay.


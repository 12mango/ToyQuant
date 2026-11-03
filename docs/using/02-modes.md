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


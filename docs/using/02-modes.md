# Modes

*Part of the [User Guide](../USER_GUIDE.md).*

Two paths over one pipeline -- `l2_replay` offline, `ws` on a socket -- plus the carry line, which is closed. The legacy
L1 modes still run and still hold the regression invariants; they are folded at the bottom of this page.

## Current modes

| mode | what it reads | what it answers |
|---|---|---|
| `l2_replay` | a trade file and a depth file | the mainline: what a quote earns behind a modelled queue |
| `ws` | the same shapes, over a venue socket | the same question, against a live feed |
| `carry` | two funding histories | whether one leg's funding pays for the round trip |

`l2_replay` is the source of every market-making number the documents quote: it is the only path that models the queue
standing in front of a quote, and a fill depends on that queue rather than on the price alone. `ws` is the same delivery
path with a socket where the file was, so a run that works offline is the run that works live. `carry` is a closed line
rather than a third input mode: it measures a position against its hedge and is described at the end of this guide.

/// details | Legacy modes: the L1 paths, kept for the regression invariants

They have no depth, so they cannot compute a queue position -- the thing this project is about -- and the tier 1
invariants are pinned on them at the tag `v1.0-demo`. [The mainline page](../MAINLINE.md#what-to-drop-and-why) records
what they are worth; this fold is only how to run them.

```text
csv         -> legacy Tick scenario
udp         -> legacy Tick stream
replay      -> v2 Binance Trade+BBO MarketEvent replay
```

### The Binance Trade+BBO replay

```bash
./out/build/linux-debug/toy_quant replay \
  data/v2/fixtures/test_aggTrades_5k.csv \
  data/v2/fixtures/test_bookTicker_5k.csv \
  BTCUSDT 0 optimized 1000000
```

The format is `replay <trades_csv> <bbo_csv> <symbol> [delay_ms] [strategy] [quantity_scale]`. Both files are merged by
transaction timestamp: a BBO event replaces the external touch and triggers decisions, an aggregate trade drives a fill,
and `is_buyer_maker=true` means the seller was the aggressor. `quantity_scale` turns decimal sizes into integer units, so
the default `1000000` makes `0.001 BTC` a thousand. A print more than five basis points from the BBO midpoint is counted
as filtered rather than matched, because a data-alignment problem must not create a synthetic fill; the L2 path applies
the same threshold as a count, a book being a better reference than a BBO.

### The legacy CSV and UDP pair

```bash
./out/build/linux-debug/toy_quant csv data/scenarios/sample_ticks.csv 0 optimized
./out/build/linux-debug/toy_quant udp 9000 optimized &
python3 tools/legacy/udp_sender.py 127.0.0.1 9000 data/scenarios/sample_ticks.csv 0
```

CSV is easier for repeatable experiments and UDP for watching a stream; both use the legacy `Tick` contract, and neither
is equivalent to the Binance replay above. `delay_ms` is normally `0`, and the available strategies are `naive`,
`optimized` and `l1`.

///


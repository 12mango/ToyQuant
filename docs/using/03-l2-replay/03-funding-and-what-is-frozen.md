*Part of the [User Guide](../../USER_GUIDE.md), part 3 of 3. The index is [`../03-l2-replay.md`](../03-l2-replay.md).*

## Funding as an input

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
`tools/fetch/fetch_funding_rates.py` fetches either for one instrument and [`DATA.md`](../../DATA.md) records what the two
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
[Performance History](../../performance/history-03-studies.md#funding-rates-as-an-input) A file with no settlement for the
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

## Comparing strategies: the benchmark

Every strategy name, with what each one is and whether it is frozen, is in
[the strategy table](../../L2_FLAGS.md#strategy-names); this section is about running them side by side.

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

## What the demo claims, and what it does not

The replay strategies are intentionally different teaching baselines, not production algorithms.
L1 implementations are frozen; `flow_aware_l1` is an experimental transition strategy
whose useful bounded behavior has partly been incorporated into `l1`. `active_l1` is a separate
experiment that produces more observable activity and inventory rotation while accepting more
adverse-selection risk. After a fill it rebuilds quotes around the new inventory on the next BBO
cycle, making the inventory-reversion behavior visible in the benchmark diagnostics.


*Part of the [User Guide](../../USER_GUIDE.md), part 1 of 3. The index is [`../03-l2-replay.md`](../03-l2-replay.md).*

The mainline: what goes into a replay, what the reader does with it, and what comes out. Every flag is in
[the flag reference](../../L2_FLAGS.md) -- this page runs one.

## The two inputs

A replay is a trades file and a depth file for one symbol, both from the same venue-day:

| input | what it is | which reader takes it |
|---|---|---|
| trades | every print the venue published, with the aggressor side | the trade reader, gzip or plain |
| depth, incremental | the venue's own book updates (insert, change, delete per level) | the incremental reader, which applies them to a book |
| depth, snapshot | the first N levels written out repeatedly | the snapshot reader, which replaces the book each time |

`data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz` is the recorded window every number in this repository
came from, and its depth file is 1.5 GB for the full day -- so a run takes a slice. `bash tools/gate/verify_l2.sh smoke`
cuts the 200k-row slice the gate uses (`/tmp/depth_200k_base.csv`) and asserts the invariants while it is there, which
makes it the cheapest way to get an input.

## One run

```bash
bash tools/gate/verify_l2.sh smoke      # cuts /tmp/depth_200k_base.csv if it is not there
./out/build/linux-debug/toy_quant l2_replay \
    data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz \
    /tmp/depth_200k_base.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation
```

The positionals are: trades file, depth file, symbol, a delay in milliseconds, the strategy name, the quantity scale, and
the queue model. `--fast-validation` skips the per-event checks that files already known to be good do not need, and is
what every recorded run uses.

The run prints four summary lines, and each answers one question:

| line | what it counts | read it with |
|---|---|---|
| `[L2 DATA]` | events, trades, batches, two data-quality counts | [the glossary](../../GLOSSARY.md) |
| `[EXECUTION]` | orders rested, cancelled, filled, and the queue stood behind | [reading the output](../06-reading-the-output.md) |
| `[PNL]` | what the fills were worth after fees | the same page, `breakeven_maker_rate` first |
| `[PORTFOLIO]` | the account | the same page |

## What the reader does with the rows

The incremental reader groups rows by `local_timestamp` and emits **one decision per batch**, not one per row: a
snapshot's burst of updates arrives as one book state, and a strategy that decided on each line would be deciding on a
book that never existed. Snapshot files keep the original reader, which replaces the book per line instead.

Three rules decide what a run *looks* like before any flag is passed:

| rule | what the default is | what a run should do |
|---|---|---|
| batching | one decision per `local_timestamp` batch | nothing; it is the book's own shape |
| sizing | quantity scale/1000, which assumes fractional quantities | pass `--order-size` and `--inventory-limit` |
| queue | `prorata`, the calibrated model | pick the seventh positional for a bound |

> A perpetual's quantity is a contract count, so the derivation gives **one contract** and every PnL is a few cents --
> [sizing](../../L2_FLAGS.md#sizing-and-the-trap-under-it) is the arithmetic. The queue model is the largest single
> choice in a run: `conservative` and `optimistic` are the bounds, `prorata` the calibrated one, `lumpy` the same mean
> with real variance, and what each decides is in
> [the queue section](../../performance/history-03-studies.md#what-the-queue-model-decides-and-what-the-per-order-floor-does).

## Where the rest of that path lives

Optional replay flags -- quoting, risk, recording, the venue gateways and the gates -- are listed in
[the flag reference](../../L2_FLAGS.md). The venue's own behaviour, a broker that keeps a statement and a gateway that
keeps client state, is [part 2](02-the-venue-side.md); funding as an input and one order's life traced are
[part 3](03-funding-and-what-is-frozen.md).

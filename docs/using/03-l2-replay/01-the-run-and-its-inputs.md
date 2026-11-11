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

The run prints four summary lines. `[L2 DATA]` is what arrived -- events, trades, batches, and the two data-quality counts
described in [the glossary](../../GLOSSARY.md); `[EXECUTION]` is what the strategy did with it, orders rested and
cancelled and filled, with the queue it stood behind; `[PNL]` is what those fills were worth after fees;
`[PORTFOLIO]` is the account. [Reading the output](../06-reading-the-output.md) goes through them one field at a time.

## What the reader does with the rows

The incremental reader groups rows by `local_timestamp` and emits **one decision per batch**, not one per row: a
snapshot's burst of updates arrives as one book state, and a strategy that decided on each line would be deciding on a
book that never existed. Snapshot files remain supported through the original full-snapshot reader, which replaces the
book per line instead.

Two defaults are worth knowing before the numbers surprise you, and both are the instrument's, not the flags':

- **sizing is derived from the quantity scale**, which assumes fractional quantities. A perpetual's quantity is a
  contract count, so the derivation gives one contract -- see [sizing](../../L2_FLAGS.md#sizing-and-the-trap-under-it)
  and pass `--order-size` and `--inventory-limit` explicitly.
- **the queue model is a positional argument** (`conservative`, `prorata`, `lumpy`, `optimistic`), and it is the largest
  single choice in a run: `conservative` is a lower bound on fills, `prorata` the calibrated one. What each model decides
  is in [the queue section](../../performance/history-03-studies.md#what-the-queue-model-decides-and-what-the-per-order-floor-does).

## Where the rest of that path lives

The replay's optional behaviour is a reference page rather than a narrative, because it is longer than the instructions
and nobody reads it in order: [the flag reference](../../L2_FLAGS.md) covers quoting and risk flags, the two-book run,
the recorder, the session snapshot, `--feed-thread`, the paper broker and the order gateway, and the gates. The venue's
own behaviour -- a broker that keeps a statement, a gateway that keeps client state -- is [part 2](02-the-venue-side.md);
funding as an input and one order's life traced are [part 3](03-funding-and-what-is-frozen.md).

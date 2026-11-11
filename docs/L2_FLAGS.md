# The L2 replay flags

*Part of the reference tier. The map of the documents is [`MAP.md`](MAP.md). How to run one is
[the guide's part 1](using/03-l2-replay/01-the-run-and-its-inputs.md).*

`l2_replay` takes six positional arguments and then flags:

```text
toy_quant l2_replay <trades_csv[.gz]> <depth_or_incremental_csv> <symbol> [ms_delay] [strategy] [quantity_scale] [queue_model]
```

The binary prints this page's spine itself -- `toy_quant --help` -- grouped the same way, and `lint_prose.py` fails the
build if a flag exists in the parser and in no document, so the two cannot drift apart. What follows adds the part a
one-line usage text cannot carry: which flags change what a run *prints*, and the two or three caveats each has.

## Strategy names

The active L2 line is one class under several signal configs, which is why the names outnumber the classes. The L1 names
are frozen: they exist so recorded results stay reproducible, and new work belongs to the L2 line.

| name | what it is |
|---|---|
| `passive_l2` | depth-aware reference baseline (`l2_baseline` is the same config) |
| `l2_depth`, `l2_micro`, `l2_flow` | the same class with one signal isolated: depth, micro-price, or trade flow |
| `flow_aware_l2` | flow and depth together |
| `inventory_aware_l2` | the same root with the inventory shift in the quote |
| `active_l2` | the mainline: depth, micro-price, flow, inventory limits, toxicity and weak-flow guards, volatility adaptation |
| `adaptive_l2` | compatibility alias for `active_l2` |
| `trend_l2` | the non-market-making line: an EWMA cross sets a target position, clips at the touch build it, and a stop, a time stop or the exit band closes it |
| `execution_l2` | the execution line: works one parent order, takes no view on direction, reports `[EXECUTION_ALGO]` |
| `optimized`, `naive` | the touch-only baselines; neither implements the L2 entry point, so neither sees depth |
| `l1`, `passive_l1`, `inventory_aware_l1`, `flow_aware_l1`, `active_l1` | the frozen L1 line; `l1` is the complete replay-oriented one and the others are its reference baselines |

## Sizing, and the trap under it

`--order-size` and `--inventory-limit` override what the instrument derives: `order_size = quantity_scale/1000` and
`inventory_limit = quantity_scale/10`. That derivation assumes quantities are fractional, and `BTC-PERPETUAL` declares
`quantity_scale = 1` because its quantity is already a contract count -- so without the two flags a perpetual run quotes
one contract and holds at most one, every PnL is a few cents and the inventory logic is unreachable. A run that wants to
watch a position build names both: `--order-size=10 --inventory-limit=100`.


## The flags, by what they change

| purpose | flags | what to know |
|---|---|---|
| which data | the six positional arguments, `--fast-validation` | `--fast-validation` skips the per-event checks a replay of known-good files does not need, and is what every recorded run uses |
| quiet | `--no-output` | suppresses the per-event lines, not the summary |
| quoting | `--base-spread-ticks`, `--refresh-price-ticks`, `--inventory-shift-ticks`, `--edge-cover-ticks`, `--min-spread-ticks` | `0` leaves each at the strategy's own setting; `--base-spread-ticks=1` quotes at the touch |
| risk | `--max-position`, `--max-exposure-usd`, `--kill-switch-loss-usd`, `--fat-finger-bps`, `--max-orders-per-second` with `--order-burst` | `--max-exposure-usd` is enforced once named and reported either way |
| fills | `--participation-limit`, `--time-in-force`, `--post-only`, `--arrival-share`, `--queue-chunks`, `--queue-seed` | the queue model is the seventh positional argument, not a flag |
| latency | `--order-latency-us`, `--cancel-latency-us` | holds the order or the cancel for N microseconds of the market's own clock |
| fees | `--maker-fee-rate`, `--fee-schedule` | a schedule names `min`, `regulatory`, `clearing`, `audit`, `rebate`, `commission`; every term is zero unless named |
| gates | `--markout-cover`, `--flatten-band`, `--markout-horizon` | `--markout-cover` needs `active_l2`; `--markout-horizon` moves the cycle count behind the markout itself |
| output | `--metrics`, `--trace-order`, `--record`, `--close-out` | each adds a line or a file and changes no number |
| session | `--snapshot-out`, `--snapshot-in` | see below: the account comes back, the strategy's own state does not |
| threads | `--feed-thread`, `--ring-capacity`, `--profile-stages` | see below: what it reports is the hand-off, not the strategy |
| paper trading | `--paper-broker` and its five fault flags, `--order-gateway` and its two | see below: they put a component between the strategy and the venue |
| execution | `--parent`, `--algo`, `--algo-entry` | needs the `execution_l2` strategy |
| two books | `--symbol-b`, `--trades-b`, `--depth-b` | see below: one account, two venues, three extra lines |

## The four flags that need more than a line

**Two books.** Each symbol gets its own book, decision layer, venue and pipeline, and they share one account. The four
readers are merged by timestamp with ties going to the stream listed first. Three things change in what is printed: one
`[EXECUTION]` line per book carrying `symbol=`, one `[BOOK]` line per book, and one `[EXPOSURE]` line for the account whose
legs are valued with their own instrument's face value. `[PORTFOLIO]` prints once and each book prints its own
`[PNL] symbol=..`, taken where the account's totals are written so the legs add up exactly. The broker, the gateway,
`--metrics`, `--parent`, `--record` and `--funding-rates` are *refused* with a second book rather than applied to one of
the two.

**The recorder.** `--record=PREFIX` writes `PREFIX_trades.csv` and `PREFIX_depth.csv` in the replay's own shapes, so a
paced session or a live feed becomes a file this binary replays; `verify_l2.sh fast` records the 200k slice, replays the
recording and requires every number to match. Three limits: the readers' own running ids for a snapshot and a batch are
not written (the formats have no column for them), a stream that is both a snapshot and a batch -- the OKX archive -- has
no single replay file and is refused, and so is any event the formats cannot express, such as a trade with no aggressor
side, because a hole in a file reads back as a book that is quietly wrong.

**The session snapshot.** `--snapshot-out` writes the account, every position with its entry price and riding fee, the
last midpoint per symbol, the orders the ledger holds, the clock and the next order id; `--snapshot-in` restores the
account, asks the venue about every order the snapshot listed (a live one is cancelled, a forgotten one is counted), and
replays its file from the first line while holding its own orders until the snapshot's clock. **The strategy is not
restored**: the new process starts with the account and no memory of its own quotes, which the first fill after a resume
reports as `strategy_position_mismatches` instead of hiding. `--snapshot-in` refuses `--order-gateway`, whose table is
client state a resumed process does not have.

**The feed thread.** `--feed-thread` reads on its own thread and hands each event to the strategy thread over a bounded
ring, reporting the hand-off as `[LATENCY]`: the ring wait, the processing, and the total, each as mean, p50, p99 and max,
with the deepest the ring got and how often the feed found it full. Two ways to misread it: the figures are microseconds,
so a mean below one prints as `0`, and an *unpaced* replay reads faster than the strategy works, so the wait column is
backpressure rather than the hand-off -- `ring_depth_max` one above the capacity says so, and the paced run is the one
that measures the hand-off. A full ring is backpressure, never a lost event. `--profile-stages` works on this path with
one block per thread; it is refused with `--feed-thread` on a two-book run, where one profiler would be written by two
pipelines as well as the feed.

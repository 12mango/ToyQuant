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

<!-- The table below is inlined into `using/01-getting-started.md`, so it carries no relative links: a link inside an
     included snippet resolves against the page that includes it, and `using/06-...` became `using/using/06-...` and
     aborted `mkdocs --strict` the first time the fold was parsed as Markdown. Anything that needs a link lives in the
     prose below the table, which stays on this page. -->
<!-- --8<-- [start:flags] -->
| purpose | flags | what to know |
|---|---|---|
| which data | the six positional arguments, `--fast-validation` | `--fast-validation` skips the per-event checks a replay of known-good files does not need, and is what every recorded run uses |
| quiet | `--no-output` | suppresses the per-event lines, not the summary |
| quoting | `--base-spread-ticks`, `--refresh-price-ticks`, `--inventory-shift-ticks`, `--edge-cover-ticks`, `--min-spread-ticks` | `0` leaves each at the strategy's own setting; `--base-spread-ticks=1` quotes at the touch. `--advance-debounce-ms=D` holds back only the side that would move towards the market, for D ms after the fair price last moved, while the side that retreats goes out at once -- measured at 10 ms it turned a -4.61 bps closed median into +12.40 on 6 of 7 days and halved the fills, and at 80 ms it changed nothing. `--close-side-ticks=N` brings the side that *reduces* the position in to N ticks while it is not flat, and at 10 ms with the debounce it read +11.41 on 4 of 7 days against the debounce's +12.40 on 6: it trades capture for turnover and does not pay here. `--extra-layers=L` quotes L further passive layers one tick behind the first, and at 10 ms with one contract it lifted the fills from 17 to 26 a window and the closed median from +12.40 to **+26.18** on 5 of 7 days -- the inventory goes from 3-9 contracts to 4-10, so it is a sized decision rather than a free one. `--jump-window-ms`/`--jump-ticks`/`--jump-cooldown-ms` pull the quotes when the mid crosses `--jump-ticks` inside the window and hold them off for the cooldown; at 10 ms that fires often enough that the fills fall to 0-1 a window, which is why all of these default to off. The first version of this table explained it as "two ticks in 40 ms is normal on a half-basis-point tick", which was 10x low on the tick (5.2 bps here) and is not the mechanism; the measurement is the fills, the cause is not established |
| risk | `--max-position`, `--max-exposure-usd`, `--kill-switch-loss-usd`, `--fat-finger-bps`, `--max-orders-per-second` with `--order-burst` | `--max-exposure-usd` is enforced once named and reported either way |
| account | `--starting-cash` | the cash the account opens with (default 1000), which is also the exposure ceiling; a live run against a funded account passes the venue's balance or its `[RECON]` reports the break |
| fills | `--participation-limit`, `--time-in-force`, `--post-only`, `--arrival-share`, `--queue-chunks`, `--queue-seed` | the queue model is the seventh positional argument, not a flag; the kind of order is what the venue's `ordType` says, so `--post-only` is `post_only` and an IOC is `ioc` rather than a limit that rests |
| latency | `--order-latency-us`, `--cancel-latency-us` | holds the order or the cancel for N microseconds of the market's own clock; without a latency `--post-only` changes nothing in a replay, because the quote and the book are the same event and nothing is marketable on arrival. With a latency a quote that lands in a book that moved is declined, which is the whole of the flag's effect -- and why a replay without one cannot measure it. A cancel naming an order the venue has not received yet is held and applied when the order lands, which is the case a market maker's requote meets on any link where the two legs share the delay |
| fees | `--maker-fee-rate`, `--fee-schedule` | a schedule names `min`, `regulatory`, `clearing`, `audit`, `rebate`, `commission`; every term is zero unless named |
| gates | `--markout-cover`, `--flatten-band`, `--markout-horizon` | `--markout-cover` needs `active_l2`; `--markout-horizon` moves the cycle count behind the markout itself |
| output | `--metrics`, `--trace-order`, `--record` | each adds a line or a file and changes no number |
| the one that does change a number | `--close-out`, `--close-out-cross-bps` | flattens the carried position at the touch and prints `[WIND-DOWN]`, so `[PNL]` becomes realisable instead of a mark; the band puts the closing IOC through the midpoint for a live venue whose book is not the market leg's last snapshot |
| the handover a replay cannot fill | `--settle-residual`, `--settle-taker-bps`, `--settle-slippage-ticks` | prices the leftover position against the far touch plus N ticks with a taker fee and prints `[SETTLE]` **beside** `[PNL]`; needs one book, and off by default leaves every recorded line where it was |
| session | `--snapshot-out`, `--snapshot-in` | see below: the account comes back, the strategy's own state does not |
| order leg | `--record-orders`, `--replay-orders` | see below: one file per venue, the reader that replays it, and the clock it replays on |
| live order leg | `--live-orders`, `--venue-endpoint`, `--venue-max-orders`, `--venue-orders-per-second` | see below: the venue becomes the relay's order channel; the first three are required together and only `l2_replay` and `ws` take them, and the rate is optional beside the session cap |
| signals | `--trend-params`, `--trend-entry` | `--trend-params` is the non-market-making line's EWMA cross and its exits, and `--inventory-limit` doubles as the size of a full signal; `--trend-entry=side=own` prices its clip on our own side of the book so it rests and pays the maker rate, and `max_age=VIEWS` cancels an entry nobody hit |
| threads and pacing | `--feed-thread`, `--ring-capacity`, `--profile-stages`, `--live-pacing` | see below: the first three report the hand-off; pacing replays against the wall clock and reports `[LIVE]` |
| paper trading | `--paper-broker` and its five fault flags, `--order-gateway` and its two | see below: they put a component between the strategy and the venue |
| execution | `--parent`, `--algo`, `--algo-entry` | needs the `execution_l2` strategy |
| two books | `--symbol-b`, `--trades-b`, `--depth-b` | see below: one account, two venues, three extra lines |
<!-- --8<-- [end:flags] -->

`--close-out` is the one flag that changes a number rather than a line: it is in the table above, and what it does to
`[PNL]` is in [what a run's numbers leave out](using/06-reading-the-output.md#what-a-number-in-that-line-leaves-out).
Its companion `--close-out-cross-bps=N` puts the closing IOC N basis points through the midpoint; 0, the default, is the
touch every recorded run uses. A live venue needs the band because its book is not the market leg's last snapshot: a
touch from a second ago can be on the wrong side of the venue's best price, and the IOC then closes nothing
(`no_liquidity=1`). The band has an upper bound the venue enforces: the closing sell must stay above the venue's price
floor, which is 0.5% under its mark. The demo's 2026-11-17 40 bps run crossed that bound -- the market leg's mid 64,250.9
against a mark of 64,368.86 put a 40 bps sell at 63,993, under the 64,047 floor, and the venue refused it; 20 bps sits at
64,122. The fill is still the venue's own best price, so the band is the ceiling `spread_bps` reports, not the price the
exit gets.

`--settle-residual` is the exit a replay cannot print. Once the input ends the touch is a displayed quantity no print
restores, so the closing IOC above finds no counterparty, fills nothing, and the residual is marked again -- measured on
2026-11-25, `--close-out` over an eight-hour window returned numbers identical to the same run without it, to the last
digit of every column. The settlement prices what a session that must hand over would pay instead: the far side of the
touch, `--settle-slippage-ticks` (default 1) further out, and `--settle-taker-bps` (default 5) of taker fee on the
notional crossed, reported as `[SETTLE]` with `settle_cost_usd`, `settle_cost_bps` and the marked result with that cost
taken out (`settled_net_usd`, `settled_net_bps`). It does not book a fill, so `[PNL]` keeps its own definition and the
ledger keeps the position the run actually left -- two numbers, one definition each. A two-book run refuses it, because
the two books would be settled separately and the account's line cannot say which leg would be crossed.

## The eight whose semantics need more than a line

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

> On a live venue the question is `GET /api/v5/trade/orders-pending` through the relay, so the order leg needs
> `--order-rest`; a venue that cannot answer refuses the resume instead of assuming the orders are gone. The live page
> has the rule and the `[RESUME]` line it prints.

**The account's opening balance.** `--starting-cash=USD` sets the portfolio's opening cash, the exposure ceiling
`[EXECUTION]` reports, and the number a live run's venue-equity `[RECON]` compares the venue against -- at the start
and again at the end. The default 1000 is the replay account. A funded demo account is why the flag exists: measured
2026-11-17, its `totalEq` was 80,031.54 but its **USDT equity** was 4,981.77 -- the rest is 1 BTC and 100 OKB in spot
-- so the flag takes the USDT equity, not the total. A flat account opened at `--starting-cash=4981.77` printed
`at=start venue_equity_usd=4981.77 ledger_equity_usd=4981.77 equity_break_usd=0.004472 status=ok`. A venue that holds a
position the ledger did not open is a break, not a tolerance to widen -- a flat account or a `--snapshot-in` is the
clean start. A resumed run takes its opening cash from the snapshot instead, so `--snapshot-in` wins over the flag; the
start line then marks the restored positions at the snapshot's own prices.

**The order leg.** `--record-orders=PREFIX` writes `<prefix>_orders.csv`: one row per submission, per cancel and per
execution report, in the order they happened, with every field a report carries and a price written as the shortest
decimal that parses back to the same double. It is off unless named, which is what every recorded run is, and
`[ORDER-LOG]` prints its path and its three counts so a reader can check the row count against them. A field the format
cannot carry -- a symbol holding a comma -- is refused rather than escaped.

**It was not a silent observer, and the defect behind that is fixed.** Measured 2026-11-25, the same day and the same
eight-hour window of `UP-USDT-SWAP` with the same frozen configuration: **123 fills and net -12.22 bps without the flag,
207 fills and net +2.71 with it**, every queue counter at zero in the second. The cause was a wrapper that forwarded
`process_bbo` and not `process_l2_top`: the interface's default for the L2 entry point answers the BBO one, so every L2
update reached the engine as a BBO update and the queue accounting inside `MatchingEngine::process_l2_top` never ran.
`RecordingEngine` now forwards both, plus the six query methods it used to answer from the interface's defaults, and the
same A/B reads **123 fills on both sides** with every queue field equal. A decorator has to hand-forward every virtual,
and a forgotten one changes behaviour silently -- the same failure as the wrapper that forgot `on_queue_activity`, one
layer down -- so `order_recorder_test` now pins the forwards, the L2 entry point included. `data/runtime/trades.csv`
(which every replay already writes) is still the fill record this repository reads, because it holds the fills rather
than the venue's calls; its line counts match the sweep's across all seven days. The file is read back by `RecordedVenue`, which answers from it as an engine
instead of from a venue; `--replay-orders=PREFIX` is the flag that makes it the run's venue, and each recorded report
is released when the replay's own clock passes the timestamp it carries, so the strategy sees a fill where the session
saw it rather than when it asked. A live venue's frames carry no time, so `LiveVenue` stamps each answer with the run's
market clock; what that buys is a file whose `ts` column is a timeline, not a replayable stream -- a `ws` session
through the live adapter is one realization of a wire whose answers arrive between events, and two runs of the same
command have already differed in the orders they placed ([the live page](using/04-live-and-carry.md) measures it). A
replay whose market data ends first releases the rest at the end and counts them in
`flushed`, which is the honest statement that the two inputs were not the same length. Both flags are `l2_replay` and
`ws` only and both are refused on a two-book run, which has two engines; `--replay-orders` is also refused with
`--live-orders` (a replay and a socket are two venues) and with `--record-orders` (a second file of the same answers).

**What a replay cannot answer, and which sessions the round trip is exact for.** `RecordedVenue` has no book, so the six
queue queries -- `queue_ahead_at`, the two `queue_ahead_consumed`, `queue_ahead_levels_cleared` and
`queue_ahead_from_quantity_changes` -- answer zero, which is exactly what a live venue answers. A **live** session is what
this pair was built for: neither side has a book, so both read zero and the reports come back as recorded. That is the
argument rather than a measurement, because the number the record used to carry (`[PNL] net_pnl_usd=0.0071445` on the
200k slice) came from a **simulated** session -- and a recording of one no longer replays: measured 2026-11-25, it refused
at line 37 of the file (order 5, `0.1635` asked against `0.1643` recorded), because the recording run's strategy read the
engine's queue and the replay's has none. The refusal names the file and the line, which is this format's own rule rather
than a gap. What would lift it is recording the queue queries beside the venue's answers; nothing needs that today,
because fills are measured from `data/runtime/trades.csv` and the order leg exists to make a live session checkable.

**The live order leg.** `--live-orders --venue-endpoint=HOST:PORT --venue-max-orders=N` replaces the in-process matcher
with `LiveVenue` over `TcpVenueTransport`, so the venue is the relay's order channel rather than a simulation; the run
prints `[VENUE]` with the frames sent, the frames received and the reports decoded. The three flags are required
together -- an endpoint without the opt-in reaches nothing, and a live leg with no cap of its own is the case ADR 0005
decision 4 forbids -- and both are refused outside `l2_replay` and `ws`, and on a two-book run. `--venue-orders-per-second=N`
adds a rate beside the session cap: the cap counts the orders a whole run sends, which a maker that replaces its quotes
spends in seconds, so the rest of the session is refused -- 383 quotes and 362 refusals against a twenty-order cap in
one 60 s demo run -- while a rate bounds how fast the risk accumulates without ending the quoting, and a close is exempt
from both. `--venue-max-notional=USD`
adds the cap a reader can price: one order's notional, computed with `fill_notional_usd`, the same helper the ledger
books with, so the number refused is the number the accounting would have recorded. What the venue echoes back is only
an order id, so the adapter stamps the owner and the side from the order it sent: a report with no owner is a fill that
was reported and never booked, and the report's default side is Buy, which would turn every sell fill into a buy. A
fill's fee is the venue's own: the `FILL` frame's fifth field is what the venue charged on that fill, read from its
`fillFee` and negated into the engine's sign, because the venue writes a charge as a negative number. Before that field
existed every live fill arrived with `fee = 0`, and a live run's `[PNL]` was gross of a fee the venue had already taken.
The role is the venue's too when it reports one: OKX's `execType` (`M` rested, `T` took) rides as a sixth field, and a
fill the venue reports no fee for states a zero to keep the position. Without it the adapter's rule books the fill -- a
Gtc order rests, so it is a maker -- which is what every live number before the field was carried was booked under. A
venue that declines an add-liquidity-only quote ends the order as `CANCELED <id> post_only` (the relay remembers the
order asked for it), so `post_only_cancels` counts it; a refusal at placement arrives as a rejected order instead.
`tests/live_venue_test` pins those fields, the optional fee and the notional bound, and a run against the relay's
simulated venue reconciles (`[RECON] status=ok`, no quantity break) -- by hand, not yet in the gate; the same run
against the demo venue's own feed fills and reconciles, and ADR 0005 quotes it.

**The feed thread.** `--feed-thread` reads on its own thread and hands each event to the strategy thread over a bounded
ring, reporting the hand-off as `[LATENCY]`: the ring wait, the processing, and the total, each as mean, p50, p99 and max,
with the deepest the ring got and how often the feed found it full. Two ways to misread it: the figures are microseconds,
so a mean below one prints as `0`, and an *unpaced* replay reads faster than the strategy works, so the wait column is
backpressure rather than the hand-off -- `ring_depth_max` one above the capacity says so, and the paced run is the one
that measures the hand-off. A full ring is backpressure, never a lost event. `--profile-stages` works on this path with
one block per thread; it is refused with `--feed-thread` on a two-book run, where one profiler would be written by two
pipelines as well as the feed.

**The paper broker and its faults.** `--paper-broker` puts a broker between the strategy and the engine -- a layer a live
path has and a replay does not -- and with no fault configured it forwards every call and every report, which the gate
asserts by comparing a brokered run with the same run without it. The faults are counts of orders or fills rather than drawn probabilities, so a faulted run is reproducible
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

**The order gateway.** `--order-gateway` puts the client's side of the order path outside the broker: it keeps a table of
orders from submission to answer and reports `[GATEWAY]`. `--gateway-ack-timeout-us=N` counts an order the venue has not said anything about within
N microseconds of market time, and `--gateway-retry-timeouts` sends it again under the same id. The venue
already refuses a duplicate id, so the second copy is a refusal rather than a fill: on the 200k slice with
each order held for 100 ms and a timeout of 50 ms, the gateway timed out and re-sent all six orders, the
engine refused six duplicates on stderr, and the run's trades, P&L, fees and inventory were exactly those
of the same run with the latency and no retry. A cancel for an order that has already finished is counted
in `cancels_after_fill` instead of being sent to a venue with nothing to cancel. With every knob at zero
the gateway forwards every call and every report, and a run with it reports the numbers of the same run
without it.

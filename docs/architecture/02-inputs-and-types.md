# Inputs and shared types

*Section 2 of the module reference. The index is [`../ARCHITECTURE.md`](../ARCHITECTURE.md).*

Every reader, the event types they produce, and the validation boundary between them:
what a reader refuses, what the validator refuses, and what the book refuses.

## 2. Shared Types and Input Boundary

**Files:** `src/common/market_event.h`, `src/common/tick.h`, `src/lines/legacy/tick_csv_parser.*`, `src/lines/legacy/csv_feed.*`, `src/lines/legacy/udp_feed.*`

The legacy CSV and UDP feeds produce the original `Tick` contract:

```cpp
namespace legacy
{
struct Tick
{
    uint64_t ts = 0;
    std::string symbol;
    double price = 0.0;
    uint64_t size = 0;
    Side side = Side::Unknown;
};

using TickCallback = std::function<void(const Tick&)>;
}
```

The legacy CSV and UDP paths retain this contract. Trades+BBO replay and L2 snapshot input use a
richer boundary:

```cpp
using MarketEvent = std::variant<MarketTrade, BboQuote, MarketDepthSnapshot, IncrementalBookBatch>;
```

`IMarketEventReader` isolates vendor schemas from replay. The Binance readers map aggregate trades
and book ticker rows into these domain events; `ReplayFeed` only performs streaming timestamp
merge. To support another vendor, implement readers for its files and add a factory branch in
`make_market_data_readers` without changing `Pipeline`, the strategy, or matching code.

Every file backed reader obtains its rows from `LineReader` (`src/market/line_reader.*`). It reads
1 MiB blocks, finds line boundaries with `memchr`, and returns a `std::string_view` into the block,
which removes both the per-character scan of `std::getline`/`gzgets` and the per-row `std::string`
allocation. The view is only valid until the next call, so a reader that must hold a row across
`next()` calls copies it into its own buffer. [Performance](../PERFORMANCE.md) records the isolated
line-layer measurement and the end-to-end effect, and [Low-Latency
Design](../performance/design.md#1-the-line-layer-linereader) explains why it is faster.

Each v2 event carries an optional `exchange` identity. Binance L1 readers set it to `binance`,
while the Deribit L2 snapshot reader preserves the source column. `Pipeline` locks to the first
non-empty exchange and rejects later events from another exchange. This prevents Binance `BTCUSDT`
L1 parameters from being silently combined with Deribit `BTC-PERPETUAL` L2 data.

Before dispatch, `MarketDataValidator` rejects structural errors: non-positive or non-finite
prices, zero quantities, unknown trade sides, crossed BBO, timestamp regression, and non-increasing
per-stream sequence numbers. Cross-stream conditions are reported rather than rejected: trades
without a prior BBO, trades using a BBO older than one second, and trades more than 5 basis points
from the latest BBO midpoint. The replay `Pipeline` also filters trades without a usable BBO or beyond
the 5-basis-point deviation threshold before sending them to the strategy and matching engine,
preventing invalid alignment from creating synthetic fills.

`InstrumentSpec` is shared by the market-data adapter, strategy, order book, and matching engine.
Fee rates are carried in the specification and injected into the replay matching engine. The
current Deribit BTC perpetual benchmark assumes a 0.02% maker fee and 0.05% taker fee; these are
simulation assumptions, not account-tier guarantees.
For BTCUSDT it defines a `0.10` price tick and `1000000` integer quantity units per BTC. The replay
strategy therefore uses `0.001 BTC` base orders, a two-tick base spread, and a `0.1 BTC` inventory
limit.

`InstrumentSpec::unit_notional_usd` states what one quantity unit is worth in USD when the traded
price does not say it. Zero means the price *is* the unit value, which is right for an instrument
quoted in quote currency per unit: one BTCUSDT unit is one BTC and is worth `price`. A nonzero value
marks a contract with a fixed USD face value. Deribit perpetuals quote USD per contract while one
contract is worth a fixed 10 USD, so BTC-PERPETUAL sets 10 and a fill of one contract is 10 USD
rather than roughly 6300 USD.

Both the fee model and `Portfolio` read this field, so they cannot disagree about what a fill is
worth:

- `MatchingEngine::report_trade` charges `unit_notional_usd * quantity * rate`, so one
  BTC-PERPETUAL contract pays 0.02% of 10 USD.
- `Portfolio` values cash and positions through `position_value(unit, quantity, entry, price)`. For
  a price based unit this reduces to `quantity * price`, which is the spot case. For a fixed face
  value it is `unit * quantity * price / entry`, because the position holds a fixed USD exposure
  whose base amount was acquired at the entry price. That is the inverse contract relationship, so
  a one contract long gains exactly `10 USD * (price / entry - 1)`.

Before `unit_notional_usd` existed, both the fee and the cash balance were computed as
`price * quantity`. At 2020 BTC prices that overcharged a Deribit contract by roughly 630x, so
every L2 fee and every L2 cash and equity figure was meaningless. BTCUSDT and the legacy `Tick`
paths leave the field at zero and are unaffected.

Execution reports then carry the liquidity role and fee for each MarketMaker fill; the current
offline backtest can consume those recorded fees while retaining a fee-rate fallback for legacy
six-column trade files.

`InstrumentSpec::fee_per_share` is the second half of the same idea for the first instrument where the
fee is not a fraction of the notional. A US equity broker charges cents a share, and a rate cannot say
that: half a cent a share is 0.5 basis points on a hundred dollar share and 5 basis points on a ten
dollar one, so the cost of a fill depends on the share price in a way a fraction of notional cannot
represent. `us_equity_spec` therefore states a one cent tick, one quantity unit per share, and a
commission per share, and it leaves the maker and taker rates at zero, because that is how an equity
fill is billed. It is registered for the three symbols this repository has measured on a US venue --
SPY, and the screen's AAPL and TSLA -- **by name rather than by rule**, because registering a symbol is
a claim that its numbers are re-derivable and three named symbols keep that claim small. `fee_on_fill`
adds both terms rather than choosing between them, so an instrument can
carry a rate, a commission, both or neither, and every schedule that existed before the equity entry
leaves the new field at zero and is arithmetically unchanged.

The two terms differ in what they scale with, which is the reason both are kept: a rate charges for
size in dollars and a commission charges for size in shares, so a rate on a cheap instrument and a
commission on an expensive one are the two ways a venue can make a tight quote unprofitable. On a
hundred dollar share, one tick is a basis point and the tiered commission is a third of it, which is
the first fee in this repository that a tick can cover.

`InstrumentSpec::multiplier` is the third field of the same kind, for an instrument whose unit is
worth a multiple of its price. One micro E-mini S&P 500 contract is five dollars per index point, so
a unit that is one contract is worth `price * 5`, a quarter point tick is 1.25 USD, and a position of
one contract is notional of five times the price rather than the price. Without the field the fee and
the cash balance would both be five times too small, which is the same class of error the fixed face
value was added to fix in the other direction. It defaults to 1 and multiplying by 1 is exact, so no
schedule that predates the futures entry moves.

That combination is the point of the futures entry: a tick worth 1.25 USD against a commission of
about sixty cents a contract is the first market measured here where a one tick quote can pay for
itself before the queue and the adverse selection are counted, which is the question the perpetuals
kept answering with 2.5 ticks of fee against 0.1 ticks of capture.

BBO events update the external top of book and trigger quoting. Trade events carry aggressor side
and drive matching. External Tick/BBO state is stored separately from local strategy orders.
`market_top()` exposes only venue prices, and the coordinator no longer holds a book to ask: a `BboQuote`
is converted where it arrives. Strategy orders are owned only by `MatchingEngine`; there is intentionally
no local or combined view in `OrderBook`, so strategy code cannot accidentally include its own orders in
the market reference.

`L2OrderBook` is a separate multi-level view for `MarketDepthSnapshot` events. It replaces the
snapshot atomically, validates ordering and sequence, and derives an `L2MarketView`. The L2 replay
path passes that view explicitly to the selected strategy after venue and instrument validation;
raw vendor rows never enter strategy code, and Binance L1 data is not combined with Deribit L2.

One instance of `L2OrderBook` holds one instrument, and that follows from what sits beneath it: a
`TickLadder` is a flat array sized around one instrument's price span, so a book per symbol would be a map
of arrays rather than one array. The class enforces the instrument it was built for instead of assuming
it, and refuses an update that names another symbol.

The two entry points are the two shapes a venue publishes, not two styles of the same code.
`apply_snapshot` takes whole sides, requires each of them to arrive in price order, and refuses a crossed
book. `apply_incremental_batch` takes one side at a time, where an amount of zero is the venue's deletion
message, and checks nothing about the two sides together, because an incremental stream's sides arrive in
separate messages and the state between them can legitimately read crossed. Both build their result aside
and swap it in only after every check has passed, so a refused update leaves the previous book live rather
than half applied. Both emit the same `L2MarketView`, and both produce an empty one when the touch cannot
be read: an empty side or a crossed pair gives a view whose every field is zero, and the three callers
that can act on a view each test that field rather than an error.

Two details of the read path are worth knowing before changing it. `price_from_tick` converts a tick back
to a price and then rounds the result onto the tick grid a second time, so a price the feed wrote as
`100.0` is reported as `100.0` rather than as `100.00000000000001`; the reports and the order records
carry those values, so this is what keeps a round trip through ticks equal to the input. And `sequence()`
is the feed's own sequence on the snapshot path and a batch counter on the incremental one, because an
incremental message carries no sequence of its own, so a caller that compares sequences has to know which
path it is on. What the incremental rows do carry is a snapshot flag per row, so a burst of rows belonging
to one snapshot is recognised as a burst and the book is cleared once at its start rather than once per
row.

A replay is delivered as fast as the file reads unless the run asks for pacing. `ReplayPacing` maps the file's
timestamps onto the wall clock, optionally at a speed, and `L2ReplayFeed` reports how late each delivery was
against that schedule: this process being busy, the reader being slow and the operating system not waking the
thread are all inside that number, which is why it is the feed-side figure a live session watches. The clock and
the sleep are injectable, so the schedule is asserted exactly in a test instead of waited for, and one real-time
case proves the default sleep is wired. Pacing changes no decision, only when the decision is taken, so the
`[PNL]` fields of a paced run are the same as the unpaced one and a non-zero fifth positional argument, which was
a fixed sleep per event, now means the same thing at real time.

The incremental L2 path uses `IncrementalBookBatch` events. The reader groups rows sharing a
`local_timestamp`, skips buffered updates before the first snapshot, and preserves snapshot-batch
reset boundaries. Each update sets an absolute level amount; amount zero removes the level. After
the batch is applied, the same `L2MarketView` interface is emitted, so strategies do not depend on
whether the source was a reconstructed snapshot or an incremental feed.

A real incremental message carries one or two level changes, so `IncrementalUpdates` keeps the first
four inside the event object and only moves to a vector when a batch grows past that. On the Deribit
incremental replay that removes one allocation and one free from nearly every batch. `begin()` and
`end()` still expose a single contiguous range, so range-for, `std::any_of` and `operator[]` behave
as they did when the storage was a plain vector. The same reader resolves each CSV column position to
a role once at construction, so the per-row loop switches on a jump table rather than comparing
against eight member indices per field.

`MarketEventRecorder` (`src/market/market_event_recorder.h`) reads the same boundary backwards: it
taps the delivery callback and writes every event into the two replay shapes, so a paced replay or a
live feed becomes a file the same binary reads again. A price is written as the shortest decimal that
parses back to the same double and a quantity as an integer, a batch's rows carry the batch's own two
timestamps, which is the field the reader regroups on, and the interleaving of the two streams is
re-derived from the timestamps by the rule the replay used the first time. What the formats cannot
express is refused rather than approximated: a trade with no aggressor side, a field holding the
separator, a batch whose updates disagree about their local timestamp, a snapshot whose level count
changes, or a stream that carries whole-book snapshots and incremental batches at once. The readers'
own sequence ids for a snapshot and a batch are the one field a recording does not reproduce, because
no column holds them. `bash tools/gate/verify_l2.sh fast` records the 200k slice, replays the recording,
and requires the two runs to report the same numbers.

The live path's threading is implemented against the replay first, because the boundary is the same one
and a file reader is easier to hold still than a socket. `ThreadedReplayFeed`
(`src/market/threaded_l2_replay.*`) runs the feed on a thread of its own and hands each event to the
strategy thread over `concurrency::SpscRing` (`src/concurrency/spsc_ring.h`), a bounded
single-producer/single-consumer ring whose two indices sit on separate cache lines and whose
publication is a release store paired with an acquire load. The consumer runs on the calling thread, so
the strategy, the book and the portfolio keep the thread they always had and only the reader moves.
[Low-Latency Design section 6](../performance/design.md#6-the-hand-off-between-two-threads-spscring) prices the
primitive against the alternatives and
[the performance reference](../PERFORMANCE.md#39-the-hand-off-between-two-threads) carries the table, measured
by `ring_bench` (`src/bench/ring_bench_main.cpp`) rather than asserted: the hand-off is a couple of
nanoseconds of mechanism and the event's own move dominates it.
Three decisions are worth stating because each has a cheaper-looking alternative that this path does
not take. A full ring is backpressure, not a drop: the feed waits for room and counts the wait, because
a replay that lost an event would not be the run anybody asked for, and the pacing line is where the
wait then shows up as a feed that fell behind its schedule. The consumer spins with a pause instruction
rather than sleeping on a condition variable, because at these rates the wake-up costs more than the
wait it replaces; the reported occupancy is what would say otherwise. And the two threads do not share
a `StageProfiler`: one instance accumulates on whichever thread calls it, so each thread has its own and
`--feed-thread` with `--profile-stages` prints two labelled blocks — the feed's `read+parse` and `validate`,
then the consumer's six — rather than a race. The two-book path still refuses that combination with a reason,
because there one profiler would be written by two pipelines as well as the feed.

What the hand-off costs is measured rather than described. Each event is stamped when the feed hands it
over, when the strategy thread takes it, and when the consumer returns, and the three differences are
fed to a `LatencyHistogram` (`src/utils/latency_histogram.h`, the same power-of-two-microsecond
convention the pacing lateness report uses). `run()` prints them as a `[LATENCY]` line: queue wait,
processing, and the total, each with a mean, a p50, a p99 and a maximum, plus the deepest the ring got
and how often the feed waited for room. The processing figure is where the decision and the orders the
synchronous engine accepted inside it both land, which is why there is no separate order leg here: the
venue's leg is what `--order-latency-us` models. The threaded path reports the same run as the direct
one, and tier 1 of `tools/gate/verify_l2.sh` asserts that by comparing every number except the `[LATENCY]`
line itself.

The broker layer is a decorator over that engine rather than a second execution model. `PaperBroker`
(`src/lines/paper_broker.*`) implements `IMatchingEngine`, takes over the venue's report callback and
forwards every call and every report, so the pipeline, the strategy, the book and the portfolio are
unchanged and a broker with no faults configured produces the run without it. What it adds is what a venue
does not do: refusals for reasons a strategy cannot see in the price, each counted because a refusal emits
no report — the same reason `IMatchingEngine::rejected_orders()` exists at all; a fill delivered as two
prints with the fee split the same way, which is fee- and position-neutral by construction so only the
report count moves; a report withheld on the way out while the venue's statement still records the
execution; and an outage that holds orders and delivers them in submission order when the venue returns,
answering a cancel for an order still held because that order is gone for good. Each fault is a count of
orders or fills rather than a drawn probability, so a faulted run is reproducible from its command line.
The broker also keeps a statement of its own, symbol by symbol, and `reconcile_positions`
(`src/lines/reconciliation.h`) compares it against the portfolio's positions, gross traded quantity,
fill count and fees, which is the check the strategy already runs against the engine extended one layer
out. Cash is deliberately outside that comparison: the broker's USD arithmetic would be a second
implementation of the instrument model, and a copy that agrees because it was copied is not a check.

The other half of that seam is the client's. `OrderGateway` (`src/lines/order_gateway.*`) is a second
decorator over the same interface, wrapped outside the broker, and it holds the state the engine does not
have because the engine's `send_order` is synchronous: the order sits in the gateway's table from the moment
it is submitted until the venue answers. That is where an idempotent submit lives. The client order id is
the order's id, a second submission under an id already in the table is absorbed rather than forwarded, and
a retry after a timeout goes out under the same id, so the venue's duplicate check — the same one that
refuses a repeated id in `send_order_now` — is what stops a second position rather than the client's
optimism. The timeout is measured on the market's clock, the only clock a replay and a live session share,
and a cancel for an order that has already finished is counted rather than sent. The gateway reports its
counts as a `[GATEWAY]` line; with every knob at zero it forwards every call and every report, which is what
makes a run with it comparable to one without.

The venue leg is the same seam with the wire injected. `LiveVenue` (`src/lines/live_venue.*`) is an
`IMatchingEngine` whose transport is an `IVenueTransport` -- `TcpVenueTransport` (`tcp_venue_transport.*`) is the
socket -- so a run that names `--live-orders` hands its orders to a process on the other side instead of to the
matcher, and the reports that come back are the `ExecutionReport`s the ledger already reads
([ADR 0005](../adr/0005-a-live-venue-is-another-matching-engine.md)). The adapter refuses an order past its own
count and size bounds before anything is written, counts a failed write as a failure rather than as a sent order,
and stamps every order with an `owner` so a report can be matched to the order that caused it: the venue answers
asynchronously, so nothing about the loop's ordering changes. `RecordedVenue` (`recorded_venue.*`) answers the same
interface from a recorded order log rather than from a venue, which is what makes a recorded session checkable
instead of remembered; no app mode drives it yet.

Levels are held in `TickLadder`, a flat array indexed by price tick. A `std::map` access descends
about ten pointer levels, and a sorted vector moves several kilobytes per insertion once a book holds
a thousand levels; Deribit BTC-PERPETUAL keeps more than a thousand live levels per side across a
span of about twenty thousand ticks, so both structures pay for that shape, and indexing by tick
turns a level change into one computed address. The range is learned from the levels themselves and
the array grows when an update arrives outside it, so no price window is configured and no update is
dropped. Iteration starts from a remembered best slot that is repointed when the touch is emptied,
which keeps a top-of-book walk inside the few populated slots near the touch. `L2OrderBook` keeps the
same interface, so `Pipeline`, the strategies and the replay tests are unchanged.

The validator enforces monotonic exchange and local timestamps and counts snapshot/reset batches
as reconnect diagnostics. The current normalized CSV schema does not expose Deribit
`prev_change_id`, so a true exchange sequence-gap check is intentionally not claimed; a future
adapter can add that field without changing the strategy interface.

The matching engine also retains the latest BBO liquidity for aggressive strategy orders. A buy
limit at or above the best ask executes at the ask; a sell limit at or below the best bid executes
at the bid. Execution is capped by the displayed BBO quantity, which is consumed across subsequent
orders until the next BBO update. Any unfilled remainder enters the existing local limit-order book.

`CsvFeed::run` reads rows, converts them into `Tick`, and invokes `cb_(t)`. The parser lives under
`src/lines/legacy`, and `legacy::TickPipeline` is the explicit compatibility boundary before the old
Tick processing path. Malformed rows are reported and skipped.

`UdpFeed` has a different transport but the same legacy output contract. On Linux, `recvmmsg` receives
datagrams in batches. The parser uses `std::string_view` and `find`, then publishes parsed ticks
through a fixed-size ring buffer:

```cpp
size_t h = head_.load(std::memory_order_relaxed);
size_t next = (h + 1) % RING_SIZE;

if (next != tail_.load(std::memory_order_acquire))
{
    ring_[h] = t;
    head_.store(next, std::memory_order_release);
}
```

The producer publishes the tick before release-storing `head_`; the consumer acquire-loads the
index before reading the slot. This is a single-producer/single-consumer atomic queue. A full ring
drops the incoming tick by design.


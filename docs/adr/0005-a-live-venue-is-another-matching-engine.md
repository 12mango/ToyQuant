# 0005. A live venue is another matching engine, and the demo stops at the sandbox

- **Status**: accepted
- **Date**: 2026-11-11
- **Supersedes**: none

## Context

The project can already take live market data: the `ws` mode reads a venue's stream through the same
`IMarketEventReader` the file readers implement, and a two-hour soak against a public channel is recorded. What it cannot
do is send an order anywhere. Orders go to a matching engine inside the process, so the chain ends at the desk.

Two components already stand in for a real venue, and both are written against the same interface the in-process engine
implements: `PaperBroker` (a broker with its own statement and a reconciliation) and `OrderGateway` (the client side of
the order path, with timeouts and duplicate-id absorption). In other words the shape "a venue is a plugin" is not a
proposal, it is already built twice. See `src/lines/paper_broker.h`, `src/lines/order_gateway.h` and
`src/exchange/matching_engine.h`.

So the question is not how to architect a live path. It is how much of one to build for a demonstration that claims
nothing about profit, and what stops it from becoming a program that spends money by accident.

## Decision

1. **A live venue is a third implementation of `IMatchingEngine`**, in `src/lines/` beside the two that exist, because it
   is the only layer allowed to know a venue's message shapes. The core -- book, strategy, pipeline, ledger -- does not
   change. If that turns out to be impossible, the interface is what is wrong and gets a record of its own, not a
   workaround in an adapter.
2. **The demonstration target is the venue's sandbox**, not its production endpoint: the same calls, the same order
   lifecycle, no money. Production becomes a configuration change rather than a second project.
3. **It is off unless asked for twice.** A second opt-in flag (proposed name `live-orders`) on top of choosing a live
   venue; without it the venue is read-only, which is the `ws` mode of today. Credentials come from the environment and
   are never printed, because a run's command line reaches `[Mode]` and every recording.
4. **The bounds are enforced where the damage would happen.** The pre-trade pipeline already refuses on notional, rate,
   fat-finger and a latched loss; the venue adapter enforces a demo cap of its own as well, because those controls protect
   against a strategy's behaviour and not against a bug in the adapter.
5. **The order leg is recorded, not just the market data.** A live session that is not replayable cannot be checked, and
   every other claim in this repository is checked by re-running something. A second recorder (proposed name
   `record-orders`) writes what went out and what came back, and a reader feeds those reports back as the engine.
6. **Acceptance is the three checks the project already trusts**, applied to the live leg: the session's recording
   replays to the same `[PNL]` and `[EXECUTION]`; `[RECON]` is `status=ok` against the venue's own position; the pre-trade
   invariants still fire under `risk_check.sh`. The round trip the venue adds is reported with its own label, because the
   `[LATENCY]` fields describe the modelled legs and mixing the two would produce a number that means neither.

## Consequences

- **Step one has landed.** `src/lines/live_venue.{h,cpp}` takes an `IVenueTransport` and turns an order into one frame of
  its own protocol; `tests/live_venue_test.cpp` pins the frame, the two bounds, a wire that refuses, and the four replies
  a venue can send mapped onto `ExecutionReport`. No socket existed at that point, which was the point: the shape of the
  exchange is proved before anything can reach production by accident.
- **Step two has landed, and it is loopback.** `src/lines/tcp_venue_transport.{h,cpp}` is the first `IVenueTransport` with
  a socket behind it: frames out one per line, replies in split on newlines, a closed connection reported instead of
  written into. `tests/tcp_venue_transport_test.cpp` runs the adapter against a listener the test opens on an ephemeral
  loopback port, so the round trip is proved with no endpoint, no credential and no market. **The order channel on the
  relay has landed with it.** `tools/live/venue_relay.py` accepts the adapter's lines, translates them into the venue's
  order messages, and translates the venue's answers back into the four replies the adapter decodes; `--simulate-orders`
  answers as a sandbox venue, deterministically, with no key, no endpoint and no money. What `tests/relay_order_channel_test`
  adds is the half neither of the other two could prove: the adapter, the transport and that relay process, running
  together on loopback, so the protocol is shown to cross two processes and one translation rather than being pinned
  twice in isolation. What it was not, at that point, is a route to a real venue: a private channel needs a key and a
  login handshake. That route has since landed in the same tool -- login, the `orders` subscription, and a reconnect
  that logs in and subscribes again -- so `--order-listen` now serves either backend and is refused only when it has
  neither a simulation nor the three credentials, naming the variable that is missing. The amendment below says why the
  acceptance criterion moved with it.
- **Step three has landed as the recorder and its reader, not yet as a run.** `src/lines/order_recorder.{h,cpp}` writes
  `<prefix>_orders.csv` -- one row per submission, per cancel and per execution report, every field a report carries and
  the shortest decimal for a price -- and `src/lines/recorded_venue.{h,cpp}` is the reader: `RecordingEngine` decorates a
  venue and records what it was asked and what it answered, and `RecordedVenue` is an `IMatchingEngine` that answers from
  the file instead, emitting the recorded reports in order. `tests/order_recorder_test.cpp` pins the header and two rows
  literally, the round trip (the replayed reports are the recorded ones field for field, a rebate's sign included), the
  `rows_left` count that says a replay stopped short, and the refusals that make a recording a record rather than a
  summary. `--record-orders=PREFIX` is the flag; it is refused outside `l2_replay` and `ws` and refused on a two-book run,
  because a recording is one file per venue. What is not wired is a run that takes the recorded reports back: the reader
  exists and is tested, and pointing an app mode at it needs a flag and a claim about what a replayed order leg is
  measured against, which is the step after the sandbox.
- **Step four has landed: the adapter is the run's venue.** `--live-orders --venue-endpoint=HOST:PORT
  --venue-max-orders=N` puts `LiveVenue` over `TcpVenueTransport` in place of the in-process matcher, and the broker,
  the order recorder and the gateway stay around it unchanged -- the decorator stack is the one every recorded run
  uses, which is what decision 1 bought. The three flags are required together and refused outside `l2_replay` and
  `ws`, and the live leg's replies are applied on the consuming thread: the transport's receive thread only queues a
  line, so a fill that arrived between two events is seen before the next decision and no accounting state is touched
  from two threads. The first run against the relay's simulated venue reported every fill and booked none of them --
  the venue's frames carry an order id and nothing else, so the reports had no owner, the report's default side
  (`Buy`, which would turn a sell into a buy) and no liquidity role. The adapter now stamps all three from the order it
  sent, `tests/live_venue_test` pins them, and the same run reconciles: `[RECON] status=ok`,
  `report_count_difference=0`, `[VENUE] orders=4 cancels=4 reports=12` against the relay's own count.
- **The first sandbox order left under the wrong name, and the venue's answer was dropped.** Two errors, both the
  relay's. The order carried `instId: "BTC-PERPETUAL"` -- the replay's Deribit instrument -- to a venue whose name for
  the thing is `BTC-USDT-SWAP`; `OrderTranslator.encode` now sends the relay's `--instrument` and keeps the engine's
  symbol for its own bookkeeping, because the mapping belongs in the layer that knows a venue's shapes. And the venue
  *did* answer -- `{"op":"order","code":"50014","msg":"Parameter id can not be empty.","data":[]}` -- but the client
  dropped it twice over: the request carried no `id` for the venue to correlate, and `handle` returned early on an empty
  `data` list before the decoder could see a refusal. So the engine was told nothing and the run looked like a venue that
  ignored an order. Both are fixed (the request carries its order id, and a top-level non-zero `code` becomes a
  `REJECT`), `--order-dump=FILE` now records every frame both ways, and `--self-test` pins the redaction (no key, no
  signature) and the four refusal shapes.
- **What the sandbox run now says, and where it stops.** With the id, the venue correlates its answer and names its own
  complaint: `sCode 50014, "Parameter instIdCode can not be empty"`. That field is the venue's internal code for the
  contract -- 10459 for `BTC-USDT-SWAP`, read from its public instruments endpoint -- and `--order-inst-code` passes it
  through rather than deriving it. With it the complaint becomes `50026 "System error. Try again later."`, three times, a
  second apart, as a number and as a string, which rules out type errors and transients; the host's clock is within
  120 ms of the venue's, which rules out the timestamp. The same order over REST -- the venue's canonical order surface,
  which needed ISO-8601 milliseconds where the websocket login needs epoch seconds (`50112` against `60004`) -- answers
  `sCode 51010, "You can't complete this request under your current account mode."`, and the account reports
  `acctLv=1`: spot-only. A perpetual swap is not a spot instrument, so `50026` was that refusal collapsed into a system
  error by the websocket handler. The round trip is proven (`[VENUE] frames_sent=2 frames_received=2 reports=2`, both
  rejections booked); a fill is blocked on a venue-side account setting, and the fix is to switch the demo account to
  single-currency margin (`acctLv` 2 or up) rather than a change here. The size stays the other open item: `sz` is in the
  replay instrument's units, not the venue's.
- **The sandbox run happened, and it decided where placement lives.** The account was `acctLv=1` (spot-only), which is why
  the websocket trade operation answered `50026` and REST answered `51010 "You can't complete this request under your
  current account mode"`. With the account switched to margin (`acctLv=2`) REST places the order -- `code 0, sCode 0,
  "Order placed"`, an order id, and a cancel that answers `code 0` -- while the websocket trade operation still answers
  `50026`. So the demo venue's placement surface is REST and its websocket is the `orders` channel, and this record's
  decision 1 is why that costs one class: `RestPlacer` sends the translator's own `args[0]` as the REST body, the same
  object the websocket op would have carried. The acceptance run against the demo venue measures
  `[VENUE] frames_sent=2 frames_received=2 orders=1 cancels=1 reports=2 dropped=0`, `[ORDER-LOG] submissions=1
  cancels=1 reports=2`, `[RECON] status=ok`, and on the relay's side `placed=1 venue_refusals=0 transport_failures=0`,
  with the venue naming the order it made (`ordId 4096362816047378433`).
- **What a fill would and would not prove.** When this was written the `orders` channel was not subscribed on the REST
  route, so a fill would not have been seen there; the relay now opens the private socket for that subscription beside
  its placer -- placement is REST on this venue and state is a subscription, so the two surfaces are used at once --
  and `--order-no-subscribe` is the placement-only route it used to have. The de-duplication that needed is the
  channel's cumulative `accFillSz` counter, booked once per order. `sz` reaches the venue in the venue's units
  (`--contract-size`, `--quantity-scale`), but the run prices a 2020 replay against the 2026 book, so no order crosses
  and no fill exists to see. That is why this is an acceptance of the wire rather than of a strategy.
- **The recording needed the clock its answers were heard on.** A live venue's frames carry no timestamp, so every
  report `LiveVenue` decoded had `ts = 0` and `--record-orders` wrote that: a session recorded through the adapter
  replayed with every answer due at the first event, and stopped at the third submission -- three replays, the same
  order, one tick apart. The adapter now stamps each report with the run's market clock, one microsecond before the
  event the answer is applied at, because `RecordedVenue` releases strictly past a timestamp and an answer that arrived
  between two events has to reach the strategy at the event it reached it live. What the stamp cannot fix was measured
  rather than argued: the same live command, run twice on one market recording, placed 154 and 261 orders and wrote two
  different order legs, while the two market legs were byte-identical, because *which* answers have been drained when an
  event is handled is the receive thread's timing. So a live recording is an audit trail, not a replayable stream, and
  the loop that reproduces is the matcher's -- one reader, one row order, `[ORDER-REPLAY] submissions=305 cancels=290
  reports=655 rows_left=0`, ending on the same `[PNL]`. A live run still measures the wire and not a strategy, which is
  what this record said before the stamp existed.
- **The acceptance run happened, on the venue's own book.** A ninety-second `ws` session against the demo venue's public
  `BTC-USDT-SWAP` channel, placing through `LiveVenue` over REST and hearing the private `orders` channel, filled and
  reconciled: `[WS] frames=197067 trades=23743 depth_updates=173322 ring_waits=0`, `[VENUE] orders=18 cancels=13
  reports=83 dropped=0`, `[EXECUTION] submitted_orders=18 trade_reports=42 fill_rate=0.523889`, `[BROKER] fills_seen=42`,
  `[RECON] status=ok` with `quantity_breaks=0 report_count_difference=0 fee_difference=0.000000`, and `[PNL]
  gross_pnl_usd=-1.33058 fees_paid=2.19004 net_pnl_usd=-3.52062` -- a decomposition that adds up, with a sign that does
  not flatter the line. The relay printed `placed=18 venue_refusals=3` and, for the state channel, `logins=1
  subscribes=1 reconnects=0`. Two defects the run found, both fixed here: a contract fills **in parts** and the venue
  reports them as decimals (`accFillSz: "0.45"`), where the channel parsed an integer and dropped the private connection
  on the first such push; and a live fill carried **no fee**, because the adapter's `FILL` frame had no field for one,
  so `fees_paid` was zero while the venue had charged `fee: "-0.14999409"` on the same fill.
- **What that run does not settle.** `[PORTFOLIO] equity` at the end equals cash (`1950.98`), because the position was
  carried into a moment with no mark -- the feed had closed -- so the mark is zero and the equity figure is cash plus
  nothing; `[PNL]` is the figure that decomposes, and it is the one quoted. The account also ran without
  `--max-exposure-usd`, and three fills took it over collateral (`exposure_over_collateral_fills=3`), which is a setting
  rather than a finding. What the run measures is the wire with real fills in it, on a demo venue's book, one contract
  an order, for ninety seconds -- not an edge.
- **What it costs**: one adapter, one wire, one relay order channel, one recorder with its reader, and the flag
  plumbing -- `src/lines/live_venue.{h,cpp}`, `src/lines/tcp_venue_transport.{h,cpp}`, the order channel in
  `tools/live/venue_relay.py`, `src/lines/order_recorder.{h,cpp}`, `src/lines/recorded_venue.{h,cpp}` and a
  `--record-orders` flag, plus the stamp above (two lines in the adapter and its wiring in the application). No core file
  moved and no recorded number changed, because every part of it is off unless asked for.
- **What it forbids**: claiming anything about edge from a live run. A run that fills an order proves the wiring; the
  profit question was closed by eight measurements and a sandbox order does not reopen it.
- **The smell to watch**: `process_bbo` and `process_market_trade` are no-ops for a live venue, because the venue matches
  its own orders. That is fine for one venue adapter and is evidence that the interface mixes "be a market" with "be a
  venue"; the second live venue is the point at which the interface gets split.
- **Not in scope**: FIX, order routing, order state that survives a process restart (already decided against, because it
  would only prove the simulator is self-consistent), and any placement logic beyond what the strategies already do.
- **To revisit**: a real order on a production endpoint, which is a decision about capital and compliance rather than
  about code, and belongs to the owner.

## Amendment (2026-11-13)

Decision 6 was written before the order leg existed. The recorder and its reader have landed since, and read against
them one half of that decision is not reachable as written: "the session's recording replays to the same `[PNL]` and
`[EXECUTION]`". This amendment records why, and what the criterion is instead, so that the acceptance run is measured
against something a run can meet.

**Why the literal criterion fails.** `RecordedVenue::send_order` is a pull: it emits the recorded reports for that
order at the moment the strategy submits it. In the session that produced the recording the fill arrived when the
venue sent it, after the strategy had already taken its next decision. A pull replay therefore hands the strategy its
fill *earlier* than the session did; the next quote is computed from a position the session did not have yet, the
order sequence diverges, and `RecordedVenue` refuses the recording by design, naming the file and the line it could not
answer. The refusal is the reader behaving correctly. The criterion was the part written against an implementation
that did not exist yet.

**The two readings considered.**

- **(A) Replay on the recorded clock -- chosen.** Every row the recorder writes already carries `ts`. A replaying
  venue holds each report until the run's clock passes that report's `ts`, and delivers it then, so the strategy sees
  the fill where it saw it and the two runs can be compared verbatim. The cost is a scheduler in the reader and a
  stricter requirement on the inputs: the market recording (`--record`) and the order leg (`--record-orders`) have to
  cover the same window, because a report outside the market window has no clock to be delivered against. Both
  recordings are things a session already writes, so the cost is in the reader and in the comparison, not in a new
  measurement.
- **(B) Replay the ledger only -- not chosen.** Feed the recorded reports straight to the ledger and compare cash, fees
  and position rather than `[PNL]` and `[EXECUTION]`. That is real evidence and it is cheaper, but it takes the
  strategy out of the comparison, and the strategy's decisions are the half of the session the recording exists to
  reproduce. It would also be a narrowing of decision 6 rather than a reading of it, which is why it is recorded here
  instead of being done quietly.

**What follows for the acceptance run.** Decision 6 stands, read as (A): the same `[PNL]`, the same `[EXECUTION]`,
`[RECON] status=ok`, and the pre-trade invariants still firing under `risk_check.sh` -- with two recordings taken in
the same session and compared, not one recording replayed alone.

**The endpoint is the venue's demo environment, not its production one.** The market-data half of this repository
already speaks this venue -- the relay decodes its `books` and `trades` channels, `src/market/okx_readers.cpp` reads
its archive, and the contract size and quantity scale of `BTC-USDT-SWAP` are in the tree -- so the order leg uses the
same venue rather than adding a second protocol to the same demonstration. **A probe on 2026-11-13 reached it**, which
is the confirmation this record asked a first sandbox run for: the private socket accepts the login and the `orders`
subscription on `wss://wspap.okx.com:443/ws/v5/private?brokerId=9999`; the venue's own `notice` on port 8443 says that
port is being retired; and the login timestamp is Unix **epoch seconds**, because the ISO form is refused with `60004
Invalid timestamp` and epoch milliseconds with `60006 Timestamp request expired`. What the probe did not do is send an
order; the wiring that can (the three flags in the consequences above) landed the same day and was verified against
the relay's simulated venue, so the sandbox order is the next *run* rather than the next change.

**The venue round trip is not measured, and it does not join `[LATENCY]`.** The last sentence of decision 6 stands:
the `[LATENCY]` fields describe modelled legs, and a real round trip belongs under its own `[VENUE]` label if it is
measured at all. The order recording carries no request-to-report time, so a run of this line reports no venue latency
and claims none.

## Evidence

- [`IMatchingEngine`](https://github.com/12mango/ToyQuant/blob/main/src/exchange/matching_engine.h) -- the interface a
  venue already implements twice.
- [`PaperBroker`](https://github.com/12mango/ToyQuant/blob/main/src/lines/paper_broker.h) and
  [`OrderGateway`](https://github.com/12mango/ToyQuant/blob/main/src/lines/order_gateway.h) -- the two stand-ins, with the
  statement and the reconciliation this record reuses.
- [`WebSocketFeed`](https://github.com/12mango/ToyQuant/blob/main/src/market/ws_feed.h) -- the live half that already
  works, and [`soak_push.sh`](https://github.com/12mango/ToyQuant/blob/main/tools/live/soak_push.sh) -- the run that proves
  it.
- [The gate](../performance/history-03-studies.md) -- the record-and-replay comparison the order leg is measured by.

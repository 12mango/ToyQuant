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
  twice in isolation. What it is not is a route to a real venue: a private channel needs a key and a login handshake, and
  `--order-listen` without `--simulate-orders` is refused rather than accepted and ignored.
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
- **What it costs**: one adapter, one wire, one relay order channel, one recorder with its reader, and the flag
  plumbing -- `src/lines/live_venue.{h,cpp}`, `src/lines/tcp_venue_transport.{h,cpp}`, the order channel in
  `tools/live/venue_relay.py`, `src/lines/order_recorder.{h,cpp}`, `src/lines/recorded_venue.{h,cpp}` and a
  `--record-orders` flag. No core file moved and no recorded number changed, because every part of it is off unless
  asked for.
- **What it forbids**: claiming anything about edge from a live run. A run that fills an order proves the wiring; the
  profit question was closed by eight measurements and a sandbox order does not reopen it.
- **The smell to watch**: `process_bbo` and `process_market_trade` are no-ops for a live venue, because the venue matches
  its own orders. That is fine for one venue adapter and is evidence that the interface mixes "be a market" with "be a
  venue"; the second live venue is the point at which the interface gets split.
- **Not in scope**: FIX, order routing, order state that survives a process restart (already decided against, because it
  would only prove the simulator is self-consistent), and any placement logic beyond what the strategies already do.
- **To revisit**: a real order on a production endpoint, which is a decision about capital and compliance rather than
  about code, and belongs to the owner.

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

# 0006. The venue's order session moves into the engine

- **Status**: accepted
- **Date**: 2026-11-15
- **Supersedes**: none. This record amends the cost list of
  [0005](0005-a-live-venue-is-another-matching-engine.md) — the relay was where a venue's shapes lived, and the order
  session is leaving it — but 0005's decision that a venue is a third `IMatchingEngine` stands, so nothing is superseded:
  the pair of statuses stays `accepted` / `accepted` and the amendment is stated here and in 0005's own consequence list.

## Context

The live path works and its acceptance run fills: a `ws` session takes the venue's own book as the market leg,
`LiveVenue` places over REST, and the private `orders` channel books the fills, with `[RECON] status=ok`. That run needs
two Python processes. The market leg's session is already C++ — `src/lines/venue_relay.cpp` holds the TLS socket, the
WebSocket client, the reconnect loop and the market decoder, and CMake links OpenSSL for it — but everything about an
*order* is still in `tools/live/venue_relay.py`: the login handshake and its signature, the `orders` subscription, the
request translation, the response decoding, the cumulative-`accFillSz` de-duplication, and the REST placement this venue
needs because its websocket trade operation answers `50026`.

That boundary was the right one while the wire was being discovered: a wrong preimage or a wrong field failed a Python
run in seconds, and the engine knew nothing about a venue's JSON. It is the wrong boundary now. The engine's own claim is
that a venue is another `IMatchingEngine`; a session that a Python process holds is a second implementation of the
engine's liveness, and the two cannot be tested together — the relay's stand-in venue and the adapter's wire have never
run in one process. It also means the demo cannot run without an interpreter, which is a dependency the deployment story
does not mention.

## Decision

The order session moves into the engine, in four slices, each with its own test and each landed on its own:

1. **Authentication.** `base64(HMAC-SHA256(secret, timestamp + "GET" + "/users/self/verify"))` for the login and
   `base64(HMAC-SHA256(secret, timestamp + method + path + body))` for a REST call, dependency-free beside
   `src/lines/venue_auth.*`, with the relay's pinned signature as one vector and RFC 4231's as the others. *This slice is
   landed here*; it is first because it is the one thing a reader cannot check by reading, so it is the one that has to
   be checked against a vector computed elsewhere.
2. **The translator.** The engine's `ORDER`/`CANCEL` lines and the venue's `args[0]` object, the response's `sCode`, the
   decimal `accFillSz` and `fillFee`, and the rule that one fill is booked per increment of the cumulative counter. Pinned
   against the cases `tools/live/venue_relay.py --self-test` already states, so a port that loses one fails a test rather
   than a session.
3. **The session.** Login, the `orders` subscription, ping/pong, and a reconnect that logs in and subscribes again —
   beside the TLS/WebSocket code the market leg already uses. Its test is a local stand-in venue in C++ that
   verifies the signature it receives and drops a connection once, which is the claim the Python test makes today and this
   port cannot inherit.
4. **REST placement.** An HTTP/1.1 client over the TLS socket the tree already has, because this venue places over REST
   and answers `50026` on the websocket trade operation. The Python relay stays the TLS terminator on a host where
   OpenSSL's headers are absent, which is the case CMake's comment already allows.

## Consequences

- **What it forbids**: saying "the live path has no Python in it" before slice four, and translating on both sides — one
  message, one translator, or the two disagree about a size in a way only a fill would reveal.
- **What it costs**: a second implementation of the translator until the Python one is retired, and a crypto primitive in
  the tree that has to be right the first time: SHA-256 and HMAC written by hand, as `ws_feed.cpp` already writes SHA-1
  and base64 for the opening handshake, because linking OpenSSL into `toy_lines` would make every build of the engine
  depend on it. The vectors are the guard.
- **What stays in Python**: the discovery tools — `--self-test`, the stand-in venues, the frame dump. After slice four
  they are fixtures for a port rather than the client, and retiring them is a separate decision.
- **To revisit**: if the venue opened a websocket trade operation, slice four would disappear; if a TLS client in the
  engine were refused, the relay would stay the terminator and slices one to three would still be worth having.

## Evidence

- [`src/lines/venue_relay.cpp`](https://github.com/12mango/ToyQuant/blob/main/src/lines/venue_relay.cpp) — the market
  leg's session, already C++: TLS, the WebSocket client, the reconnect loop.
- [`tools/live/venue_relay.py`](https://github.com/12mango/ToyQuant/blob/main/tools/live/venue_relay.py) — the order half,
  its pinned signature vector and the stand-in venue that checks a login across a socket.
- [ADR 0005](0005-a-live-venue-is-another-matching-engine.md) — why a venue is a third engine, and the acceptance run this
  record's slices have to keep green.
- [The live page](../using/04-live-and-carry.md) — the two processes, the routes and the acceptance numbers.

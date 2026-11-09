# Architecture decision records

*Part of the reference tier. The map of the documents is [`index.md`](../index.md).*

A decision that closes a technical question rather than a scope question gets a file of its own here. The
[change log](../ROADMAP.md#change-log) records what the project will and will not build, one row per release; a row is the
right size for a scope decision and the wrong size for a technical one, because the thing a reader needs is a link they
can send — "why is the event a variant?" — and a row in a three-hundred-row table cannot be that link.

## The four rules

1. **One decision per file**, named `NNNN-slug.md` in the order they were accepted. A number is never reused.
2. **Append-only.** The status line is the only part that changes: `accepted`, `superseded by 0007`, or `rejected`. A
   decision is superseded by writing a new record, never by editing the old one — the old one is the reason the new one
   exists.
3. **An ADR quotes no number it does not own.** Every figure it needs is a link to the marked block that re-derives it.
   That is what keeps this layer cheap: an ADR cannot drift, so a re-recorded measurement never sends anyone back here.
4. **It says what it costs.** A decision record that lists only benefits is an argument; the `Consequences` section names
   what the decision forbids, what it makes more expensive, and what would have to be true to revisit it.

## The records

| # | accepted | decision | state |
|---|---|---|---|
| [0001](0001-the-event-stays-a-variant.md) | 2026-11-07 | the market event stays a variant; it is not restructured to be trivially copyable | accepted |
| [0002](0002-retire-the-orderbook-map-by-counting.md) | 2026-11-07 | the order book's `std::map` is retired by counting the orders, not by replacing the container | accepted |
| [0003](0003-split-the-gate-by-what-a-change-pays-for.md) | 2026-11-10 | the gate is split into `smoke`, `code`, `docs` and `fast` by what a change pays for | accepted |

## What this layer is not

It is not the evidence and it is not the history. The measurement behind a decision stays in
[Performance](../PERFORMANCE.md#5-optimization-ledger) or [Performance History](../PERFORMANCE_HISTORY.md), the rejected
designs stay in [Low-Latency Design](../performance/design.md#7-designs-that-were-measured-and-rejected), and the research
that closed a market stays in [the studies](../archive/studies.md). A record here is a pointer plus a consequence, which is
why it can be forty lines and stay true.

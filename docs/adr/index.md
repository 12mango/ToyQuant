# Architecture decision records

*Part of the reference tier. The map of the documents is [`MAP.md`](../MAP.md).*

A decision that closes a technical question rather than a scope question gets a file of its own here. The
[change log](../ROADMAP.md#change-log) records what the project will and will not build, one row per release; a row is the
right size for a scope decision and the wrong size for a technical one, because the thing a reader needs is a link they
can send — "why is the event a variant?" — and a row in a three-hundred-row table cannot be that link.

## The four rules

1. **One decision per file**, named `NNNN-slug.md` in the order they were accepted. A number is never reused.
2. **Append-only.** The status line is the only part that changes: `proposed` (written down, not started), `accepted`,
   `superseded by 0007`, or `rejected`. A decision is superseded by writing a new record, never by editing the old one —
   the old one is the reason the new one exists. `tools/gate/check_adr.py` holds the status line and this page's table to
   the same words, and refuses a supersede that only one end states, so the agreement is checked rather than promised.
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
| [0004](0004-the-document-tree-keeps-its-names.md) | 2026-11-10 | the document tree keeps its names; the layer it gains is addressability, not vocabulary | accepted |
| [0005](0005-a-live-venue-is-another-matching-engine.md) | 2026-11-11 | a live venue is a third `IMatchingEngine`; the demonstration stops at the venue's sandbox | accepted |
| [0006](0006-the-venue-order-session-moves-in.md) | 2026-11-15 | the venue's order session moves into the engine in four slices, the authentication first | accepted |

## Where the research lives

This page is the door into the vault. A record answers "why is it like this?"; the material it points at answers "what
made that the right answer", and it is filed by what was measured rather than by when:

| the question | the file |
|---|---|
| why is the engine shaped this way, and what did each change win? | [Low-Latency Design](../performance/design.md#7-designs-that-were-measured-and-rejected) |
| what did each optimisation attempt cost, including the ones that paid nothing? | [Performance: the ledger](../PERFORMANCE.md#5-optimization-ledger) |
| how was a particular number obtained? | [Performance History](../PERFORMANCE_HISTORY.md) |
| why was a market closed, and what did it cost in dollars? | [The studies](../archive/studies.md) |
| what is the standing case for a personal account, with outside evidence? | [Research Notes](../archive/research-notes.md) |
| what is still open, and what is deliberately not planned? | [Roadmap](../ROADMAP.md) |

## What this layer is not

It is not the evidence and it is not the history. The measurement behind a decision stays in
[Performance](../PERFORMANCE.md#5-optimization-ledger) or [Performance History](../PERFORMANCE_HISTORY.md), the rejected
designs stay in [Low-Latency Design](../performance/design.md#7-designs-that-were-measured-and-rejected), and the research
that closed a market stays in [the studies](../archive/studies.md). A record here is a pointer plus a consequence, which is
why it can be forty lines and stay true.

# 1. The engine and the L1 line

**This page is retained outside the public tree** (`private/docs/performance/history-01-engine.md`). It was the first part
of the performance history: the engine's own numbers -- parse and decision cost per event, the book's containers under a
growth test, the threading hand-off, and the L1 line the engine was built on.

## What it recorded

- the shape of the run and where the time went, measured rather than profiled by eye;
- the containment question -- how far a change to the engine's data structures may go before the numbers move;
- the L1 path (one aggregate tape and one best-bid/offer tape) and the invariants it pinned;
- the first latency budget: what a decision may cost before the venue's own round trip dwarfs it.

## Where the method is public

[The architecture](../ARCHITECTURE.md) describes the components; [the performance page](../PERFORMANCE.md) carries the
*measurement method* -- how a figure is taken and what it is compared against -- and [the tools](../TOOLS.md) names the
benchmarks that take them.

[Back to the performance history](../PERFORMANCE_HISTORY.md).

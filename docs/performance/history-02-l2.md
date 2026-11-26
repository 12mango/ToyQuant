# 2. The L2 mainline

**This page is retained outside the public tree** (`private/docs/performance/history-02-l2.md`). It was the part of the
performance history that followed the L2 line: the same measurement taken on an equity book and on a crypto tape, the
queue model's effect on a real fill count, and the latency the L2 path costs per decision.

## What it recorded

- the same measurement on two books, and which half of it is the model rather than the market;
- the queue position as a first-class input, with the fill count it produces under each model;
- the per-event cost of the decision path, and at what point the venue's round trip dominates it;
- the comparison that closed the L1 line as a *model* question and opened the L2 one.

## Where the method is public

[The architecture](../ARCHITECTURE.md) is the map of the components, [the performance page](../PERFORMANCE.md) the method
behind the figures, and [the L2 flags](../L2_FLAGS.md) every knob the runs varied.

[Back to the performance history](../PERFORMANCE_HISTORY.md).

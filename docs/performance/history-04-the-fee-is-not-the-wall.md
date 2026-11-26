# 4. Where the fee is not the wall

**This page is retained outside the public tree** (`private/docs/performance/history-04-the-fee-is-not-the-wall.md`). It
was the part of the performance history that separated the fee from the other walls: the screen that ranked instruments by
the capture they could pay, the latency a window would have needed, and the ratio the markets this repository owns settle
at.

## What it recorded

- the screen, its passes and its refusals, instrument by instrument;
- the maker fee each window would have needed for a round trip to be paid, and the windows that could not reach it;
- the latency that would have made a window viable, against the route that was measured;
- the ratio across the markets the repository has data for, which is the sentence the whole history ends on: the fee is
  not the wall, the **distance between the quote and the print** is.

## Where the method is public

[The tools](../TOOLS.md) names the screen (`okx_spread_screen.py`) and the capacity table that turns depth and breadth
into a quote; [the performance page](../PERFORMANCE.md) carries the measurement method; [the L2 flags](../L2_FLAGS.md)
documents the geometry the screen ranks.

[Back to the performance history](../PERFORMANCE_HISTORY.md).

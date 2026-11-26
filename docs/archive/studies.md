# The studies record

**This page is retained outside the public tree.** What was here was the research ledger of the L2 line: every measured
cell, its bar, and the verdict it earned -- the numbers that decide which lever a maker can afford, at what size, on which
instrument, and what each refusal cost. Those numbers are the repository's evidence and they are also a trading result, so
the record is kept privately (`private/docs/archive/studies.md`) and this page is its public face.

## What the record contained

The ledger was organised by question rather than by date, and each entry had the same three parts: the cell (instrument,
window, size, shape), the bar that was registered *before* the run, and the outcome. It covered, in order:

- the queue model and what a fill needs to be reachable at all;
- the fee schedule, the round trip and the per-order floor;
- the quote geometry -- distance in ticks and in basis points, the refresh, the second layer;
- the maker's state switches: the toxicity gate, the cooldown, the asymmetric refresh, the amend;
- the tail: the wind-down, the conditional wind-down, the half-cap retreat and the close-out mechanisms;
- the size ladders, per instrument, with the dispersion test that ended the BLUR ladder;
- the cross-book and trend lines, both closed.

## Where the method is public

The *method* is in the tree, because it is what a reader can re-run:

| question | the public page |
|---|---|
| how a bar has to be written before a run | [the retrospective](../RETROSPECTIVE.md), "how a line is closed" |
| what the engine measures and why each column exists | [the L2 flags](../L2_FLAGS.md), [reading the output](../using/06-reading-the-output.md) |
| how a sweep is launched and summarised | [the tools](../TOOLS.md) (`okx_maker_baseline.py`, `sweep_summary.py`) |
| what the queues, the fees and the latency are modelled as | [the architecture](../ARCHITECTURE.md) |

A reader who wants the numbers rather than the method asks the repository's owner; the point of the split is that the
number is not published by accident, not that it cannot be had.

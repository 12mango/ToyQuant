# 0002. Retire the order book's `std::map` by counting, not by replacing it

- **Status**: accepted
- **Date**: 2026-11-07
- **Supersedes**: none

## Context

`MEOrderBook` keeps a `std::map` per price level, and the low-latency plan carried it as a container-optimisation
candidate: a map allocates per insert, and a structure with a better constant would be the obvious win on the hot path.
The plan's own condition was "needs a workload with real order flow", which had never been tested.

So it was tested by counting rather than by optimising: the strategy was run on the recorded window at the setting every
document quotes, and at the two settings that post the most orders. The three counts are in the
[optimization ledger](../PERFORMANCE.md#5-optimization-ledger).

## Decision

The `std::map` stays. The candidate is retired rather than scheduled, and no container change is made.

## Consequences

- The cost of the map is bounded by the number of own orders per price level, and that number is one or two. A container
  change would need three orders of magnitude to show against a spread of a few tens of nanoseconds per order.
- **What this forbids**: any future argument of the form "the book's data structures are the latency problem" that does
  not come with a strategy that posts orders densely. The finding here is that the *conservative quoter* is what this
  project measured, so the book's shape is not the constraint.
- The same counting is what a reader should ask for before any container work: the number of elements per container per
  run, not the container's asymptotics.
- **To revisit**: a strategy with dense order flow. That is a different project, and the record says so rather than
  leaving the item open in a plan forever.

## Evidence

- [Performance: optimization ledger](../PERFORMANCE.md#5-optimization-ledger) — the three order counts and what each
  setting posts.
- [Performance: where the remaining time is](../PERFORMANCE.md#6-where-the-remaining-time-is) — why the remaining cost is
  the reader and the hand-off, not the book.

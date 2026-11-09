# 0001. The market event stays a variant

- **Status**: accepted
- **Date**: 2026-11-07
- **Supersedes**: none

## Context

The threaded path hands one `MarketEvent` per message from a reader thread to the consumer through an `SpscRing`. The
question that gets asked every time the stage profile is read is whether that hand-off is paying for the event type: a
`std::variant` over the event kinds is not trivially copyable, so the ring moves and destroys rather than memcpy-ing, and
the benchmark that drives the ring in isolation measures that cost directly.

The measured facts are in two places and neither is repeated here:

- the bench's per-message cost of move plus destructor, and what a trivially copyable payload of the same size costs, are
  in [`ring_bench`'s table](../PERFORMANCE.md#5-optimization-ledger);
- what the hand-off costs in a real run — every stage slower than the same stage done serially — is in the threaded
  reading of the [hand-off design](../performance/design.md#6-the-hand-off-between-two-threads-spscring).

The second is larger than the first by an order of magnitude, which is the fact the decision turns on: the ring's own
benchmark is the cheapest version of the hand-off, not the typical one.

## Decision

The event type is not restructured for trivial copyability. The variant stays, and the packed payload stays inside it.

## Consequences

- The bench's per-message number remains a property of the bench. Quoting it as the engine's hand-off cost is the error
  this record exists to prevent.
- Making the event trivially copyable is not a rename: `MarketDepthSnapshot` carries two `std::vector<DepthLevel>`, so the
  change means either bounding a snapshot's levels or a pooled-buffer protocol. That is a redesign of the input contract,
  and it would move every recorded number on the path it touched.
- No recorded number uses the segmented-allocator path the change would optimise, so the gain is bounded by a cost that
  no measurement in this repository pays.
- **To revisit**: a workload where the payload's move is the dominant term — which needs a strategy with dense order flow,
  and this repository does not have one. The threshold is stated in the record above, not invented here.

## Evidence

- [Performance: optimization ledger](../PERFORMANCE.md#5-optimization-ledger) — the paired bench measurement.
- [Low-Latency Design: the hand-off](../performance/design.md#6-the-hand-off-between-two-threads-spscring) — the
  cross-core reading that made the bench number insufficient.
- [The gate's threaded comparison](../performance/history-01-engine.md) — direct against threaded, re-run on every edit.

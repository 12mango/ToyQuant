# The low-latency line: what was tried, what it returned, and where it stopped

*The map of the documents is [`MAP.md`](MAP.md).*

This is the retrospective of the engine work. It is not the place for a number — those are in the
[optimization ledger](PERFORMANCE.md#5-optimization-ledger) — and not the place for a mechanism, which is in
[Low-Latency Design](performance/design.md). It is the page to read before proposing more of this work, because it
answers three questions a reader arrives with: what the line was for, what it actually returned, and why it stopped
where it did.

The boundary with the [design review](DESIGN_REVIEW.md) is worth stating because the two look alike: that document
reviews the **design** — what each C++ decision was and what the gate can prove — and this one reviews the **line** —
the order the attempts came in, the condition that ended it, and what it never measured.

## 1. What the line was for

The project's question is a market one, but the engine had to be able to answer it. If the answer is "no", someone will
ask whether the answer is really "no" or only "no for this engine", and the difference has to be settled with
measurement rather than with confidence.

So the line had two jobs, and the second is the one that survived: make the path fast enough that the answer is not the
engine's fault, and make every claim about it reproducible. The second job is why the line produced more machinery than
speed — a workload and a host definition, a paired A/B harness, a stage profiler, a byte-exact reference and four gate
tiers. A number without that machinery is a memory.

## 2. The order it happened in

| phase | what it established | where |
|---|---|---|
| the workload and the host | four inputs, one of them the latency target, and a host described well enough that a microsecond means something | [History, part 1](performance/history-01-engine.md) |
| a paired harness | every claim is a *paired* measurement, so a change and its baseline are taken on the same machine, minutes apart | [`tools/bench/ab_bench.sh`](https://github.com/12mango/ToyQuant/blob/main/tools/bench/ab_bench.sh) |
| a profile, then a stage table | the cost is in the reader and the book, not in the decision — which is what makes a strategy-shaped answer plausible at all | [History, part 2](performance/history-02-l2.md) |
| the ladder of accepted changes | a reader, a book container, integer symbol ids, an integer-plus-scale parse, a ring, a profiler: each with a mechanism and a proof that behaviour did not move | [Low-Latency Design](performance/design.md) |
| the stage shares | what optimizing a term can return, computed before the code was written | [Design §8.5](performance/design.md#8-the-transferable-lessons) |
| the threaded path | a second thread, a profiler per thread, and the reading that every stage is slower than the same stage done serially | [ADR 0001](adr/0001-the-event-stays-a-variant.md) |
| two candidates retired by counting | the order book's `std::map` and the event's shape: nothing to win at this workload | [ADR 0001](adr/0001-the-event-stays-a-variant.md), [ADR 0002](adr/0002-retire-the-orderbook-map-by-counting.md) |
| the stop | the remaining term is the reader and the hand-off, and both are bounded by the data path rather than by this code | [§6](#6-why-it-stopped-stated-as-a-condition) |

The order matters more than the list. Each phase decided what the next one was allowed to ask: without the stage table,
the ladder would have been aimed at the decision layer, where there was almost nothing to win.

## 3. What it returned

Three kinds of result, and they are not interchangeable:

- **A win that survived the end-to-end test.** The reader is the largest term and it was roughly halved; the book
  container became much faster inside its own stage and returned a smaller share end to end, which is the same fact
  seen twice. See the [ledger](PERFORMANCE.md#5-optimization-ledger).
- **A win that did not survive it.** Several attempts measured as no effect, and they are in the ledger as rows rather
  than being deleted, because the row is the evidence that the question was asked.
- **A measurement that replaced a change.** The event type and the order book's map are the two candidates that were
  retired by a count instead of by an optimization ([ADR 0001](adr/0001-the-event-stays-a-variant.md),
  [ADR 0002](adr/0002-retire-the-orderbook-map-by-counting.md)). That is the cheapest kind of result the line produced
  and the one most likely to be dismissed as "we did not do anything".

## 4. What lost, and the two kinds of loss

[Low-Latency Design §7](performance/design.md#7-designs-that-were-measured-and-rejected) holds the designs that were
measured and rejected. Two of them are worth reading for the method rather than the outcome:

- **An ablation is only evidence if it changes one thing.** Removing a call by replacing it with a constant measured a
  gain several times larger than the real one, because the constant also emptied the book and shrank the working set.
- **The isolation benchmark and the stage table answer different questions.** A container can be much faster in
  isolation and return a fraction of that end to end; both numbers are correct, and quoting the first as the result is
  the error.

## 5. The rules the line produced

Four rules came out of this line, and they are now enforced elsewhere rather than remembered:

- **count before designing** — the two retirements are the worked examples, and the rule is in
  [`.clinerules`](https://github.com/12mango/ToyQuant/blob/main/.clinerules);
- **a stage's share caps what optimizing it can return** — the arithmetic to do before writing code;
- **an ablation must change one thing**, or it measured two;
- **a number that looks too good is a bug until its decomposition reconciles** — capture, markout and fee have to add up
  to the net before the net is quoted.

## 6. Why it stopped, stated as a condition

Not "we ran out of ideas". The stop has a measurement behind it and a condition that would reopen it:

- **The remaining time is the reader, and the two attempts to reduce it measured as no effect.** That is the finding
  that ends a line: the largest term is not unoptimised, it is optimised to the point where this code no longer decides
  it — the data path does. See [§6 of the performance reference](PERFORMANCE.md#6-where-the-remaining-time-is).
- **The hand-off between threads costs more than its own benchmark says**, because the benchmark hands over a value the
  consumer already has in cache while a real event is written on one core and read on another. Measured the only way it
  can be — by a second thread and a per-thread profiler — the split is not free and every stage is slower than its serial
  equivalent ([ADR 0001](adr/0001-the-event-stays-a-variant.md)).
- **Two candidates had nothing to win at this workload.** One or two resting levels a side make the book's map worth
  tens of nanoseconds per own order, and the conservative quoter is what this project measured; the event's shape costs
  less than the cross-core transfer that any redesign would still pay
  ([ADR 0002](adr/0002-retire-the-orderbook-map-by-counting.md), [ADR 0001](adr/0001-the-event-stays-a-variant.md)).

What would reopen it, stated so that a future proposal can be tested against it rather than argued with:

1. **a strategy with dense order flow** — hundreds of own orders per level per run, which is a different project and
   the only thing that makes the book's data structures the constraint;
2. **a redesign of the cross-core payload** — a pooled-buffer protocol or a bounded snapshot, which is a change to the
   input contract and would move every recorded number on the path it touched;
3. **a different data path** — a venue or a format where the reader's cost per event is not what it is here.

Absent one of those three, more work on this line returns less than the spread of its own measurement, which is what the
ledger's no-effect rows say in their own words.

## 7. What the line never measured

This is the part a retrospective owes a reader, and it is why the page exists at a level above the numbers:

- **No live venue round trip.** The latency legs are modelled and swept, not measured against a broker: the order leg
  reaches a demo venue ([ADR 0005](adr/0005-a-live-venue-is-another-matching-engine.md)), but no round-trip time was
  measured on it, and the [design review](DESIGN_REVIEW.md) records that as a boundary rather than a gap.
- **One host.** The measurements are from a single machine under WSL2. There is no second physical core, no NUMA
  distance and no cross-machine comparison in anything here, so the hand-off's cost is one number from one topology.
- **One workload for the byte-exact reference.** Tier 2 compares a full-file hash of a single workload, which makes it
  an exact regression test and not a distribution.
- **The bench's hand-off cost exists on one flag path only.** The cheapest form of the hand-off is what the bench
  measures and no recorded number uses that path ([ADR 0001](adr/0001-the-event-stays-a-variant.md)).
- **No per-fill PnL series**, so nothing here connects a microsecond of latency to a cent of PnL at the fill level; the
  trade report carries price, quantity, side, fee and role, and adding a PnL column is recorded as open in the
  [roadmap](ROADMAP.md).

## 8. Where the numbers live

| for | read |
|---|---|
| the current numbers, the host, and the ledger of every attempt | [Performance](PERFORMANCE.md#5-optimization-ledger) |
| why each accepted change is faster, and each rejected one was not | [Low-Latency Design](performance/design.md) |
| the measurements in the order they were taken | [Performance History](PERFORMANCE_HISTORY.md) |
| the two decisions that closed the last candidates | [ADR 0001](adr/0001-the-event-stays-a-variant.md), [ADR 0002](adr/0002-retire-the-orderbook-map-by-counting.md) |
| how the gate keeps all of it true, and what each half costs | [ADR 0003](adr/0003-split-the-gate-by-what-a-change-pays-for.md) |

**This page is maintained, not frozen.** It is the one place where the line is described as a whole, so a change to the
stop condition — a new candidate accepted, a retired one reopened — updates §6 and the ledger row behind it in the same
commit. The per-decision layer beneath it is [the records](adr/index.md); this page is the narrative that the records
are the footnotes to.


# ToyQuant

**A C++20 limit-order-book replay engine: deterministic execution, a stage-level latency budget, and a gate that
re-derives every number this project publishes.**

ToyQuant tests trading hypotheses with arithmetic rather than assumption. It reads market data, builds an L2 ladder, runs
a price-time matching engine with an explicit queue model, charges a broker's fee schedule, and books the result in a
portfolio ledger -- and it holds all of that to byte-for-byte reproducibility, so a result can be re-derived instead of
remembered.

The question it was built to answer is whether a personal account's programmatic quote can pay for itself. Eight markets
were measured, each was closed by a number rather than an opinion, and the record of each is kept with the command that
re-derives it. The engine is the instrument; the closures are its output.

## Start here

Requirements: CMake 3.16+, a C++20 compiler, and Python 3.

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug
bash tools/gate/verify_l2.sh fast      # tier 0 and tier 1: the invariants, and every quoted number re-derived
```

Then [the quickstart](using/01-getting-started.md) puts the whole system on one page -- one diagram, the eight closures
in one table, three commands -- with one command per input format on [the modes page](using/02-modes.md). The mainline is
the Deribit L2 replay, which is the path every number in these documents came from.

## What it is

| piece | what it guarantees |
|---|---|
| a market data reader | one interface for CSV, gzip, socket and recorded streams, so a live feed is another reader and not a second pipeline |
| an L2 book | a ladder sized from the data, with the queue ahead of a quote modelled explicitly rather than assumed |
| a matching engine | price-time priority, self-trade prevention, partial fills, GTC/IOC/FOK |
| a fee model | a broker's schedule charged the way a broker charges it, including a per-share commission and a minimum per order |
| a portfolio ledger | cash, position, equity and realized PnL, reconciled against the engine's reports on every run |
| a gate | four tiers that re-run the invariants and every number the documents quote; a figure that moves fails the build |

It is described in [the architecture](ARCHITECTURE.md), the order to read the source in is [the main line](MAINLINE.md), and
what each output field means is [the learning path](LEARNING.md).

It is **not** a production trading system: no venue connectivity, no authentication, no market impact, no contract
lifecycle, and no claim about future profitability. Each boundary is stated with its reason in
[the design review](DESIGN_REVIEW.md) and [the roadmap](ROADMAP.md#deliberately-not-planned).

## Findings

An instrument that cannot close a hypothesis is not measuring anything, so the closures are reported as results rather
than as defeats. Through measurement this project closed several approaches that look plausible from the outside: market
making against a fee that is a multiple of the spread, an options straddle whose premium is smaller than the fee floor, a
twelve-month funding carry across two venues, and a low-turnover reversion rule whose sign does not replicate.

The record is kept in three places:

- [**Retrospective**](RETROSPECTIVE.md) -- the eight closures, the friction each one met, and the arithmetic that closed
  it. Every number in it is re-derived by the gate.
- [**Engine retrospective**](LOW_LATENCY.md) -- what the low-latency line returned, the condition that stopped it, and what
  it never measured.
- [**Decisions**](adr/index.md) -- one append-only file per technical decision, so "why is the event a variant?" has a link
  to send rather than a paragraph to find.

## Where to go next

The three tiers, the path for a question, and which document owns which claim are in
[the documentation map](MAP.md); the navigation on the left is the short version of the same thing. If you only read one
more page, read [the retrospective](RETROSPECTIVE.md) -- it is why any of this exists.


<div align="center">

# ToyQuant

**A C++20 limit-order-book replay engine: deterministic execution, a stage-level latency budget, and a gate that re-derives
every number this project publishes.**

[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)](CMakeLists.txt)
[![CMake](https://img.shields.io/badge/CMake-3.16+-064F8C?logo=cmake&logoColor=white)](CMakePresets.json)
[![Tests](https://img.shields.io/badge/tests-CTest-brightgreen)](tests)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)

[Documentation map](docs/MAP.md) · [Quickstart](docs/using/01-getting-started.md) ·
[Retrospective](docs/RETROSPECTIVE.md) · [Decisions](docs/adr/index.md)

</div>

---

ToyQuant tests trading hypotheses with arithmetic rather than assumption. It reads market data, builds an L2 ladder, runs a
price-time matching engine with an explicit queue model, charges a broker's fee schedule, and books the result in a
portfolio ledger -- and it holds all of that to byte-for-byte reproducibility, so a result can be re-derived instead of
remembered.

The question it was built to answer is whether a personal account's programmatic quote can pay for itself. Eight markets
were measured, each was closed by a number rather than an opinion -- the fee against the tick, the position's own move
against the edge, the actions a window contains, the print count against a reference -- and the record of each is kept,
with the command that re-derives it, in the archive linked below. The engine is the instrument; the closures are its
output.

## Quick start

Requirements: CMake 3.16+, a C++20 compiler, and Python 3.

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug
bash tools/gate/verify_l2.sh fast      # tier 0 and tier 1: the invariants, and every quoted number re-derived
```

The whole system on one page -- a diagram of the path from a market row to a booked fill, the eight closures in one
table, and three commands -- is the [quickstart](docs/using/01-getting-started.md), with one command per input format on
[the modes page](docs/using/02-modes.md). This page keeps one command so that it cannot disagree with them.

## What it is

| piece | what it guarantees |
|---|---|
| a market data reader | one interface for CSV, gzip, socket and recorded streams, so a live feed is another reader and not a second pipeline |
| an L2 book | a ladder sized from the data, with the queue ahead of a quote modelled explicitly rather than assumed |
| a matching engine | price-time priority, self-trade prevention, partial fills, GTC/IOC/FOK |
| a fee model | a broker's schedule charged the way a broker charges it, including a per-share commission and a minimum per order |
| a portfolio ledger | cash, position, equity and realized PnL, reconciled against the engine's reports on every run |
| a gate | four tiers that re-run the invariants and every number the documents quote; a figure that moves fails the build |

It is **not** a production trading system: no venue connectivity, no authentication, no market impact, no contract
lifecycle, and no claim about future profitability. Each boundary is stated with its reason in
[the design review](docs/DESIGN_REVIEW.md) and [the roadmap](docs/ROADMAP.md#deliberately-not-planned), rather than left
as a gap nobody wrote down.


## Findings

An instrument that cannot close a hypothesis is not measuring anything, so the closures are reported as results rather
than as defeats. Through measurement this project closed several approaches that look plausible from the outside: market
making against a fee that is a multiple of the spread, an options straddle whose premium is smaller than the fee floor, a
twelve-month funding carry across two venues, and a low-turnover reversion rule whose sign does not replicate.

The record is kept in three places:

- [**Retrospective**](docs/RETROSPECTIVE.md) -- the eight closures, the friction each one met, and the arithmetic that
  closed it. Every number in it is re-derived by the gate.
- [**Engine retrospective**](docs/LOW_LATENCY.md) -- what the low-latency line returned, the condition that stopped it, and
  what it never measured.
- [**Decisions**](docs/adr/index.md) -- one append-only file per technical decision, so "why is the event a variant?" has a
  link to send rather than a paragraph to find.

## Where to read what

| I want | Read |
|---|---|
| to run something | [Quickstart](docs/using/01-getting-started.md), then [the L2 replay](docs/using/03-l2-replay.md) |
| what the project found | [Retrospective](docs/RETROSPECTIVE.md) |
| what the engine work returned | [Engine retrospective](docs/LOW_LATENCY.md) |
| how the engine is built | [Main line](docs/MAINLINE.md), then the [module reference](docs/ARCHITECTURE.md) |
| a field, a flag or a format | [Learning path](docs/LEARNING.md), [Data files](docs/DATA.md), [Tools](docs/TOOLS.md) |
| the evidence behind one number | [Performance history](docs/PERFORMANCE_HISTORY.md), [the studies](docs/archive/studies.md), [Decisions](docs/adr/index.md) |
| what comes next | [Roadmap](docs/ROADMAP.md) |

The three tiers, the path for a question, and which document owns which claim are in
[the documentation map](docs/MAP.md).

## Contributing

Bug reports, documentation improvements, and focused tests are welcome; please open an issue before proposing a new
feature. `.clinerules` states the working rules for an automated contributor -- what it may change, which gate has to be
green before a change counts, and how a number is quoted.

## Versions

The line that predates the queue model is frozen at the tag `v1.0-demo` and stays downloadable for reproducing the
earlier results. [CHANGELOG.md](CHANGELOG.md) says what changed, release by release; [the roadmap](docs/ROADMAP.md) says
what comes next.

## Disclaimer

A measurement exercise, not investment advice, and not to be used for live trading: it has no venue connectivity, and
none of the eight markets it examined cleared its gates.

<div align="center">
Created and maintained by <a href="https://github.com/12mango">12mango</a> · <a href="mailto:1498159938@qq.com">Contact</a> · <a href="LICENSE">MIT License</a>
</div>

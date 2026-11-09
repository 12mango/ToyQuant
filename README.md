<div align="center">

# ToyQuant

**A C++20 limit-order-book replay engine, built to answer one question with arithmetic: can a personal account's
programmatic quote pay for itself?**

[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)](CMakeLists.txt)
[![CMake](https://img.shields.io/badge/CMake-3.16+-064F8C?logo=cmake&logoColor=white)](CMakePresets.json)
[![Tests](https://img.shields.io/badge/tests-CTest-brightgreen)](tests)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)

[Documentation map](docs/index.md) · [Getting started](docs/using/01-getting-started.md) ·
[Retrospective](docs/RETROSPECTIVE.md) · [Decisions](docs/adr/index.md)

</div>

---

The answer it recorded is **no, in eight markets**, and each closure is a quotient rather than an opinion: the fee
against the tick, the position's own move against the edge, the number of actions a window contains, the print count
against a reference. The strategies lost. What is left is the machinery that can say *why*, with a figure attached.

**Every number these documents quote is re-derived by the build.** A figure carries the command that produced it,
`tools/gate/verify_l2.sh` re-runs that command on every edit, and a number that moves fails the gate.

## Quick start

Requirements: CMake 3.16+, a C++20 compiler, and Python 3.

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug
bash tools/gate/verify_l2.sh fast      # tier 0 and tier 1: the invariants, and every quoted number re-derived
```

The full tour -- the four run modes, recording a session and replaying it, the two-thread path, the benchmark workflow
and what each output line means -- is the [getting-started guide](docs/using/01-getting-started.md), with one command
per input format on [the modes page](docs/using/02-modes.md). This page keeps a single command so that it cannot
disagree with them.

## The instrument, and what it is not

A market data reader, an L2 ladder, a matching engine with price-time priority and an explicit queue model, an order
state machine, a fee model that charges a broker's schedule the way a broker does, and a portfolio ledger -- re-derived
by four gate tiers on every commit.

It is **not** a production trading system: no venue connectivity, no authentication, no market impact, no contract
lifecycle, and no claim about future profitability. The boundary is stated with a reason per item in
[the design review](docs/DESIGN_REVIEW.md) and [the roadmap](docs/ROADMAP.md#deliberately-not-planned).

## The graveyard

The system has formally falsified strategies that sound like they work: market making against a fee that is a multiple
of the spread, options straddles whose premium is smaller than the fee floor, a twelve-month funding carry across two
venues, and a low-turnover reversion rule that wins small and loses big.

[**Retrospective**](docs/RETROSPECTIVE.md) is the closing document -- every dead end, what it cost in dollars, and the
number that closed it. [**Decisions**](docs/adr/index.md) is one file per technical decision that closed a question, so
a reader who asks "why is the event a variant?" has a link to send rather than a paragraph to find.

## Where to read what

| I want | Read |
|---|---|
| to run something | [Getting started](docs/using/01-getting-started.md), then [the L2 replay](docs/using/03-l2-replay.md) |
| what the project found | [Retrospective](docs/RETROSPECTIVE.md) |
| how the engine is built | [Low-latency retrospective](docs/LOW_LATENCY.md), then [Main line](docs/MAINLINE.md) and the [module reference](docs/ARCHITECTURE.md) |
| a field, a flag or a format | [Learning path](docs/LEARNING.md), [Data files](docs/DATA.md), [Tools](docs/TOOLS.md) |
| the evidence behind one number | [Performance history](docs/PERFORMANCE_HISTORY.md), [the studies](docs/archive/studies.md), [Decisions](docs/adr/index.md) |
| what comes next | [Roadmap](docs/ROADMAP.md) |

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

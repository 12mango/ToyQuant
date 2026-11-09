# ToyQuant

**A C++20 limit-order-book replay engine, built to answer one question with arithmetic: can a personal account's
programmatic quote pay for itself?**

The answer it recorded is **no, in eight markets**, and each closure is a quotient rather than an opinion: the fee against
the tick, the position's own move against the edge, the number of actions a window contains, the print count against a
reference. The strategies lost. What is left is the machinery that can say *why*, with a figure attached.

**Every number these documents quote is re-derived by the build.** A figure carries the command that produced it,
`tools/gate/verify_l2.sh` re-runs that command, and a number that moves fails the gate.

## Start here

Requirements: CMake 3.16+, a C++20 compiler, and Python 3.

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug
bash tools/gate/verify_l2.sh fast      # tier 0 and tier 1: the invariants, and every quoted number re-derived
```

Then [the getting-started guide](using/01-getting-started.md) walks one replay from input to output, with one command per
input format on [the modes page](using/02-modes.md). The mainline is the Deribit L2 replay, which is the path every number
in these documents came from.

## What it is

A market data reader, an L2 ladder, a matching engine with price-time priority and an explicit queue model, an order state
machine, a fee model that charges a broker's schedule the way a broker does, and a portfolio ledger -- re-derived by four
gate tiers on every commit. It is described in [the architecture](ARCHITECTURE.md), the order to read the source in is
[the main line](MAINLINE.md), and what each output field means is [the learning path](LEARNING.md).

It is **not** a production trading system: no venue connectivity, no authentication, no market impact, no contract
lifecycle, and no claim about future profitability. Each boundary has a reason in [the design review](DESIGN_REVIEW.md)
and [the roadmap](ROADMAP.md#deliberately-not-planned).

## The graveyard

The system has formally falsified strategies that sound like they work: market making against a fee that is a multiple of
the spread, options straddles whose premium is smaller than the fee floor, a twelve-month funding carry across two venues,
and a low-turnover reversion rule that wins small and loses big.

[**Retrospective**](RETROSPECTIVE.md) is the closing document -- every dead end, what it cost in dollars, and the number
that closed it. [**The low-latency retrospective**](LOW_LATENCY.md) is the same treatment for the engine line: what was
tried, what it returned, the condition that stopped it, and the five things it never measured.
[**Decisions**](adr/index.md) is one file per technical decision, so a reader who asks "why is the event a variant?" has a
link to send rather than a paragraph to find.

## Where to read what

| I want | Read |
|---|---|
| to run something | [Getting started](using/01-getting-started.md), then [the L2 replay](using/03-l2-replay.md) |
| what the project found | [Retrospective](RETROSPECTIVE.md) |
| what the engine work returned | [Low-latency retrospective](LOW_LATENCY.md) |
| how the engine is built | [Main line](MAINLINE.md), then the [module reference](ARCHITECTURE.md) |
| a field, a flag or a format | [Learning path](LEARNING.md), [Data files](DATA.md), [Tools](TOOLS.md) |
| the evidence behind one number | [Performance history](PERFORMANCE_HISTORY.md), [the studies](archive/studies.md), [Decisions](adr/index.md) |
| what comes next | [Roadmap](ROADMAP.md) |

The three tiers, the path for a question, and which document owns which claim are in [the documentation map](MAP.md).

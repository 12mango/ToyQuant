# Architecture

This page is the module reference for the instrument: a small event-driven market-making simulator. It connects market-data input, a simplified order book, strategy decisions, order matching, execution reports, and CSV output. What the instrument was pointed at, and what each measurement concluded, is in the [Retrospective](RETROSPECTIVE.md); how to run it is in the [User Guide](USER_GUIDE.md).

## How to read this document

| You want to understand... | Start here |
|---|---|
| End-to-end event flow | [System Map](architecture/01-system-map.md#1-system-map) |
| Input formats and validation | [Shared Types and Input Boundary](architecture/02-inputs-and-types.md#2-shared-types-and-input-boundary) |
| Object ownership and callbacks | [Application Coordinator](architecture/03-coordinator-and-book.md#3-application-coordinator) |
| External book versus strategy orders | [OrderBook](architecture/03-coordinator-and-book.md#4-orderbook-the-frozen-lines-external-view) and [MatchingEngine](architecture/04-matching-engine.md#5-matchingengine-orders-queues-and-reports) |
| How strategies make decisions | [Strategy](architecture/05-strategy/01-the-model-and-the-baselines.md#6-strategy-the-main-decision-module) |
| PnL and reusable metrics | [Backtest and Performance Metrics](architecture/06-accounting-and-the-summary.md#7-backtest-and-performance-metrics) |
| What the engine costs and how that is measured | [Performance](PERFORMANCE.md) |
| Why a latency-critical design is built the way it is | [Low-Latency Design](performance/design.md) |

The diagrams show control flow. The tables show ownership and responsibilities. The code blocks
show the smallest representative implementation surface; detailed prose explains only the design
decisions that are not obvious from those views.

## For a reviewer in a hurry

The four things this document is worth reading for, in the order an engineer would ask about them:

* **the queue**, because it is the part most simulators fudge: a quote's fills are decided by what is ahead of it, the
  model carries four settings from a conservative lower bound to an optimistic upper bound, and the recorded bound sweep
  shows the conclusions of this repository do not move between them;
* **the fee**, because it is charged the way a broker charges it: a per-unit commission, a maker and taker rate, a floor
  per order instance, the fees that follow a sale's proceeds, and a credit for displayed liquidity — with the equity
  schedule that made a one cent tick visible as a cost rather than as an assumption;
* **the state machine**, because the order audit keeps what a strategy did rather than what it intended: filled order
  instances with their size histogram, cancels before and after a fill, the queue consumed, and the markout of the fills
  that arrived;
* **the honesty of the measurement**, because the numbers quoted in [Performance](PERFORMANCE.md) and
  [Performance History](PERFORMANCE_HISTORY.md) are re-derived by `tools/gate/verify_l2.sh` rather than copied, and the
  designs that were measured and then rejected are kept rather than deleted — [Low-Latency Design](performance/design.md) is
  that record.


## The module reference, in six parts

This document was 1,574 lines — a single page that nobody read and everybody searched. It is now six parts, because the
questions arrive six ways: how does an event get in, who owns the state, how is a fill decided, what does a strategy
decide, and what does the run report.

| part | what it covers | sections of the old document |
|---|---|---|
| [1. System map](architecture/01-system-map.md) | the whole flow in one picture, and the ownership rule behind it | 1 |
| [2. Inputs and shared types](architecture/02-inputs-and-types.md) | every reader, the event types they produce, and the validation boundary between them | 2 |
| [3. Coordinator and the book](architecture/03-coordinator-and-book.md) | how the application wires the pieces together, then the external book view the strategies read | 3, 4 |
| [4. Matching engine](architecture/04-matching-engine.md) | price–time priority, the queue model, partial fills, self-trade prevention, and the reports | 5 |
| [5. Strategy](architecture/05-strategy.md), in two parts | the decision layer, its lifecycle, the order state machine, and every strategy family including the mainline | 6 |
| [6. Accounting and the summary](architecture/06-accounting-and-the-summary.md) | what a fill does to cash and inventory, and what every field of every report line means | 7 |

Read 1 and 3 first if you want the shape; read 5 if you are working on a strategy; read 6 when you are reading a report
line and need to know where a number comes from. The rationale behind any of it is in the
[Design Review](DESIGN_REVIEW.md), and the order to read the *source* in is [Main Line](MAINLINE.md).


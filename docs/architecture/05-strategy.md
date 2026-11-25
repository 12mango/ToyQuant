# Strategy

*Section 6 of the module reference. The index is [`../ARCHITECTURE.md`](../ARCHITECTURE.md).*

The decision layer: what a strategy is handed, what it has to hand back, the states its orders move through, and every
family the repository has -- the three baselines, the `active_l2` mainline, and the two non-maker lines beside them.

Two parts, because the questions arrive two ways: how the strategy layer is shaped and what the simple families do with
it, and what the mainline actually computes from a book.

**To write a strategy of your own rather than read one of these**, the task-oriented version is
[the guide's last part](../using/08-writing-a-strategy.md): the interface, the two seams, registration and what a strategy
may not own.

## The two lines, and the directories that say which one a file is in

The layer holds two kinds of strategy that differ in *what they decide*, and the directory is the answer to "which kind is
this" -- it was not, before: three files ending in `_l2.h` sat beside each other while one of them quoted, one set a target
position and one followed a schedule.

| directory | what a strategy there decides | who lives there |
|---|---|---|
| `src/strategy/maker/` | a **quote**: two prices and two sizes, refreshed against the book | `L2MarketMakerBase`, `ActiveL2MarketMaker`, and under `legacy/` the two maker classes the mainline left behind |
| `src/strategy/signal/` | a **position**: a target it opens and closes, with no quote lifecycle | `TrendL2Strategy` |
| `src/strategy/execution/` | a **schedule**: a parent order worked over time, with no direction of its own | `ExecutionL2Strategy` |

`strategy.h` stays at the top because both kinds implement it, and the seams it already exposes are why a new line needs no
new interface: `on_l2_market_view` is what a maker answers, `on_market_trade` is what a signal needs to measure itself, and
`cancel_requests` is what a quote has and a schedule may not. A new strategy joins a line by being *of that kind* and going
in that directory; the factory maps mode names to classes and nothing else, which is what keeps the two lines free to grow
apart.

Where the two non-maker lines stand as research is not this page: their walls, the directions registered against them and the
number that would buy each one are in [the retrospective's frontier map](../RETROSPECTIVE.md#the-two-non-maker-lines-six-directions-and-the-number-that-would-buy-each),
because a line with no measurement of its own is an open row rather than a reference section.

## How to read this

| You want to understand... | Start here |
|---|---|
| what a strategy receives, and what it returns | [The model and the baselines](05-strategy/01-the-model-and-the-baselines.md#6-strategy-the-main-decision-module) |
| how a quote is born, refreshed and retired | [Strategy lifecycle](05-strategy/01-the-model-and-the-baselines.md#strategy-lifecycle-at-a-glance) |
| the states an order moves through, and why a reject is terminal | [Order state machine](05-strategy/01-the-model-and-the-baselines.md#order-state-machine) |
| which families exist, and what each one can do | [The family map](05-strategy/01-the-model-and-the-baselines.md#strategy-family-map) |
| the mainline: fair price, inventory, toxicity, gates, halt | [The L2 mainline](05-strategy/02-the-l2-mainline.md#64-activel2marketmaker-l2-mainline) |
| every strategy in one table, to choose one | [The comparison](05-strategy/02-the-l2-mainline.md#65-strategy-families-and-comparison) |

## The two parts

| part | what it covers | sections |
|---|---|---|
| [1. The model and the baselines](05-strategy/01-the-model-and-the-baselines.md) | the interface, the quote lifecycle, the order state machine, the family map, and the three simple makers | 6, 6.1–6.3 |
| [2. The L2 mainline and the comparison](05-strategy/02-the-l2-mainline.md) | the view the mainline quotes from, its five pieces, and every family in one table | 6.4, 6.5 |

The eight source files this section covers are named at the top of part 1, because the file names alone do not say which
family a strategy belongs to. The rationale behind any of it is in the [Design Review](../DESIGN_REVIEW.md), and the
order to read the *source* in is [Main Line](../MAINLINE.md).

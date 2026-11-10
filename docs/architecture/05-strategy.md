# Strategy

*Section 6 of the module reference. The index is [`../ARCHITECTURE.md`](../ARCHITECTURE.md).*

The decision layer: what a strategy is handed, what it has to hand back, the states its orders move through, and every
family the repository has — the three baselines and the `active_l2` mainline.

This section was 663 lines and is now two parts, because the questions arrive two ways: how the strategy layer is shaped
and what the simple families do with it, and what the mainline actually computes from a book.

**To write a strategy of your own rather than read one of these**, the task-oriented version is
[the guide's last part](../using/08-writing-a-strategy.md): the interface, the two seams, registration and what a strategy
may not own.

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

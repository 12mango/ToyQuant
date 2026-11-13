# ToyQuant

**A C++20 limit-order-book replay engine: deterministic execution, a stage-level latency budget, and a gate that
re-derives every number this project publishes.**

--8<-- "docs/_figures/path-front.mmd"

ToyQuant tests trading hypotheses with arithmetic rather than assumption: it reads market data, builds an L2 ladder,
matches in price-time with an explicit queue model, charges a broker's schedule, and books the result in a ledger --
byte-for-byte reproducibly, so a result is re-derived instead of remembered. Eight markets were measured, each closed by
a number rather than an opinion, and the engine is the instrument rather than the product.

## Run it

Requirements: CMake 3.16+, a C++20 compiler, and Python 3.

```bash
cmake --preset linux-debug && cmake --build --preset linux-debug
bash tools/gate/verify_l2.sh fast      # the invariants, and every number the documents quote re-derived
```

## Read it

| I want | Read |
|---|---|
| the whole system on one page | [Quickstart](using/01-getting-started.md) -- one diagram, the eight closures, three commands |
| what the project found, and why each line ended | [Retrospective](RETROSPECTIVE.md) |
| the tiers, the route for a question, who owns which claim | [Documentation map](MAP.md) |

**Not a production trading system**, and not investment advice: no venue connectivity, no authentication, no market
impact, no contract lifecycle, and none of the eight markets it examined cleared its gates. Each boundary is stated with
its reason in [the design review](DESIGN_REVIEW.md) and [the roadmap](ROADMAP.md#deliberately-not-planned). The navigation
beside this page is [the documentation map](MAP.md) in a shorter form.


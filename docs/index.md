# ToyQuant

**A high-precision C++20 market simulator that tests whether a trading strategy survives the queues, the fees and the
latency of a real venue -- and every number it publishes is re-derived by its own gate rather than remembered.**

<figure>
--8<-- "docs/_figures/path-front.svg"
</figure>

A backtest that fills your quote because the price touched it answers a question the venue never asks. ToyQuant replays
a full L2 ladder with an **explicit queue position**, so a fill depends on the size standing ahead of the quote and on how
it is consumed; it charges a venue's **fee schedule**, including the floor that a cancel and replace pays twice; and it can
hold each **order and cancel** for a measured latency before the venue sees it. One ledger books the result, and one
command re-derives every figure this repository quotes -- so a claim that stops being true fails a build instead of
surviving in a README.

## Run it

Requirements: CMake 3.16+, a C++20 compiler, and Python 3.

```bash
cmake --preset linux-debug && cmake --build --preset linux-debug   # the simulator
bash tools/gate/verify_l2.sh fast      # the invariants, and every quoted number re-derived
./out/build/linux-debug/toy_quant l2_replay \
    data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_200k_base.csv \
    BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation            # one replay, one [PNL]
```

## What a strategy has to survive

| what it has to survive | how this simulator models it | what eight markets said |
|---|---|---|
| the queue in front of a quote | an explicit position ahead of every order, four models from optimistic to conservative | the fill count moves with the model; the conclusion does not |
| the fee a venue charges | maker and taker rates, a commission, and four share terms -- the per-order floor among them | a 0.088-tick capture against a 2.57-tick fee: 29x |
| the latency of the round trip | a per-stage budget, and an order or cancel held for a measured time | 1 ms of it took a +262 USD window to +1.89 |

The three rows behind that table are closed by quotients rather than opinions, and the negative ones are kept: the fee
is why no crypto quote width pays, and the sign of a momentum rule does not replicate across two days. Every figure is in
[the retrospective](RETROSPECTIVE.md), one line beside the block that re-derives it.

Four more things a run can do, because a queue model alone is not a simulator: **record itself** and replay the recording
to the same `[PNL]`; put a **broker** between the strategy and the venue and reconcile its statement against the account;
**refuse an order at a pre-trade limit** before it leaves; and send the order to a **testnet** instead of resting it
in-process, with the venue's own answer booked like any other report.

## Read it

The whole system on one page is [the quickstart](using/01-getting-started.md) -- one diagram, the eight closures, three
commands. What each line of output means is [the reading guide](using/06-reading-the-output.md); what the project found
and why each line ended is [the retrospective](RETROSPECTIVE.md). The navigation beside this page is
[the documentation map](MAP.md) in a shorter form, and it owns the tiers and the route for a question.

**Boundaries, stated rather than implied.** No market impact, no contract lifecycle, no claim about future
profitability, and none of the eight markets examined cleared its gates. The venue leg is real but narrow: an order and
its cancel reach a testnet through `--live-orders`, and the mapping between a replay instrument's quantity and a venue
contract's is measured and named rather than solved. CI re-checks what needs no market data -- the prose, the links, the
site, the field lists; the numbered blocks need the 1.5 GB depth file and run on the machine that has it.


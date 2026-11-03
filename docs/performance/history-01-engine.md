# Performance History, part 1: the engine and the L1 line

*Part 1 of 3. The index, and why the notebook is split, is [`../PERFORMANCE_HISTORY.md`](../PERFORMANCE_HISTORY.md);
part 2 is [the L2 mainline](history-02-l2.md) and part 3 is [the studies](history-03-studies.md).*

This part covers the workload and the host the numbers come from, the pre-queue-model L1 line, quote width against the
fee, the levers and the invariant none of them move, and the five-day out-of-sample check that broke the first version of
that claim. It is the oldest material here and the most superseded: the L1 path is frozen at `v1.0-demo` and every
conclusion in part 2 onwards was measured on the L2 path.

Where things live now:

| Question | Document |
|---|---|
| What does the engine cost today, and how is that measured? | [Performance](../PERFORMANCE.md) |
| Why is a given design faster, and what was rejected? | [Low-Latency Design](../performance/design.md) |
| What was measured earlier, and what did it say then? | this document |

Read a number here only as history. The entries that still hold are repeated in the
[optimization ledger](../PERFORMANCE.md#5-optimization-ledger); anything not in that ledger was either
superseded by a later measurement or was never a latency claim in the first place.

The order below is the order the work happened. It starts with a reproducible end-to-end baseline
taken before any latency-focused change, which was a regression reference rather than a production
throughput or latency claim.

## Workload

- Executable: `out/build/linux-debug/strategy_benchmark`
- Trades: `data/v2/fixtures/test_aggTrades_5k.csv` (3,412 data rows)
- BBO: `data/v2/fixtures/test_bookTicker_5k.csv` (35,310 data rows)
- Symbol and quantity scale: `BTCUSDT`, `1000000`
- Each invocation replays the inputs sequentially for five L1 strategy variants and writes intermediate order/trade files under `/tmp`.
- Trades SHA-256: `f3dc0bcc056dbe27ddfdae330d9e9fa6ebbacd787a3d71692a898ba762450ca3`
- BBO SHA-256: `0b795bd584c16c3a3049690bf4149e5b8aadddbc1f6dc3f6680a295b75a35b1e`

## Environment

- Linux x86_64 under WSL2
- CPU: Intel Core Ultra 7 258V, 8 online CPUs
- CMake preset/build directory: `linux-debug`
- `CMAKE_BUILD_TYPE=Debug`; project compiler options also include `-O2 -march=native` and Debug adds `-g`.
- `/usr/bin/time` is available. `perf` and `valgrind` are unavailable. `gprof` is installed, but the current binary is not built with `-pg`.

## Measurement

The executable was run seven times with the same inputs. Wall-clock seconds, including process startup, input parsing, all five replays, and temporary output writing:

| Run | Seconds |
|---:|---:|
| 1 | 0.20 |
| 2 | 0.14 |
| 3 | 0.15 |
| 4 | 0.16 |
| 5 | 0.16 |
| 6 | 0.16 |
| 7 | 0.16 |

Median: `0.16 s`; range: `0.14–0.20 s`. Captured stdout/stderr hashes matched across all seven runs, indicating deterministic benchmark output for this workload.

Reproduce from the repository root:

```bash
/usr/bin/time -f '%e' ./out/build/linux-debug/strategy_benchmark \
  --trades=data/v2/fixtures/test_aggTrades_5k.csv \
  --quotes=data/v2/fixtures/test_bookTicker_5k.csv --symbol=BTCUSDT --quantity-scale=1000000
```

## The L1 Line, Before the Queue Model

The project began as a Binance Trade+BBO replay with a legacy CSV and a legacy UDP tick path, and the strategy
line that came with it (`naive`, `passive_l1`, `inventory_aware_l1`, `flow_aware_l1`, `active_l1`,
`optimized`) is still in the repository. Everything in this section is history: the design it describes was
superseded, and the numbers stay because the L1 invariants are still one of the sets the fast gate compares.

**What an L1 fill meant.** The feed carries trades and a best bid and offer, with quantities but no depth. The
engine has no level to attach a queue to, so a resting order starts at the touch with nothing ahead of it and
a trade printing through its price fills it. That is a price-touch model, and it answers a real question: did
the market trade at or through my price. It cannot answer the question that decides whether a quote gets
filled, which is what happened to the queue standing in front of it.

**What it measured.** An L1 run is pinned as the second invariant set in the fast gate: 13,734 orders, 2,277
fills, a fill rate of 0.175872, realized PnL of -33.7861 and equity of 966.196 on the recorded input, compared
on every code change. That is the role the line has now. On strategy results the earlier note stands: an L1
sample is unsuitable for profitability conclusions, and nothing here changes that.

**Why the mainline moved to L2.** The question worth answering turned out to be the queue, and a top-of-book
feed cannot see one. The Deribit depth replay added a book with levels, a per-level queue that a new order
joins behind, and the queue models the rest of these documents measure. The same strategy code was then used
to ask whether a fill is a price event or a queue event, and the answer — a median quote that never came
within four percent of the front, on the window the main tables use — is the result this project is built
around.

**What stayed.** The L1 names remain available, the legacy CSV and UDP inputs remain as demo data, and the L1
invariants remain in `tools/gate/verify_l2.sh fast`. New strategy work belongs to the L2 path, and a change to the
legacy makers that moves those numbers is a change to a recorded result rather than an improvement. The
documentation that describes the line as it still runs is in
[User Guide](../using/02-modes.md#current-modes) and [Architecture](../ARCHITECTURE.md).

## Quote Width and the Fee Frontier

The mainline policy quotes one tick wide while the fee is worth 2.57 ticks of the same contract, so the
strategy question with an answer is not "can it beat the market" but "where does the fee stop being the whole
story". The knob is `config_.base_spread`, and widening it moves both sides away from the fair price before
the tick rounding and the clamp that stops a quote from being more aggressive than the touch:

```cpp
// src/strategy/l2_market_maker_base.h:377
const double raw_bid_price = std::floor((fair_price - config_.base_spread - inventory_shift -
                                         weak_flow_spread_shift) / config_.tick_size) * config_.tick_size;
const double raw_ask_price = std::ceil((fair_price + config_.base_spread - inventory_shift +
                                        weak_flow_spread_shift) / config_.tick_size) * config_.tick_size;
```

Sweep on the 200,000-row window, `--refresh-price-ticks=4`, `prorata`:

| `--base-spread-ticks` | fills | fill rate | captured ticks/contract | captured USD | fees USD | realized USD |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 21 | 11.5% | 0.095 | 0.00157 | 0.042 | -0.109 |
| 2 | 13 | 5.9% | 0.154 | 0.00156 | 0.026 | -0.052 |
| 3 | 5 | 1.8% | 1.2 | 0.00470 | 0.010 | -0.017 |
| **5** | 1 | 0.35% | 5.5 | 0.00432 | 0.002 | -0.002 |
| 8 | 1 | 0.35% | 8.5 | 0.00667 | 0.002 | -0.002 |
| 12 | 1 | 0.35% | 12.5 | 0.00981 | 0.002 | -0.002 |

<!-- toyquant:check
run: l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_200k_base.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=5 --refresh-price-ticks=4
then: submitted_orders=285 trade_reports=1 fill_rate=0.00350877 captured_edge_per_unit_ticks=5.5 captured_edge_usd=0.00431508 fees_paid=0.002 realized_pnl=-0.002
-->

Three things fall out of the table, and the third is the one that matters.

1. **Captured edge per contract rises with width**, from 0.095 to 12.5 ticks. A quote further from the mid is
   a better price on the side that gets filled, which is the same mechanism the arrival latency sweep found.
2. **Fees fall** from 0.042 to 0.002 USD, because they are charged per fill and the fills disappear.
3. **The two cross at about five ticks**, and that is the result. At one tick the strategy earns 3.7% of the
   fee it pays; at five ticks it earns 2.2 times the fee for the first time. It cannot have both, because the
   width that pays the fee is the width at which the queue in front stops clearing: the queue distribution's
   median closest approach is 0.961 at one tick and exactly 1.0 from two ticks upward, so the deeper quote is
   a quote that never reaches the front at all.

**The fill counts are the caveat, and they forbid any stronger reading.** At 5, 8 and 12 ticks the run
produces one fill each, and the identical fill rate means the three runs submitted the same 285 orders and
filled one of them. Those rows establish a direction, not a result. The project's floor is thirty fills, so
the honest sentence is: at a width that can pay the fee, this window holds one observation, and the reason is
structural rather than a matter of tuning.

### The same sweep on ten times the window

The 200,000-row window cannot settle the question, so the same sweep runs on the 2,000,000-row slice of the
same file, which is the same data at ten times the length. At one tick it clears the thirty-fill floor:

| `--base-spread-ticks` | fills | fill rate | captured ticks/contract | captured USD | fees USD | realized USD |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 267 | 12.6% | 0.088 | 0.0186 | 0.534 | -1.387 |
| 3 | 91 | 2.9% | 0.181 | 0.0131 | 0.182 | -0.365 |
| 5 | 3 | 0.08% | 1.67 | 0.00395 | 0.006 | -0.013 |
| 8 | 1 | 0.03% | 6.5 | 0.00515 | 0.002 | -0.002 |
| 12 | 1 | 0.03% | 10.5 | 0.00831 | 0.002 | -0.002 |

<!-- toyquant:check
run: l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_2m.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4
then: submitted_orders=2126 trade_reports=267 fill_rate=0.125588 captured_edge_per_unit_ticks=0.088015 captured_edge_usd=0.0186193 fees_paid=0.534 realized_pnl=-1.38723
-->

Two conclusions, and the second is the one a strategy review has to accept.

1. **With enough fills the answer is unambiguous.** At one tick the policy pays 0.534 USD of fees and captures
   0.0186 USD of edge, twenty-nine times less, for a realized loss of 1.387 USD on 1,000 of starting cash. That
   is the fee structure, not a tuning problem: the same run's `queue_min_fraction_p50` is 0.965 and 275 of its
   orders reached the front, so the strategy is quoting where the flow is and still cannot pay for the fills.
2. **Widening the quote does not create value, it reduces participation.** The edge per contract rises by two
   orders of magnitude, from 0.088 to 10.5 ticks, while the total captured value *falls* from 0.0186 to
   0.0083 USD. The realized loss shrinks for the same reason, because within ten ticks the run produces one
   fill instead of 267. A five-fold improvement in realized PnL that comes from not trading is not an
   improvement, and it is the reason the wider rows above are reported with their fill counts beside them.

The 2M table is covered by the second marked block, which adds 1.5 seconds to the fast gate: the slice is a
`head -n` prefix of the tracked file, so the run is one command and no extra data, and the strongest strategy
claim in the repository is worth checking on every change.

## The Levers, and the Invariant None of Them Move

Every knob the strategy exposes was swept on the 2M-row slice with enough fills to read: quote width,
requote threshold, order size and inventory limit. The two remaining sweeps are here, and both were chosen
because they are the ones a reviewer would ask about next.

**The four L2 strategies on the same config** (`--base-spread-ticks=1 --refresh-price-ticks=4 prorata`):

| Strategy | submitted | fills | fill rate | captured ticks/contract | captured USD | fees USD | realized USD |
|---|---:|---:|---:|---:|---:|---:|---:|
| `passive_l2` | 4326 | 432 | 10.0% | -0.102 | -0.0349 | 0.864 | -2.128 |
| `inventory_aware_l2` | 3333 | **635** | **19.1%** | **-0.221** | -0.111 | **2.137** | **-2.578** |
| `flow_aware_l2` | 2052 | 289 | 14.1% | 0.057 | 0.0131 | 0.578 | -1.512 |
| `active_l2` | 2126 | 267 | 12.6% | **0.088** | **0.0186** | **0.534** | **-1.387** |

Two things to read. The mainline choice is now evidence rather than a claim: `active_l2` has the highest
captured edge per contract, the lowest fees and the smallest loss, and the strategy that maximises fills
(`inventory_aware_l2`, 635 of them at 19.1%) has the most negative edge per contract, the highest fees and the
worst result. Fills obtained by being easier to hit are the fills that arrive when the market is moving
against the quote, which is the adverse-selection result this project is built on, reproduced across four
strategies instead of argued from one.

**Order size and inventory limit** (`active_l2`, same config). Both defaults are derived rather than chosen,
which is why this sweep exists:

```cpp
// src/app/strategy_factory.cpp:65
//   instrument->quantity_scale = 1 for Deribit contracts, so:
//   order_size      = max(min_order_quantity, quantity_scale / 1000) = 1 contract
//   inventory_limit = max(1, quantity_scale / 10)                    = 1 contract
```

| `--order-size`/`--inventory-limit` | fills | fill rate | captured ticks/contract | captured USD | fees USD | realized USD | max abs inventory |
|---|---|---:|---:|---:|---:|---:|---:|
| 1 / 1 | 267 | 12.6% | 0.088 | 0.019 | 0.534 | -1.39 | 1 |
| 10 / 10 | 365 | 11.0% | 0.122 | 0.286 | 5.93 | -13.9 | 19 |
| 10 / 50 | 351 | 9.4% | 0.133 | 0.311 | 5.91 | -13.0 | 57 |
| 50 / 50 | 387 | 10.1% | 0.113 | 1.265 | 28.2 | -69.6 | 97 |
| 50 / 200 | 368 | 8.8% | 0.152 | 1.695 | 28.1 | -59.6 | 210 |

**The invariant.** Scale multiplies everything and changes no ratio. Captured value rises about fifteenfold
from the smallest row to the largest, fees rise about elevenfold, and the loss rises tenfold, while the
captured edge per contract moves only from 0.088 to 0.152 ticks. Divided out, the two gated numbers above say
it directly: 0.0186193 USD over 267 contracts is 6.97e-5 USD per contract captured, and 0.534 USD over the same
267 is 0.002 USD per contract paid. The fee is 29 times the edge at one contract, and at fifty contracts it is
still seventeen times, because both sides scale together. **The loss is a rate, not a parameter**, which is
why four sweeps all end in the same place, and why the honest strategy report is a frontier rather than an
improvement.

Both tables are reproduced by the commands in their sections above; their inputs to the invariant are the two
numbers in the marked block, which the fast gate re-computes on every change.

## Out of Sample: the Same Policy on Five Days

Every conclusion above comes from one day, and four more days of the same instrument were added later: a 25-level
snapshot file and a trades file for the first of each month from 2020-05-01 to 2020-08-01. All five days are
replayed through the **same depth format**, which is the point of the design — a difference between the rows below
is the day, not the feed.

| Window | Depth source | submitted | fills | fill rate | captured ticks/contract | captured USD | fees USD | fees / captured | realized USD | queue min-fraction p50 |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 04-01 | snapshot (control) | 1146 | 146 | 12.7% | 0.086 | 0.00987 | 0.292 | 29.6x | -0.757 | 0.992 |
| **05-01** | snapshot (day two) | 526 | 58 | 11.0% | 0.034 | 0.00114 | 0.116 | **102x** | -0.287 | 0.951 |
| **06-01** | snapshot (day three) | 306 | 31 | 10.1% | 0.242 | 0.00397 | 0.062 | 15.6x | -0.115 | 0.905 |
| **07-01** | snapshot (day four) | 196 | 29 | 14.8% | 0.379 | 0.00604 | 0.058 | **9.6x** | -0.083 | 0.906 |
| **08-01** | snapshot (day five) | 339 | 26 | 7.7% | 0.462 | 0.00531 | 0.052 | 9.8x | -0.063 | 0.991 |
| 04-01 | incremental (mainline) | 182 | 21 | 11.5% | 0.095 | 0.00157 | 0.042 | 26.8x | -0.109 | 0.961 |

<!-- toyquant:check
run: l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/snap_20200401_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4
then: submitted_orders=1146 trade_reports=146 fill_rate=0.1274 captured_edge_per_unit_ticks=0.0856164 captured_edge_usd=0.00987095 fees_paid=0.292 realized_pnl=-0.757038 queue_min_fraction_p50=0.991843
run: l2_replay data/v2/crypto/deribit_trades_2020-05-01_BTC-PERPETUAL.csv.gz /tmp/snap_0501_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4
then: submitted_orders=526 trade_reports=58 fill_rate=0.110266 captured_edge_per_unit_ticks=0.0344828 captured_edge_usd=0.00113946 fees_paid=0.116 realized_pnl=-0.287468 queue_min_fraction_p50=0.951289
run: l2_replay data/v2/crypto/deribit_trades_2020-06-01_BTC-PERPETUAL.csv.gz /tmp/snap_20200601_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4
then: submitted_orders=306 trade_reports=31 fill_rate=0.101307 captured_edge_per_unit_ticks=0.241935 captured_edge_usd=0.00396559 fees_paid=0.062 realized_pnl=-0.114748 queue_min_fraction_p50=0.904849
run: l2_replay data/v2/crypto/deribit_trades_2020-07-01_BTC-PERPETUAL.csv.gz /tmp/snap_20200701_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4
then: submitted_orders=196 trade_reports=29 fill_rate=0.147959 captured_edge_per_unit_ticks=0.37931 captured_edge_usd=0.00603727 fees_paid=0.058 realized_pnl=-0.0832212 queue_min_fraction_p50=0.905757
run: l2_replay data/v2/crypto/deribit_trades_2020-08-01_BTC-PERPETUAL.csv.gz /tmp/snap_20200801_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4
then: submitted_orders=339 trade_reports=26 fill_rate=0.0766962 captured_edge_per_unit_ticks=0.461538 captured_edge_usd=0.00531196 fees_paid=0.052 realized_pnl=-0.0633193 queue_min_fraction_p50=0.990675
-->

What survives is the reason for doing it.

1. **The fill rate stays between 8 and 15% on all five days**, so "a quote rarely fills" is not a property of one
   day.
2. **The captured edge per contract stays positive and below one tick** (0.034 to 0.462 ticks), while the fee per
   contract is fixed at 0.002 USD by the fee schedule and cannot move.
3. **The fee is between 9.6 and 102 times the captured edge.** An earlier version of this section said "never
   below 27 times", on three windows; days four and five fell below that at 9.6 and 9.8 times, so the bound is the
   measured range rather than the three-window minimum. The direction of the claim does not move: on every day the
   fee is at least an order of magnitude larger than what the fills capture, and every realized PnL is negative.
4. **The realized loss scales with the fill count, not with the parameters.** 21 fills lose 0.109 USD and 146
   fills lose 0.757 USD, about seven times the loss for about seven times the fills. The added days repeat it: 58
   fills lose 0.287 USD against 26 fills losing 0.063 USD. That is the same scale-invariance the order-size sweep
   found, reproduced on days the parameters were not chosen on.

### The Fair Comparison, and the Second Venue

Widening a quote and refusing a narrow book both cut the fills, so comparing them head to head means comparing them
at the same participation. On 2020-04-01, with the same slice and the same policy except for the mechanism:

| condition | submitted | fills | fees USD | realized USD | USD per fill |
|---|---:|---:|---:|---:|---:|
| no condition | 1146 | 146 | 0.292 | -0.757 | -0.00519 |
| refuse a book narrower than two ticks | 737 | 35 | 0.070 | -0.097 | -0.00276 |
| refuse narrow, plus fee cover | 372 | 12 | 0.024 | +0.00085 | +0.00007 |
| quote five ticks out | 1789 | 6 | 0.012 | +0.00160 | +0.00027 |
| quote eight ticks out | 1800 | 2 | 0.004 | **+0.00955** | +0.00478 |
| quote twelve ticks out | 1800 | 2 | 0.004 | **+0.01593** | +0.00797 |

At matched participation, quoting further out is better per fill than refusing narrow books, which corrects the
earlier claim that widening only reduces participation. Both routes still end in a handful of fills, and the
positive totals rest on two to six fills each, so neither is an edge: the honest reading is that the loss disappears
by not trading rather than by trading well.

The second venue is the same conclusion at a different fee scale. The OKX archive is usable from the portal rather
than the API, so these rows are measured from a prepared slice and are not held by the gate:

| run | records | snapshots | orders | fills | fees USD | captured USD | realized USD |
|---|---:|---:|---:|---:|---:|---:|---:|
| 2026-01-07, one hour | 300k | 5 | 2085 | 1323 | 167.7 | 1.18 | -189.9 |
| 2026-01-07, full day | 6.33M | 96 | 37593 | 25534 | 3149.0 | 15.26 | -4674.4 |
| 2026-01-11, one hour | 300k | 6 | 321 | 135 | 28.4 | 0.11 | -30.5 |

The fee to captured ratio is 142x on one hour, 206x on the full day and 262x on the second day. It is larger here
than on Deribit because the tick is 0.1 on a price near 93000, which puts the fee at about 186 ticks against 2.55 on
the Deribit contract, which is the same mechanism at two scales rather than a second phenomenon.

The pre-trade gate already exists and was simply off, which is worth saying because a roadmap row claimed it was
missing. `--max-position` refuses an order whose projected position, including the quantity resting in other orders,
would exceed the limit. On the same OKX hour it does what it is for and it also shows the trap in its units:

| order size | max position | fills | refused | max exposure USD | equity |
|---:|---:|---:|---:|---:|---:|
| 100000 | none | 1323 | 0 | 18384 | 810.2 |
| 100000 | 20000 | 0 | 372660 | 0 | 1000.0 |
| 10000 | none | 505 | 0 | 10099 | 1014.8 |

The middle row is a limit smaller than a single order, so every order is refused and the run does nothing: loud in
the counter, silent in the output. The last row is the reason the gate matters. With a smaller order the same hour
ends positive, +13.89 USD, but the exposure reached ten times the starting cash, so what it earned was a favourable
move taken with unhedged size rather than anything the quotes captured. A result without the limit is not evidence
about a strategy.

With the limit on and an order size below it, the same hour behaves: exposure is held at 2,808 USD against the
1,000 USD it started with, the gate refuses 34,704 orders, and the equity ends at 973. The run also reproduces the
bucket split on the second venue, which had only ever been measured on Deribit:

| bucket | ticks | fills |
|---|---:|---:|
| buy, at least a tick from the midpoint | 110.2 | 97 |
| buy, at the midpoint | 44.4 | 121 |
| sell, at least a tick from the midpoint | 78.7 | 72 |
| sell, at the midpoint | 34.3 | 125 |

Fills away from the midpoint are worth two to three times the ones at it, on a venue with a different tick, price
and contract size, which is the strongest form the finding has taken.

<!-- toyquant:check
run: l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/snap_20200401_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --min-spread-ticks=2
then: submitted_orders=737 trade_reports=35 fees_paid=0.07 realized_pnl=-0.0967336
run: l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/snap_20200401_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=8 --refresh-price-ticks=4
then: submitted_orders=1800 trade_reports=2 fees_paid=0.004 realized_pnl=0.00954906
run: l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/snap_20200401_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=12 --refresh-price-ticks=4
then: submitted_orders=1800 trade_reports=2 fees_paid=0.004 realized_pnl=0.0159314
-->

### Out of Sample: the One Surviving Corner Does Not Travel

The sweep left one corner positive, so it was taken out of the five days it was selected on. On the same instrument
it holds: the same day's other window, two million records instead of two hundred thousand, returns +0.073 USD on 19
fills. On the second venue it does not trade at all, because the gate asks a quote to cover the fee and OKX's tick is
0.1 on a price near 93000, so the fee is about 186 ticks and no reachable quote covers it, which is why the run
submits nothing.

| run | orders | fills | fee USD | PnL USD |
|---|---:|---:|---:|---:|
| five Deribit days, snapshots, 200k | 372 / 62 / 56 / 23 / 37 | 12 / 2 / 0 / 2 / 1 | 0.034 | **+0.033** |
| 2020-04-01, the other window, 2M | 502 | 19 | 0.038 | **+0.073** |
| OKX 2026-01-07, one hour | 0 | 0 | 0 | 0 |
| OKX 2026-01-11, one hour | 0 | 0 | 0 | 0 |

That is the answer to the question the sweep raised. The corner is not a strategy, it is the fee floor written as a
policy: where the fee is a couple of ticks it becomes "quote rarely and well", and where the fee is two orders of
magnitude larger it becomes "never quote", which is the same conclusion the fee model, the coverage sweep, the
conditional rule and the bucketed markout reached separately. One hundred and fifty settings were searched to find
it, which is also why a positive total from ten or twenty fills cannot be read as an edge.

### The Mechanism Space, Swept to Its Edge

Every lever that decides when and where to quote, crossed with the others, on all five days: a base spread of 1, 2,
3, 5 or 8 ticks, a minimum spread of 0 or 2 ticks, and a fee-covering requirement of 0, 0.001 or 1 tick. One hundred
and fifty runs in total.

| fills in a run | runs | fills | PnL USD | PnL per fill |
|---|---:|---:|---:|---:|
| 0 to 2 | 116 | 80 | -0.023 | -0.00029 |
| 3 to 10 | 13 | 71 | -0.676 | -0.00952 |
| 11 to 30 | 13 | 247 | -1.036 | -0.00420 |
| 31 to 100 | 6 | 260 | -1.176 | -0.00452 |
| over 100 | 2 | 248 | -1.194 | -0.00481 |

Every band loses money in total, which is the same statement the earlier sweeps made from three other directions:
participation is what pays the fee, and the fee is larger than what participation earns. The loss per fill is worst
in the band where a setting trades a handful of times and best where it trades constantly, so no corner of this
space is both busy and profitable.

One corner is positive on four of the five days: a one tick base spread with a two tick minimum spread and a
fee-covering requirement of 0.001 tick. It trades 17 times across the five days for +0.033 USD. It is recorded
because it is the only corner that survives the sweep, not because it is a business: that is about three fills a
day, its total is dominated by a single day, and the size is the venue minimum, so it cannot be scaled. Treating it
as an edge would be the mistake this section exists to prevent.

### The Conditional Rule: Refusing a One Tick Book

The bucketed markout says which fills are bad rather than only how bad the population is: fills that landed a tick
or more away from the midpoint resolved positive on all five days, while fills at the midpoint resolved at or below
zero, because a one tick book leaves the touch half a tick from the midpoint and that is where a passive quote gets
picked off. `--min-spread-ticks` acts on that: when the top of book is narrower than the value asked for, nothing is
quoted at all. A zero value leaves it off, so every run recorded above keeps its numbers.

| Day | Condition | submitted | fills | fees USD | realized USD |
|---|---|---:|---:|---:|---:|
| 04-01 | none, and no gate | 1146 | 146 | 0.292 | -0.757 |
| 04-01 | spread at least two ticks | 737 | 35 | 0.070 | -0.097 |
| 04-01 | two ticks plus fee cover | 372 | 12 | 0.024 | **+0.00085** |

Two things are worth saying plainly. The loss falls by four to eight times, and the loss **per fill** falls too, so
the rule is not only trading less. But a wider condition is not a better one: asking for three ticks on 04-01 was
worse than asking for two, and the far bucket's markout turned negative under it, so the condition changes which
fills arrive rather than only how many. That is the reason the rule is a decision about the situation and not a
parameter to be increased.

<!-- toyquant:check
run: l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/snap_20200401_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --min-spread-ticks=2
then: submitted_orders=737 trade_reports=35 fees_paid=0.07 realized_pnl=-0.0967336
run: l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/snap_20200401_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --min-spread-ticks=2 --edge-cover-ticks=0.001
then: submitted_orders=372 trade_reports=12 fees_paid=0.024 realized_pnl=0.000845982
-->

### The Decision Layer: Refusing to Quote

Every row above comes from a policy that quotes whenever it can. The fee is between 9.6 and 102 times what a fill
captures, so the arithmetic says a quote that cannot pay for itself should not be placed at all, and that is a
decision rather than a parameter: `--edge-cover-ticks` requires a quote to capture the maker fee, measured in
ticks at the reference price, plus the ticks the flag names. A zero value leaves the gate off, which is why every
run above keeps its numbers.

| Day | Gate | submitted | fills | captured USD | fees USD | realized USD |
|---|---|---:|---:|---:|---:|---:|
| 04-01 | off | 1146 | 146 | 0.00987 | 0.292 | -0.757 |
| 04-01 | fee-covering | 463 | 10 | 0.00474 | 0.020 | **-0.168** |
| 05-01 | off | 526 | 58 | 0.00114 | 0.116 | -0.287 |
| 05-01 | fee-covering | 121 | 5 | 0.00345 | 0.010 | **+0.002** |

Refusing to quote removes about nine tenths of the fills and with them about nine tenths of the fee, which is the
whole point: it turns a slow loss into roughly nothing, and on the second day into a slightly positive number.
Five fills on one day is not evidence of an edge, and the gate as written covers the fee only, not the adverse
selection a fill suffers, which is the next thing the same gate should carry.

<!-- toyquant:check
run: l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/snap_20200401_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --edge-cover-ticks=0.001
then: submitted_orders=463 trade_reports=10 captured_edge_usd=0.00474021 fees_paid=0.02 realized_pnl=-0.167803
run: l2_replay data/v2/crypto/deribit_trades_2020-05-01_BTC-PERPETUAL.csv.gz /tmp/snap_0501_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --edge-cover-ticks=0.001
then: submitted_orders=121 trade_reports=5 captured_edge_usd=0.00345258 fees_paid=0.01 realized_pnl=0.00218216
-->

One claim needs narrowing rather than repeating. On the mainline window `queue_zero_orders` equalled the fill
count exactly, which supported "every order that reached the front traded". On the second day it is 59 orders
against 58 fills, and on the control 150 against 146, so the honest form is **almost every** order that reached
the front traded, within a few percent on all three windows. The direction survives; the exact equality was a
property of one window.

## What the Queue Model Does Not Change

The queue in front of a quote is the weakest part of this simulator, and the obvious upgrade is
order-by-order data. Before spending anything on it, the four queue models the engine already has answer the
question: the truth lies somewhere between `conservative` and `optimistic`, so a conclusion that holds at both
ends cannot be moved by knowing the truth.

Same window, same policy, one row per model:

| Queue model | fills | fill rate | captured ticks/contract | captured USD | fees USD | realized USD | queue min-fraction p50 |
|---|---:|---:|---:|---:|---:|---:|---:|
| `conservative` (lower bound) | 103 | 4.2% | -0.083 | -0.0067 | 0.206 | -0.537 | 1.000 |
| `prorata` (default) | 267 | 12.6% | 0.088 | 0.0186 | 0.534 | -1.387 | 0.965 |
| `lumpy` | 269 | 12.5% | 0.063 | 0.0134 | 0.538 | -1.354 | 0.996 |
| `optimistic` (upper bound) | 305 | 14.0% | 0.111 | 0.0269 | 0.610 | -1.446 | 0.952 |

Three readings, and the third is the one that decides whether finer data is worth buying.

1. **The timing conclusions are model-dependent.** Fill counts move by a factor of three across the bounds
   (103 to 305) and the number of orders that reached the front of their queue moves by a factor of six, so
   those numbers are always reported next to the model that produced them.
2. **The economics barely move, and never in the strategy's favour.** The captured edge stays between -0.083
   and +0.111 ticks while the fee is fixed at 0.002 USD per contract by the fee schedule. In the three rows
   where the edge is positive the fee is 23 to 40 times it, and in the conservative row the edge is negative,
   so there the fee is simply lost. The realized loss grows monotonically with the fill count, which is the
   same scale-invariance the order-size sweep and the second day both showed.
3. **Knowing the true queue cannot change the binding result.** The truth lies inside this range, and the
   range is unprofitable at every point, so finer data would buy precision on the fill count and on the queue
   percentiles rather than a different conclusion. Its one honest use here would be calibration — a day of it
   offline, to fit the four rules — instead of a new runtime path, and only if the goal becomes proving the
   queue reconstruction itself.


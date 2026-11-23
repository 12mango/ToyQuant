# Performance History, part 4: where the fee is not the wall

*Part 4 of the history. The index is [`../PERFORMANCE_HISTORY.md`](../PERFORMANCE_HISTORY.md), and parts
[1](history-01-engine.md), [2](history-02-l2.md) and [3](history-03-studies.md) are the engine, the L2 mainline and the
crypto studies.*

This page exists because of a cost, and the cost is counted. Eight of the recorded runs below replay an equity window of
their own, and measured on 2026-11-16 they are 44 s of the 107 s a `docs` run spends on the whole set: on a page of their
own a prose edit to part 3 costs 65 s rather than 107 s, and a change to a window here pays for the window it touches. The
text is unchanged -- these were part 3's second half -- and
[ADR 0003](../adr/0003-split-the-gate-by-what-a-change-pays-for.md) is the decision that asked for the measurement before
the split: a page whose blocks a change does not touch is the page to split, not the verb to widen.

## The Market Where the Fee Is Not the Wall

The last two sections of [part 3](history-03-studies.md) end on the same sentence: on a perpetual the fee is 2.57 ticks
against a capture of 0.095, so nothing about the quoting can matter until the instrument changes. Two markets where the
fee is a *fraction*
of a tick are reachable from a Hong Kong personal account, and both are now measured with the decision layer the
perpetuals were measured with, on the same two hours of one session (2026-09-29 13:30 to 15:30 UTC, the US cash
open), from Databento `mbp-10` downloads converted by `tools/fetch/fetch_databento.py` into the CSV pair the existing
readers take. A micro E-mini S&P 500 contract pays about 0.62 USD of commission against a tick worth 1.25, and a
US share pays 0.0035 USD against a tick of one cent.

| Window | Fills | Units | Fee USD | Capture, ticks | Markout, ticks | Gross USD | Net USD | Net per fill | Breakeven maker rate |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| micro E-mini, one tick | 1675 | 3934 | 2439.08 | 0.013091 | 0.157092 | -387.5 | -2826.58 | -1.68751 | -2.54567e-06 |
| micro E-mini, two ticks | 57 | 122 | 75.64 | 0.479508 | -0.688525 | -196.25 | -271.89 | -4.77 | -4.15733e-05 |
| micro E-mini, four ticks | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| micro E-mini, gate on | 1599 | 3750 | 2325 | 0.0110667 | 0.1596 | -98.75 | -2423.75 | -1.51579 | -6.806e-07 |
| **SPY, one tick** | 799 | 33352 | 116.732 | 0.267855 | 0.0919135 | 379.24 | 262.508 | 0.328546 | 1.48655e-05 |
| SPY, two ticks | 263 | 12279 | 42.9765 | 0.672978 | -0.246926 | -32.83 | -75.8065 | -0.288238 | -3.49544e-06 |
| SPY, one tick, gate on | 791 | 33116 | 115.906 | 0.247146 | 0.111895 | 216.18 | 100.274 | 0.126769 | 8.53436e-06 |
| SPY, one tick, flattening above 1000 shares | 832 | 34618 | 121.163 | 0.241161 | 0.114406 | 282.58 | 161.417 | 0.194011 | 1.07956e-05 |

<!-- toyquant:check
run: l2_replay data/v2/futures/glbx_MESZ6_trades_2026-09-29_1330_1530.csv.gz data/v2/futures/glbx_MESZ6_depth_2026-09-29_1330_1530.csv.gz MESZ6 0 active_l2 1 conservative --order-size=5 --inventory-limit=20 --base-spread-ticks=1
then: trade_reports=1675 trade_report_quantity=3934 fees_paid=2439.08 captured_edge_per_unit_ticks=0.013091 markout_per_unit_ticks=0.157092 gross_pnl_usd=-387.5 closed_pnl_usd=-2841.98 net_pnl_usd=-2826.58 net_per_fill_usd=-1.68751 breakeven_maker_rate=-2.54567e-06 max_abs_inventory=18

run: l2_replay data/v2/equities/xnas_SPY_trades_2026-09-29_1330_1530.csv.gz data/v2/equities/xnas_SPY_depth_2026-09-29_1330_1530.csv.gz SPY 0 active_l2 1 conservative --order-size=100 --inventory-limit=1000 --base-spread-ticks=1
then: trade_reports=799 trade_report_quantity=33352 fees_paid=116.732 captured_edge_per_unit_ticks=0.267855 markout_per_unit_ticks=0.0919135 gross_pnl_usd=379.24 closed_pnl_usd=330.519 net_pnl_usd=262.508 net_per_fill_usd=0.328546 breakeven_maker_rate=1.48655e-05 max_abs_inventory=1099
run: l2_replay data/v2/equities/xnas_AAPL_trades_2026-09-29_1330_1530.csv.gz data/v2/equities/xnas_AAPL_depth_2026-09-29_1330_1530.csv.gz AAPL 0 active_l2 1 conservative --order-size=100 --inventory-limit=1000 --base-spread-ticks=1
then: trade_reports=2324 trade_report_quantity=60167 fees_paid=210.584 captured_edge_per_unit_ticks=0.640152 markout_per_unit_ticks=0.042515 gross_pnl_usd=-2954.95 closed_pnl_usd=-3103.2 net_pnl_usd=-3165.53 net_per_fill_usd=-1.36211 breakeven_maker_rate=-0.000147613 max_abs_inventory=1096
run: l2_replay data/v2/equities/xnas_TSLA_trades_2026-09-29_1330_1530.csv.gz data/v2/equities/xnas_TSLA_depth_2026-09-29_1330_1530.csv.gz TSLA 0 active_l2 1 conservative --order-size=100 --inventory-limit=1000 --base-spread-ticks=1
then: trade_reports=1894 trade_report_quantity=50753 fees_paid=177.635 captured_edge_per_unit_ticks=0.97942 markout_per_unit_ticks=0.34062 gross_pnl_usd=-1297.89 closed_pnl_usd=-1476.02 net_pnl_usd=-1475.53 net_per_fill_usd=-0.779053 breakeven_maker_rate=-7.22386e-05 max_abs_inventory=1098
-->

The whole table is re-derived by `tools/sweep/f4_windows.sh`, which needs the four converted file pairs and says so
rather than printing numbers without them. The pinned rows are the four claims the per-edit gate keeps true, one
per market: SPY, which put the fee question in play; AAPL and TSLA, whose capture is the first to cover the fee
at every queue model; and the micro E-mini, which is the row `tools/study/viability.py` reads the screen's fee gate for.
Each costs about three seconds, which is affordable inside the loop that runs on every edit; the other eighteen
rows cost about a minute together, which is not, so the script is their re-derivation instead.

Three of the rows are the same finding the perpetuals produced, so they are read the same way. Widening the quote
multiplies the capture and cuts the fills — on SPY from 799 fills to 263, and on the micro E-mini from 1,675 to 57
before four ticks produce none at all in two hours. Flattening the position costs more than it saves, on SPY
lowering 262.508 to 161.417, which is now the second market to say so. And the 0.013091 of capture on the
micro E-mini is a hundredth of a tick against a fee of half a tick: a fee a tick can cover is not the same thing
as a capture that a fee can pay for, and the micro E-mini is where those two came apart.

Two rows are new, and they are the reason this section exists. **SPY's closed PnL is +447.251 USD gross and
+330.519 after fees**, so the 262.508 net is quoting rather than a position carried into a favourable mark — the
opposite of the perpetuals row the flatten-band section ends on. And its **breakeven maker rate is +0.0000149
rather than negative**: a rate on top of the commission would still leave the window flat, where every earlier
window needed a rebate. The other new row is the gate behaving as a filter rather than an improvement: it cuts
the micro E-mini gross loss from -387.5 to -98.75, which is its purpose, and on SPY, whose measured markout is
positive, it lowers +262.508 to +100.274 by refusing fills that were paying.

What this is not: the book is Nasdaq's alone, so the queue ahead of a quote is one venue's displayed size out of
a consolidated market and the fill population is optimistic where it matters; it is two hours of one session
taken from the part of the day with the most mean reversion; the commission is a large retail broker's tiered
rate and the tier a Hong Kong account receives is unverified; and the venue's own maker rebate, of the same order
as that commission, is not in the model. The claim this section makes is only that the inequality the perpetuals
could never satisfy **is satisfiable**, and that the row which satisfies it says so in a field the gate
re-derives.

### The Latency That Window Would Have Needed

The row above was measured with a synchronous engine, which is the one assumption a personal account cannot
make: the strategy decides and the exchange has the order in the same instant. `tools/sweep/latency_sweep.sh` varies
the two latencies the engine models, order and cancel, on the same window and the same policy, and then on a
slower policy as a control, because a quote that lives longer ought to be less exposed to delay if delay is the
problem.

| Window | order/cancel | fills | shares | cancels | capture ticks | gross USD | net USD | net/fill USD | breakeven rate |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| SPY fast, synchronous | 0/0 | 799 | 33352 | 51903 | 0.267855 | 379.24 | 262.508 | 0.328546 | 1.48655e-05 |
| SPY fast, 1 ms each way | 1000/1000 | 921 | 38487 | 52262 | 0.213825 | 136.59 | 1.8855 | 0.00204723 | 4.6397e-06 |
| SPY fast, 5 ms each way | 5000/5000 | 9 | 500 | 113 | -0.08 | -1142 | -1143.75 | -127.083 | -0.00297873 |
| SPY fast, 20 ms each way | 20000/20000 | 13 | 600 | 77 | -0.166667 | -753 | -755.1 | -58.0846 | -0.00163672 |
| SPY fast, 50 ms each way | 50000/50000 | 23 | 918 | 51 | -0.533769 | -1040.86 | -1044.07 | -45.3945 | -0.00147879 |
| SPY fast, 20 ms order only | 20000/0 | 12 | 500 | 78 | -0.6 | -371 | -372.75 | -31.0625 | -0.000967678 |
| SPY fast, 20 ms cancel only | 0/20000 | 928 | 39588 | 51632 | 0.213082 | 232.92 | 94.362 | 0.101683 | 7.69175e-06 |
| SPY slow refresh 8, synchronous | 0/0 | 1720 | 66409 | 16599 | -0.354929 | 5.22 | -227.212 | -0.1321 | 1.02766e-07 |
| SPY slow refresh 8, 5 ms each way | 5000/5000 | 1510 | 58240 | 12899 | -0.445167 | -567.25 | -771.09 | -0.510656 | -1.27345e-05 |
| SPY slow refresh 16, synchronous | 0/0 | 1710 | 65571 | 16630 | -0.40553 | 106.01 | -123.489 | -0.0722155 | 2.11372e-06 |
| SPY slow refresh 8, two ticks wide, 5 ms each way | 5000/5000 | 890 | 35043 | 13986 | -0.253874 | 333.69 | 211.04 | 0.237123 | 1.24485e-05 |
| MES fast, synchronous | 0/0 | 1675 | 3934 | 23823 | 0.013091 | -387.5 | -2826.58 | -1.68751 | -2.54567e-06 |
| MES fast, 5 ms each way | 5000/5000 | 1671 | 3941 | 23639 | 0.0185232 | -610.625 | -3054.04 | -1.82768 | -4.00432e-06 |

<!-- toyquant:check
run: l2_replay data/v2/equities/xnas_SPY_trades_2026-09-29_1330_1530.csv.gz data/v2/equities/xnas_SPY_depth_2026-09-29_1330_1530.csv.gz SPY 0 active_l2 1 conservative --order-size=100 --inventory-limit=1000 --base-spread-ticks=1 --order-latency-us=1000 --cancel-latency-us=1000
then: trade_reports=921 trade_report_quantity=38487 cancel_requests=52262 captured_edge_per_unit_ticks=0.213825 gross_pnl_usd=136.59 net_pnl_usd=1.8855 net_per_fill_usd=0.00204723 breakeven_maker_rate=4.6397e-06
run: l2_replay data/v2/equities/xnas_SPY_trades_2026-09-29_1330_1530.csv.gz data/v2/equities/xnas_SPY_depth_2026-09-29_1330_1530.csv.gz SPY 0 active_l2 1 conservative --order-size=100 --inventory-limit=1000 --base-spread-ticks=1 --order-latency-us=5000 --cancel-latency-us=5000
then: trade_reports=9 trade_report_quantity=500 cancel_requests=113 captured_edge_per_unit_ticks=-0.08 gross_pnl_usd=-1142 net_pnl_usd=-1143.75 net_per_fill_usd=-127.083 breakeven_maker_rate=-0.00297873
-->

Re-deriving all thirteen rows costs about a minute and a half, so only the crossing and the collapse are pinned
and the script prints the rest.

**One millisecond each way is where the positive row goes to zero**, and the mechanism is not that latency stops
you trading: fills *rise*, from 799 to 921, while the capture per fill falls from 0.267855 to 0.213825 ticks, so
the delay changes which fills you get rather than how many. The margin the window had was 0.328546 USD a fill,
and the delay cost is of the same order, which is what a margin of a third of a cent a share means against a one
cent tick.

**Five milliseconds gives the price away.** Fills collapse from 799 to 9, because a quote that arrives late
arrives while the market is already somewhere else, and the run ends holding 500 shares marked at -1142, which is
where its -1143.75 comes from: the position again rather than the quoting.

**The two latencies are not symmetric, and order latency is the damaging one.** Twenty milliseconds on orders
alone is as bad as both at once, at 12 fills and -372.75, because an order that becomes live late becomes live at
the wrong price. Twenty milliseconds on cancels alone is far milder, at 928 fills and +94.36: stale quotes get
filled more often and each of them slightly worse, which is a cost rather than a cliff.

**Slowing the quotes down does not rescue it, and this is the result worth keeping.** A policy that re-quotes
only after eight or sixteen ticks of fair-price movement, at zero latency, earns -227.212 and -123.489 with a
capture of -0.354929 and -0.40553 ticks: the frequent cancelling was itself doing the fill selection, and a quote
that waits longer is a quote that gets adversely filled. The reflex answer to latency, which is to trade more
slowly, is measured here and it is wrong.

**One cell of the table is positive and it is not evidence.** At two ticks wide, with a slow refresh, at five
milliseconds each way, the run makes +211.04 — and its capture is still negative at -0.253874 with a closing
position of -237 shares, so the gross came from the position carried into a favourable mark, which is the shape
of the row [the flatten band](history-03-studies.md#the-flatten-band) ends on. Picking that cell after seeing the others
is exactly the window selection this repository refuses to do.

**Verdict: the sixth gate fails.** The tolerance of this policy on this window is about one millisecond one way,
which is not a personal account's to control, and the feed latency a real deployment adds on top of it is not
modelled at all, so a live result would be worse than the worst row above. What this does not yet close is the
one cheap test the arithmetic leaves open: a symbol whose spread is three or more ticks wide has a larger capture
per fill to absorb the same delay. That is the screen in gate 1, and it is the last thing to try before the
quoting line is written down as a research result rather than as a plan.

## The Screen, Run: the First Two Symbols That Pass It

That screen has now been run, and this is the result. The tool is `tools/study/screen_spread.py`, the rule is the one
pre-registered in the [Roadmap](../ROADMAP.md) -- the half spread where trades print, at least three times the 0.35
ticks a share pays per side -- the activity floor of 2.8 trade prints a second was written down before the run
rather than after it, and the cost was **0.52 USD** of the historical credit for thirty symbols of top of book over
the same two hours:

| symbol | median book spread, ticks | median spread where trades print | half at trades / fee per side, needs 3 | trade prints a second, floor 2.8 | spread | activity |
|---|---:|---:|---:|---:|---|---|
| SPY | 2.000 | 1.000 | 1.43 | 3.97 | fail | pass |
| QQQ | 3.000 | 2.000 | 2.86 | 3.64 | fail | pass |
| **AAPL** | 4.000 | 3.000 | **4.29** | 7.65 | **pass** | **pass** |
| **TSLA** | 6.000 | 5.000 | **7.14** | 6.86 | **pass** | **pass** |
| JNUG | 41.000 | 30.500 | 43.57 | 0.016 | pass | fail |
| NUGT | 24.000 | 19.000 | 27.14 | 0.042 | pass | fail |
| LABU | 105.000 | 68.000 | 97.14 | 0.054 | pass | fail |
| EWW | 4.000 | 2.000 | 2.86 | 0.063 | fail | fail |
| ARKK | 3.000 | 2.000 | 2.86 | 0.402 | fail | fail |
| USO | 3.000 | 2.000 | 2.86 | 0.504 | fail | fail |
| the other twenty, among them GME, AMC, SOFI, BOIL, UNG and the rest of the leveraged funds | 1.000 | 1.000 | 1.43 | under 1.4 | fail | fail |

The lines that produce these are `tools/study/screen_spread.py --cost --symbols ...` before any spend and then the same
run with `--download --max-cost-usd=... --min-trades-per-second=2.8`, which prints one `[SCREEN] symbol=...` line
per name with every field above in it; the downloaded files are cached under `data/v2/screen/`, so
`--input data/v2/screen/AAPL_2026-09-29T1330_2026-09-29T1530.csv` re-derives a row without buying anything again.

Three things come out of the run, and the third one is the reason this section exists.

**The screen agrees with the two windows already recorded.** SPY comes back at a 2.000 median book spread and a
1.000 spread where trades print, which is the pair [part 3's F4 table](history-03-studies.md#the-maker-fee-each-window-would-have-needed)
measured from ten levels of depth, and its **3.97 trade prints a second is the number the activity floor's own table was
derived from**. A screen that disagreed with those two would have been the thing to question, and it does not.

**The floor is what removes the widest spreads, and that is the case it was written for.** JNUG's spread is 43.57
times the fee per side and its quotes are the ones a quoter would want; it trades 0.016 times a second, which is
one print every sixty seconds, so a resting quote there is not placed in a market. NUGT and LABU are the same
shape at 0.042 and 0.054. A screen that ranked on spread alone would have put those three first.

**The symbols that pass are among the most liquid in the market, not away from it.** SPY fails at 1.43 and the
perpetuals the project is built on fail by two orders of magnitude, so the roadmap said the search had to leave
the most liquid symbol in the world. It does not: **AAPL and TSLA are the ones that pass**, at 4.29 and 7.14 times
the fee per side, and the mechanism is a property of a single venue's book rather than of the symbol. A probe over
the same files, which reproduces SPY's 2.000 and 1.000 exactly, says where the width is:

| | AAPL, mean 332.71 | TSLA, mean 354.00 | SPY, mean 764.96 |
|---|---:|---:|---:|
| share of book rows at a 1 tick spread | 6.2% | 2.2% | 18.7% |
| book spread, median | 4 ticks | 6 ticks | 2 ticks |
| spread where trades print, median | 3 ticks | 5 ticks | 1 tick |
| trades that print on the venue's own touch | 71.9% | 72.5% | 85.4% |
| trades that print inside the venue's spread | 28.1% | 27.6% | 14.6% |

So the wide spread is not an artefact of measuring one venue's book: two thirds to three quarters of the trades
that print on that venue print against the touch of that same wide book. What the table does not say is what
happens to a quote that rests there, which is the next measurement, and it is the one the fill model exists for.

### What the two symbols do in the engine

Same policy, same two hours, ten levels of depth, one row per queue model, and the fee column is the same 0.35
ticks a fill this repository has charged since the F4 windows:

| window | fills | units | fee USD | capture, ticks | capture / fee per side | markout, ticks | net USD | peak inventory |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| SPY, one tick, for reference | 799 | 33,352 | 116.73 | 0.268 | 0.77 | +0.092 | +262.51 | 1,099 |
| AAPL, `conservative` | 2,324 | 60,167 | 210.58 | 0.640 | **1.83** | +0.043 | -3,165.53 | 1,096 |
| AAPL, `prorata` | 4,673 | 110,143 | 385.50 | 1.060 | 3.03 | -0.297 | -2,229.35 | 1,099 |
| AAPL, `lumpy` | 4,412 | 109,002 | 381.51 | 1.045 | 2.99 | -0.293 | -2,143.97 | 1,086 |
| AAPL, `optimistic` | 4,919 | 123,319 | 431.62 | 1.102 | 3.15 | -0.347 | -2,403.80 | 1,096 |
| TSLA, `conservative` | 1,894 | 50,753 | 177.64 | 0.979 | **2.80** | +0.341 | -1,475.53 | 1,098 |
| TSLA, `prorata` | 3,918 | 100,908 | 353.18 | 1.623 | 4.64 | -0.112 | -2,311.61 | 1,098 |
| TSLA, `lumpy` | 3,731 | 99,158 | 347.05 | 1.611 | 4.60 | -0.112 | -2,221.04 | 1,082 |
| TSLA, `optimistic` | 4,082 | 107,260 | 375.41 | 1.682 | 4.80 | -0.166 | -2,343.31 | 1,083 |

**This is the first market in the repository where the quoting edge covers the commission, and it covers it at
every point of the model space.** Where SPY's conservative floor covers 0.77 of the fee, AAPL's covers 1.83 and
TSLA's 2.80, and neither is a margin the model can erase: the floor is the lower bound of the four, and it is above
one. The screen predicted the direction of this -- a half spread of 1.5 and 2.5 ticks at the trades -- and the
engine's realised capture is 0.64 and 0.98, which is the halving the tool's own closing paragraph warns about and
the same factor (0.54) the F4 section recorded between SPY's half spread and what a fill captured.

**And both windows still lose money, for a reason the fee table cannot see.** At the conservative model AAPL's
capture is worth **+385.16 USD** against 210.58 of commission, so the quotes paid for themselves and 174.58 USD
over; the window's gross is **-2,954.95**, which leaves about **3,340 USD** of adverse price on the position the run
was carrying. TSLA is the same shape at a different size: capture +497.09 against 177.64 of commission, gross
-1,297.89, so about 1,795 USD of drift. The drift is not the fills' adverse selection -- AAPL's measured markout is
*positive* (+0.043 ticks), which by the engine's sign means the fills were slightly picked off, and at 0.043 ticks over
60,167 shares that is about 26 USD of the 3,340 -- it is the mark on 1,096 and 1,098 shares of inventory,
0.36 and 0.39 million USD of notional, over two hours in which the price moved against it by a few tenths of a
percent. At that size a 0.9% move is 3,300 USD and the whole quoting edge is 385.

**The SPY row's +262.51 is a mark, and `--close-out` prices the difference.** The position that window carries is 438
shares short at the end; closing it at the touch takes 400 of them at a cost of 4.00 of spread and 1.40 of taker fee, and
the other 38 find no size and stay marked. So the honest figure for that window is **257.11 net**, not 262.51 -- and the
exit costs **0.18 bps**, which is a tenth of the same operation on a perpetual: there the exit is 5.39 bps because a taker
order crosses a 5 bps fee and half of a tick that is worth real basis points, while on a US share the half spread and the
commission are both fractions of a tick.

<!-- toyquant:check
run: l2_replay data/v2/equities/xnas_SPY_trades_2026-09-29_1330_1530.csv.gz data/v2/equities/xnas_SPY_depth_2026-09-29_1330_1530.csv.gz SPY 0 active_l2 1 conservative --order-size=100 --inventory-limit=1000 --base-spread-ticks=1 --close-out
then: trade_reports=800 net_pnl_usd=257.108 closed_pnl_usd=263.008 unrealized_pnl=-5.90047 fees_paid=118.132 taker_trade_count=1 position=-438 closed=400 residual=-38 spread_bps=0.131074 fees_bps=0.0458758 total_bps=0.176949
-->

One block, because both lines come from the same run: the account after the exit and the exit itself. A reader who wants
to know what a "net" column means on a run that ends holding something now has the number rather than the adjective.


**The exposure is what moves the answer, and the run controls it.** Tightening the inventory limit lowers the loss
monotonically on both symbols, because the drift term is proportional to the exposure while the capture is
proportional to the traded volume:

| window, same policy | fills | units | capture, ticks | fee USD | net USD | peak inventory |
|---|---:|---:|---:|---:|---:|---:|
| AAPL, inventory limit 1,000 shares | 2,324 | 60,167 | 0.640 | 210.58 | -3,165.53 | 1,096 |
| AAPL, limit 1,000, flattening above 300 | 2,844 | 71,952 | 0.297 | 251.83 | -1,881.13 | 400 |
| AAPL, limit 200 shares | 2,240 | 57,813 | 0.642 | 202.35 | -1,454.75 | 299 |
| AAPL, limit derived from the instrument (one share, one 100 share order) | 1,837 | 46,356 | 0.398 | 162.25 | **-870.88** | 100 |
| TSLA, inventory limit 1,000 shares | 1,894 | 50,753 | 0.979 | 177.64 | -1,475.53 | 1,098 |
| TSLA, limit 1,000, flattening above 300 | 2,306 | 60,143 | 0.562 | 210.50 | -1,360.77 | 400 |
| TSLA, limit 200 shares | 1,783 | 47,402 | 0.973 | 165.91 | -1,246.20 | 297 |
| TSLA, limit derived from the instrument | 1,239 | 31,851 | 0.873 | 111.48 | **-844.22** | 100 |

Cutting the peak position from 1,096 shares to 100 takes AAPL's loss from 3,165.53 to 870.88, which is a quarter
of it, and TSLA's from 1,475.53 to 844.22. The flatten band does the same thing less cheaply at 1,000 shares of
limit, which is the second row of each block: it realises the position at the touch instead of carrying it, so it
pays the spread and the fee on every flatten, and AAPL's capture falls from 0.640 to 0.297 because of it. The
distinction matters for reading the SPY rows above, where flattening *lowered* the result from 262.51 to 161.42:
there the drift was in the position's favour and here it is against it, so the same control has opposite signs on
the two. What is stable across both is the size of the term, not its sign.

The derived limit is worth one sentence because it is the harness's own default: `strategy_factory.cpp` sizes it as
the instrument's quantity scale divided by ten, which for a US share -- one unit per share -- is **one share**, so a
run that passes no limit holds at most one unfilled order of 100 shares and the peak inventory is 100. Every row in
the F4 table in [part 3](history-03-studies.md#the-maker-fee-each-window-would-have-needed) was measured with an explicit
limit of 1,000 shares, which is the setting that made the exposure term large enough to dominate; the same measurement at
the harness's default would have shown a smaller number for SPY as well, and a different sign on it.

### What a fill was worth later, and why that is not the quote

The rows above price the fill and price the drift put on afterwards, and the two are easy to conflate. The SPY
window's net is **+262.51** while its fill-level capture is `captured_edge_usd=89.335` against 116.73 of commission,
so the quote alone is **27.40 USD short** of paying for itself and the rest of that net is the position. The archived
F4 write-up says the opposite in one sentence -- "it is quoting, not the position, that earned it" -- and it is not a
contradiction: that sentence is about *closed versus still open* (+330.52 against -68.01), both of which are the price
moving on inventory the quotes put on. Capture against drift is the other split, and only the second one decides
whether a quote is a business.

`--markout-horizon` makes that split measurable instead of argued: the same run, with the fill markout resolved five
quote cycles out or two hundred.

| markout horizon, quote cycles | markout, ticks | markout USD | net USD |
|---:|---:|---:|---:|
| 5, the default and the row above | +0.092 | 30.66 | 262.51 |
| 200 | +0.333 | 111.13 | 262.51 |

<!-- toyquant:check
run: l2_replay data/v2/equities/xnas_SPY_trades_2026-09-29_1330_1530.csv.gz data/v2/equities/xnas_SPY_depth_2026-09-29_1330_1530.csv.gz SPY 0 active_l2 1 conservative --order-size=100 --inventory-limit=1000 --base-spread-ticks=1 --markout-horizon=200
then: trade_reports=799 captured_edge_per_unit_ticks=0.267855 markout_per_unit_ticks=0.333188 markout_usd=111.125 net_pnl_usd=262.508
-->

Three things follow. **The drift is the term that grows and the capture is the term that does not**: across the four
horizons swept (5, 20, 60 and 200 cycles, of which the two endpoints are pinned above and the middle two are not
quoted) the capture and the net are identical to the last digit and only the markout moves. So the SPY window is not
evidence that quoting pays on that venue; it is evidence that the fills sat on the right side of a move which kept
going, and the 27.40 USD by which the capture misses the commission is the same fact stated the other way round.

**A knob on the drift term is a lens and not a lever, which is a problem for anything that reads it as a cost.**
`--markout-cover` refuses quotes the measured markout cannot pay for, and on SPY it removed profitable fills and lowered
the window from 262.51 to 100.27: a filter whose input grows with the horizon it chose is measuring a drift that is in
the position's favour and calling it adverse selection. That is why it is off in every recorded run of this document.

**Two markets, opposite signs, and the size is the finding.** Running the same configuration over MU's eight windows --
the symbol the hibernation row counts -- prints the quoting margin positive in all eight (capture 9.77 to 14.97 ticks
against a 0.35 tick commission) with the markout negative in all eight (-3.59 to -7.08 ticks), and six of the eight end
net positive because the inventory carried the move. The sign is not a property of the venue; the size against a 0.35
tick fee is, and it is the 941 USD capture against the 10,588 USD position sigma the eight-line table records. Those
eight rows are deliberately not in any table here: their tick decomposition does not reconcile with `closed_pnl_usd`
(the caveat `CHANGELOG.md` carries for this sweep), so the row below is pinned as an example of the shape and not as a
number to quote. `tools/sweep/f4_windows.sh` prints all eight.

<!-- toyquant:check
run: l2_replay data/v2/equities/xnas_MU_trades_2026-09-28_1330_1530.csv.gz data/v2/equities/xnas_MU_depth_2026-09-28_1330_1530.csv.gz MU 0 active_l2 1 conservative --order-size=100 --inventory-limit=1000 --base-spread-ticks=1
then: trade_reports=804 captured_edge_per_unit_ticks=9.76631 markout_per_unit_ticks=-3.72431 net_pnl_usd=18960.9
-->

### What this changes

The wall moved rather than fell. On these two symbols the fee is no longer the binding constraint -- the capture
clears it at the lower bound of the queue model, which is exactly what the screen was for -- and what is left is a
**position sizing and drift** question of the same shape the perpetuals had from the other side.

**The next measurement is across days, and it now has a specific question.** The capture, the fee and the fill
count are stable across the model space on both symbols; what is not stable is the sign of the drift, and one
two-hour window cannot separate a strategy from a lucky direction. That is gate 2, and the budget section's
sixty-odd sessions are what answer it: capture per fill and realised net on every session, with the closing position
marked and the exposure held to the smallest limit that still produces fills.

**None of this says the strategy pays, and the numbers that do not are large.** Both symbols lose money in the
session measured, and at the 1,000 share limit the losses are 8.2 times AAPL's quoting edge and 3.0 times TSLA's;
the one number that would say otherwise -- a positive net at a controlled exposure across several sessions with
different price directions -- does not exist yet. The honest summary of this section is one sentence: **the fee
stopped being the reason these two symbols fail, and something else has to be measured before they stop failing.**

The caveats the F4 section of [part 3](history-03-studies.md) records apply to every row above, and there is one more. The
book is **Nasdaq's alone**,
so the queue ahead of a quote is one venue's displayed size out of a consolidated market and the fill population is
optimistic where it matters; it is **two hours of one session**, taken from the part of the day with the most mean
reversion; the commission is a large retail broker's tiered rate and **the tier a Hong Kong account receives is
unverified**; and the venue's own maker rebate, of the same order as the commission, is not in the model. The extra
one is the harness's: the runs start with **1,000 USD of cash and carry up to 0.39 million USD of notional**, so the
PnL is measured on exposure the account is not funding, and `exposure_over_collateral_fills` counts every fill that
made that worse. A real account would hold that position against more capital or a smaller position against that
capital, and the row whose peak inventory is 100 shares is what the second of those looks like.

### What the account's fee schedule adds

Reading the broker's own pages on 2026-10-02 for what those terms actually are, they are these:

| term | how it is charged | AAPL, 332.71 a share | SPY, 764.96 a share |
|---|---:|---:|---:|
| the share commission, Tiered at this account's volume | per share, both sides | +0.35 ticks | +0.35 ticks |
| the Nasdaq credit for adding displayed liquidity, from the broker's own NASDAQ/Island pass-through table (read there: remove 0.0030 USD, add (0.0013) USD) | per share, on a passive fill | **-0.13 ticks** | not read |
| the SEC fee, charged on **proceeds**, at the published 0.0000206 | per share sold | **+0.685 ticks** | **+1.58 ticks** |
| the NSCC and DTC clearing fee, 0.0002 USD a share | per share | +0.02 ticks | +0.02 ticks |
| the FINRA consolidated audit trail fee, 0.000003 USD a share | per share | +0.0003 ticks | +0.0003 ticks |
| the FINRA TAF | per share sold | prints as 0.00 on the schedule | 0.00 |

A run's fills are about half buys and half sells, so the arrangements the account can actually choose cost this per
fill, against the **0.640 ticks** the AAPL conservative row captured:

| arrangement | per fill | against the capture |
|---|---:|---:|
| Tiered, SmartRouted, exchange credit passed through | **0.583 ticks** | **+0.057 ticks, about 34 USD over the window** |
| Tiered, SmartRouted, credit not passed through | 0.713 ticks | -0.073 ticks |
| Fixed, SmartRouted, exchange fee bundled into the rate | 0.863 ticks | -0.223 ticks |
| Fixed with API direct routing, 0.0075 USD a share | 1.113 ticks | -0.473 ticks |

**So the fee schedule decides nothing, and the arrangement that would let a quote choose its venue is the worst of
the four.** The best case leaves 0.057 of a tick a fill -- 34 USD on a two hour window whose position cost 3,340 --
and every other case is negative. That is why the venue question is answered by recording where orders land rather
than by paying for routing, and why the cross-day measurement is the only test left standing.

Read across symbols, the same table says something the screen rule does not: because the SEC fee is charged on
proceeds, **it grows with the share price** -- 1.58 ticks a side to sell a 765 dollar ETF against 0.685 on a 332
dollar share -- and at 0.685 it is **the largest single term in the row, 59% of the 0.583**, larger than the
commission the model has been pricing all along. The rule's "three times the fee per side" is therefore not price
independent, and the correction points the same way the rest of the equity evidence does -- at cheaper shares with
wider spreads.

**One term could still turn the sign, and the broker's own note about it is written for a strategy that requotes.**
Every market's commission table this repository could read carries a "minimum per order" row -- Japan, Singapore,
Malaysia and India all print one, in their own currencies -- the US Fixed examples on the same page demonstrate
1.00 USD for a 100 share order at 25 USD, and the API direct routed row prints `Minimum per order USD 1.00`; the
Tiered figure this repository has been given is **USD 0.35**, and it is recorded here as reported rather than read,
because three attempts to fetch the US commission table found it in the part of the page the fetcher truncates.
What was read verbatim, in the Chinese rendering of the same page, is the note that decides how such a minimum
applies:

> "modifying an order is treated as cancelling the original order and replacing it with a new one. At some
> exchanges, this can cause the modified order to be charged commission as if it were a new order. For example, if
> you submit a 200 share order, 100 shares execute, and then you modify the order and the remaining 100 shares also
> execute, then both 100 share orders are subject to the minimum commission."

**And the mechanism is now confirmed rather than inferred from a page.** The account holder states the charge
directly: partial fills of one order on the same trading day are charged once, while a cross-day execution or a cancel
and replace resets the instance, so each is charged its own floor. That promotes the paragraph above from a note this
repository read into a term the models are allowed to rely on, and it makes the requoting case the ordinary case
rather than the corner: a maker that replaces its quotes on every fair price move meets the floor once per cycle, so
the number that decides a thin margin is the floor divided by the contracts that fill before the replacement.

The counter that decides it now exists, and on this window the answer is that the minimum applies. The run reports
the filled order **instances** and their sizes, and AAPL shows **1,222 instances against 2,324 fills**, with
**969 of them -- 79% -- filling fewer than 100 shares**, which is where 0.35 stops binding at a 0.0035 share rate:

| filled order size | orders | shares |
|---|---:|---:|
| up to 50 | 737 | 17,738 |
| 51 to 99 | 232 | 17,129 |
| 100 to 199 | 253 | 25,300 |

So a 0.35 minimum charged per order instance costs `969 * 0.35 + 0.0035 * 25,300` = **427.70 USD** against the
**210.58 USD** the per share model charges, a factor of 2.03, and the per fill cost moves from **0.583 to 0.943
ticks** against a capture of 0.640. **The quoting therefore stops covering its fee**, short by about 0.30 of a tick
a fill. The window's sign does not move: 0.30 of a tick over 60,167 shares is 183 USD on a window whose position
term is 3,340.

The same three buckets also say why the minimum binds and what would unbind it, and it is not a fee finding. The
strategy replaces **45,680 of its 45,907 orders** and fills 26 shares at a time, so nearly every order it fills
fills partially; the 253 orders that did fill 100 shares or more paid the per share rate instead. Getting the other
969 into that bucket means quoting larger, which is the same direction as the drift term that costs 3,340 and
against it, so the two have to be traded off rather than fixed separately. TSLA reads the same way -- 1,128
instances, 920 under 100 shares, 394.80 USD against 177.64, 1.011 ticks a fill against a capture of 0.979, a
shortfall of 0.03 of a tick -- and SPY reads 529 instances with 324 under 100 shares, though SPY's fee was never
covered to begin with.

Nothing in this section moves a measured number: every table above is what the engine printed, and the terms here
are prose beside them so that a reader can see what the engine does not carry. `fees_paid` is still the per share
figure on purpose -- counting what the broker charges and changing what the engine charges are two changes, and
this section needed the first. The second is now named and sized: two fees on a sale, a credit on a passive fill,
and a minimum per order instance, carried by the instrument rather than by prose. It is worth making, because it is
what turns every net figure in the equity rows into one a broker's statement would agree with, and it should be made
with the rates from a first trade confirmation rather than from a page -- the two readings of the minimum are 183
USD apart on this window, and a confirmation is what settles which one is real.

**The second change is made, and the schedule above is now something a run can be priced under.** The instrument
and the venue both carry the five terms -- a floor per order, a fee on the proceeds of a sale, per-share clearing
and audit fees, and the credit for adding liquidity -- all defaulting to zero, and
`--fee-schedule=min=0.35,regulatory=0.0000206,clearing=0.0002,audit=0.000003,rebate=0.0013` sets them for a run,
which the `[Mode]` line then reports as the schedule in force. On the AAPL row that turns the venue's **210.584 USD**
of fees into the account's **565.342**, and the total reconciles to the cent:

| term | USD |
|---|---:|
| the share commission the venue charged too | 210.584 |
| the floor, charged on **969** orders and reported as `min_commission_paid` | **217.115** |
| the SEC fee on the sales, at 0.0000206 of proceeds | 203.65 |
| clearing and audit trail, per share | 12.21 |
| the credit for the fills that added liquidity | -78.22 |
| **total** | **565.342** |

So the fill costs **0.940 ticks** against the 0.640 it captured, the window's net moves from -3,165.53 to
**-3,520.29**, and the whole of that 354.76 is the fee: on the account's own schedule **the quoting does not cover
itself**, and the position term at 3,340 is still an order of magnitude larger, which is the same conclusion this
section reached before with a number instead of a bracket. Every term is a parameter, so a run that names none is
priced exactly as every recorded row was -- the baseline is unchanged to the last digit -- and the account's first
trade confirmation is what should replace the rates used here.

**The window can be cut into eight for nothing, and the answer it gives is the one gate 2 was going to buy.**
`tools/study/analyze_l2_windows.py` cannot cut these files: it re-runs the engine per window and needs a book baseline at
each cut, and a converted Databento file carries one snapshot, in its first row, so a slice would hand the engine a
book built from deltas with no state to apply them to. The engine already writes every fill it made, with its
timestamp, side, price, quantity and fee, so the same question comes out of one run of the whole window instead:
walk the depth file's absolute level sizes into a touch midpoint series, join it with the ledger, and bucket.
`out/probe_windows.py` does that, and its aggregate row is what makes its buckets readable:

| term | the probe | the engine's own line |
|---|---:|---:|
| fee USD | 210.58 | **210.584** |
| capture USD | 419.59 | 385.16 (`captured_edge_usd`) |
| the inventory term USD | -3695.40 | -- (the engine books it inside `net_pnl_usd`) |
| net USD | -3486.39 | -3165.53 |

The fee agrees to the cent, which is the ledger checking itself; the capture differs by nine percent because the
probe measures the mid at the fill while the engine measures the quote's own distance, and the net differs by ten
percent for the same reason. A ten percent proxy does not settle a tenth of a tick, so the aggregate is what it is
used for -- as a check that the buckets add up -- and the buckets are then read for their **spread**, which no
proxy error can hide:

| 15 minutes | fills | capture ticks a unit | inventory term USD | net USD |
|---|---:|---:|---:|---:|
| 00-15 | 502 | +1.3775 | -1929.82 | -1800.76 |
| 15-30 | 325 | +0.8367 | -128.28 | -83.78 |
| 30-45 | 197 | +0.3794 | +12.19 | **+13.68** |
| 45-60 | 341 | +0.3610 | -553.90 | -553.00 |
| 60-75 | 262 | +0.3475 | -112.26 | -112.44 |
| 75-90 | 185 | +0.2605 | -148.99 | -153.43 |
| 90-105 | 285 | +0.6726 | -229.12 | -206.34 |
| 105-120 | 227 | +0.6055 | -605.21 | -590.32 |

Three readings, and the third is the one that closes the question.

* **The capture is positive in all eight buckets**, +0.26 to +1.38 ticks a unit, mean +0.70. The quoting edge is
  therefore not an artefact of one window: gate 2's first half -- "capture stays positive across days" -- is
  already answered, for nothing, and the answer is yes.
* **The inventory term swings by 1930 USD inside a single quarter hour of a two hour window**, against a fee bill
  of 210.58 for the whole of it. One bucket's drift is ten fee bills.
* **One bucket in eight is positive, and it is positive because its drift was +12**, not because its capture
  covered the cost: that bucket's capture, +0.3794, is below the 0.579 the schedule charges a unit before any
  minimum.

That leaves the line's expected value, which is the whole question, computable without another download. A unit of
fill captured 0.6402 ticks and the account's schedule charges 0.940 a unit with the minimum per order and 0.579
without it, so one window's expected value is **-181 USD** if the minimum follows the order and **+36.70** if it
does not: **the floor decides the sign and nothing else does.** What the floor cannot decide is whether the sign is
observable. The inventory term drew -3695 in this window and -1929 in its first quarter hour, so its per window
standard deviation is on the order of 3000; separating an edge of 37 to 100 USD from that at two standard
deviations needs between 3,600 and 26,000 windows, or fifteen years of daily two hour sessions at the cheap end. A
smaller size does not help, because both terms are proportional to the volume quoted: the ratio is 0.64 ticks of
capture against a position term that is thirty times the edge, at any size a personal account can run.

**So the purchase that was going to decide this is no longer the purchase that would.** Gate 2's first half is
answered for nothing and its second half is unanswerable at this scale rather than merely unfunded, and what is
left of the quoting line is the fee arithmetic above: a market where the capture covers the commission 1.83 times
and a schedule -- whose smallest term, a minimum per order, is larger than the whole edge of the window. The next
purchase this document would make is not more days of the same symbol; it is the same two hours of a symbol whose
spread is wide relative to its fee, which is the one measurement the screen's price column can still select for and
the one the screen has not yet been run on.

**It has now been run, and the engine confirms it.** Thirty single names were screened on 2026-10-02 for 0.295 USD
and **three pass both halves with a schedule ratio far above anything before them**: MSTR at 7.482, MU at 10.770 and
AMD at 12.572, against TSLA's 4.132 and AAPL's 2.573, with 3.55, 7.55 and 3.99 trade prints a second against the
floor's 2.8. Their two-hour windows were then bought in ten levels for 0.202 USD and run through the pinned policy
with the account's schedule, and they produce the first positive quoting margin this repository has measured:

| symbol | price USD | capture ticks/unit | schedule round turn ticks | the floor USD | fills | units | capture less all fees USD | the position's own move USD | net USD |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| MU | 1073.76 | **12.5337** | 2.693 | 150.21 | 831 | 12,584 | **+1246.6** | +5735.3 | **+6981.9** |
| AMD | 616.15 | 9.7868 | 1.750 | 55.37 | 371 | 9,781 | **+812.8** | -1842.3 | -1029.5 |
| MSTR | 155.96 | 1.3952 | 0.802 | 68.24 | 641 | 20,604 | **+137.5** | -1021.5 | -884.0 |

Three facts, in the order they matter.

* **The quoting pays for itself, which is new.** Capture less the commission, the regulatory fee, clearing, the
  audit trail, the rebate **and the minimum per order** is positive on all three, by 137 to 1247 USD a window, and
  the minimum is the term the screen deliberately left out: 210 to 518 orders were charged it, for 55 to 150 USD.
  The rule the screen applies without the floor is therefore **conservative**, and the floor did not overturn it.
  Compare the two symbols this section is named after, where the same arithmetic is -181 USD a window.
* **The window's own result is still the position's.** MU's gross of 7312.40 contains 1577 of capture and 5735 of the
  mid moving while the inventory was held; AMD's -884.98 is 957 of capture against -1842 of position; MSTR's -884.03
  is 287 of capture against -1021. The same pattern as every other row in this document, and the same conclusion:
  the quoting is now a positive term and the position is still the larger one.
* **But the ratio has moved enough to be measurable.** Against a per-window position draw on the order of 3000 USD,
  an edge of 137 to 1247 USD needs **23 to 1900 windows** by the arithmetic above, where the -181 to 37 USD edge of
  AAPL needed 3,600 to 26,000. The best of these is one month of daily two-hour sessions rather than fifteen years,
  which is the first time any symbol in this repository has been inside a retail timeframe.

**Two caveats are written down before anything is concluded from it.** The inventory limit is in shares, so 1,000
shares is 1,165,040 USD of peak exposure on MU against 363,631 on AAPL, and a position draw rides on that number:
the three rows are not size-matched, and the next run should hold the notional comparable rather than the share
count. And one window is one draw, so what these rows establish is that the screen's second rule **selects symbols
whose quoting covers their whole schedule**, which is the claim the rule makes; they do not establish an expected
value, and the windows needed for that are the ones the third fact counts.

**Both controls now exist, and six windows of one day measure what they cost rather than what they are worth.**
`tools/sweep/limit_sweep.sh` re-runs the six US equity windows on disk at the pinned size, four kill-switch thresholds
against off and three exposure caps against off:

| symbol | off | 250 | 500 | 1000 | 2000 | when it tripped |
|---|---:|---:|---:|---:|---:|---|
| AAPL | -3520.29 | **-1802.30** | -2547.29 | -2402.27 | -2516.75 | 26, 34, 64 and 3133 seconds |
| TSLA | -1821.66 | **-574.38** | -978.27 | -1569.36 | -2032.86 | 602 to 2312 seconds |
| SPY | -35.54 | -35.54 | -35.54 | -35.54 | -35.54 | never |
| MSTR | -884.03 | -745.28 | -857.06 | -884.03 | -884.03 | 5771 and 6680 seconds |
| AMD | -1029.47 | -2305.94 | -2406.67 | -2508.41 | **-568.94** | 1004 to 1448 seconds |
| MU | **+6981.90** | -2011.23 | -2023.23 | -2095.66 | -2069.55 | 215 to 281 seconds |
| **sum** | **-309.09** | -7474.67 | -8848.06 | -9495.27 | -8107.67 | |

**The switch is insurance, and on this sample the premium is larger than the claims.** It saved 3564.55 USD across
AAPL, TSLA, MSTR and one AMD setting, and it cost 8993.13 on MU alone, where it tripped 215 seconds into a window
that closed 6981.90 up. Summed, it turns -309.09 into -7474.67 at its tightest and -8107.67 at its loosest, so **no
threshold on this sample pays**. The sweep also shows why: the threshold is in dollars and the exposure is not, so
the same 500 USD fires 245 seconds into MU's window and 1031 seconds into AMD's, and a control whose trigger is a
fixed dollar amount against a varying notional is a control on position size rather than on loss. Anything that
runs it live has to scale the threshold, and this table is the evidence for that.

| symbol | off | cap 100000 | cap 200000 | cap 350000 |
|---|---:|---:|---:|---:|
| MU | **+6981.90** | 0 (nothing submitted) | +1235.01 | -630.09 |
| AMD | -1029.47 | **+362.74** | -59.16 | -694.40 |
| MSTR | -884.03 | -1315.02 | -884.03 | -884.03 |
| AAPL | **-3520.29** | -1837.16 | -2807.82 | -3517.55 |
| **sum** | **+1548.11** | -2789.44 | -2516.00 | -5726.07 |

**The exposure cap is a variance control, and its effect on the total is the sign of the position term.** It cut
AAPL's loss by 1683.13 and turned AMD positive at the tight setting, and it removed MU's 6981.90, because that
number was the position's draw and not the quoting's. The one row a reader can misread is MU at a 100000 cap: one
order of 100 shares is 107400 USD of notional, above the cap, so **every order is refused, nothing is submitted and
the run does nothing at all** -- 549732 refusals, zero fills, which is visible rather than silent, and is what a cap
below one order's notional means.

**Both tables end in the same place: one day is one draw of the position term, and no control can be judged on
it.** The six windows sum to -309.09 uncapped and every part of that is inside a single window's position term,
which is why what follows buys days rather than thresholds.

**The eight-window campaign has been run, and the first of its rules is answered cleanly.** Seven more MU windows
were bought for 0.608 USD at 13:30 to 15:30 UTC on 2026-09-22, 09-23, 09-24, 09-25, 09-28, 09-30 and 10-01, joined
by 09-29 which was already on disk, and each window was run at the pinned size and at a notional-matched size, both
under the account's schedule. The loop is:

    for day in 2026-09-22 2026-09-23 2026-09-24 2026-09-25 2026-09-28 2026-09-29 2026-09-30 2026-10-01; do
      for size in "100 1000" "30 300"; do set -- $size
        toy_quant l2_replay data/v2/equities/xnas_MU_trades_${day}_1330_1530.csv.gz \
          data/v2/equities/xnas_MU_depth_${day}_1330_1530.csv.gz MU 0 active_l2 1 conservative \
          --order-size=$1 --inventory-limit=$2 --base-spread-ticks=1 \
          --fee-schedule=min=0.35,regulatory=0.0000206,clearing=0.0002,audit=0.000003,rebate=0.0013
      done
    done

| day | size | capture ticks | capture USD | markout USD | fees USD | the floor USD | quoting margin USD | the position term USD | net USD |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 09-22 | 100 | 11.7965 | 2131.16 | -648.21 | 412.12 | 162.52 | **+1070.83** | -15466.06 | -14395.20 |
| 09-22 | 30 | 12.4031 | 1218.11 | -447.71 | 325.62 | 194.53 | **+444.78** | -6503.36 | -6058.57 |
| 09-23 | 100 | 14.9659 | 2464.89 | -1166.73 | 369.35 | 135.91 | **+928.81** | +11106.54 | +12035.30 |
| 09-23 | 30 | 14.4728 | 1307.18 | -632.84 | 284.47 | 158.79 | **+389.87** | +2920.89 | +3310.76 |
| 09-24 | 100 | 13.5488 | 1766.22 | -678.50 | 274.31 | 109.42 | **+813.42** | -589.85 | +223.57 |
| 09-24 | 30 | 14.0461 | 1064.97 | -451.92 | 226.54 | 129.21 | **+386.51** | -812.69 | -426.18 |
| 09-25 | 100 | 13.6922 | 2158.99 | -838.84 | 325.35 | 121.91 | **+994.81** | +1293.55 | +2288.35 |
| 09-25 | 30 | 13.6041 | 1087.78 | -436.58 | 246.07 | 140.71 | **+405.13** | -1257.29 | -852.15 |
| 09-28 | 100 | 9.7663 | 1311.62 | -500.18 | 325.15 | 144.10 | **+486.30** | +18196.56 | +18682.80 |
| 09-28 | 30 | 10.3035 | 795.64 | -344.30 | 259.19 | 157.07 | **+192.15** | +7636.99 | +7829.15 |
| 09-29 | 100 | 12.5337 | 1577.23 | -610.66 | 330.50 | 150.21 | **+636.07** | +6345.83 | +6981.90 |
| 09-29 | 30 | 12.3960 | 888.42 | -385.04 | 262.81 | 163.22 | **+240.57** | -991.62 | -751.05 |
| 09-30 | 100 | 11.4330 | 1932.53 | -743.28 | 355.66 | 122.49 | **+833.59** | -2111.09 | -1277.49 |
| 09-30 | 30 | 11.3611 | 1026.25 | -449.16 | 272.17 | 149.69 | **+304.92** | -964.39 | -659.47 |
| 10-01 | 100 | 14.4143 | 3552.56 | -1244.55 | 547.15 | 211.59 | **+1760.86** | +12946.09 | +14706.90 |
| 10-01 | 30 | 14.0802 | 1764.53 | -621.42 | 410.79 | 242.44 | **+732.33** | +2824.65 | +3556.97 |

**The first rule passes, in both sizes and in every window: the quoting margin -- capture less markout less every
fee including the minimum per order -- is positive in all sixteen rows**, averaging 940.59 a window at the pinned
size with a standard deviation of only 382.24, where the two symbols this section is named after are -181 a window.
**The quoting edge is out of sample**, and it is the first claim in this document that a week of windows supports
rather than one.

**The second rule depends on the size, and the pre-registration did not say which size it applied to -- that was an
omission and it is recorded as one.** At the pinned size the eight nets average +4905.77 with a median of +4635.13,
so both halves pass. At the matched size they average +743.68 but the median is -542.82, which is rule 3: the
profit is carried by outliers, so the exposure cap is mandatory rather than optional. Both readings are true
statements about their own size.

**What the campaign also measures, and this is the part that decides the line, is that the edge is much smaller than
the noise it sits in.** Over the eight windows the quoting margin sums to **+7524.69** while the position term sums
to **+31721.57**, and their dispersions per window are **382.24** and **10587.96** -- the noise is eleven times the
edge. Separating an edge of 940.59 from a noise of 10587.96 at two standard deviations needs about **506 windows**,
or two years of daily two-hour sessions, which is a smaller number than the 3600 to 26000 the AAPL arithmetic
produced and a larger one than the 23 the matched-exposure estimate hoped for. The eight windows' own total,
+39246.13, is 81 percent position.

**So the campaign's answer is: the edge is real, the line is not yet a business, and the thing to buy next is not
more of the same symbol.** Eight windows of one symbol in a week that trended one way cannot separate a 940 a
window edge from a 10588 draw, and the rule the campaign was written to test -- whether the quoting covers its own
schedule -- is answered yes, sixteen times over, which is the answer the fee section has been looking for since the
fee was found to be larger than a tick.

**The Avellaneda and Stoikov question has now been asked of this engine too, and the answer is that the shift as
this engine implements it makes the ratio worse rather than better.** The inventory-aware strategy shifts its quote
centre by the position's fraction of the inventory limit, capped in ticks, which is the reservation price of that
model in the form the engine can use; the recorded sweep runs the same eight MU windows at the same size and spread
with a shift of one, two and six ticks against none:

| setting | fills a day | units a day | quoting margin USD, summed | the position term USD, summed | net USD, summed |
|---|---:|---:|---:|---:|---:|
| none | 867 | 8,861 | +3096.26 | +2853.20 | **+5949.46** |
| shift 1 | 1,132 | 11,320 | +3289.57 | -1524.38 | +1765.19 |
| shift 2 | 1,244 | 12,434 | +3676.55 | -5631.03 | -1954.48 |
| shift 6 | 1,411 | 14,104 | +4166.65 | -9678.39 | **-5511.74** |

**The shift raises the quoting margin by 35 percent and the position term by more.** It brings 59 percent more
units through without dropping the capture below 10.7 ticks a unit, so the margin grows -- and the position term
grows from +2853.20 to -9678.39, which is a swing of 12531.59 on a sample whose margin moved by 1070.39. Per unit
of fill the ratio goes from 4.4 ticks of margin against 4.0 of position to 3.7 against 8.6, so what the shift buys
in volume it pays for in inventory.

**The most useful part of the result is why, and it is a property of the unit rather than of the model.** The
shift is given in ticks and the quote spacing is one tick, so a six-tick shift moves the reducing side six half
spreads toward the mid -- an aggressiveness the model's optimum never asks for, since its reservation price moves
by a fraction of the spread. **The Avellaneda and Stoikov version of this is therefore not tested by this sweep**,
and what it would need is recorded rather than guessed: the same eight windows at a spread of five to ten ticks,
with the shift held at or below half of it. That is free and it is the next run this question should have.

**What the sweep does establish is that the position term is not a placement artefact.** Moving where the quotes
sit, by six times the spacing, changes the margin by a third and the position term by five times the sample's whole
margin, which is the strongest evidence yet that the term dominating every window in this document is the market's
move across whatever inventory the strategy is carrying, and not something a quote can be placed away from.

**It has since been run the way the model asks, and it closes the quote-placement axis rather than opening it.** The
same eight windows at a half spread of five and of ten ticks, each with the shift held at a quarter and at half of
it, since MU's median spread at the trades is 29 ticks and a five tick half spread is therefore still inside the
market's own spread:

| setting | fills | units | quoting margin USD | position term USD | net USD |
|---|---:|---:|---:|---:|---:|
| half spread 1, no shift | 5,989 | 70,885 | +3096.26 | +2853.20 | +5949.46 |
| half spread 5, no shift | 4,732 | 58,314 | +2827.96 | -337.33 | +2490.61 |
| half spread 5, shift 2.5 | 6,314 | 76,899 | +3402.41 | -2927.88 | +474.54 |
| half spread 10, no shift | 3,394 | 43,277 | +2360.32 | +940.00 | +3300.30 |
| half spread 10, shift 2.5 | 4,416 | 55,070 | +2887.20 | -1697.18 | +1190.02 |
| half spread 10, shift 5 | 4,666 | 58,845 | +3153.85 | -2615.19 | +538.65 |

**A sum is not the measure, and reading this table by its sums is a trap the first pass fell into.** The ratio of
the two summed columns is 8.38 for a five tick spread against 1.09 for a one tick spread, which reads as a sevenfold
improvement, and it is an artefact: the position term's *sum* is near zero in that row because this sample's eight
draws cancelled, not because its risk is smaller. What decides whether an edge is measurable is the dispersion of
the position term against the mean margin, and the same eight rows give:

| setting | margin a window, mean | margin, standard deviation | position term a window, mean | position term, standard deviation | windows at two standard deviations |
|---|---:|---:|---:|---:|---:|
| half spread 1 | +387.03 | 164.30 | +356.65 | **4145** | **458** |
| half spread 5 | +353.50 | 158.00 | -42.17 | **3363** | **362** |

**So quoting five ticks out instead of at the touch buys about twenty percent, not seven hundred percent**, and the
shift that is the model's own mechanism makes both columns worse at every width it was tried at: at a five tick
spread, adding a 2.5 tick shift raises the margin by 574.45 and the position term by 2590.55. This is consistent
with the one tick sweep above and it points the same way -- a more aggressive reducing side buys volume near the
touch, where the fills are, and pays for it in the inventory it does not reduce.

**That closes the axis.** The quote's distance, its width, its shift, the size, the exposure cap, the kill switch
and the fee schedule have between them moved the ratio by twenty percent in the best case, from 458 windows to 362,
against a target that needs it below one. Whatever decides this line is not on this list, which is the finding the
two sweeps were run to produce: **the position term is the market's move across the inventory a quoting strategy
must carry to earn its spread, and no quoting parameter changes either of the two terms it is made of.** The lever
that is left is the venue, where the same arithmetic comes out differently, and `docs/ROADMAP.md` now carries the
test for one rather than a preference between them.


## The Passive Entry, Measured (2026-11-23)

`--trend-entry=side=own` puts the clip on our own side of the book, `ticks` inside the touch and never through it, and
`max_age` cancels an entry nobody hit. Three runs on the same two-hour window, `--order-size=100`, the equity schedule
(`min=0.35,regulatory=0.0000206,clearing=0.0002,audit=0.000003,rebate=0.0013`), `conservative`:

| run | order size | fills | fill rate | captured edge | markout a share | fees USD | of which the floor | net |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| AMD, own side | 100 | 722 | 1.1% | +7.63 ticks | -0.75 ticks | 229.24 | 45% | -0.28 bps |
| AAPL, own side | 100 | 652 | 2.9% | +1.67 ticks | -0.33 ticks | 157.68 | 53% | -0.69 bps |
| AMD, own side | 500 | 781 | 0.33% | +7.43 ticks | -0.29 ticks | 266.73 | 37% | -1.01 bps |
| AAPL, own side | 500 | 760 | 1.0% | +1.44 ticks | -0.05 ticks | 192.20 | 45% | -1.04 bps |
| TSLA, own side | 500 | 1,800 | 0.84% | +0.96 ticks | **+0.76 ticks** | 508.57 | 45% | -0.76 bps |
| AMD, touch (warning) | 100 | 11,597 | 55.7% | -12.74 ticks | +13.02 ticks | 6,779.98 | -- | -63,511.9 USD |

Four things the runs say, and the second is why the flag had to exist before the question could be answered.

1. **The passive entry works mechanically.** `taker_trade_count=0` in every own-side run: the clip rests, the fee is the
   maker rate plus the per-order floor, and nothing reaches the book as an aggressor.
2. **The fill is adversely selected on four of the five.** The capture at the fill is positive (+0.96 to +7.63 ticks) and
   the markout after it is negative on AMD and AAPL (-0.75, -0.33, -0.29, -0.05) and positive only on TSLA (+0.76). The
   conditional-fill bias is real and is now a printed field; TSLA is the one name where the move continued.
3. **The floor follows the fill, not the order.** Raising the clip from 100 to 500 shares left the floor at 37 to 45
   percent of the fee, because the conservative model fills about 24 shares per order -- so the order size does not move
   the floor and the rate is the whole cost only when the *fill* reaches 100 shares.
4. **The touch entry is a warning rather than a control.** Eleven thousand taker fills lose 56,732 USD gross on the same
   window, and the run reported `invalid_order_transitions=5889`. The cause was found afterwards and it is not the taker
   path: the engine reports a marketable limit as a fill and then as resting its remainder, and the ledger counted that
   second acknowledgement as a backwards transition. An acknowledgement no longer moves an order that has filled part of
   itself (`824588e`), the field is absent on the same window afterwards, and `queue_profile_orders` rises from 0 to
   5,889 -- the queue readings those acknowledgements carry, of which 517 joined a level that was already cleared. The
   56,732 USD is therefore a clean measurement of a taker entry, not a defective one.

By the criterion registered before the run -- net at least +2 bps, markout at least +4 bps, three windows -- this square
does not clear it, and the five runs say why in one line: **the gross is negative on every name** (-0.45 to -0.79 bps of
maker notional) while the fee is 2.2 to 3.2 bps, of which 37 to 53 percent is the per-order floor. So the passive entry
moved the question from "the fee eats a positive capture" to "the signal's capture is negative and the fee is on top",
which is the shape the low-turnover reversion line was already closed on. Five runs on three names with one session each
is still not the "three windows" the criterion asks for, so the recorded sentence is "the measured runs fail on the
gross and the markout", not "the family is closed".

```bash
python3 tools/study/fee_gate_table.py --input 'data/v2/equities/xnas_mbp10_*.csv*' --clip 1,100
out/build/linux-debug/toy_quant l2_replay \
    data/v2/equities/xnas_AMD_trades_2026-09-29_1330_1530.csv.gz \
    data/v2/equities/xnas_AMD_depth_2026-09-29_1330_1530.csv.gz AMD 0 trend_l2 1 conservative \
    --fast-validation --order-size=100 --inventory-limit=1000 --starting-cash=1000000 \
    --fee-schedule=min=0.35,regulatory=0.0000206,clearing=0.0002,audit=0.000003,rebate=0.0013 \
    --trend-entry=side=own,ticks=1,max_age=200,cooldown=60 --post-only
```

## The Ratio, Across the Markets This Repository Owns

One ratio decides the sign before any strategy does: the fee per side divided by the half-spread a fill can collect. It
is unit-free, so it compares a perpetual with an equity, and a tenth of a dollar with a cent:

| market | half-spread at the trades | fee per side | fee / half-spread |
|---|---:|---:|---:|
| BTC-USDT-SWAP, demo, live | 0.101 bps | 2.00 bps | **19.8** |
| SPY | 0.5 tick | 0.35 tick | 0.70 |
| AAPL | 1.5 ticks | 0.35 tick | 0.23 |
| TSLA | 2.5 ticks | 0.35 tick | 0.14 |

The first row is the instrument this project has been trading, and both of its numbers are measurements: the half-spread
is the `[WIND-DOWN] spread_bps=0.101008` of the 2026-11-17 live window, and the fee is the venue's 2 bps maker rate. The
last three come from `tools/study/screen_spread.py --input data/v2/equities/xnas_mbp10_<symbol>_2026-09-29_1330_1530.csv`
(symbols of 2.2M, 841k and 475k rows, 28,592, 55,052 and 49,368 trades), which computes the spread where the trades
print rather than the median of the book, and the default 0.0035 USD a share.

The screen's own pre-registered bar is this ratio the other way round -- a half-spread of at least three fees, i.e. a
ratio at or below 0.33 -- and the screen section above records it passing AAPL and TSLA and failing SPY. What the first
row adds is the size of the gap on the demo's instrument: **twenty half-spreads per fill**, so a maker that collected a
whole half-spread every time would still pay twenty times its best case. That is why its five live windows are all
negative and why a width sweep, a taker-free execution layer and the toxicity gates moved none of them: on that
instrument the fee is not paid out of the spread, it is twenty spreads.

These are commands rather than marked blocks: each reads a multi-million-row file and takes about twenty seconds, which
is the reason the walk-forward and the cascade pass are commands too. Both inputs are tracked, so every number here is
re-derivable from the command line beside it.

The two gates are joined in one command -- `tools/study/fee_gate_table.py`, which imports `screen_spread.measure` and
`venue_ratio.measure` rather than recomputing either:

```bash
python3 tools/study/fee_gate_table.py --input 'data/v2/equities/xnas_mbp10_*.csv*' --clip 1,100
```

| instrument | half spread at trades | fee a side | fee / half spread | spread gate | R_max | ratio gate |
|---|---:|---:|---:|---|---:|---|
| AAPL | 1.500 ticks | 0.350 ticks | 0.23 | pass | 1273 | pass |
| SPY | 0.500 ticks | 0.350 ticks | 0.70 | fail | 255 | pass |
| TSLA | 2.500 ticks | 0.350 ticks | 0.14 | pass | 1164 | pass |

Two things the join adds. **The gates disagree on SPY**, which is why both exist: its spread is not three fees wide
(0.70, a failure) while its capture against its own move clears the 71-print reference (255). A wide book that barely
moves is a different instrument from a tight one that moves. And the two ratios read different spreads on purpose:
`R_max` uses the median of the book -- SPY's two ticks -- while the spread gate uses the rows where trades print, one
tick, because a resting quote is filled where trades print rather than at the median of the book.

Run over the sixty screened windows (`--input 'data/v2/screen/*.csv'`, the same window and the same commission), the
filter narrows the universe to fourteen: **17 of 60 clear the spread gate, 33 clear the capture gate, 14 clear both**.
The fourteen are the names whose spread is several ticks wide -- MU 14.5 ticks (fee/half-spread 0.02, `R_max` 1479),
AMD 11.0 (0.03, 1084), COIN 8.0 (0.04, 541), SHOP 5.5 (0.06, 441), BA 5.0 (0.07, 392), UAL 4.75 (0.07, 279), MSTR and
PLTR 3.0 (0.12, 577 and 616), TSLA 2.5 (0.14, 1139), HOOD 2.5 (0.14, 347), DAL 2.0 (0.18, 119), AAPL and XOM 1.5 (0.23,
1205 and 265), GM 1.5 (0.23, 254). The one-cent books -- SPY at 0.5 ticks, AAL, AMC, KWEB, T, VXX and the rest -- fail
the spread gate however well their capture ratio reads, and twenty-four names fail both (BB, BOIL, EPI, TZA, VNM, VXX
and the rest of the one-cent list).

**The filter is necessary, not sufficient, and MU is the proof.** It passes both statics better than anything else in
the sixty -- 14.5 ticks of half-spread against 0.35 of fee, `R_max` 1479 against a 71 reference -- and it is also the
instrument whose *measured* windows were closed in [`docs/RETROSPECTIVE.md`](../RETROSPECTIVE.md): a static
half-spread is not the capture a quote keeps, and a screen that stopped at the statics would have reopened a line the
measurements had already closed. What the screen is for is the order to measure in, and on that order MU was already
first.




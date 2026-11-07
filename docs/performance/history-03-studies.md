# Performance History, part 3: the studies

*Part 3 of 3. The index is [`../PERFORMANCE_HISTORY.md`](../PERFORMANCE_HISTORY.md); part 1 is
[the engine and the L1 line](history-01-engine.md) and part 2 is [the L2 mainline](history-02-l2.md).*

This part is everything the instrument was pointed at once the mainline worked, in the order it was asked: the queue
distribution over orders, funding as an input, the acceptance number, the maker fee each window would have needed, the
decision layer that refuses, the market where the fee is not the wall, the instruments that passed the screen, what the
queue model decides, the non-market-making signal at two scales, the execution line, the pair trade, and the pre-trade
controls. Most of the closures in the [Retrospective](../RETROSPECTIVE.md) were measured here.

## Queue Distribution Over Orders

The one-order trace showed a quote joining a 7,200 contract queue and being cancelled with 169 contracts
still in front of it. That was an illustration, and the claim it supports is about every order rather than
about one, so the pipeline now profiles each order that rests: the queue in front of it when it rests, the
closest that queue came while the order was alive, and the queue remaining when the order left. Five summary
fields carry the aggregate. On the mainline window, quoting one tick wide and re-quoting after a four-tick
move:

| Field | Value | Meaning |
|---|---:|---|
| `queue_profile_orders` | 182 | Orders that rested and died, which is every submitted order in this window |
| `queue_zero_orders` | 21 | Orders whose queue in front reached zero at some point |
| `queue_min_fraction_p50` | 0.961474 | Median closest approach, as a fraction of the queue joined behind |
| `queue_min_fraction_p10` | 0 | Tenth percentile of the same ratio |
| `queue_end_fraction_p50` | 0.961474 | Median queue still in front when the order left |

The median quote never came within four percent of the front, and the tenth percentile is zero, which is the
21 fills and nothing else: on this window no order reached the front of its level and went unfilled, so being
first in line was sufficient to trade and the fill rate is a statement about how long the strategy waits
rather than about where it quotes.

Two implementation facts are load-bearing, and both are covered by the pipeline test. The end-of-life queue
is read **before** a cancel is issued rather than from the `Cancelled` report, because the engine erases a
price level once no order rests in it: a reading taken after the order leaves would report an empty queue for
a level that was not empty, and the two percentiles above would both collapse to zero. And a fill sets the
closest approach to zero by construction, because a fill is what happens when the queue reaches zero, which
is why `queue_zero_orders` can never be smaller than the fill count.

The measurement is cheap: the audit map holds open orders only, so sampling every level we are resting in
costs a handful of map lookups per event, on a stage that is 0.5% of the event already.

**This changed the default summary, and the reference hash was re-recorded.** The five fields are printed
unconditionally, so `tools/gate/verify_l2.sh full` failed until the reference was refreshed. The re-record was
taken deliberately and justified by proving the difference was exactly those fields: stripping them from the
new output and hashing the remainder reproduces the previous reference, `079a955a`, byte for byte over the
1.5 GB stream, and the tier 1 invariants on both the L2 and the L1 paths are unchanged. The only new
interaction with the engine is a `const` accessor, so no run can behave differently because of this. A reader
holding an older cached reference should run `tools/gate/verify_l2.sh ref` once; the file lives in `/tmp` and is
not tracked, which is why the change is recorded here rather than in the diff.

## Funding Rates as an Input

The carry line's second step, and the first time a rate series reached the ledger: a replay now pays or receives
funding on whatever position it holds when a settlement arrives. Two free sources publish the series and they
disagree about nearly everything except the sign convention, which is why both are recorded here: each
difference is the kind that shifts a day or a factor without failing.

| Source | Archive | What its file day is |
|---|---|---|
| OKX `module=3` (`tools/fetch/fetch_funding_rates.py --source okx`) | one CSV per day for **every** swap on the venue | midnight **UTC+8**: the file named `2026-01-07` opens at `2026-01-06 16:00` UTC |
| Binance Vision `fundingRate` | one CSV per symbol per **month**; there is no daily folder, unlike `premiumIndexKlines` | UTC, and each row declares its own interval (`8` hours for BTCUSDT) |

Two runs over these files answer different questions and both are recorded, because the funding line reports
two different things.

**A window inside a month, with no position yet.** The five minute Binance fixture the L1 path already
documents starts at `2024-03-30 00:00:00.014` UTC, which is *after* 88 settlements of the March file. The replay
applies every one of them at its first event, against an account that holds nothing, so the count and the money
have to disagree:

<!-- toyquant:check
run: replay data/v2/fixtures/test_aggTrades_5k.csv data/v2/fixtures/test_bookTicker_5k.csv BTCUSDT 0 l1 1000000 --funding-rates=data/v2/funding/BTCUSDT-fundingRate-2024-03.csv
then: funding_points=93 funding_span_hours=736 funding_sum_rate=0.0314019 funding_annualized_rate=0.373751 funding_settlements=88 funding_flow_usd=0
-->

**Ten minutes around one settlement, with a position.** A Binance day file is cut on UTC midnight, which is
itself a settlement instant, so a window that begins with the day cannot show the mechanism. The slice therefore
starts at `2024-03-28 07:55` UTC and ends at `08:05`, which contains the 08:00 settlement after ten minutes of
quoting. The same command with and without the funding file is the A/B: the fills and the fees are identical and
the funding is the only difference in the account.

<!-- toyquant:check
run: replay /tmp/btc_trades_settle.csv /tmp/btc_bbo_settle.csv BTCUSDT 0 l1 1000000
then: events=78477 trade_reports=111 fees_paid=1.25542 realized_pnl=-1.37681 equity=998.122
run: replay /tmp/btc_trades_settle.csv /tmp/btc_bbo_settle.csv BTCUSDT 0 l1 1000000 --funding-rates=data/v2/funding/BTCUSDT-fundingRate-2024-03.csv
then: events=78477 trade_reports=111 fees_paid=1.25542 funding_settlements=83 funding_flow_usd=0.226775 realized_pnl=-1.15004 equity=998.349
-->

What the two runs establish:

- **The flow is positive because the position was short.** The published rate is what longs pay, the accounting
  takes the rate a long earns, and the `l1` strategy was net short when the settlement arrived, so 0.226775 USD
  was paid to the account rather than charged to it. Realized PnL moved from -1.37681 to -1.15004, which is that
  amount exactly, and equity moved with it from 998.122 to 998.349.
- **83 of the 93 settlements were reached**: the 82 before the window, applied to an empty account, and the one
  inside it, applied to a position. The ten after the window are why `funding_points` and `funding_settlements`
  are printed side by side.
- **The month is the carry number the acceptance criterion needs.** 93 settlements over 736 hours sum to
  0.0314019 and annualize to 0.373751: a long holding BTCUSDT through March 2024 paid about 37% of its notional
  to stay long. That is the base a hedged position has to beat after its own fees, and no fee schedule in this
  repository is near it, which is the first statement the carry line makes that the market-making line could
  not.
- **The venue's settlement clock is not exact.** Several `calc_time` values end in `...000001`, so the eight hour
  interval holds to within a second rather than exactly. `tests/funding_rate_test.cpp` asserts that tolerance
  with the reason attached, because a strict equality would have been a red test about Binance's clock.
- **The OKX day boundary needs both files.** Its funding archive is cut on UTC+8, so a range cut on UTC spans two
  of them, and the book archive for `2026-01-07` opens exactly at the `00:00` UTC settlement that the funding
  file named `2026-01-07` also contains. A replay on one file therefore reaches the settlements up to its own
  start and none after it.

The reader costs nothing measurable: it reads 93 rows once, and the ten minute settlement window replays in the
same 0.2 s as the five minute fixture. Both runs leave every recorded number in this document untouched, because
the funding path is entered only through `--funding-rates` and a run without it prints exactly what it printed
before.

## The Acceptance Number

The carry line's criterion, in its own words: **annualized carry against every fee the position pays**, on real
data. Two instruments of the same asset on two venues, one long and one short, held over the span their funding
histories share, and opened and closed at the venues' published taker rates. Binance BTCUSDT and OKX
BTC-USDT-SWAP in March 2024, 10,000 USD a leg:

<!-- toyquant:check
run: carry data/v2/funding/BTCUSDT-fundingRate-2024-03.csv data/v2/funding/okx_funding_2024-03-01_2024-03-31_BTC-USDT-SWAP.csv --symbol-a=BTCUSDT --symbol-b=BTC-USDT-SWAP --notional-usd=10000
then: carry_gross_notional_usd=20000 carry_net_notional_usd=0 carry_window_hours=728 carry_settlements=182 carry_leg_a_funding_usd=-304.656 carry_leg_b_funding_usd=322.241 carry_funding_usd=17.5849 carry_fees_usd=18 carry_net_usd=-0.415093 carry_annualized_carry_usd=211.599 carry_annualized_return=0.0211599 carry_to_fees=11.7555 carry_breakeven_hours=745.184
run: carry data/v2/funding/BTCUSDT-fundingRate-2024-03.csv data/v2/funding/okx_funding_2024-03-01_2024-03-31_BTC-USDT-SWAP.csv --symbol-a=BTCUSDT --symbol-b=BTC-USDT-SWAP --notional-usd=10000 --long=b
then: carry_leg_a_funding_usd=304.656 carry_leg_b_funding_usd=-322.241 carry_funding_usd=-17.5849 carry_net_usd=-35.5849 carry_annualized_carry_usd=-211.599 carry_to_fees=-11.7555 carry_breakeven_hours=0
-->

What it says, and it is not one thing:

- **The carry is the difference between the two venues, and it is small against what flows.** 304.66 USD was paid
  on Binance and 322.24 received on OKX, so the hedge cancels 94.5% of the funding it takes on and leaves 17.58
  USD of actual carry on 10,000 USD a leg. Either of the gross figures is what a report that showed one leg's
  funding would invite a reader to quote, and neither of them is the carry.
- **The ratio is above one and the month still lost 0.415 USD.** The annualized carry is 211.60 USD against 18.00
  USD of fees, a ratio of 11.76; but the fees are paid once while the carry accrued over 728 hours, which is 17
  hours short of the 745 the position needed to pay for them. Both numbers are in the line because they answer
  different questions, and a reader who takes the ratio alone reports a winner that the same line's net calls a
  loser by 0.42 USD.
- **The direction is the whole result.** Held the other way round, the identical position pays 17.58 USD of
  funding instead of earning it and loses 35.58 USD against the same 18.00 USD of fees, with a negative ratio and
  no breakeven at all. The two legs are one position and which side of it is long is a decision; the venues'
  funding difference is the only margin, and it is 2.1% annualized rather than the 37% a single venue's funding
  suggests.
- **The fee binds here too, but not the way it bound the market-making line.** There, a fee was 2.57 ticks
  against 0.088 captured and no lever moved the ratio. Here the fee is 0.18% of the notional and the carry is a
  rate against it, so the holding period, a maker entry instead of a taker one, or a wider difference between the
  venues each change the answer: this position needs 31.0 days to pay for itself and was held for 30.3. A margin
  of half a day is the honest reading of a criterion that asks whether this line has more to say than the last
  one, and the answer is that it has a number where the other line had none, and that the number is thin.

The second run's fields are the first run's with the sign flipped. [`DATA.md`](../DATA.md) records where the two funding
files come from, one command each, and both blocks are re-derived by `tools/gate/check_docs.py` on every gate run.

## The Maker Fee Each Window Would Have Needed

The market-making line's binding claim has always been about the fee: at a one-tick spread the fee is 2.57 ticks
against 0.088 captured. That comparison assumes the loss *is* the fee, and a reader is entitled to the other half
of the question: which maker rate would have made these windows profitable, and is it one a venue offers? The
answer needs two numbers the summary did not report, the PnL before fees and the notional the fees were charged
on, and both are now in every run's `[PNL]` line.

`closed_pnl_usd` is what completed, so a position the run was still holding is not in it; `net_pnl_usd` adds that
position marked at the last price; the `gross_` pair adds the fees back to each, and a negative gross PnL says the
run lost money before the venue charged anything, which no fee tier can fix. `maker_notional_usd` turns that into
a rate: `breakeven_maker_rate` is the maker fee at which the window would have been exactly flat, and a negative
one means the venue would have had to pay a rebate instead.

`tools/sweep/fee_sweep.sh` runs the sweep and prints the table with the block that re-derives every number. The three
rates are the venue's own maker fee, zero, and a rebate half the size of the fee:

| window | maker rate | fills | net USD | gross USD | closed USD | gross/fill USD | breakeven rate |
|---|---:|---:|---:|---:|---:|---:|---:|
| 0401 incremental 200k | 0.0002 | 21 | -0.123813 | -0.0818127 | -0.109283 | -0.00320398 | -0.000389584 |
| 0401 incremental 200k | 0 | 21 | -0.0818127 | -0.0818127 | -0.0672835 | -0.00320398 | -0.000389584 |
| 0401 incremental 200k | -0.0002 | 21 | -0.0398127 | -0.0818127 | -0.0252835 | -0.00320398 | -0.000389584 |
| 0401 snapshot 200k | 0.0002 | 146 | -0.757038 | -0.465038 | -0.757038 | -0.00318519 | -0.000318519 |
| 0401 snapshot 200k | 0 | 146 | -0.465038 | -0.465038 | -0.465038 | -0.00318519 | -0.000318519 |
| 0401 snapshot 200k | -0.0002 | 146 | -0.173038 | -0.465038 | -0.173038 | -0.00318519 | -0.000318519 |
| 0501 snapshot 200k | 0.0002 | 58 | -0.287468 | -0.171468 | -0.287468 | -0.00295634 | -0.000295634 |
| 0501 snapshot 200k | 0 | 58 | -0.171468 | -0.171468 | -0.171468 | -0.00295634 | -0.000295634 |
| 0501 snapshot 200k | -0.0002 | 58 | -0.055468 | -0.171468 | -0.055468 | -0.00295634 | -0.000295634 |
| 0401 incremental 2M | 0.0002 | 267 | -1.39317 | -0.859172 | -1.38723 | -0.00319563 | -0.000321787 |

<!-- toyquant:check
run: l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_200k_base.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --maker-fee-rate=0.0002
then: trade_reports=21 net_pnl_usd=-0.123813 gross_pnl_usd=-0.0818127 closed_pnl_usd=-0.109283 gross_closed_per_fill_usd=-0.00320398 breakeven_maker_rate=-0.000389584
run: l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_200k_base.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --maker-fee-rate=0
then: trade_reports=21 net_pnl_usd=-0.0818127 gross_pnl_usd=-0.0818127 closed_pnl_usd=-0.0672835 gross_closed_per_fill_usd=-0.00320398 breakeven_maker_rate=-0.000389584
run: l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_200k_base.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --maker-fee-rate=-0.0002
then: trade_reports=21 net_pnl_usd=-0.0398127 gross_pnl_usd=-0.0818127 closed_pnl_usd=-0.0252835 gross_closed_per_fill_usd=-0.00320398 breakeven_maker_rate=-0.000389584
run: l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/snap_20200401_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --maker-fee-rate=0.0002
then: trade_reports=146 net_pnl_usd=-0.757038 gross_pnl_usd=-0.465038 closed_pnl_usd=-0.757038 gross_closed_per_fill_usd=-0.00318519 breakeven_maker_rate=-0.000318519
run: l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/snap_20200401_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --maker-fee-rate=0
then: trade_reports=146 net_pnl_usd=-0.465038 gross_pnl_usd=-0.465038 closed_pnl_usd=-0.465038 gross_closed_per_fill_usd=-0.00318519 breakeven_maker_rate=-0.000318519
run: l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/snap_20200401_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --maker-fee-rate=-0.0002
then: trade_reports=146 net_pnl_usd=-0.173038 gross_pnl_usd=-0.465038 closed_pnl_usd=-0.173038 gross_closed_per_fill_usd=-0.00318519 breakeven_maker_rate=-0.000318519
run: l2_replay data/v2/crypto/deribit_trades_2020-05-01_BTC-PERPETUAL.csv.gz /tmp/snap_0501_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --maker-fee-rate=0.0002
then: trade_reports=58 net_pnl_usd=-0.287468 gross_pnl_usd=-0.171468 closed_pnl_usd=-0.287468 gross_closed_per_fill_usd=-0.00295634 breakeven_maker_rate=-0.000295634
run: l2_replay data/v2/crypto/deribit_trades_2020-05-01_BTC-PERPETUAL.csv.gz /tmp/snap_0501_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --maker-fee-rate=0
then: trade_reports=58 net_pnl_usd=-0.171468 gross_pnl_usd=-0.171468 closed_pnl_usd=-0.171468 gross_closed_per_fill_usd=-0.00295634 breakeven_maker_rate=-0.000295634
run: l2_replay data/v2/crypto/deribit_trades_2020-05-01_BTC-PERPETUAL.csv.gz /tmp/snap_0501_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --maker-fee-rate=-0.0002
then: trade_reports=58 net_pnl_usd=-0.055468 gross_pnl_usd=-0.171468 closed_pnl_usd=-0.055468 gross_closed_per_fill_usd=-0.00295634 breakeven_maker_rate=-0.000295634
run: l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_2m.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --maker-fee-rate=0.0002
then: trade_reports=267 net_pnl_usd=-1.39317 gross_pnl_usd=-0.859172 closed_pnl_usd=-1.38723 gross_closed_per_fill_usd=-0.00319563 breakeven_maker_rate=-0.000321787
-->

Three things follow, and the first is not the one this line has been repeating:

- **The fee is not what loses the money at a tight quote.** Before any fee is charged, every window loses between
  0.00296 and 0.00320 USD per fill, which is three to four ticks a fill, against a capture of 0.1 to 0.3 ticks.
  `breakeven_maker_rate` is negative on all four windows, between -0.000296 and -0.000390, which says the venue
  would have had to *pay* a rebate larger than its own maker fee for these windows to break even. At zero maker
  fees the largest sample is still down 0.86 USD over 267 fills.
- **Widening the quote does not pass through a profitable middle.** On the largest window, asking for two ticks
  halves the loss per fill and asking for four reverses the improvement, while the capture rises from 0.088 to
  0.367 ticks: the fills that arrive at a wider quote are worse per fill, not better, which is what the markout
  buckets and the out-of-sample corner had already said in a different form.
- **The capture finally beats the cost at eight ticks, with one fill.** There the capture is 6.5 ticks and
  `breakeven_maker_rate` turns positive at +0.000515, above the venue's 0.0002, so the window is up 0.0051 USD —
  but nothing closed in it, so that gain is the mark on the position the run is still holding, and one fill is not
  evidence. The honest form of this row is that the door exists and its width is unknown.
- **So the constraint is the fill population, not the schedule.** A maker is profitable when the capture per fill
  covers the fee plus the adverse selection it suffers, and on this instrument that second term is three to four
  ticks at every width where fills actually happen. That is why the next step is a rule that refuses a quote
  unless the expected fill covers both, rather than a hunt for a cheaper venue: at 0.0032 USD of loss a fill, no
  fee schedule on earth makes the tight-quote population profitable.

| quote width | fills | captured ticks/fill | gross USD | gross/fill USD | breakeven rate |
|---:|---:|---:|---:|---:|---:|
| 1 tick | 267 | 0.088015 | -0.859172 | -0.00319563 | -0.000321787 |
| 2 ticks | 207 | 0.0966184 | -0.480513 | -0.00232323 | -0.000232132 |
| 4 ticks | 15 | 0.366667 | -0.0452864 | -0.00315111 | -0.000301909 |
| 8 ticks | 1 | 6.5 | 0.00514688 | 0 | 0.000514688 |

<!-- toyquant:check
run: l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_2m.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=2 --refresh-price-ticks=4 --maker-fee-rate=0.0002
then: trade_reports=207 captured_edge_per_unit_ticks=0.0966184 gross_pnl_usd=-0.480513 gross_closed_per_fill_usd=-0.00232323 breakeven_maker_rate=-0.000232132
run: l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_2m.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=4 --refresh-price-ticks=4 --maker-fee-rate=0.0002
then: trade_reports=15 captured_edge_per_unit_ticks=0.366667 gross_pnl_usd=-0.0452864 gross_closed_per_fill_usd=-0.00315111 breakeven_maker_rate=-0.000301909
run: l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_2m.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=8 --refresh-price-ticks=4 --maker-fee-rate=0.0002
then: trade_reports=1 captured_edge_per_unit_ticks=6.5 gross_pnl_usd=0.00514688 gross_closed_per_fill_usd=0 breakeven_maker_rate=0.000514688
-->

### What the window's inputs actually cover

Every number in this section is a replay of "the 200k slice", and that phrase hid two facts about the inputs until
the run was made to say them. The depth slice is the first 200,000 updates of the day, which is **quarter of an
hour**; the trade file is the whole day. The replay reads both to the end, so 117,903 of its 119,237 prints arrive
after the book has stopped moving: the strategy can no longer requote, because a quote cycle begins with a book
update, and the midpoint the prints are scored against is frozen rather than wrong.

`[L2 DATA]` now says which case a window is in, splitting the prints by *why* they were not comparable:

| window | prints | no current midpoint | beyond 5 bps of one |
|---|---:|---:|---:|
| 200k depth slice | 119,237 | 117,889 | 9 |
| the full day's depth | 119,237 | 0 | 554 |

<!-- toyquant:check
run: l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_200k_base.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --maker-fee-rate=0.0002
then: events=236525 stale_trades=117889 dislocated_trades=9
-->

Three things follow, and the first is the one that matters for reading everything above:

- **The fills are all inside the live book.** The 21 fills of this configuration run from 1585699244 to 1585700097,
  and the depth slice ends at 1585700098: the last fill is 1.2 seconds before the book stops. So the *measurement*
  is not made against a frozen book, even though 99% of the prints are — which is the difference between a caveat
  and a defect, and the reason the field is printed rather than the window redefined.
- **The nine prints beyond five basis points are consistent with the tick, and the 554 of the full day are 0.46% of
  it.** On a 0.5 tick against 6,358, one tick is 0.78 bps, so a print eight ticks from the midpoint is a sweep that
  crossed several levels rather than a bad row. The threshold is not a filter: nothing is dropped, because dropping a
  print would move every fill in the run instead of describing it.
- **A window whose inputs cover the same span says nothing at all.** On the full day's depth the stale count is zero
  and the field is absent, which is what keeps the line honest: the token appears exactly where the coverage gap is.

The tier-2 reference was re-recorded for this, from `270d8085` to `d66e88e0`. The change is one token on the
`[L2 DATA]` line and nothing else: deleting ` dislocated_trades=554` from the new output reproduces `270d8085` byte
for byte over the 1.5 GB stream, which is how the re-record is justified rather than asserted.


### The interval each of those numbers carries

Every rate above is one number from one window, and the rule this document applies by hand -- a fill count below
thirty is not a conclusion -- fails on all of them: the four windows here produced 267, 207, 15 and 1 fills. The
maker break-even gap quoted further down between the two Deribit perpetuals is worse, because it compares two
single windows and reads a 6.9 bps difference out of them.

So the number's own uncertainty is measured rather than asserted. `tools/study/pnl_interval.py` takes a series, resamples
the mean out of it, and prints the interval, the median, the range and a flag for the thirty-fill rule; its window
mode replays the windows itself rather than reading a saved log, so the series cannot go stale. The series chosen
here differs by the month and by nothing the measurement chose: the same configuration as the table above, one
25-level snapshot slice per month at 200k rows, five months of the archive.

| month | slice covers | fills | break-even maker rate | captured edge | net per fill |
|---|---:|---:|---:|---:|---:|
| 2020-04-01 | 0.49 h | 146 | -3.19 bps | 0.086 ticks | -0.0052 USD |
| 2020-05-01 | 0.59 h | 58 | -2.96 bps | 0.035 ticks | -0.0050 USD |
| 2020-06-01 | 0.86 h | 31 | -1.93 bps | 0.242 ticks | -0.0039 USD |
| 2020-07-01 | 2.16 h | 29 | -0.86 bps | 0.379 ticks | -0.0029 USD |
| 2020-08-01 | 0.62 h | 26 | -0.44 bps | 0.462 ticks | -0.0024 USD |

<!-- toyquant:check
tool: python3 tools/study/pnl_interval.py --window btc-months
then: n=5 mean=-1.87368 median=-1.93129 ci95_lo=-2.84287 ci95_hi=-0.904496 spread=2.74983 includes_zero=0 sign_consistent=1 thin_sample=1
-->

<!-- toyquant:check
tool: python3 tools/study/pnl_interval.py --window btc-months --window-field captured_edge_per_unit_ticks --unit ticks
then: n=5 mean=0.240576 median=0.241935 ci95_lo=0.096171 ci95_hi=0.384726 spread=0.427055 includes_zero=0 sign_consistent=1 thin_sample=1
-->

<!-- toyquant:check
tool: python3 tools/study/pnl_interval.py --window btc-months --window-field net_per_fill_usd --unit usd
then: n=5 mean=-0.00387368 median=-0.00393129 ci95_lo=-0.00484287 ci95_hi=-0.0029045 spread=0.00274983 includes_zero=0 sign_consistent=1 thin_sample=1
-->

The last column is the same replay's third answer, and it is the one with the least room to argue: a fill costs between
two and five tenths of a cent on every day of the series.

The second column of each row is the same replay's other half: `--window-field` summarises any `name=value` a run prints,
so the capture side is measured on the same days at the same setting rather than quoted from a different run.

Six things follow, and the first two change how the rest of this section should be read:

- **The binding ratio is not one window's accident, which is the point of measuring the second column.** The five free
  days give the capture side as well as the rate, and the capture is 0.086, 0.035, 0.242, 0.379 and 0.462 ticks against a
  fee worth 2.57. On every day the policy earns between **1.2% and 18% of the fee it pays**, and even the best day is
  5.6 times short — so the sign of the conclusion survives the month, and so does the ratio, which is a stronger
  statement than the break-even rate alone supports. The two columns move together (April worst on both, August best on
  both), which is the month effect appearing in both rather than a second finding. What the second column cannot say is
  whether the month is the cause: that is the slice-length question, and the clock-cut series below answers it.

- **The number is not one number, and the interval is narrower than the thing it estimates.** The mean is -1.87 bps
  with a 95% interval of -2.84 to -0.90, while the five draws themselves span 2.75 bps, from -3.19 to -0.44, and
  they fall in order. An interval says how well the mean is pinned; it says nothing about the spread of the
  population, and here the spread is the finding: a window's answer depends on the month more than on anything the
  strategy does. The median is printed beside the mean for the same reason -- on five points with a trend, the mean
  is a point on a slope pretending to be a level.
- **The 6.9 bps gap is larger than the month effect, and that is all that can be said for it.** Moving from April to
  August moves one instrument by 2.75 bps, which is 40% of the gap the two-window comparison reports. The gap's
  sign survives every window measured here, since BTC is worse than ETH on both same-day pairs; its size does not,
  because a difference between two single windows carries both instruments' month effects and nothing in the data
  separates them afterwards.
- **The pair that controls for the day is the right design and has two draws.** BTC minus ETH on the same day, same
  file type, same length, is -3.54 bps over 200k rows and -1.35 over 2M. Both negative, so the direction is
  consistent; the interval over two points is the range between them, so nothing else is. That is what "recorded as
  a hint" should have meant from the start, and the honest version of the hint is a direction rather than a number
  to two decimals.
- **The slice length is a confound, and the section below removes it rather than only naming it.** A 200k-row slice
  covers half an hour in April and more than two hours in July, because the archive is a stream of rows rather than of
  time, so the five draws differ by the month *and* by how much of the day the slice happened to cover. The series is
  still the honest one available at this length -- the alternative is one window and no interval at all -- but it is a
  series over windows, not over months, and a reader who takes it as a month effect is reading more than was measured.
  Bounding the windows by the clock instead is what says how much of it was the slice, and the answer is not "none":
  the ordering two bullets up does not survive it.
- **The floor numbers need no interval, which is the useful contrast.** The 5.39 bps a crossing costs is fee plus
  half a tick and is arithmetic: run it twice and it is the same to the last digit, so its interval has zero width
  and quoting one would be theatre. Intervals belong to the numbers that are estimates over a sample, and the value
  of the distinction is that it sorts this document's claims into two piles that were previously one.

### The same five days cut by the clock

The fifth bullet above names the slice length as a confound, which is not the same as removing it: `rows` is not the only
thing a window can be bounded by. The same tool cuts by **time** as well, keeping the first half hour of each day, and
thirty minutes is the closest round span to what the largest slice above already covers
-- April's 200k rows are 0.49 h -- so April barely moves and the other four are cut down to meet it, which is the
direction a confound has to be removed in. The row count then becomes the thing that varies, and it varies by a factor
of 2.8: the same half hour carries 204k rows in April and 73k in July.

| day (first 0.5 h) | rows | fills | break-even maker rate | captured edge | net per fill |
|---|---:|---:|---:|---:|---:|
| 2020-04-01 | 204,139 | 149 | -3.14 bps | 0.094 ticks | -0.0051 USD |
| 2020-05-01 | 174,264 | 53 | -3.52 bps | -0.038 ticks | -0.0055 USD |
| 2020-06-01 | 138,032 | 27 | -1.38 bps | 0.222 ticks | -0.0034 USD |
| 2020-07-01 | 72,807 | 17 | **+0.05 bps** | 0.353 ticks | -0.0020 USD |
| 2020-08-01 | 156,549 | 23 | -2.26 bps | 0.326 ticks | -0.0043 USD |

<!-- toyquant:check
tool: python3 tools/study/pnl_interval.py --window btc-months-span
then: n=5 mean=-2.04956 median=-2.25572 ci95_lo=-3.11574 ci95_hi=-0.875152 spread=3.569 includes_zero=0 sign_consistent=0 thin_sample=1
-->

<!-- toyquant:check
tool: python3 tools/study/pnl_interval.py --window btc-months-span --window-field captured_edge_per_unit_ticks --unit ticks
then: n=5 mean=0.191495 median=0.222222 ci95_lo=0.0613679 ci95_hi=0.310685 spread=0.390677 includes_zero=0 sign_consistent=0 thin_sample=1
-->

<!-- toyquant:check
tool: python3 tools/study/pnl_interval.py --window btc-months-span --window-field net_per_fill_usd --unit usd
then: n=5 mean=-0.00404956 median=-0.00425572 ci95_lo=-0.00511574 ci95_hi=-0.00287515 spread=0.003569 includes_zero=0 sign_consistent=1 thin_sample=1
-->

Four things follow, and the first is a correction to what the series above appeared to show:

- **Removing the confound moves the answer, and one draw crosses zero.** The mean goes from -1.87 to -2.05 bps and the
  spread widens from 2.75 to 3.57, but the change that matters is July: its break-even rate stops being the best of a
  bad set and becomes **+0.05 bps**, so the tool's `sign_consistent` flag falls from 1 to 0 and it prints that the
  draws do not all fall on one side of zero. The ordering the row-count series showed -- "they fall in order" from -3.19
  to -0.44 -- does not survive at equal time: -3.14, -3.52, -1.38, +0.05, -2.26 is not a slope. Part of that trend was
  the slice length, so the caveat the series carried ("a series over windows, not over months") was the right one, and
  the way to find out how much was to remove the confound rather than to describe it again.
- **The capture shows the same crack, one day wide.** May's capture is -0.038 ticks, which is a loss per contract on a
  half hour of quoting, and it is the same crack the break-even column shows in July: one draw of five against the sign
  the other four share. What the fee comparison loses in tidiness it keeps in margin: the best capture is 0.353 ticks
  against a fee worth 2.57, so even at equal time the policy earns at most 14% of what it pays, and the conclusion that
  the fee is a multiple of the edge does not depend on the sign of a draw.
- **What survives, and it is the claim the dead-ends table rests on, is the money.** Net per fill is negative on all
  five days, from -0.0055 to -0.0020 USD, mean -0.0040 with an interval of -0.0051 to -0.0029, and the flag agrees. A
  day that samples +0.05 bps is 0.00005 USD a fill on a 10 USD contract — five thousandths of a cent, against the two
  tenths of a cent that same day loses at the fee it actually pays — so the honest way to report it is the flag the tool
  prints rather than a rounding to the sign the other four days have.
- **The cost is fills, which is why both series stay.** July drops from 29 fills to 17 and May from 58 to 53, so the
  controlled design buys its control with sample size -- and on five draws that are each under thirty fills, neither
  series is a conclusion. The half-hour series is the one that answers "is this the month or the slice?", and the
  row-count series is the one the rest of this document quotes, because those are the runs recorded elsewhere.

What is not here is a per-fill interval. A fill's own PnL is not on disk: the trade report carries price, quantity,
side, fee and liquidity role, so a per-fill series would have to be reconstructed by matching trades, which is a
second implementation of the portfolio's accounting and would disagree with it exactly when it mattered. Adding a
PnL column to the trade report is a change to the binary and is recorded as still open in `docs/ROADMAP.md`.


**The reference hash was re-recorded.** The `[PNL]` line is printed unconditionally and the `[Mode]` line now
echoes the maker rate, so `tools/gate/verify_l2.sh full` failed until the reference was refreshed. The re-record is
justified by proving the change is exactly those two additions: stripping the `[PNL]` line and the
`maker_fee_rate=` token from the new output reproduces the previous reference `f95d1c98` byte for byte over the
1.5 GB stream. The new reference is `cfabcd88`, and a reader holding a cached one should run
`tools/gate/verify_l2.sh ref` once.

## The Decision Layer Refuses, and the Position Is Not the Profit

F1 measured that at a tight quote the fee is not what loses the money: the gross PnL is already negative, so the
fills themselves are the cost. Two changes follow from that, and both came out differently from what they were
built for, which is why both are recorded.

### The adverse-selection gate

`--markout-cover` (l2_replay, active_l2) makes the decision layer require a quote to cover the maker fee **and**
what passive fills at that quote's distance from the midpoint have actually cost. The first version measured that
cost from the venue's own prints — every trade, resolved five cycles later, as if we had been resting on the
passive side — and the record is why the decision now uses our own fills instead. Both rows come from the same
run, whose `[GATE]` line reports them side by side:

| what is measured | buy near | buy far | sell near | sell far | samples |
|---|---:|---:|---:|---:|---:|
| the venue's prints, as if we had been passive | 0.0539353 | 1.05413 | 0.0305481 | -0.189796 | 12683 |
| our own fills | 0.333333 | -0.7 | 0.285714 | -0.75 | 17 |

A print at a distance and a quote at that distance being hit are not the same event, and on this window they
disagree in sign in the far bucket: the prints say being passive there earned a tick, our fills say it cost
seven tenths of one. **A measurement of the market is not a measurement of us**, which is why the gate is built
on our own fills, and why it waits for twenty of them before tightening at all — a gate that tightened from its
first decision would refuse everything and never learn anything.

With the gate on, on the two million record window and at the venue's own maker fee:

| quote width | fills | net USD | gross USD | breakeven rate | refusals |
|---:|---:|---:|---:|---:|---:|
| 1 tick | 21 | -0.0920398 | -0.0500398 | -0.000238285 | 9709 |
| 2 ticks | 17 | -0.302228 | -0.268228 | -0.00157781 | 0 |
| 4 ticks | 7 | **+0.0963399** | **+0.11034** | **+0.00157628** | 0 |

<!-- toyquant:check
run: l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_2m.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --markout-cover
then: trade_reports=21 net_pnl_usd=-0.0920398 gross_pnl_usd=-0.0500398 breakeven_maker_rate=-0.000238285 markout_gate_skips=9709
run: l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_2m.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=2 --refresh-price-ticks=4 --markout-cover
then: trade_reports=17 net_pnl_usd=-0.302228 gross_pnl_usd=-0.268228 breakeven_maker_rate=-0.00157781 fill_markout_buy_far_ticks=-0.7 fill_markout_sell_far_ticks=-0.75 fill_markout_samples=17 market_markout_buy_far_ticks=1.05413 market_markout_samples=12683
run: l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_2m.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=4 --refresh-price-ticks=4 --markout-cover
then: trade_reports=7 net_pnl_usd=0.0963399 gross_pnl_usd=0.11034 breakeven_maker_rate=0.00157628 markout_gate_skips=0
-->

The ungated rows of the same window are in the F1 block above: 267 fills at one tick with a gross of -0.859172,
207 at two with -0.480513 and 15 at four with -0.0452864. Read together, the gate does two things and they are
worth separating:

- **Refusing the quotes that cannot pay is most of the improvement.** At one tick the fill count falls from 267 to
  21 and the gross loss from 0.859 to 0.050 USD, a seventeen-fold reduction in what that population loses, and the
  four-tick row turns positive with a breakeven rate eight times the venue's own fee.
- **The measured part is a refinement this sample cannot evaluate.** The gate's own success starves its estimator:
  the more it refuses, the fewer fills resolve, and seven to twenty-one fills is below the twenty the gate waits
  for. What is established here is the mechanism and its direction, not a number of its own.

### The flatten band

`--flatten-band=N` prices an order at the touch on the reducing side as soon as the position passes N units, so the
engine fills it as a taker instead of the position being carried into the final mark. On the two hundred thousand
record window, ten contracts a quote and a limit of forty:

| band | fills | max abs inventory | flatten orders | flatten quantity | taker fees | gross USD | net USD |
|---:|---:|---:|---:|---:|---:|---:|---:|
| off | 11 | 21 | 0 | 0 | 0 | **+0.315192** | **+0.107192** |
| 10 | 21 | 20 | 3 | 21 | 0.105 | -0.619526 | -1.08253 |
| 5 | 25 | 15 | 6 | 46 | 0.23 | -0.378591 | -0.986591 |

<!-- toyquant:check
run: l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_200k_base.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=2 --refresh-price-ticks=4 --order-size=10 --inventory-limit=40
then: trade_reports=11 max_abs_inventory=21 taker_fees=0 gross_pnl_usd=0.315192 net_pnl_usd=0.107192
run: l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_200k_base.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=2 --refresh-price-ticks=4 --order-size=10 --inventory-limit=40 --flatten-band=10
then: trade_reports=21 max_abs_inventory=20 flatten_orders=3 flatten_quantity=21 taker_fees=0.105 gross_pnl_usd=-0.619526 net_pnl_usd=-1.08253
run: l2_replay data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_200k_base.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=2 --refresh-price-ticks=4 --order-size=10 --inventory-limit=40 --flatten-band=5
then: trade_reports=25 max_abs_inventory=15 flatten_orders=6 flatten_quantity=46 taker_fees=0.23 gross_pnl_usd=-0.378591 net_pnl_usd=-0.986591
-->

The band does what it says — the peak position falls from 21 units to 15 as the band tightens — and it costs more
than it saves: 0.105 to 0.23 USD of taker fees plus the spread on every flattening order. And the one positive row
is the row that carried its position: 210 USD of exposure into a favourable final mark is where its +0.107 came
from, and removing the position removes the profit. **The position paid, not the quoting.** That is the same
statement the closed-versus-total split makes from the other side, and it is why the flatten band stays off by
default.

### What this leaves

Both mechanisms work, and neither makes the quoting profitable on this instrument: at every width where fills
happen at all, the capture does not cover the fee plus what the fills cost. That is the argument for the last step
rather than another attempt at this one — a market whose spread is wide relative to its fee, where this same
decision layer has something to capture in the first place.

## The Market Where the Fee Is Not the Wall

The last two sections end on the same sentence: on a perpetual the fee is 2.57 ticks against a capture of 0.095,
so nothing about the quoting can matter until the instrument changes. Two markets where the fee is a *fraction*
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
of the row the flatten-band section above ends on. Picking that cell after seeing the others is exactly the
window selection this repository refuses to do.

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
1.000 spread where trades print, which is the pair the F4 section above measured from ten levels of depth, and its
**3.97 trade prints a second is the number the activity floor's own table was derived from**. A screen that
disagreed with those two would have been the thing to question, and it does not.

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
-1,297.89, so about 1,795 USD of drift. The drift is not adverse selection -- AAPL's measured markout is *positive*
(+0.043 ticks), so the fills were not picked off -- it is the mark on 1,096 and 1,098 shares of inventory,
0.36 and 0.39 million USD of notional, over two hours in which the price moved against it by a few tenths of a
percent. At that size a 0.9% move is 3,300 USD and the whole quoting edge is 385.

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
the F4 table above was measured with an explicit limit of 1,000 shares, which is the setting that made the exposure
term large enough to dominate; the same measurement at the harness's default would have shown a smaller number for
SPY as well, and a different sign on it.

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

The caveats the F4 section records apply to every row above, and there is one more. The book is **Nasdaq's alone**,
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

## What the queue model decides, and what the per order floor does

Two questions were left open by the equities rows above, and both are answered from files that were already
on disk.

**The first is which model the tape supports.** `conservative` lets only a trade move a queue; `prorata`
lets a level's whole decrease move it. The attribution is the one `tools/study/calibrate_queue_model.py` already
makes for the perpetuals -- every level decrease split into the part the trades explain and the part they do
not -- run on the same two hours of AAPL and of AMD:

| window, 400,000 depth rows | decrease events | explained by trades | therefore cancelled |
|---|---:|---:|---:|
| AAPL 2026-09-29 | 201,696 | 450,041 units, **5.0 %** | 8,639,668 units, **95.0 %** |
| AMD 2026-09-29 | 202,318 | 380,161 units, **4.0 %** | 9,115,442 units, **96.0 %** |

**One unit in twenty that leaves a price level leaves because it traded; the other nineteen are cancelled.**
So `conservative` is not a conservative model of a US equity book, it is a model of a different book, one in
which nobody ever cancels: a floor by a factor rather than by a shade, which is a stronger statement than the
one F4 could make when it called its row "a floor rather than the window's answer".

**The second is whether the sign survives the whole schedule, and here the sentence has to be narrowed
rather than repeated.** F4 says the model "decides the sign as well", and that was measured against one
side's commission, 0.35 of a tick. Under the account's own terms -- commission, clearing, the audit fee, the
SEC fee on the proceeds of a sale, the credit for adding liquidity and the minimum per order -- the same
window, policy and clip come out:

| queue model | fills | units | capture ticks/unit | markout ticks/unit | fee ticks/unit | of which the floor | closed USD | net USD |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| conservative | 2,294 | 59,601 | 0.655 | +0.033 | 0.936 | 0.354 | -2046.48 | -2046.19 |
| prorata | 4,614 | 108,667 | 1.020 | -0.272 | 1.001 | 0.418 | -2206.57 | -2201.62 |
| lumpy | 4,304 | 105,769 | 1.032 | -0.281 | 0.985 | 0.402 | -2026.32 | -2016.91 |
| optimistic | 4,775 | 118,290 | 1.073 | -0.330 | 0.965 | 0.382 | -2188.78 | -2178.31 |

**The model doubles the fill count and moves the capture by 64 percent, from 0.655 to 1.073 ticks a unit, and
the two hours lose money in all four rows** -- closed between 2,026 and 2,207 USD. So the model decides how
many fills there are and what each one is worth, and against the whole schedule it does not decide the sign.
The capture the other three models find does clear the 0.35 commission the earlier sentence compared it with;
it does not clear the 0.94 to 1.00 a fill actually pays, and that is the whole difference between the two
claims.

**The fee term a policy can move is the floor, and it moves by requoting less rather than by quoting bigger.**
The clip first, `prorata` at the default refresh:

| clip | fills | units | capture ticks/unit | markout ticks/unit | fee ticks/unit | of which the floor | closed USD |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 4,614 | 108,667 | 1.020 | -0.272 | 1.001 | 0.418 | -2206.57 |
| 500 | 5,160 | 153,927 | 0.948 | -0.176 | 0.877 | 0.297 | -5058.14 |
| 1,000 | 5,152 | 164,599 | 0.922 | -0.132 | 0.856 | 0.278 | -8788.51 |
| 2,000 | 5,114 | 168,626 | 0.923 | -0.113 | 0.845 | 0.270 | -11485.20 |

Twenty times the clip takes the floor from 0.418 to 0.270 ticks a unit and the whole fee from 1.001 to 0.845,
so the floor **dilutes but never disappears**, and the reason is that its numerator is the number of orders that
end below it rather than the size of them: of the 44,537 orders two hours of this policy submits, about **2,000**
end below the floor whatever the clip is, because that count follows how often the quote is replaced and not how
much each one quotes. The refresh is the control that owns the term:

| refresh | order instances | fills | units | capture ticks/unit | markout ticks/unit | capture plus markout | fee ticks/unit | of which the floor | closed USD |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 97,094 | 2,778 | 68,743 | 1.258 | -0.479 | 0.779 | 1.184 | 0.603 | -1393.97 |
| 4 | 19,784 | 6,329 | 145,741 | 0.742 | +0.025 | 0.767 | 0.843 | 0.260 | -2586.43 |
| 16 | 11,545 | 7,236 | 163,034 | 0.389 | +0.358 | 0.747 | 0.769 | 0.187 | -3696.10 |
| 64 | 11,535 | 7,276 | 163,974 | 0.374 | +0.364 | 0.738 | 0.769 | 0.186 | -3786.17 |

**A slower requote buys a smaller fee by giving back exactly the capture it saves.** The floor falls from 0.603
to 0.186 ticks a unit -- a third of the fee at the fast end and a quarter at the slow end -- and the capture
falls with it while the markout rises, so **capture plus markout is 0.779, 0.767, 0.747, 0.738: flat**. The fee
is the only term in the table that moves independently, and what it buys is 0.415 of a tick, which takes the
gap between cost and edge from 0.405 to 0.031 ticks and leaves a loss between 1,394 and 3,786 USD. That is the
third time this document has found the same shape, and the first time on a venue where the fee is a fraction
of a tick rather than a multiple of one: **capture and markout are one number in two halves, and a policy that
changes one changes the other by the same amount.**

**What this does not show is that the tick fields add up to the money -- but the money does add up, and where it
comes from is the finding.** Four schedules on the same window and policy, with the capture and the markout identical
in all four because the fills are identical:

| schedule | fee USD | fee ticks/unit | of which the floor | gross closed USD | net USD |
|---|---:|---:|---:|---:|---:|
| the commission alone | 380.33 | 0.350 | 0 | -1119.04 | -1494.42 |
| the account's terms without the maker credit | 1228.80 | 1.131 | 454.77 | -1119.04 | -2342.89 |
| the account's terms with it | 1087.53 | 1.001 | 454.77 | -1119.04 | -2201.62 |
| the same, with no minimum per order | 632.77 | 0.582 | 0 | -1119.04 | -1746.86 |

**The maker credit is worth 141.27 USD a window and the minimum per order 454.77, and the column that decides the
line is the one that does not move.** The gross closed P&L is **-1,119.04 USD, or -1.03 ticks a unit, before any fee
at all**, in every row -- so at the best terms any broker in that table offers, the credit passed through and no
floor, the window still loses 1,746.86 USD, and at a fee of zero it would still lose 1,119.04. That is the same
sentence the OKX maker line produced at -0.64 and -0.99 basis points gross, in a fourth market: **the quoting is
negative before the fee, so the fee is not the term to bargain over.** It also settles a question that had been left
as an argument: no rebate arrangement, however favourable, can close a line whose fills lose money before they pay
anything.

**And the reconciliation that the paragraph above recorded as missing is this**: `net_pnl_usd` is the gross closed
P&L less `fees_paid`, to the cent (-1119.04 - 380.33 = -1499.37, and so on down the column), and the tick
decomposition does not add up to the gross because a window's closes include what a five-cycle markout cannot see.
The money column is the gross, and the tick fields are what explain it rather than a substitute for it.

**The volume tier is the largest fee lever this account has, and on a quoting strategy the floor takes it back.**
The broker prices its commission a share by monthly volume: 0.0035 up to 300,000 shares a month, 0.0020 to
3,000,000, 0.0015 to 20,000,000. The same window, the same fills and the same schedule at each of the three, in
ticks a unit:

| commission | tier | venue and schedule | the floor | total | net USD |
|---|---|---:|---:|---:|---:|
| 0.0035 | 1, to 300,000 shares | 0.582 | 0.418 | 1.001 | -2201.62 |
| 0.0020 | 2, to 3,000,000 | 0.432 | 0.569 | **1.001** | **-2201.62** |
| 0.0015 | 3, to 20,000,000 | 0.382 | 0.619 | **1.001** | **-2201.62** |

**The total is identical at all three rates and the net does not move by a cent.** The commission falls 0.15 of a
tick a tier and the floor rises by exactly the same, because this strategy's orders fill about 23.6 shares at a
time -- 108,667 shares over 4,614 fills -- and an order that fills a quarter of its size pays a quarter of the
commission a full one would, against the whole 0.35 floor. Raising the clip to where the floor would stop binding
at a *complete* fill, 175 shares at tier 2 and 234 at tier 3, does buy something and much less than the arithmetic
suggests: 1.001 ticks a unit becomes **0.881 and 0.847**, about 15 percent, because 128,583 shares over 4,897
fills is still 26 a fill. **The tier pays in full only where orders fill in full**, which is a taker or a factor
book rather than a quoter, and that is the whole difference between the two fee structures the research notes
compare.

**The tier and the clip decide between them, and the fee is smallest exactly where the money is worst.** Seven
runs on the same window: three tiers at the clip the tier would need for a *complete* fill and at the default, plus
the two tiers at a clip large enough for the floor to be small.

| tier | commission | clip | fee ticks/unit | of which the floor | gross closed USD | net USD |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 0.0035 | 100 | 1.001 | 0.418 | -1119.04 | -2201.62 |
| 2 | 0.0020 | 100 | **1.001** | 0.568 | -1119.04 | -2201.62 |
| 3 | 0.0015 | 100 | **1.001** | 0.618 | -1119.04 | -2201.62 |
| 2 | 0.0020 | 175 | 0.881 | 0.449 | -1619.28 | -2749.44 |
| 3 | 0.0015 | 234 | 0.847 | 0.465 | -2319.68 | -3477.88 |
| 2 | 0.0020 | 1,000 | **0.783** | 0.355 | -7378.88 | -8826.23 |
| 3 | 0.0015 | 1,000 | **0.766** | 0.388 | -7378.88 | -8798.13 |

**The fee falls 23 percent down the column and the money falls by four times**, because the clip that buys the
fee buys inventory with it: at a clip of 1,000 the position term is six times the one at a clip of 100, and the
gross closed P&L goes from -1,119 to -7,379. The two rows the tier was supposed to rescue -- 175 and 234 shares,
where the floor stops binding at a complete fill -- do cut the fee to 0.881 and 0.847, and they cost more in gross
than they save, so the net gets worse in both. **The fee is not the term a quoter should be optimising**, and this
grid is the fourth table in this document that says so from a different direction.

Reproduction: `tools/sweep/equity_schedule_sweep.sh` prints every table in this section from the two files on disk,
including the three volume tiers and the tier by clip grid, and the attribution is
`tools/study/calibrate_queue_model.py` on the same window -- the tool the perpetuals' queue model was calibrated with,
which is what makes the two venues' numbers comparable at all.



## The non-market-making signal: what 27 parameter sets say

`trend_l2` is not a market maker, so the questions are different: not where to quote but how much to hold, and
whether the signal that sets the target survives the cost of taking liquidity. `tools/sweep/trend_sweep.sh` replays the
same 200k-row 1 April slice for a grid of `--trend-params` with `--order-size=10 --inventory-limit=100`; every
run takes liquidity, pays the venue's fees and reads the same book. The CSV is the output; this is what the first
grid said.

| fast / slow | entry ticks | stop ticks | fills | net USD | fees USD |
|---|---:|---:|---:|---:|---:|
| 5 / 30 | 3 | 4 | 106 | -6.04458 | 5.30 |
| 5 / 30 | 2 | 4 | 366 | -21.0702 | 18.3 |
| 5 / 30 | 1 | 4 | 1,676 | -92.0967 | 83.8 |
| 10 / 60 | 3 | 4 | 174 | -10.3209 | 8.70 |
| 20 / 120 | 3 | 4 | 252 | -13.585 | 12.6 |
| 5 / 30 | 1 | 8 | 1,822 | -101.491 | 91.1 |

The grid is negative in every cell. The entry band is the dominant knob: at entry 1 the strategy takes 1,676 to
1,850 fills, at entry 2 366 to 550, and at entry 3 106 to 278; the loss follows the fill count because every fill
pays the spread and the taker fee. The best cell, 5/30 with a 3-tick entry and a 4-tick stop, still loses 6.04 USD
of which 5.30 is fees, so its gross loss is 0.74: the stop and the cooldown reduce the number of attempts but do
not create an edge. At entry 3 the fastest pair is best at every stop; at entry 1 the ordering flips with the stop
and the spread between the pairs is a few dollars against a loss near ninety. That is a statement about this
window, not a parameter to freeze. The next step is a signal with a forecast, or a cost structure in which a taker
cross can pay, not another sweep of the same three bands.

```bash
FAST_SLOW="5:30 10:60 20:120" ENTRIES="1 2 3" STOPS="4 6 8" bash tools/sweep/trend_sweep.sh
```

## The non-market-making signal at the scale it trades

The 27-cell grid above measured `trend_l2` at 5/30, 10/60 and 20/120 views. The 200k slice delivers 117,288
views over 838.5 seconds, so one view is 7.2 milliseconds and those averages are 36 milliseconds to about a
second: the grid was measuring a rule that could not hold a trend, because at its own horizon there was none to
hold. That does not make the grid wrong, but it makes it an answer about milliseconds. This section asks the
question the grid left, stated before the runs: **does the same signal, held for seconds rather than
milliseconds, earn enough before fees to clear the venue's taker fee, on two non-adjacent windows?** The
measurement is the gross per fill the run already reports, turned into the taker rate it would take to break
even.

Only the two averages, the time stop and the cooldown move; the entry band, the exit band, the stop, the sizing
and the queue model stay at the grid's best cell (`entry 3 ticks, exit 0.5, stop 4, cooldown 60 views,
--order-size=10 --inventory-limit=100`, conservative, holding `2 x slow`). `--metrics` supplies what the older
summary could not: `gross_pnl_usd` against `fees_paid`, the round-trip distribution, and the taker edge at 5, 20
and 60 cycles. Each window trades and reads the book of its own day, so the second is not one day's trades
against another day's depth.

| window | fast / slow | fills | gross USD | net USD | fees USD | gross / fill | fee / fill | break-even taker | win | mean hold | taker edge @60 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2020-04-01 | 200 / 1200 | 452 | +9.07 | -13.53 | 22.60 | 0.0201 | 0.0500 | 2.01 bps | 18.1 % | 7.4 s | -0.67 |
| 2020-04-01 | 500 / 3000 | 330 | +8.27 | -8.23 | 16.50 | 0.0251 | 0.0500 | 2.51 bps | 18.8 % | 25.8 s | -0.25 |
| 2020-04-01 | 2000 / 6000 | 150 | +3.79 | -3.71 | 7.50 | 0.0253 | 0.0500 | 2.53 bps | 28.6 % | 33.2 s | -0.31 |
| 2020-04-01 | 5000 / 15000 | 90 | +6.04 | **+1.54** | 4.50 | 0.0672 | 0.0500 | 6.72 bps | 25.0 % | 63.8 s | -0.43 |
| 2020-06-01 | 200 / 1200 | 840 | +6.23 | -35.77 | 42.00 | 0.0074 | 0.0500 | 0.74 bps | 2.4 % | 18.8 s | -0.29 |
| 2020-06-01 | 500 / 3000 | 760 | +7.13 | -30.87 | 38.00 | 0.0094 | 0.0500 | 0.94 bps | 10.5 % | 55.0 s | -0.29 |
| 2020-06-01 | 2000 / 6000 | 670 | +0.90 | -32.60 | 33.50 | 0.0013 | 0.0500 | 0.13 bps | 9.1 % | 59.8 s | -0.17 |
| 2020-06-01 | 5000 / 15000 | 510 | -0.62 | -26.12 | 25.50 | -0.0012 | 0.0500 | -0.12 bps | 4.0 % | 89.7 s | -0.43 |

`break-even taker` is the rate at which the gross would be exactly consumed: gross over the notional the fills
traded, in basis points. The venue charges 5 bps a side, which is the `fee / fill` column stated against a 100
USD fill, so a cell needs to be above that line to have a question rather than a cost.

**The answer is no, and the fee is not what decides it.** On 1 April the gross does scale with the horizon:
2.01 bps of break-even at a 7-second hold, 2.53 at 33 seconds, and 6.72 at 64, where the run is the only one of
the eight whose net is positive (+1.54 USD over 90 fills). On 1 June the same cells give 0.74, 0.94, 0.13 and
-0.12 bps; the long cell's gross turns negative and the shortest cell's win rate falls from 18.1 % to 2.4 %.
Two non-adjacent windows disagreeing is the continuity check this project's own bar asks for, and it fails. The
grid's twelve sub-second cells were hopeless because they paid two fees per thirty-six milliseconds; these are
hopeless because the edge that existed on one day did not exist on the other.

The taker edge at sixty cycles (about 0.43 s) is negative in all eight runs, from -0.17 to -0.67 ticks. That is
not a contradiction with the April long cell: the drift it earned arrived over a minute, two orders of
magnitude beyond the markout horizon. The two measurements answer two questions, and reading them together is
the point of having both -- the short horizon prices the crossing, and the round trip prices the signal.

**The taker variant of this line is closed by non-replication.** A maker entry would remove roughly 0.8 bps of
half spread and replace the 5 bps taker rate, which cannot turn a negative gross positive: on 1 June the gross
is already below zero at the long end and near it everywhere else. A next attempt needs a signal whose gross
survives more than one window, which is a different measurement with its own bar.

```bash
# one cell on one window; the second window is the same command with the 2020-06-01 files
# depth slices: head -n 200001 of the incremental file, zcat | head -n 200001 of the 06-01 snapshot
for cell in 200,1200 500,3000 2000,6000 5000,15000; do
  fast=${cell%%,*}; slow=${cell##*,}
  out/build/linux-debug/toy_quant l2_replay \
    data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_200k_base.csv \
    BTC-PERPETUAL 0 trend_l2 1 conservative --fast-validation --paper-broker \
    --order-size=10 --inventory-limit=100 \
    --trend-params=$fast,$slow,3,0.5,4,60,$((slow * 2)) --metrics
done
```

## The execution line: does a schedule beat one crossing?

The two closed lines both end at a cost, so this one stops asking for a direction and works an order instead.
`execution_l2` takes a parent -- side, quantity, deadline -- and sends crossing children, and the pipeline
measures the parent against the midpoint it arrived at, which is the benchmark a desk reports against when it
has no view. The question was stated before the runs: **at five basis points a side, does spreading a parent
over its deadline lower implementation shortfall against a single crossing, on two non-adjacent windows?**

The parent is 1,000 contracts (10,000 USD of face value) over 600 seconds of market time. Three schedules:
`market` (one child, the control), `twap:10` (ten equal children) and `pov:0.1` (children sized to a tenth of
the volume printing). Each window trades and reads the book of its own day.

| window | schedule | children | filled | ratio | is_filled | is_fees | is_opportunity | is_total |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| 2020-04-01 | market | 1 | 1,000 | 1.00 | 0.39 | 5.00 | 0.00 | 5.39 |
| 2020-04-01 | twap:10 | 10 | 910 | 0.91 | 11.74 | 4.55 | 0.98 | 17.27 |
| 2020-04-01 | pov:0.1 | 5 | 1,000 | 1.00 | 1.89 | 5.00 | 0.00 | 6.89 |
| 2020-06-01 | market | 1 | 1,000 | 1.00 | 0.26 | 5.00 | 0.00 | 5.26 |
| 2020-06-01 | twap:10 | 10 | 920 | 0.92 | -9.82 | 4.60 | 1.57 | -3.66 |
| 2020-06-01 | pov:0.1 | 5 | 1,000 | 1.00 | -0.76 | 5.00 | 0.00 | 4.24 |

Every figure is a basis point of the notional at the arrival midpoint, positive meaning money paid away.

**The schedule is not the lever, and the two windows disagree by more than the schedules differ.** `twap` beat
the single crossing by 8.92 bps on 1 June and lost 11.88 on 1 April; `pov` moved 1.02 and -1.50. The
pre-registered rule -- two basis points better on both windows with the same sign, opportunity under a quarter
of the total -- is met by neither. What is stable is the fee: 4.6 to 5.0 bps in every cell, three to thirteen
times the differences between schedules. The next subsection shows why the two windows disagree, and it is not
the schedule.

The reason is in the filled term. A single crossing fills completely on both days, because 1,000 contracts is
small against the displayed depth, so its cost is the half spread (0.26 to 0.39 bps) plus the fee. `twap`
holds the same order open for ten minutes, which is long enough for the price to drift: the same schedule paid
11.74 bps on the first day and was *paid* 9.82 on the second. At this size scheduling adds variance without
lowering the mean. A parent large enough that one crossing cannot take it would have to be spread, and that is
a different question with its own pre-registration; it is recorded as the open increment rather than run here.

The opportunity term behaved as designed: 0.98 and 1.57 bps on the two `twap` cells that ended with nine
percent of the parent unfilled, and zero where the parent completed. It is small here only because the parent
was almost filled, and a schedule that trades less is charged proportionally, which is what keeps "work
nothing" from looking cheap.

### The same runs against the TWAP benchmark

The arrival price answers "what did this order pay relative to the moment it was decided", which is the right
question for one decision and the wrong one for comparing schedules: a parent that buys into a rising midpoint
pays more the longer it waits, whatever the schedule is. `[EXECUTION_ALGO]` therefore also reports the
time-weighted midpoint of the parent's own window, and the same three terms against it.

| window | schedule | filled | price vs TWAP | opportunity vs TWAP | fees | total vs TWAP |
|---|---|---:|---:|---:|---:|---:|
| 2020-04-01 | market | 1,000 | -11.92 | 0.00 | 5.00 | -6.92 |
| 2020-04-01 | twap:10 | 910 | +0.52 | -0.13 | 4.55 | +4.94 |
| 2020-04-01 | pov:0.1 | 1,000 | -10.43 | 0.00 | 5.00 | -5.43 |
| 2020-06-01 | market | 1,000 | +7.79 | 0.00 | 5.00 | +12.79 |
| 2020-06-01 | twap:10 | 920 | -2.90 | +2.17 | 4.60 | +3.86 |
| 2020-06-01 | pov:0.1 | 1,000 | +6.77 | 0.00 | 5.00 | +11.77 |
| 2020-04-01 | twap:10, 100k parent | 76,360 | +0.01 | -0.34 | 3.82 | +3.49 |
| 2020-06-01 | twap:10, 100k parent | 79,460 | -3.17 | +5.57 | 3.97 | +6.37 |

**The benchmark check passes, and it changes what the table above means.** `twap` lands within 3.2 basis points
of the average it is measured against in all four of its cells, which is the half spread it paid and nothing
else: the schedule does what it says it does. The single crossing does not. Its price against the benchmark is
-11.92 bps on one day and +7.79 on the other, and those are the window's own drift with the sign flipped,
because it trades at the arrival while the benchmark keeps moving. `pov` behaves like a fast `twap` -- five
children is most of the way to "trade now" -- so it inherits most of the same bet.

So the choice is not "which schedule is cheaper". It is **whether to take a ten basis point bet on where the
window goes, or to pay a stable cost of about four and a half basis points**, of which 4.55 to 5.00 is the fee
and roughly half a basis point is the spread the children cross. The same holds at 100,000 contracts, where a
single crossing is not enough to absorb the order on the thinner day: `twap` stays within three points of the
benchmark there too, and the next subsection shows what size does change and what it does not.

**What this leaves.** The only term with a stable sign is still the fee: this venue charges 5 bps to cross
against 2 bps to rest. With the schedule's own price cost now measured at about half a basis point, the
maker-entry question is worth roughly 3.5 bps -- 3 of fee and half a spread -- against the half point a
schedule can win, which is why it is the next step and scheduling is not.

```bash
for algo in market twap:10 pov:0.1; do
  out/build/linux-debug/toy_quant l2_replay \
    data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_200k_base.csv \
    BTC-PERPETUAL 0 execution_l2 1 conservative --fast-validation \
    --parent=buy:1000:600 --algo=$algo --metrics
done
```

### Posting instead of crossing: the other side of the fee

The schedule's half point is small because the fee is not in it. The other way to look for the three basis
points between the taker and the maker rate is to change how a slice is sent, so the same parent was re-run with
`--algo-entry=post:N`: each slice rests at the near touch and whatever it has not filled after N seconds is
crossed. The question was stated before the runs: **does a resting entry save more in fees than it loses in
units it never acquires?**

| window | entry | filled | ratio | fees | vs TWAP | opportunity vs TWAP |
|---|---|---:|---:|---:|---:|---:|
| 2020-04-01 | cross | 1,000 | 1.00 | 5.00 | +5.29 | -0.00 |
| 2020-04-01 | post:5 | 1,000 | 1.00 | 5.00 | +5.44 | -0.00 |
| 2020-04-01 | post:30 | 800 | 0.80 | 3.40 | +5.86 | -0.29 |
| 2020-04-01 | post:60 | 800 | 0.80 | 3.40 | +5.71 | -0.29 |
| 2020-06-01 | cross | 1,000 | 1.00 | 5.00 | +3.74 | +0.00 |
| 2020-06-01 | post:5 | 800 | 0.80 | 3.40 | +3.57 | +5.42 |
| 2020-06-01 | post:30 | 700 | 0.70 | 2.60 | +6.50 | +8.14 |
| 2020-06-01 | post:60 | 600 | 0.60 | 1.80 | +9.22 | +10.85 |

All conservative-queue runs on the same 200k slices; the prorata model moved the crossing cells by less than a
tenth of a basis point and is reported in the change log rather than in a second table.

**The answer is no, and patience makes it worse rather than better.** A five-second rest almost never fills --
its fee is still 5.00 bps on 1 April, which is the tell that every child crossed -- and once the timeout is long
enough for the queue to reach us, the fee does fall (5.00 → 3.40 → 1.80) while the units acquired fall with it
(100 % → 80 % → 60 %) and the opportunity cost rises faster: 0.00 → 5.42 → 10.85 bps against the benchmark on 1
June. The maker rate saves up to 3.2 bps on what fills and gives back more than that on what does not, and no
timeout on either window ends up better than crossing.

That is the same statement as the schedule measurement, from the other side: a resting order only escapes the
taker fee by not trading, and the parent still has to be filled. The fee is a floor on this venue, not a
parameter to be optimised away by patience.

**What this closes.** The execution line now has three chapters and one conclusion: the schedule pays a stable
half point and can win at most that; the entry cannot buy back the fee without giving up fills; and the
remaining question is not execution at all but a venue whose maker and taker rates differ by less than the fill
risk costs. A parent larger than the displayed depth is still an open increment, because there the crossing
choice stops being available -- but that is a capacity question with its own pre-registration.

```bash
for entry in cross post:5 post:30 post:60; do
  out/build/linux-debug/toy_quant l2_replay \
    data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_200k_base.csv \
    BTC-PERPETUAL 0 execution_l2 1 conservative --fast-validation \
    --parent=buy:1000:600 --algo=twap:10 --algo-entry=$entry --metrics
done
```

### Capacity: what size changes and what it does not

The open question after the two measurements above was whether a parent larger than the displayed depth flips
the answer, because that is where a single crossing stops being available. The ladder below is the same parent
and the same window at six sizes, in the conservative queue model, with the number of children each schedule
needed to complete it.

| window | size | `market` children | `market` vs TWAP | `twap:10` children | `twap:10` vs TWAP |
|---|---:|---:|---:|---:|---:|
| 2020-04-01 | 1,000 | 1 | -6.92 | 19 | +5.29 |
| 2020-04-01 | 5,000 | 1 | -6.92 | 25 | +5.29 |
| 2020-04-01 | 10,000 | 1 | -6.92 | 25 | +5.29 |
| 2020-04-01 | 20,000 | 2 | -6.92 | 26 | +5.29 |
| 2020-04-01 | 50,000 | 3 | -6.92 | 29 | +5.29 |
| 2020-04-01 | 100,000 | 6 | -6.92 | 34 | +5.29 |
| 2020-06-01 | 1,000 | 1 | +12.79 | 14 | +3.74 |
| 2020-06-01 | 50,000 | 1 | +12.79 | 94 | +3.88 |
| 2020-06-01 | 100,000 | 1 | +12.79 | 97 | +3.89 |

**Size does not change the cost in basis points, and the reason is dimensional.** Every term of the shortfall
is proportional to the quantity: the fee is a rate, the spread is a price per unit, and the drift is a price per
unit. Doubling the parent doubles every term and the basis point figure does not move -- the two columns are
flat to the last digit across two orders of magnitude. What size does change is the number of crossings a
schedule needs: one on the deep day, six on the thin one, and thirty-four to ninety-seven for the time-sliced
schedule, because its slices fill in pieces and are worked again.

So there is no size at which the schedule starts winning, and the earlier question is answered: a parent larger
than the displayed depth does not flip the comparison, it simply requires more crossings, each of which the
five basis point fee already prices. The one number that looked like a capacity limit -- a single crossing
filling nineteen percent of a hundred thousand lot parent -- was a run with an older scheduler that made exactly
one attempt; the current one keeps working the order and completes at every size in the table, which is what a
desk would do and what makes the comparison meaningful.

```bash
for size in 1000 5000 10000 20000 50000 100000; do
  out/build/linux-debug/toy_quant l2_replay \
    data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_200k_base.csv \
    BTC-PERPETUAL 0 execution_l2 1 conservative --fast-validation \
    --parent=buy:$size:600 --algo=twap:10 --metrics
done
```

## The pair trade, checked before building it

The next structural direction was a second instrument. The data question turned out to be trivial: Tardis
publishes free samples of the first day of every month without a key, ETH-PERPETUAL included, at
`datasets.tardis.dev/v1/deribit/<type>/<year>/<month>/<day>/<symbol>.csv.gz` -- 22 MB of depth and 181 KB of
trades for one day, in exactly the schema the L2 parser already reads. So before writing a multi-symbol
pipeline, the cheap question is whether the ratio is worth its cost.

The rule was fixed before the run: a rolling z-score of the ETH/BTC ratio over the window, enter at |z| > 2,
exit at |z| < 0.5 or after five windows, both legs taken with crossing orders, and the round trip charged at
28 bps -- 5 bps of taker fee and about 2 bps of half spread on each leg, in and out.

| window | entry | trades (1 Apr / 1 Jun) | median net (1 Apr / 1 Jun) | gross captured | win rate |
|---|---|---:|---|---:|---|
| 60 s | z > 2 | 1,089 / 1,058 | -25.79 / -26.45 bps | +2.2 | 0.1 / 0.3 % |
| 300 s | z > 2 | 415 / 321 | -22.53 / -23.78 | +5.5 | 1.2 / 2.2 % |
| 900 s | z > 2 | 194 / 119 | -20.08 / -21.25 | +7.9 | 1.5 / 0.8 % |
| 900 s | z > 3 | 74 / 62 | -17.60 / -21.18 | +10.4 | 5.4 / 3.2 % |

**The gross capture grows with the window and stops near ten basis points, against a cost of twenty-eight.**
The fastest setting is thirteen times short; the slowest and most selective one is 2.7 times short. That gap
is the whole answer, and it is not the answer the amplitude suggested: the ratio moves 16 bps at the median
over a minute and 83 over ten, which looked like room, but a simple rule collects about a fifth of that
amplitude and pays for a crossing on both legs twice.

The maker version is the only variant left, and the earlier posting measurement is the reason not to be
hopeful about it: resting at the touch gave up twenty to forty percent of the fills and the opportunity cost
of what it missed exceeded the fee it saved. A pair that needs both legs to fill passively is that problem
squared.

**So the multi-instrument line stays closed, and now it is closed by a measurement rather than by missing
data** -- the data costs 22 MB and ten minutes to obtain, and the trade is three times short of its own fee.
What would reopen it: a venue where a pair round trip costs under ten basis points with fills this venue does
not offer, or a spread whose reversion is an order of magnitude larger than this one's.

```bash
# the two trade files are the whole measurement; no build, no pipeline
curl -sS -O 'https://datasets.tardis.dev/v1/deribit/trades/2020/04/01/ETH-PERPETUAL.csv.gz'
python3 tq_pair_rule.py 900 3.0 data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz \
  ETH-PERPETUAL.csv.gz data/v2/crypto/deribit_trades_2020-06-01_BTC-PERPETUAL.csv.gz ETH_0601.csv.gz
```

### The tick curve, in five points

Registering a second instrument answers a question the BTC numbers cannot: how much of the floor is the fee and
how much is the spread. `ETH-PERPETUAL` trades on the same venue under the same 5 bps taker rate, but its tick is
five cents against a price near 132 rather than half a dollar against 6421, so a crossing pays 1.88 bps of half
spread instead of 0.39. Three more points come from the futures venue, whose symbols differ in tick by a factor of
ten thousand, so the curve can be drawn rather than inferred: `BTCUSDT` at ten cents, `XRPUSDT` at a hundredth of a
cent, `DOGEUSDT` at a hundredth of a millicent, all on the same 4 bps taker rate and the same market day.

| venue | instrument | tick, in bps | half a tick | `is_filled` | `is_fees` | floor |
|---|---|---:|---:|---:|---:|---:|
| Binance USD-M | BTCUSDT | 0.0144 | 0.0072 | 0.0072 | 4.00 | 4.0072 |
| Binance USD-M | DOGEUSDT | 0.524 | 0.262 | 1.346 | 4.00 | 5.3465 |
| Deribit | BTC-PERPETUAL | 0.78 | 0.39 | 0.39 | 5.00 | 5.39 |
| Binance USD-M | XRPUSDT | 1.633 | 0.816 | 0.816 | 4.00 | 4.8169 |
| Deribit | ETH-PERPETUAL | 3.76 | 1.88 | 1.88 | 5.00 | 6.88 |

Four of the five land on half a tick to three decimals, and the fifth is the interesting one. `DOGEUSDT` pays 1.35
bps of half spread, five ticks rather than one, and the run says why: the parent needed forty-five children. The
reason is in the quote rather than in the tick -- the best ask on that book held thirty-six DOGE, seven dollars --
so a parent of any size a venue would accept walks the ladder. Shrinking the parent to seventy dollars still costs
0.73 bps over two children, and the touch it is crossing is the same seven dollars. So the depth term is not a
small correction on that book, it is the whole cost, and the formula is `fee + max(half a tick, impact)`.

That distinction is also why the capacity ladder on the perpetual read flat: the ladder varied the parent from ten
thousand to a million dollars of notional and the shortfall did not move, because that book is deep enough for the
largest rung. A flat ladder is a statement about the book, not about size in general, and `DOGEUSDT` is the
counter-example that shows what the ladder would have looked like on a thin one.

Across the four points that are tick-bound, the fee moves by one basis point and the tick by a factor of 260, so the
floor spans 4.0 to 6.9 -- it is a property of the instrument and the venue's fee schedule, and of nothing the
schedule does. The futures point also settles that the half-spread term is not an artefact of thin crypto books: on
a market that quotes a hundredth of a basis point wide, a crossing costs its fee and no more. The two perpetuals
then read as one instrument quoted on two venues, where the coarser tick costs 1.4 bps more to cross and the higher
fee 1.0.


The fee term is identical across the two Deribit runs, as it must be; the spread term scales with the tick, and the
floor moves by 1.5 bps for the same venue, the same schedule and the same parent notional. The drift and the
benchmark behave the same way as they do on BTC: the single crossing sits 29 bps away from the window's average and
the time-sliced schedule sits 0.3 from it. So the execution line's findings are not an artefact of one instrument --
they are the arithmetic of the fee plus the tick, and the second instrument is registered, so any of the earlier
runs can be repeated on it.

The three futures runs take the `replay` mode, which reads a trade file and a best-bid-offer file rather than a
book, so the execution line can only ask them for `--algo=market`: the L1 touch callback carries no timestamp, and a
schedule that spaces its slices by market time would silently emit one child. That path also keeps the venue's
millisecond event time where the L2 path carries microseconds, so both the strategy's deadline and the reported
`span_seconds` are converted at the call site rather than assumed to share the other path's unit. Its window here is
the first 300k quote rows of the day -- half an hour to an hour depending on how busy the symbol is -- with one
parent per symbol at the same notional as the BTC run, from the venue's daily dumps at
`data.binance.vision/data/futures/um/daily/{aggTrades,bookTicker}/<symbol>/<symbol>-<type>-2024-03-28.zip`. Adding
the two symbols also meant the path had to look its specification up by symbol instead of taking the BTCUSDT one:
each spec's quantity scale is the reciprocal of that symbol's lot step, so a unit is 0.001 BTC, 0.1 XRP or one DOGE,
and the parent quantity is written in those units.



The same pair of runs says what the tick does to the **maker** side, which is the line that was closed by cost.
On the same 2M-row window with the same strategy, `[PNL]` already reports the maker rate that would have made
each run exactly flat:

| instrument | maker fills | gross | fees | break-even maker rate |
|---|---:|---:|---:|---:|
| BTC-PERPETUAL | 25 | -5.65 bps | 2.00 | **-5.65 bps**, a rebate |
| ETH-PERPETUAL | 86 | +1.26 bps | 2.00 | **+1.26 bps** |

The venue charges 2 bps to rest, so both are outside what it offers -- but they are 6.9 bps apart on one fee
schedule, and the whole difference is the tick: a passive fill that captures a tick earns 0.78 bps on the BTC
contract and 3.76 on the ETH one for the same 2 bps of fee.

That gap was recorded as a hint and not as a result, because the sample could not settle it: the maker line produces
1 to 86 fills on a 200k-row window and 25 to 86 on a 2M-row one, so a four-day study would compare a few dozen
trades per cell. It is settled below, on a venue whose order book archive is free and daily from March 2023.

### The tick effect, measured where the tick is negligible

The venue that settles the hint also removes the tick from the comparison. `BTC-USDT-SWAP` quotes a tenth of a
dollar against 66,272, which is 0.015 bps of tick, and `ETH-USDT-SWAP` a cent against 3,072, which is 0.033: the two
symbols differ by a factor of 2.2, and both ticks are a rounding error beside the venue's 2 bps maker fee. So the
same BTC/ETH pair, the same strategy and one parameter set on both venues, two days and two 200k-record windows per
cell, gives the tick hypothesis a control it never had:

| venue | symbol | windows | fills per window | break-even maker rate |
|---|---|---:|---:|---|
| Deribit | BTC-PERPETUAL | 4 | 7 to 24 | **+0.19 bps** |
| Deribit | ETH-PERPETUAL | 4 | 22 to 44 | **-2.64 bps** |
| OKX | BTC-USDT-SWAP | 4 | 115 to 138 | **-1.14 bps** |
| OKX | ETH-USDT-SWAP | 4 | 171 to 246 | **-1.24 bps** |

Deribit's gap is **+2.83 bps** across four days, and that number is not one a reader should trust: the daily gaps
are **-3.27, +8.14, -0.27 and +6.74**, so the standard error is 2.7 bps and the estimate sits one standard error from
zero. An earlier two-day version of this table quoted +7.44 bps and called it agreement; the two days it used were
the two that happened to agree, and the two days added afterwards disagree. **58 and 127 fills are not enough to pin
a gap on that venue** -- which is the original complaint, restated honestly rather than escaped.

What the four days do establish is the mechanism, from the other venue. Where both ticks are negligible the two
symbols are indistinguishable: a gap of **+0.10 bps** across eight windows and 1,287 fills, with the two daily means
on opposite sides of zero (+0.49 and -0.29). The gap therefore **cannot be the asset** -- the venue that removed the
tick removed the gap -- and the sign and size of it on a coarser venue is what a few hundred fills cannot measure.

A second thing the four days show, and it is the plainest evidence that frequency is not a knob: the same strategy,
the same parameters, holds a round trip for **0.03 to 0.14 seconds** on the venue whose tick is 0.015 bps and for
**60 to 720 seconds** on the venue whose tick is 0.78. The quote's distance from the mid, in units of the noise,
decides how often it trades; nothing in the configuration does.

The better sample also answers what the hint could not. On the venue where the tick is negligible the gross is
**negative in all eight windows**, -0.50 to -1.87 bps, so the passive quote loses money *before* the 2 bps maker fee
is charged. That figure is the run's gross PnL over the notional it quoted, and it carries the mark of whatever
position the window ended holding, so it was audited against the closed-pnl figure that does not: on the two windows
re-run, the closed gross is -0.64 bps on BTC and -0.99 on ETH against totals of -0.72 and -1.00, which is the same
sign and nearly the same size. **The loss is not an artefact of the final mark.** And on the coarse-tick venue the ETH
contract loses in all four of its days (-1.52 to -3.23), while the BTC contract is positive on two of four (-6.44,
+5.47, -3.50, +5.22), which is a coin landing the way the drift pushed it. The maker line's closing therefore survives
both venues: at 2 bps to rest, neither a hundredth of a basis point of tick nor a tick worth 3.76 pays.


Two caveats belong with the numbers. Round trips close in 0.03 to 0.14 seconds on the fine-tick venue, so this
measures a quote that is run over and re-flattened rather than one that rests, and the gross includes the inventory's
mark to market, which is why the level moves with the day's drift and only the gap is stable. Both are visible in the
lines the runs print (`average_holding_seconds`, `gross_pnl_usd`), so a reader can weigh them rather than take the
summary.

The data is the venue's own archive: the order book is one `tar.gz` per symbol and day from the download portal, cut
on **UTC** midnight and re-sending a full snapshot about once a minute, while the trades come from the same venue's
public endpoint as one daily `allswap-trades-<day>.zip` cut on **UTC+8** midnight. Pairing them means taking the
window from the book and filtering the trades to it, which is what the slices here do; the ETH book file also begins
mid-stream, so a window has to start at a snapshot rather than at a line number.


## The pre-trade controls: what the backoff changed

`tools/sweep/risk_check.sh` runs four 200k-row replays: no risk flags, a permissive setting, a 1 bp fat-finger band
and a bucket at 0.01 orders a second. Each line below is what the run reported about itself; every one ends
with `[RECON] status=ok`.

| run | submitted | refused | reconciling |
|---|---:|---:|---|
| no flags | 55 | 0 | ok |
| 100 bp / 1,000 a second | 55 | 0 | ok |
| 1 bp | 38 | 13 fat-finger | ok |
| 0.01 a second | 6 | 15 rate-limited | ok |

The first version of the controls refused 161 and 33,685 orders on the last two runs. The 33,685 was a retry
storm, not a measurement of liquidity: `trend_l2` asked again on every view because a pre-trade refusal never
reached it. `Strategy::on_order_rejected` gave it the reason, and a deterministic exponential backoff (60 views,
doubling to 3,840) cut the two counters to 13 and 15. The baseline prints no `[RISK]` line, and the permissive
run's `[EXECUTION]` line is identical to the baseline's, which is the invariant that a control merely enabled
changes nothing. The rate bucket, the exposure cap and the position gate also let a reducing order through even
when the resulting position is still outside the limit, so a position can be worked down rather than trapped.

```bash
bash tools/sweep/risk_check.sh
```





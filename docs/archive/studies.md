# The studies

The record of what this project tried, in the order it was tried, extracted from the roadmap when the roadmap became a
plan again: the phases of the earlier live-system plan, what data was used and rejected, the three candidates that would
have needed speed, the venues that fit a personal user, the carry line's gates, and the plan from a positive window to a
running system — including the studies that closed each line and the ones that only narrowed it.

Two things to know before reading it:

- **It is a record, not a plan.** What is still open is in the [Roadmap](../ROADMAP.md); a study here is finished, and
  the numbers in it are the ones the [Retrospective](../RETROSPECTIVE.md) cites.
- **It is the long half on purpose.** A line gets closed once and then never again, so the reason has to survive the
  person who closed it. That is what this file is for.

## The phases

**Historical view.** The phases below are the earlier live-system plan; the
[Current iteration plan](../ROADMAP.md#current-iteration-plan-maintained) decides what is built next. Where they disagree,
the current plan wins.

| Phase | Work | What it buys | Cost |
|---|---|---|---|
| **0 — packaging** | one branch, the `v1.0-demo` tag, the documents in the published navigation, a code CI | a reader sees a maintained repository with a history rather than two unfinished ones | done |
| **1 — evidence** | the headline conclusions re-run on a second day, which the repository already holds, and the queue-model bounds experiment recorded | closes the two questions the documents could not answer: does it generalize, and how much does the queue model matter. Both are now recorded, and the second day's numbers are behind a marked block in the gate | done |
| **2 — the live paper milestone** | a websocket book feed, a paper-trading mode reusing the book and the matching engine, and a recorder that writes the live feed in the replay format | the strongest statement available here: one strategy drives a replay and a live session, and a live session becomes replayable | done: the `ws` mode is the L2 replay path with its two readers moved onto a socket, so the same book, engine, recorder and paper broker run unchanged; the feed speaks RFC 6455 and reads the replay CSV shapes, and a local `tools/live/websocket_sender.py` closes the loop at the desk — a recording streamed back over the feed reproduces itself byte for byte |
| **2b — from a positive window to a running system** | the plan below, derived backwards from what a live path can be: the deployment filter, the instrument arithmetic, the seven things that must be true before any plumbing, and only then live data, a recorder, an order gateway, risk limits and reconciliation, then minimum size against a real account | a system that is measured rather than asserted, with a stated pass and fail condition at every step | see the stage table below |
| **2a — a second venue** | a reader for the OKX historical format, added to `src/market/` beside the existing ones, plus a reconciliation check that the rebuilt book matches an independent recomputation of the top of book | consecutive-day free data with discrete levels, which is what a multi-day test needs; the existing pinned lineage is untouched because a new venue is an additional workload | done: the readers exist, `market_data_format_of` routes a depth file to the right one, and a paired replay of one hour of the 2026-01-07 archive completes with the summed order book, the exchange's fee schedule and the contract size of 0.01 BTC |
| **3 — system only** | persistence and recovery, the pre-trade gate enabled by default, with a check that the limit exceeds one order, a real venue adapter at small size, multi-instrument accounting | none of it is checkable by a reader, so it waits until the demo is finished | later |

## Data: what is used, and what was rejected

Checked once, at the time of writing, so re-verify before paying for anything.

| Source | What it gives | Verdict |
|---|---|---|
| **Tardis.dev free samples** | the first day of each month, in the format this repository already parses, for any symbol in the catalogue | **used**: the multi-day evidence grows one free day at a time, with no new code; `BTC-PERPETUAL` carries the six measurement days and `ETH-PERPETUAL` is registered as the second instrument, whose tick is five times coarser against its own price and is what separates fee from spread in the cost floor |
| **OKX official historical data** | trade history from 2021-09, funding rates from 2022-03, and high-resolution L2 order book from 2023-03, free and daily | **used**: the maker study's data, and the venue that settled the mechanism behind the BTC/ETH gap, because its two symbols quote a hundredth of a basis point of tick each and show no gap at all. The trades are one public zip per day on a plain CDN url and need no account; the order book is a per-symbol `tar.gz` from the download portal, which is the one manual step left in the project's data path. Two boundary traps are recorded in the data note: the book file is cut on UTC midnight and the trade file on UTC+8, and a book file can begin mid-stream, so a window has to start at a snapshot |
| **IEX HIST, US equity depth** | free DEEP and TOPS archives, one pcap per day, T+1, behind a login and the IEX terms of use. DEEP is an **order-level** feed — order adds, executions, deletes and cancels, each carrying an order reference — rather than aggregated price levels, and it is IEX's own book rather than a consolidated one | **rejected**: it is not a venue an account opened from Hong Kong can rest on, so its book can neither rank a symbol (the screen measures where we would trade) nor validate a fill; and turning order-level messages into the price-level ladder this model consumes is new code and a new lineage, against a paid route at about two dollars a session |
| **Databento `mbp-10`** | ten price levels a side with the whole book on every row, so the trades are the `action=T` rows of the same file, billed on the compressed size at half a dollar a gigabyte on the micro E-mini and two on an equity | **used** (F4): the paid route to a tradable market's depth, converted to the CSV pair the existing readers take, with the conversion checked against the vendor's own levels row by row |
| Binance Vision, USD-M futures | free daily aggTrades, trades, book ticker (best bid and offer) updates, premium index klines and open-interest metrics | **used**: funding and basis work, which needs no price ladder, and the five-point tick curve, whose three futures rows come from this venue because it quotes every symbol on one 4 bps schedule while spacing its ticks by a factor of ten thousand. Those runs take the best-bid-offer path, which carries no clock in its touch callback, so the execution line works a single crossing there and refuses a schedule |
| Binance Vision, spot | daily aggTrades per symbol, but no book-ticker dump for the same day the futures archive serves, and no depth | **rejected**: a crossing floor needs a touch, and there is no free best-bid-offer file to supply one |
| Binance Vision `bookDepth` | percentage bands around the mid, not discrete levels | **rejected**: a tick ladder cannot be rebuilt from it |
| Bybit public data | trades, premium index and spot index; no order book directory | **rejected**: no L2 history |
| Tardis paid, Crypto Lake, LOBSTER, Databento | wider L2 and L3 coverage | **rejected for now**: Tardis has a minimum order far above a few days of data, LOBSTER wants an academic address, and Databento is CME and Nasdaq, which would start a different measurement lineage rather than extend this one |

## Three candidates that would need speed, measured

The lines above all failed on cost per action. These three were chosen because each is a place where **being fast is
worth money for a reason that can be measured**: a slope that decays over time, a forced seller whose footprint is in
the trade tape, and a quote that is stale on one venue while fresh on another. All three are priced here from free
data, before any code was written for them.

### The CME calendar, over 23 quarterly holds

The crypto calendar spread failed because its level was a slope and a slope is a price. A rates-driven slope behaves
better in one window and not in the sample: one pair, `MESZ6` against `MESH7`, drifts **+0.54 bps a day (+64.6 in
total, +11.4 a month)** against a round trip of 1.94 bps -- and that is one window. Held over **23 quarterly holds from
2021 to 2026**, front against next, ninety days each, at ES's fifty-dollar point value:

| item | value |
|---|---:|
| net per hold | **mean +8 USD (-0.14 bps)**, median +20, worst -617 |
| positive holds | **12 of 23** |
| cost per hold | 30 USD (1.20 bps) |
| worst mark-to-market drawdown per hold | mean -647 USD, worst **-1,900 USD** |
| at 3,000 USD of spread margin | **+0.3 % per hold, +1.1 % a year**, worst hold -21 %, worst drawdown **-63 % of margin** |

So the cost is not the problem here either -- a hold costs 1.2 bps and the good windows pay 20 -- and the trade still
fails, because **the outcome is a coin flip whose tail is twice the average gain and, at any retail margin, most of the
margin itself**. The 2026 pair that looked like a positive drift was the window that happened to work, which is the
same error as the two days that agreed, one level up.

Two findings are worth keeping from it. The CME structure is the *cheapest* thing measured in this repository -- 1.2 bps
per round trip against 4 to 24 in crypto -- so any future trade with a real edge should be expressed here rather than
on a perpetual. And a slope's drift does not survive sampling: a single contract pair is one observation of a
refinancing cycle, not a strategy.


### Large aggressive prints: the sign is right and eight times too small

A liquidation is not in Binance's daily archive, but its footprint is a large market order the taker crossed into the
book. Taking the top percentile of sizes on BTCUSDT for 2024-03-28 -- 7,468 aggressive buys and 7,266 aggressive sells
out of 1.47 million trades -- and measuring the price 1, 5, 30 seconds and 5 minutes later:

| print | 1 s | 5 s | 30 s | 5 m |
|---|---:|---:|---:|---:|
| large aggressive buy | **+0.71 bps** | +0.77 | +0.98 | -0.35 |
| large aggressive sell | **-0.68 bps** | -0.66 | -0.37 | +0.02 |
| random trade (control) | -0.09 | -0.00 | +0.46 | -1.56 |

The sign is exactly what a cascade should produce and the control shows no such thing, so the mechanism is real. It is
also **eight times too small**: a round trip at the venue's own taker rate costs 8 bps and the continuation is worth
one. Following a cascade is not a trade, it is a donation of the spread; fading it is worse, because the sign is
against you.

### A stale quote on one venue while the other is fresh

Two venues, one asset, one free day, one vendor so the clocks and schemas match: Binance's linear perp against
Deribit's inverse perp on 2024-03-01, 40.2 million aligned quote states. The raw executable gap -- one venue's ask
against the other's bid -- is **positive 99.0 % of the day at a median of +2.99 bps**, which is not an opportunity but
a level: two different contracts carry a small permanent basis, and only the deviation from that level can be
harvested repeatedly.

So the deviation is measured against each minute's median, over 3.1 million merged states in 1,440 minutes:

| statistic | value |
|---|---:|
| per-minute baseline | median **+2.95 bps** (range +0.11 to +9.61 across the day) |
| deviation, median / p95 / p99 | 0.00 / **+1.60** / **+2.39 bps** |
| deviation, worst one-way | -9.34 bps |
| share of the day above 9 bps (both taker fees) | **0.012 %** |
| spells above 9 bps | **16 in the day**, median 63 ms, p90 258 ms, longest 2 s |
| total time above 9 bps | **under a minute** |

That is the answer to the stale-quote hypothesis, and it is no: the deviations are two and a half basis points at the
99th percentile while a crossing round trip costs nine, so the opportunity exists for sixteen flashes of about sixty
milliseconds in a day. A fast client changes the *reaction time* to those flashes; it does not change their number.
What it would change, and what this measurement cannot see, is a version that **rests** on the stale venue instead of
crossing it -- posting on the far side of the level and letting the deviation come to you. That is a maker trade, and
the maker measurements above say what a resting quote earns here: negative before fees.

### Options: selling the straddle, which is the last thing that looked like an income

Every line above earns per action or per day; selling options earns a **premium up front** for accepting a tail, which
is the one structure this repository had never priced. Ten quarterly samples, 2020-02 to 2021-08: the at-the-money
straddle of the nearest quarterly expiry, priced from its own best bid and offer at noon, its implied volatility
recovered from the at-the-money straddle approximation, the realised volatility taken from the same window's daily
closes, and the payoff in the coin-settled form the venue uses (an option pays |S - K| / S in BTC).

| day | IV % | RV % | IV - RV | premium USD | payoff USD | PnL USD |
|---|---:|---:|---:|---:|---:|---:|
| 2020-02-01 | 59.4 | **155.2** | **-95.8** | 1,724 | 3,880 | -2,156 |
| 2020-03-01 | 55.6 | **219.2** | **-163.6** | 1,027 | 2,911 | -1,884 |
| 2020-05-01 | 77.2 | 61.9 | +15.3 | 2,179 | 159 | +2,020 |
| 2020-06-01 | 66.2 | 44.9 | +21.3 | 1,317 | 351 | +966 |
| 2020-08-01 | 64.3 | 52.4 | +11.9 | 2,330 | 342 | +1,988 |
| 2020-11-01 | 59.6 | 70.9 | -11.3 | 2,513 | 6,500 | -3,987 |
| 2021-02-01 | 112.3 | 90.1 | +22.2 | 11,702 | 14,308 | -2,606 |
| 2021-03-01 | 101.7 | 68.2 | +33.5 | 10,188 | 6,107 | +4,081 |
| 2021-05-01 | 66.3 | 109.9 | -43.6 | 11,909 | **44,742** | **-32,833** |
| 2021-08-01 | 79.9 | 70.3 | +9.6 | 10,225 | 788 | +9,437 |

**Then the sample was audited, twice.** The first version applied the at-the-money approximation to strikes up to
6.4 % away from spot, where it is not valid; splitting by moneyness made the premium look like +19.9 points on the four
samples that were genuinely at the money and -45.4 on the six that were a skew trade. **That split was itself the same
error one level down -- four samples is not a measurement.** So the question was re-run properly: the strike chosen as
the nearest 100 (250 and 500 as fallbacks), the implied volatility inverted from Black-Scholes per leg, the expiry the
nearest listed one at least twenty days out so every month contributes, and **92 straddles** from 2020-01 to 2026-09
with a worst moneyness of 3.3 %.

| year | samples | IV % | RV % | IV - RV | mean PnL per straddle | positive |
|---|---:|---:|---:|---:|---:|---:|
| 2020 | 21 | 64.2 | 63.8 | +0.4 | -138 | 9 of 21 |
| 2021 | 14 | 86.6 | 84.7 | +1.9 | -979 | 9 of 14 |
| 2022 | 12 | 63.8 | 63.9 | -0.1 | -977 | 7 of 12 |
| 2023 | 12 | 43.2 | 43.7 | -0.4 | -337 | 6 of 12 |
| 2024 | 12 | 53.5 | 51.9 | +1.6 | **+1,421** | 8 of 12 |
| 2025 | 12 | 43.1 | 41.8 | +1.3 | **+1,148** | 8 of 12 |
| 2026 | 9 | 40.9 | 45.4 | -4.5 | -900 | 4 of 9 |
| **all** | **92** | **58.4** | **58.1** | **+0.2** | **-105** | **51 of 92** |

**The premium is real and it is worthless.** Implied exceeds realised in 63 % of months and averages **+0.2 volatility
points** over six and a half years, against which the short straddle is a coin flip -- 51 of 92 -- with a mean of
**-105 USD**, a median of +319, and a left tail that runs to **-25,450** against a best of +8,367. Two years pay and
five do not. That is what a risk premium priced at fair value looks like: the seller is compensated in most months and
not compensated on average, and the compensation he does take is smaller than the noise he carries.

The collection is `tools/fetch/fetch_deribit_options.py`, which is where this measurement stops being a scratch script: the
ninety-two rows, their strikes and their inverted volatilities are re-derived by running it, and its own self-test
checks the at-the-money identity against the pricing function rather than trusting the one that produced the first
version.

So this line closes, and it closes differently from how it opened twice. The first version called the premium negative
on ten mixed samples; the audit called it +19.9 points on four; the measurement calls it +0.2 on ninety-two. **The
progression is the point:** both earlier answers read a statistic off a sample small enough to say anything, and this
repository's whole method is the correction of exactly that.



### Lead-lag: a large BTC print and the five alts that are supposed to follow it

The last untested idea was that BTC moves first and the alts follow within a few hundred milliseconds, which is the one
shape where a fast client is paid for being fast: you are not fighting a queue, you are trading on information that is
already public on another symbol. One free day of Binance's USDT-perp tape, 2024-03-28, with 5,664 BTC triggers (the
top percentile of trade size, at least five seconds apart) and the followers measured from their last print before
each trigger to the first one after each horizon, in basis points:

| symbol | 100 ms | 250 ms | 500 ms | 1 s | 2 s |
|---|---:|---:|---:|---:|---:|
| BTC (the trigger itself) | +0.00 | +0.00 | -0.00 | -0.02 | -0.03 |
| ETH | +0.00 | +0.00 | -0.01 | -0.04 | -0.04 |
| SOL | -0.01 | -0.00 | -0.02 | -0.05 | -0.05 |
| XRP | -0.02 | -0.00 | -0.02 | -0.02 | -0.02 |
| DOGE | +0.04 | +0.06 | +0.03 | -0.00 | +0.00 |
| ADA | +0.01 | +0.01 | +0.00 | -0.01 | -0.00 |
| control (ETH at evenly spaced moments) | +0.03 | +0.03 | +0.04 | +0.03 | +0.03 |

**Every follower is within six hundredths of a basis point of the control.** The effect people describe is either
absent at this granularity on this day or already arbitraged into the noise, and the noise itself is a fifteenth of
what the effect would have to be: a round trip costs nine basis points and the largest number in the table is 0.06.
One day and trade prices rather than book states are the limits, and neither is large enough to change the verdict --
a 0.06 bps effect does not become 9 bps on another day.

**What the three together say.** Each candidate had a mechanism that speed genuinely serves, and each was priced
against its own cost: the CME slope drifts +11.4 bps a month against a 1.94 bps round trip but carries 30 bps of daily
noise; a forced seller's footprint continues 0.7 bps against an 8 bps round trip; a stale quote deviates 2.4 bps at the
99th percentile against a 9 bps round trip. Two of the three are out by an order of magnitude and the third is a risk
premium, not an edge. **Being fast does not move any of these ratios** -- it decides whether you are the one who takes
the 0.7 bps or the one who provides it.

### The funding extreme: a bet on one crisis year

The last unmeasured class of information was the funding rate as an event signal: when longs are paying heavily, the
crowd is levered and a contrarian position collects both the funding and whatever the squeeze does. The test was
registered before the numbers: at each settlement a z-score of the rate against the previous ninety days, a short when
the z is high and a long when it is low, an eight-hour hold, two taker crossings at 8 bps, and three gates -- mean net
above three times that cost, worst trade shallower than half the mean, and positive in two thirds of the years, with
2024-2026 held back.

Seven thousand three hundred and ninety-five settlements of BTCUSDT, 3,863 with a full ninety-day window:

| threshold | trades | mean net | median | worst | positive | by year |
|---|---:|---:|---:|---:|---:|---|
| abs z >= 1.5 | 453 | +2.66 bps | -6.39 | -709.3 | 212 of 453 | 2020 -22.1, 2021 +35.1, **2022 +79.2**, 2023 -0.3, 2024 -11.6, 2025 +1.5, 2026 +2.1 |
| abs z >= 2.0 | 251 | +3.36 bps | -4.81 | -709.3 | 119 of 251 | 2020 -17.7, 2021 +30.6, **2022 +135.6**, 2023 -0.2, 2024 -15.5, 2025 +2.5, 2026 -7.2 |
| abs z >= 3.0 | 104 | -1.03 bps | -9.59 | -709.3 | 48 of 104 | 2020 -51.9, 2021 +39.3, **2022 +185.6**, 2023 +4.7, 2024 -38.2, 2025 +40.6, 2026 +56.5 |

**All three gates fail at every threshold**, and the shape behind the failure is the finding. The mean is positive and
the **median is negative at every threshold** -- most of these trades lose a little and a handful win enormously, which
is a lottery ticket rather than an edge. The whole positive mean is **one year**: 2022 contributes +136 bps at the
middle threshold, the other six years are a scatter between -18 and +31, and at three sigma the rule loses money
outright. 2022 is the year of the deleveraging and the FTX collapse, when a negative funding extreme really did precede
a squeeze.

And the held-out period agrees with that reading rather than with the full sample: **2024-2026 averages -11.63 bps**
across 117 trades. There is no version of this rule that survives out of sample, and the reason is the same one that
closed the carry line: a signal that pays in the year you cannot foresee and loses in the six you can is not a
strategy, it is a lottery with a story attached.

**The first version of this measurement was wrong, and wrong in the flattering direction.** It priced the entry from
the close of the hour that *starts* at the settlement and then credited the position with that same settlement's
funding -- a trade that began an hour late, paid for a settlement it never held through, and the rule selects exactly
the settlements whose rate is largest. Aligning the entry to the settlement instant and crediting the *next* row's
funding, which is the settlement the hold actually spans, moved the two-sigma mean from +7.29 to **+3.36** bps and the
held-out mean from -1.06 to **-11.63**. The verdict did not change; the rule looked better than it was for exactly as
long as it was paying itself.

**The rule and its gates live in `tools/study/funding_extreme_test.py`**, not in this prose, so the table above is that
tool's output rather than a paragraph that agreed with it -- the collection reads the same free monthly archives as the
carry line's funder, `tools/fetch/fetch_funding_rates.py`, and writes no file of its own. Its own self-test -- `python3
tools/study/funding_extreme_test.py --self-test` -- checks both legs
against hand arithmetic, and it caught its own author writing the long leg as a payment: with a negative rate the short
side pays, so the contrarian long *collects* the funding, and the sign of that term is the entire carry argument in one
line of code.


Sixteen structures have now been priced, and the shape of the answer is the same in each: **what is paid per action
does not clear the fee charged per action, and what is paid per unit of time does not clear the noise it arrives
through.** That is not a statement about effort or skill; it is what the arithmetic says at retail size in markets
anyone can reach.

Two things do survive, and neither is a strategy:

* **Execution cost, as a multiplier rather than a rescue.** CME's futures cost 1.2 bps for a round trip against 4 to
  24 in crypto, and resting instead of crossing saves 10 to 20 bps of it -- but the revival test below shows that this
  multiplies an edge and does not create one. **Nine quoting policies on the fine-tick venue stay negative before the
  fee is charged**, and the best of them is -0.21 bps against a 2 bps fee: no execution improvement closes that gap,
  because the gap is adverse selection and execution does not select.

  | policy | BTC gross | ETH gross |
  |---|---:|---:|
  | 2 ticks wide, refresh every 1 / 4 / 16 | -1.39 / -1.11 / -0.97 | -1.84 / -1.13 / -1.20 |
  | 4 ticks wide, refresh every 1 / 4 / 16 | -1.00 / -0.92 / -0.59 | -1.21 / -1.01 / -1.07 |
  | 8 ticks wide, refresh every 1 / 4 / 16 | -0.69 / -0.67 / **-0.21** | -1.19 / -1.22 / -1.15 |

  The BTC column improves with width and patience and is still nineteen basis points short of the fee; the ETH column
  does not improve at all. That is the sharper form of the closing: **the loss is in the fill, not in the plumbing.**
* **Breadth.** One trade worth 0.5 bps is not worth doing; two hundred of them a day across twenty symbols is a
  different question, and it is a question about infrastructure and cost, not about alpha.


What remains genuinely unmeasured is narrower than the list above, because the first item on it has now been measured:
**funding-rate extremes as an event signal fail their own gates** (see above), and they fail in the way that matters --
positive mean, negative median, one crisis year carrying the average, and -11.63 bps across the held-out 2024-2026.
What is left is the information this repository has no feed for at all: on-chain flows, exchange netflows, stablecoin
issuance, unlock calendars. Those are research rather than execution, they need their own pre-registration and
out-of-sample discipline, and this repository's history says most such rules die the same way these did. It is the only
door still open, and it opens onto work rather than onto money.

## What stays inside this repository, and what does not

The demo and a personal trading system share a method and one program, not a codebase. The line is drawn by
what a change produces:

* **Inside**: an input format behind a reader in `src/market/`, a new workload, a new pinned invariant, a
  measured table. This repository accepts **data, configuration and conclusions**.
* **Outside, and a separate project**: strategy research of any kind — designing, fitting and selecting a
  funding, basis or execution strategy, portfolio construction, or scheduling. That work uses this simulator
  as a fee and execution auditor when it needs to know what a trade costs, and it does not add a strategy here
  until the work has become a measurement rather than a search.

The same line explains the rejections above: the sources that would extend the evidence are used, and the
sources that would start a different project are not.

## Venues: what fits a personal user after the demo

Judged as a personal trader rather than as a simulator, and after the gate that comes before every other one: the
venue has to be one you may legally use, and one you can open an account with, where you live. A venue that fails
that gate can still be the best measurement input in the table, which is why access and fitness are separate
columns of reasoning below rather than the same one.

| Venue and instrument | Fit | Why |
|---|---|---|
| Crypto USDT perpetuals (OKX, Binance, Bybit) | **best for measurement** | free consecutive book history with discrete levels, a published fee schedule, and one leg of limit orders is all the model needs, but access is restricted in some jurisdictions, so this is the test bench rather than the trading venue |
| Locally licensed crypto spot venues | possible | legal and openable, but spot only for retail in some regimes, and a fee schedule an order of magnitude above the offshore perpetuals |
| Micro futures on a major exchange | good | accessible through a local broker, a small contract size, and nearly round the clock operation; a quarter point tick on the micro E-mini is worth 1.25 USD against a commission of about sixty cents a contract, so a round turn costs one tick, and Databento sells ten levels of its book for about two dollars a session |
| Equities and ETFs | good | a fee that is a fraction of a tick once the share commission is set against a notional, free daily depth behind a login rather than an endpoint, a free paper account at the broker, and the natural home for a low frequency position rather than a quote |
| Local index futures and cash equities | possible | accessible, but a transaction levy on both sides and paid depth make this the most expensive of the listed markets |
| Derivative products of a licensed crypto venue | only for professional investors in some regimes | the rule is about the account category, not the venue, so it is worth checking before it is planned around |
| Options anywhere | no | expiry, exercise, multiple legs and greeks are not in the model |
| FX and contracts for difference | no | worse data and cost, and the broker is usually the counterparty |

The rule behind the table: a personal account wins on capital and patience and loses on speed, so the venues
that fit are the ones where a position is held rather than won by being first in a queue. Adapting to another
USDT perpetual is a reader and one instrument entry, and the chain of measurements that exists today does not
move, because a second venue is an additional workload rather than a replacement for one.

### Access, cost and data, market by market

The table above ranks fit. This one is the three facts that decide whether a market can be measured at all, asked
for a Hong Kong personal account specifically: may this account trade it, is the fee smaller than a tick, and is
there continuous free depth to test the answer on.

| Market | Tradable by a Hong Kong individual | Fee in ticks | Free continuous depth |
|---|---|---|---|
| US stocks and ETFs | yes, through a broker with US market access | a share commission is a fraction of a cent on a hundred dollar share whose tick is one cent, so about a third to a half of a tick a side; **the zero commission schedule is not available to a Hong Kong account (read 2026-10-02)**, and the per side cost is **not price independent**, because the SEC fee on a sale is charged on proceeds -- see the fee section below | **no usable free depth**: IEX HIST is IEX's own book, so no national best bid can be built from it, and it is an order-level feed for a venue this account cannot rest on. The depth route that is used is paid and cheap: ten levels plus the trades for about two dollars a session |
| HKEX index futures | yes, through local brokers | no stamp duty on futures; the exchange and levy per contract are a fraction of the HK$50 HSI tick and a broker commission is of the same order, so about a tick a side, which is close to the width of a one tick quote | **no**: realtime and historical depth are licensed subscription products, so the only route is to record a broker's own realtime feed into the replay format |
| HK cash equities | yes | a stamp duty of 0.1% a side is about ten basis points, an order of magnitude over any spread measured in this repository, plus levies and commission | no |
| A-shares through northbound | yes | a sell side stamp duty of 0.05% plus commission, and settlement is T+1 | no |
| Micro futures on a US exchange | yes, through a broker | a quarter point tick is 1.25 USD because the micro E-mini multiplier is 5, against a commission of about 0.62 USD a contract a side, so a round turn costs one tick and the fee is half of it a side | **paid and cheap rather than free**: ten levels plus the trades in one file from Databento for about two dollars a session on the micro E-mini, and there is no free archive for it |
| Offshore crypto USDT perpetuals | no: the offshore venues are not licensed here, so this stays a measurement bench | a fraction of a tick at a hundred thousand dollar price, which is the whole problem | yes, and this repository's readers already consume it |
| Locally licensed virtual asset venues | spot yes, derivatives generally professional investors only | above the offshore perpetuals by an order of magnitude | no archive exists, so the same self-recording route as index futures |

Read on 2026-09-30: the IEX market-data page's T+1, pcap, IEX-TP and specification wording, its example file
name, and its Login column; and that IBKR advertises "$0 commissions on US stocks and ETFs with IBKR Lite" on its
own commissions page. Read earlier in this work: the 0.1% Hong Kong stamp duty a side, the 0.05% northbound sell
duty, that HKEX publishes free statistics and announcements but no free depth archive, and that offshore
perpetual depth is free because this repository reads it daily.

Still to verify before anything is planned around it: **whether a Hong Kong resident is eligible for IBKR Lite or
is on the per-share Pro schedule** (the advertised tier is a product fact, not an entitlement), the current list
of SFC licensed virtual asset venues, and the exact HKEX futures exchange fee and levy per contract, since the
contract specification pages this repository tried did not answer.

Read on 2026-10-01, from the broker's own published pages rather than from the account, because three questions
that were being treated as account questions are public:
**whether an order can be pinned to a named venue** — yes, and it is a priced routing choice rather than a
switch, because the commission schedule lists "Fixed - Direct Routing" beside "Fixed - IB SmartRouting", so a
directed order is a different schedule on the account that has it;
**depth data** — the market data pricing page sells depth of book per venue, states that a depth service does not
include a top-of-book quote (which has to be subscribed beside it), and waives the monthly fee only above a
stated commission threshold, so a quiet month pays full price;
**the account rules** — the order-status list answers half of the borrow question: an order can come back
`Inactive` because it is a short sale waiting for shares to be located. What the API itself imposes is public
too: a hundred simultaneous top-of-book market data lines by default, market data subscribed per username and
billed the full month rather than pro-rated, and a historical-request pacing that counts fifty simultaneous
requests, sixty requests in ten minutes, six requests for one contract in two seconds, or an identical request
within fifteen seconds as a violation.
**Not confirmed by any page reached from here, and therefore not relied on**: a fifty messages per second client
limit, a forced daily restart of the gateway, and whether a Hong Kong resident is offered the Lite schedule at
all. Those need the account, and none of them changes what can be built next.

Read on 2026-10-02, from the account's own pricing page and from the broker's SmartRouting page, now that an
account exists. **The Lite schedule is not available to a Hong Kong resident**, so the zero commission tier this
document's tables were written against is out, and both remaining schedules are Pro ones: **Fixed at 0.005 USD a
share** and **Tiered at 0.0005 to 0.0035** a share by monthly volume. They are not the same product at two prices.
**Fixed bundles the commission and the exchange fees and still passes the regulatory fees on a sale** — SEC and
FINRA TAF — while **Tiered is fully unbundled**: a low commission, then exchange, clearing and regulatory fees per
trade, and it is the schedule that **passes the exchange's maker rebate through to the account**. Routing is where
they part company. The commission tables list **`Fixed - Direct Routing`** as a schedule of its own, which is the
shape this document read on 2026-10-01; the SmartRouting page says **SmartRouting for US-listed securities is
ordinarily for Pro accounts only** and that it is the facility which can be told to prefer the venue **paying the
highest rebate**, per order or as a global default, so a maker that wants the rebate uses SmartRouting and a maker
that wants to choose the venue uses the Fixed schedule, **and the two may not be available together**. One
exception appears in the same tables -- "tiered - direct routing" on the EUIBSI venue -- so the combination is
decided venue by venue rather than in general, which is why it is a question for the channel and not a fact to
plan on.

What that does to the fee arithmetic is in
[Performance History](../performance/history-04-the-fee-is-not-the-wall.md#what-the-accounts-fee-schedule-adds) and it is not small. Reading the
same pages for the terms themselves gives three numbers that matter: the **SEC fee is 0.0000206 of the proceeds of a
sale**, so in ticks it **scales with the share price** -- 0.685 of a tick on a 332 dollar share and 1.58 on a 765
dollar one -- and it is the largest single term in an equity fill; the **Nasdaq credit for adding displayed
liquidity is 0.0013 a share** against 0.0030 for removing it, which is less than half what this document had
assumed a maker rebate was worth; and **every market's schedule carries a minimum per order**, whose note on the
same page says a modified order is charged as a new order. With those three, the four arrangements the account can
choose come out at 0.583, 0.713, 0.863 and 1.113 ticks a fill against the 0.640 an AAPL fill captured, so **the fee
decides whether the quoting covers itself and nothing more** -- and the arrangement that buys venue control is the
worst of the four. All five terms are parameters the engine now prices with, `--fee-schedule=...`, zero unless a run
names them so every recorded row is unchanged, and a run under the account's own figures puts an AAPL fill at
**0.940 ticks against the 0.640 it captured**: on the account's schedule the quoting does not cover itself, by
354.76 USD over the two hours, which the position term of 3,340 still dwarfs.

**The conclusion is narrower than the question.** Only one market in this table has all three columns at once:
a US ETF, priced in cents, with a commission that is a fraction of one, and a free daily depth archive to test
the fee question on. That is why the equity entry is the one being built, and why the local index futures are the
runner up rather than the start: they are tradable and their fee is about a tick, but their depth has to be
recorded before anything can be said about it.

## The carry line: financing flows before carry strategies

**The line is closed and it is kept on purpose, for two reasons.** The recorded run in
[Performance History](../PERFORMANCE_HISTORY.md) is a checked block, so the gate re-runs it and compares every value on
every full run: deleting the code would either break that gate or turn the sharpest measurement in this project into a
copied number, and **claims being re-derivable is the property this repository sells**. And it is the only mechanism
here whose income accrues with **time** rather than per action -- an interval flow, a rate series, a two-leg ledger that
reports gross and net notional separately, and a payback measured in hours -- which is what the one variant of this
trade a Hong Kong account can reach and a 50 ms latency cannot touch would need: the **futures basis** on a CME micro,
where the edge is term structure convergence over weeks rather than a venue differential over hours. **Measured at
the ceiling, 2026-11:** 120 sessions of `MESZ6`/`MESH7` daily settlements from GLBX.MDP3, two fixed expiries rather
than a continuous series so no rollover jump enters the spread, give a spread that runs from +153 bps in May to +89 in
October -- converging at **2.7 bps a week**, against a **daily change of 29.8 bps in standard deviation**, a worst
single day of -134, and a worst excursion of **-201 bps** from a running high. The round trip costs about 1.3 bps of
notional, which is not the obstacle; the obstacle is that the income is roughly the risk-free rate and arrives
through the noise of an equity position. That makes the basis a **money-market trade rather than an edge**, so this
variant does not reopen the line either -- and the cost of finding that out was a fraction of a cent of data and one
afternoon, which is the property the machinery was kept for.

### The calendar basis: a level, not a return

Registered before the run, and on the right instrument this time: the calendar pair, two dated legs, no spot custody,
91 monthly samples from 2019-04 to 2026-10, basis from trade VWAP per contract, cost from the best-bid-offer archive
on twelve days at a fixed hour. The gates were a mean net above three times the cost, a worst inversion under half the
mean, and a positive net in two thirds of the samples.

The **level** passes them: mean +696 bps annualised, median +689, worst -150, 85 of 91 samples positive, against a
round trip of 20.5 to 30.2 bps -- seven times the cost. Every year is positive on average, and the six negatives
cluster where the market was in trouble (April and May 2019, April 2020, July 2022).

**Then the level was turned into a return, and the result reversed.** What a holder earns is the change in that level
over the holding period, not the level itself. Holding each pair for one month and pricing the same two contracts a
month later gives 60 realised observations:

| statistic | value |
|---|---:|
| mean realised, gross | **-6.7 bps per month** |
| positive holds | **28 of 60** |
| worst / best month | -329.7 / +386.3 bps |
| net of one round trip per hold | **-30.7 bps** |

So the gates were applied to a statistic that is not a return, and that pass is withdrawn. The arithmetic behind the
reversal is not subtle once it is written down: what converges to spot at expiry is a **future against spot**. The gap
between two futures converges to *the gap at the near expiry*, which is not zero -- it is the other leg's remaining
term premium. A calendar spread therefore does not harvest carry at all; it bets on the **slope** of the curve, and in
this sample the slope moved by up to 330 bps in a single month. The level is a slope, the slope is a price, and a
price is not an income.

What survives the correction is the instrument the level was standing in for: **one dated future against the index**.
Priced that way on the same 91 samples -- the venue's own index price against the future's mark at noon -- the entry
basis has a median of **+124 bps** and is negative in 3 of 31 contracts. Held from its first sample to its last, the
basis decays by a median of **+77 bps**, the adverse widening before that decay is a median of **0** (the worst is
+189 bps, September 2020), and one round trip at 24 bps leaves a median of **+53 bps per quarter of notional**.

Then the capital arithmetic, which is where it ends:

| item | value |
|---|---:|
| net per quarter of notional | +53 bps |
| capital per unit notional | spot leg at 1:1 plus a 4 % initial margin on the future, so about 1.04 |
| **net return on capital** | **about 2 % a year** |
| worst adverse widening, against that 4 % margin | 1.9 % of notional, half the margin |
| median size resting at the touch of a dated future | 20,000 to 240,000 USD, by year |
| what that pays on a 100,000 USD position | **about 2,000 USD a year** |

**Two per cent a year, below the risk-free rate the trade is supposed to harvest**, before the spot leg's venue is
considered, on a position sized by a book whose top level holds tens of thousands of dollars. That is the honest end
of this line: the convergence is contractual and real, the arithmetic that turns it into money on a retail account is
not. The lesson is worth more than the number: **a carry statistic has to be the change in a price over the holding
period, not the price itself**, and the way this one passed its own gates is the way a wrong statistic usually passes
them -- by looking exactly like an income.



The level, for the record, on all 91 samples: mean +696 bps annualised, median +689, worst -150, 85 of 91 positive,
and every year positive on average. The round trip measured from the book is 20.5 to 30.2 bps, and the venue's own
instrument metadata prices a dated future at 1.5 bps to rest and 3.5 to cross, so most of that is fees. Those are the
numbers a level series produces, they look exactly like an income, and the realised series above is what a holder
earned on the same samples.




An earlier six-sample version of this question (the free days of 2026-05 to 2026-10) put the registered gates on the
**dated legs against the perpetual**, failed them on the second -- the June contract inverted at -186 bps annualised as
BTC fell from 77,889 to 72,043 -- and left the calendar spreads, which were positive in 22 of 22 samples, recorded as
a hypothesis rather than a result, because the gates had been fixed on the other instrument. The table above is that
hypothesis's registered test: the calendar pair, 91 samples, and the same three gates written down first.


### The carry gates executed: the three conditions the owner set, and the answer on nine months

The line's own measurement needed 745 hours to pay for its fees against 728 hours held, and it charged the fee once.
Charging the round trip -- entry and exit, because a differential that flips forces an exit -- makes it 36.00 USD, and
the owner's three gates were then applied to the free funding histories of both venues: a year of Binance monthly
archives against a year of OKX daily archives, both keyless, for nothing.

| month | carry USD on 10,000 a leg | net after the round trip | net annualised on the capital | payback hours | runs of a positive differential | median run | longest run | runs that pay the round trip |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 2024-01 | 26.80 | -9.20 | -0.54% | 999 | 17 | 24 h | 128 h | 0 |
| 2024-02 | 4.34 | -31.66 | -1.99% | 5768 | 16 | 16 h | 72 h | 0 |
| 2024-03 | 19.79 | -16.21 | -0.95% | 1354 | 27 | 8 h | 56 h | 0 |
| 2024-04 | 14.68 | -21.32 | -1.30% | 1765 | 21 | 16 h | 136 h | 0 |
| 2024-05 | 12.94 | -23.06 | -1.36% | 2070 | 18 | 20 h | 104 h | 0 |
| 2024-06 | 39.29 | **+3.29** | **+0.20%** | 660 | 15 | 16 h | 160 h | 0 |
| 2024-07 | 13.44 | -22.56 | -1.33% | 1993 | 15 | 16 h | 104 h | 0 |
| 2024-08 | 3.72 | -32.28 | -1.90% | 7204 | 22 | 16 h | 80 h | 0 |
| 2024-09 | 14.83 | -21.17 | -1.29% | 1748 | 17 | 24 h | 96 h | 0 |
| 2024-10 | 2.43 | -33.57 | -1.98% | 11002 | 18 | 8 h | 72 h | 0 |
| 2024-11 | 63.59 | **+27.59** | **+1.68%** | 408 | 16 | 16 h | 112 h | 0 |
| 2024-12 | 31.69 | -4.31 | -0.25% | 845 | 11 | 16 h | 216 h | 0 |

**No month clears all three gates, and every gate fails by an order of magnitude rather than by a margin.** The best
level is June's 0.20 percent annualised against a gate of ten, and eight of the nine months lose money after the
round trip. The best payback is June's 660 hours against a gate of 72.

**The third gate is the one that explains the other two.** The differential does not persist: a month holds eleven to
twenty-seven separate runs of it being positive, the median run is eight to twenty-four hours, and **not one run in any
month, including the longest of two hundred and sixteen hours, earns enough to pay the 36.00 USD round trip.** That is
the bleeding loop stated as a measurement: enter, pay the fee, watch the sign flip inside a day, leave, and pay it
again.

Two limits on the sample are recorded with the result. All twelve months of 2024 are measured, the three whose first
OKX fetch failed having been re-fetched, and the monthly figures are the two files' own windows rather than the
engine's clock, which is why March reads 19.79 here against the recorded run's 17.58. The fee is the repository's own
measured 18.00 USD a side for two legs, doubled for the exit, and the venue risk, the counterparty risk and the KYC
wall the owner names are not in these numbers at all, because they can only make a failing result worse.


Everything measured so far prices one income stream, the spread a fill captures, and one cost, the fee. That is why
every sweep ends in the same place: the spread is a fraction of a tick and the fee is a multiple of it. A second
income stream exists that does not depend on speed or on capturing a spread — a perpetual pays funding, a future
pays or charges the basis at roll, a stock position pays dividends and charges financing — and today the accounting
cannot represent any of it. Adding it is the next capability, and it is deliberately built in this order:

* **First a flow with a fixed rate**, applied per interval to a position, verified by a unit test that states the
  cash, the equity and the realized PnL after N intervals. This makes the mechanism checkable before any data
  plumbing exists, in the same way the queue model was verified before live fills were discussed.
  **Done:** `Portfolio::accrue_interval_flow` and its per-interval pass over the whole book, with those three
  fields asserted after N intervals in `tests/portfolio_test.cpp` for a price-quoted position, a fixed-face
  contract and a short, and the channel described in [Architecture](../architecture/06-accounting-and-the-summary.md#7-backtest-and-performance-metrics)
  The rate is the per-interval return to a long on the position's notional, so the position's sign decides the
  direction and one rate serves both sides of a market, which is the shape the next step needs.
* **Then a rate series as an input**, because the real rate varies by the hour and changes sign. The free sources are
  the exchange's own funding history, which costs nothing and is a few megabytes per symbol per year.
  **Done:** `tools/fetch/fetch_funding_rates.py` fetches either source for one instrument (OKX `module=3`, and the Binance
  Vision monthly `fundingRate` archive, which has no daily folder), `src/market/funding_rate.*` reads both formats and
  the replay applies each settlement as the event clock passes it, reported as a `[FUNDING]` line with the file's own
  arithmetic beside the USD the window moved. Two facts from the real files are recorded with it: the OKX day is cut
  on midnight UTC+8 while its book archive is cut on UTC, and the Binance settlement clock carries milliseconds of
  jitter, so its interval is eight hours to within a second rather than exactly.
* **Then two legs**, because a carry position is a position and its hedge, and the ledger has to hold both without
  letting one leg's exposure be read as the account's.
  **Done:** the `carry` mode holds a stated USD notional on each of two instruments and reports them separately
  and together, because a hedged book holds two notionals and is exposed to none of them. `carry_gross_notional_usd`
  and `carry_net_notional_usd` are both printed so neither can be mistaken for the position, each leg's funding is
  charged at that leg's own mark, and the fills are priced through the same `fee_on_fill` the matching engine uses,
  so a leg booked outside a book cannot cost something else. `carry_breakeven_hours` is the same comparison in the
  form a decision needs, because a fee is paid once while a carry accrues every interval.

The acceptance criterion is one number a reader can check: **annualized carry against every fee the position pays**,
on real data, per venue. **Recorded:** Binance BTCUSDT against OKX BTC-USDT-SWAP in March 2024, 10,000 USD a leg,
728 hours: 304.66 USD of funding paid on one venue and 322.24 received on the other leave 17.58 USD of carry
against 18.00 USD of taker fees, so the annualized ratio is 11.76 while the month itself ends 0.42 USD down, and
the position needed 745 hours to pay for itself rather than the 728 it ran. Held the other way round the same
position loses 35.58 USD with a negative ratio. The criterion's answer is therefore that the line has a number
where the market-making line had none — and that a criterion expressed as an annualized ratio against a one-off
fee can read as a winner on a position that did not quite pay for itself. The two runs and every field of them are
recorded in [Performance History](../performance/history-03-studies.md#the-acceptance-number) with the command that re-derives
them, and the two funding files are fetched with one command each, named in
[the data index](../DATA.md).

## Back to the market-making line: what would make a quote pay for itself

The carry line answered its criterion and the answer was thin, so the demo's first line is the one worth
returning to — with the question stated as an inequality rather than as a hope:

```text
capture per fill + rebate  >=  maker fee + adverse selection
```

Every term in it is already measured here, and the third was the one no run had reported as a target: the fee is
in the portfolio, the capture and the markout are in `[EXECUTION]`, and the `[PNL]` line now turns the fee into a
rate. The first experiment (`tools/sweep/fee_sweep.sh`) asked the fee question directly, on four windows and three
maker rates:

* At a one-tick quote a window loses 0.0030 to 0.0032 USD **before any fee** — about three to four ticks a fill
  against a capture of 0.1 to 0.3 ticks — so its breakeven maker rate is -0.0003, which means the venue would have
  to pay a rebate larger than its own 0.0002 maker fee. Zero maker fees still leave the largest window down 0.86
  USD over 267 fills. **At a tight quote the fee is not the binding constraint; the fill population is.**
* The capture only exceeds that cost at eight ticks of width, where one fill traded and nothing closed: the door
  exists and its width is unknown, which is a statement about sample size rather than about the mechanism.
* Both tables are recorded with every field in
  [Performance History](../performance/history-03-studies.md#the-maker-fee-each-window-would-have-needed)

The order this leaves, each step measurable on its own:

| Step | What it does | What it settles |
|---|---|---|
| **F1 — the fee question** (done) | `[PNL]` splits quoting from the position left open and reports the breakeven maker rate; `--maker-fee-rate` and `tools/sweep/fee_sweep.sh` sweep it | whether a fee tier could pay for this, answered per window |
| **F2 — a decision rule that covers adverse selection** (done) | the gate now requires a quote to cover the fee **and** what our own passive fills at that distance have cost, measured from the venue's prints first and from our own fills after the record showed the two disagree in sign | the mechanism works: refusing the quotes that cannot pay cut the one-tick loss from 0.859 to 0.050 USD and turned the four-tick row positive. What it cannot yet settle is the measured part, because the gate's own success starves its estimator below twenty samples |
| **F3 — inventory discipline** (done) | `--flatten-band=N` reduces a position past N units with a taker order at the touch, and the closed-versus-total split says which part of a result came from quoting and which from the position carried | flattening works and costs more than it saves, and the one positive run in the table was positive *because* it carried a position: the bet paid, not the quoting |
| **F4 — a market where the inequality can hold** (done) | the instrument half is in: `us_equity_spec` states the one cent tick and the commission per share, `micro_es_spec` states the quarter point tick, the multiplier of 5 and the commission per contract, and `fee_on_fill` charges both terms. The data half is Databento `mbp-10` converted to the CSV pair the existing readers take | **the answer is the first positive window in this repository, and it is narrower than the question.** See below |

The three levers a market maker actually has are a rebate, a wide spread and speed. This repository can measure
the first two, model the third, and say which of them a given window needs; what it will not do is present a
window selected for being positive as an edge.

### What F4 found

The same decision layer, the same two hours of one session (2026-09-29 13:30 to 15:30 UTC, the US cash open), on
the two markets where the fee is a fraction of a tick instead of a multiple of one. Every row is re-derived by
`tools/sweep/f4_windows.sh` from Databento files that `tools/fetch/fetch_databento.py` downloads and converts.

| Window | Fills | Units | Fee USD | Capture, ticks | Markout, ticks | Gross USD | Net USD | Net per fill | Breakeven maker rate |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| micro E-mini, one tick | 1675 | 3934 | 2439.08 | 0.0131 | +0.157 | -387.50 | -2826.58 | -1.6875 | -0.00000255 |
| micro E-mini, two ticks | 57 | 122 | 75.64 | 0.4795 | -0.689 | -196.25 | -271.89 | -4.7700 | -0.00004157 |
| micro E-mini, gate on | 1599 | 3750 | 2325.00 | 0.0111 | +0.160 | -98.75 | -2423.75 | -1.5158 | -0.00000068 |
| micro E-mini, four ticks | 0 | 0 | 0 | 0 | 0 | 0.00 | 0.00 | 0.0000 | 0 |
| **SPY, one tick** | **799** | **33352** | **116.73** | **0.2679** | **+0.092** | **+379.24** | **+262.51** | **+0.3285** | **+0.00001487** |
| SPY, two ticks | 263 | 12279 | 42.98 | 0.6730 | -0.247 | -32.83 | -75.81 | -0.2882 | -0.00000350 |
| SPY, one tick, gate on | 791 | 33116 | 115.91 | 0.2471 | +0.112 | +216.18 | +100.27 | +0.1268 | +0.00000853 |
| SPY, one tick, flattening above 1000 shares | 832 | 34618 | 121.16 | 0.2412 | +0.114 | +282.58 | +161.42 | +0.1940 | +0.00001080 |

What the table says, in the order it has to be read:

* **The fee stopped being the whole story, and on one market it stopped being a loss at all.** A US share pays
  0.35 of a tick in commission against a one cent tick, and at a one tick quote the run ends **+379.24 USD
  before fees and +262.51 USD after them** over two hours, on 799 fills of 33,352 shares. Its breakeven maker
  rate is **+0.0000149**, which is positive: the venue could charge a rate on top of the commission and the
  window would still be flat. Every window measured before this one had a negative breakeven rate, which is the
  statement that no fee tier could have saved it.
* **It is quoting, not the position, that earned it.** The closed part of the same run is **+447.25 USD gross
  and +330.52 after fees**, while the position left open at the end cost 68.01 — the opposite of the earlier
  perpetuals row, which was positive because the carried position paid, not the quoting. The F3 band, which
  exists to stop a run carrying a position, is applied in the last row and *lowers* the result from 262.51 to
  161.42: flattening costs more than it saves, again, and the measure of that is now two markets instead of one.
* **On the micro E-mini the fee is half a tick and the answer is still no, for a different reason.** There the
  commission is 0.62 USD a contract against a tick worth 1.25, so two sides cost about one tick, and the run
  still loses: at the touch the fills capture **0.0131 ticks** each, a hundredth of one, against a fee of half a
  tick. Widening to two ticks multiplies the capture by thirty-seven and cuts the fills from 1,675 to 57, with a
  markout of -0.689: the quotes that fill at two ticks fill because the market is going through them. At four
  ticks, nothing fills in two hours. **The constraint moved from the fee to the fill population and its
  selection, and the fee is no longer what to blame.**
* **The gate that was built for the perpetuals is not a universal improvement.** `--markout-cover` cuts the
  micro E-mini gross loss by three quarters, from -387.50 to -98.75, which is what it was built to do. On SPY,
  whose measured markout is positive, the same gate removes profitable fills and lowers the window from +262.51
  to +100.27. A rule that refuses quotes it cannot pay for is a filter for the case it was built for, and
  applying it to a market that does not have that case costs money.

**Why this is not yet a strategy.** Four things have to be said in the same breath as the positive row. The book
is **Nasdaq's alone**, so the queue ahead of a quote is the displayed size on one venue out of a consolidated
market: a quote that would have been first in the national book can be third here, and the fill population is
therefore optimistic in the one direction that matters. It is **two hours of one session**, in the part of the
day with the most mean reversion. The commission is the tiered rate of a large retail broker and **the tier a
Hong Kong account actually gets is unverified**, and a zero commission tier would add 116.73 to the result. The
model does not include the **venue's own maker rebate**, which on this market is of the same order as the
commission, so the number understates the economics on that side — and it also does not include the regulatory
fees of a few hundredths of a cent a share on the other. **And the window does not survive latency**: one
millisecond of one-way delay takes it from +262.51 to +9.65, so it is the margin and not the sign that goes, and
above one millisecond the column is dominated by the position two hours of one session carries — which is the sixth
gate of phase 2b failing and is recorded in
[Performance History](../performance/history-04-the-fee-is-not-the-wall.md#the-latency-that-window-would-have-needed).
The -1143.75 row this page used to quote at five milliseconds was not the link: it was the replay dropping the
cancels of orders still in flight, and with that fixed the same cell reads -179.95 on 901 fills.

## From a Positive Window to a Running System

The section above ends at a measurement. This one runs **backwards from the deployment** rather than forwards
from the code, because what a live path can actually be decides the shape of everything else: which markets a
Hong Kong personal account may trade, how many legs that leaves, and whether the fee survives the arithmetic at
the scale a quote really earns it. Each step still states what has to be true, what would kill it, what it costs
and what it leaves behind, so a step that fails is a result rather than a setback.

Two numbers from the recorded row frame the whole chain. About **40% of the positive window's closed profit was
the two legs of the spread** and about **60% was the mid drifting in our favour while the position was held**:
0.267855 ticks of capture per share per fill is about 0.54 ticks on both legs of a round trip, against 1.34
ticks per share realised. And that same run's peak inventory was **1099 shares, about 841164 USD of exposure**.
Drift is not a skill, and the exposure is why size belongs in the claim rather than in a footnote.

### The deployment filter, applied first

| Market | Live for a Hong Kong personal account | What it is left as |
|---|---|---|
| Offshore crypto perpetuals | **no**, not licensed here | the measurement bench, and nothing else |
| Locally licensed crypto venues | spot only; derivatives generally for professional investors | not on the critical path |
| HKEX index futures | yes, but a round turn costs about 0.8 to 1.2 of a tick against a one tick spread | a hold or multi-leg candidate, not a quoting one |
| US stocks and ETFs | yes | **a single leg candidate** |
| CME micro futures | yes | **a single leg candidate** |

The two survivors are one instrument with two-sided quotes, which is what single leg means here: one symbol, one
venue, a bid and an ask resting together, and inventory as the only directional exposure. The account rules then
separate the two, and they separate them in favour of futures:

| Constraint | US shares | CME micro futures |
|---|---|---|
| Day trading minimum equity | applies in a margin account | does not apply |
| Short side | borrow, locate and the short sale price rule | native, no borrow |
| Settlement | T+1, with turnover limits in a cash account | none |
| Margin | Reg T or portfolio margin | a lower intraday requirement |

Everything that table removes is removed from the critical path on purpose: crypto stays a bench, the local index
future stays a hold candidate, the two-legged carry stays a documented capability, and the engineering goes to
one instrument rather than to a portfolio of them.

### The arithmetic that picks the instrument

A fee below one tick is necessary and not sufficient. What decides a market is the capture a fill actually
realises against the fee it pays, and both markets that passed the fee filter have now been measured:

| Market | spread where the trades print | half of it | captured per fill, measured | fee per side | fee / capture |
|---|---:|---:|---:|---:|---:|
| SPY, 2026-09-29 window | 1.000 tick | 0.500 | **0.267855** | 0.350 | **1.31** |
| micro E-mini, same window | 1.000 tick | 0.500 | **0.013091** | 0.496 | **37.9** |

Two things come out of that, and the first one corrected this plan while it was being written. **The spread a
market shows in its book is not the spread its fills happen at**: SPY's median book spread is 2.000 ticks while
the rows where trades print have a median of 1.000, so a screen written against the book would rank symbols by a
number nobody trades at. The second is the gap between the half spread at the trades and what a fill actually
captured — a factor of 0.54 on SPY and 0.026 on the micro E-mini — which is the adverse selection and the timing
inside the fill itself, and which is why neither market pays for its fee even though that fee is a fraction of a
tick.

**Those captures are the conservative queue model, and on SPY that is a floor rather than the window's answer.**
The same window under the other three models fills 2,281 to 2,716 times instead of 799 and captures 0.4189 to
0.4364 ticks instead of 0.267855 — so the model decides the fill count by 3.4x, and against the commission
alone it decides the sign as well: a fill pays 0.35 ticks of commission, so the floor's capture covers 77% of
it while the other three cover 120 to 125%. SPY is therefore the first window measured whose **quoting edge
sits on the fee rather than under it**, which is what its positive breakeven rate says from the other side, and
it changes what the search has to find: not a market whose fee is small, but one whose spread is wide enough
that a fill's capture clears the fee even at the model's lower bound. The measurement behind that, the equity
book's churn against its traded volume, and why the effect is larger here than the perpetuals' small fill
counts could show are in
[Performance History](../performance/history-02-l2.md#the-same-measurement-on-an-equity-book)

**Two things have since been measured about that sentence, and the second narrows it.** The tape says which
model is the right one: on AAPL and on AMD, **5.0 and 4.0 percent of the units that leave a price level leave
because they traded, and the other 95 or 96 percent are cancelled**, so a model that moves a queue only on
trades is a floor by a factor and not by a shade. And the sign: under the **account's whole schedule** — 0.94
to 1.00 ticks a fill rather than 0.35 — the four models fill 2,294 to 4,775 times, capture 0.655 to 1.073
ticks a unit, and **lose money in all four**, 2,026 to 2,207 USD on the same two hours. So the model decides
the fill count and the capture, and against what a fill actually pays it does not decide the sign. The tables,
the clip sweep that shows the per order floor diluting from 0.418 to 0.270 ticks a unit, and the refresh sweep
that shows the floor is bought with capture, are in
[Performance History](../performance/history-03-studies.md#what-the-queue-model-decides-and-what-the-per-order-floor-does)

The screen is therefore **the half spread at the trades, at least three times the fee per side**, measured by
`tools/study/screen_spread.py` from top of book before any depth is bought; the tool prices a window before it spends
and refuses to download without a stated cap. SPY fails it at **1.43** of the required 3, which is the expected
result for a one tick market, and the sentence that used to end this paragraph said the search therefore had to
leave the most liquid symbol in the world. **The run falsified that sentence.** Thirty symbols over the same two
hours, for **0.52 USD**, and the two that pass both halves of the gate are **AAPL at 4.29 and TSLA at 7.14**,
which are among the most liquid names in the market rather than away from it: on a single venue their displayed
books are three to six cents wide where SPY's is two, and 72% of their trades print against those wider quotes.
The run itself, and what those two symbols then did in the engine, are in
[Performance History](../performance/history-04-the-fee-is-not-the-wall.md#the-screen-run-the-first-two-symbols-that-pass-it).

### The activity floor, pre-registered

A wide spread with no trades is not a market, and the screen deliberately does not choose this number, so it is
recorded here **before the next screen runs** rather than picked after seeing one. What it is a floor on is the
predicted fill count, because that is what a conclusion needs and the screen cannot measure fills from top of
book. What it can measure is the trade prints, so the question is what a trade print is worth in fills, and the
two windows measured so far answer that:

| Window, 2026-09-29 13:30-15:30 | trade rows | rows the screen read | fills, synchronous | trade rows per fill |
|---|---:|---:|---:|---:|
| SPY | 28,592 | 2,205,840 | 799 | 35.8 |
| micro E-mini | 119,842 | 3,843,413 | 1,675 | 71.5 |

**The floor is 2.8 trade prints a second**, which is 20,000 in a two-hour session. The arithmetic, with each
input named: this repository does not draw a conclusion from fewer than thirty fills
([Design Review](../DESIGN_REVIEW.md)); the worse of the two measured yields is one fill per 71.5 trade rows; and
the margin is tenfold, which is the one judgment in the number. The judgment is sized from the F4 sweep's own
result that a quote one tick further from the touch fills about three times less (799 to 263 to none as SPY's
quote widened), taken at two ticks of unknown difference between a candidate's book and these two. Thirty fills
at 71.5 rows per fill is 2,145 trade rows and the margin makes it 21,450, which is written down as 20,000 so
that the floor is a number a reader can hold in their head: 20,000 trade prints in a two-hour session is 2.8 a
second.

Both symbols measured so far pass the floor, at 3.97 and 16.6 trade prints a second, which is the right shape
for it: it is aimed at a market too quiet to measure, not at the two liquid markets this line has already
failed on spread. It is applied by `tools/study/screen_spread.py --min-trades-per-second=2.8`, which prints
`trades_per_second` and its own verdict beside the spread verdict; the tool does not carry the number, so
changing the floor means editing this paragraph, and doing that *before* the next screen is what makes that
screen's verdict pre-registered.

**It has now been applied, and it is what decided the run recorded above.** The three funds whose spreads are
widest trade far too rarely: JNUG at 43.6 times the fee per side manages 0.016 trade prints a second, NUGT 0.042,
LABU 0.054, against the floor's 2.8, so the floor removes them and their spread is not what disqualified them,
which is exactly the case this paragraph exists for. The two symbols the floor kept trade 7.65 and 6.86 times a
second, about 2.5 times the floor, and SPY comes back at 3.97, which is the number this table derived the floor
from -- the check that the screen and the floor are counting the same thing.

**A second rule is now pre-registered, before any candidate has been screened under it, and it is the first one
with a price in it.** The rule above charges every symbol the same fee per side. That was right while the fee was a
commission and it is wrong now that the account's schedule is recorded: a commission of 0.0035 a share is the same
0.35 of a tick on any share whose tick is a cent, but the **SEC fee is charged on the proceeds of a sale**, so it
is 0.685 of a tick on a 332 dollar share and 0.021 on a 10 dollar one; the account also charges clearing and the
audit trail a share a side, and credits 0.0013 a share for the fills that add liquidity. The round turn in ticks is
therefore a function of the share price rather than a constant:

    round_turn_ticks(price) = 2 * (commission + clearing + audit - rebate + 0.5 * regulatory * price) / tick

with the recorded figures -- commission 0.0035, clearing 0.0002, audit 0.000003, rebate 0.0013, regulatory
0.0000206 -- and the half in front of the regulatory term is because the fee is charged on a sale while a market
maker sells on half its fills. **The rule is the one above with that denominator**: a symbol passes when its median
spread at the trades is at least three times the round turn of its own price. Two consequences are written down
before the run rather than read out of it. The commission-only rule passes a 332 dollar share whose spread is three
ticks; the same share under this rule needs 3.508, so **AAPL and TSLA are expected to fail the rule they passed**,
which is not a reason to change the rule but the arithmetic that makes the price column a search dimension rather
than a footnote. And a ten dollar share with a two cent spread passes here, where the commission-only rule would
have ranked it at 2.86 and failed it. **The minimum per order is deliberately not in the denominator**: it is
charged per order instance rather than per share, so it depends on how often a policy replaces its quotes, which
top of book cannot measure. It stays where it is measured, which is the engine, and a symbol that passes here is
what gets run there. The rule is applied by
`tools/study/screen_spread.py --fee-schedule=min=0.35,regulatory=0.0000206,clearing=0.0002,audit=0.000003,rebate=0.0013`,
which prints both verdicts, the old one beside the new, so the recorded table stays re-derivable and a reader can
see which rule produced which fail.

**It has since been run, over the thirty symbols already in the cache, for nothing, and it both confirmed and
falsified what was written above.** The confirmation: **AAPL fails the rule it passed**, at a schedule ratio of
2.573 against a threshold of 3.498 ticks, which is the price column doing its job -- a 332 dollar share's SEC fee
is 0.685 of a tick a sale, and it is what pushed the round turn from 0.700 to 1.166. The falsification: **TSLA
passes**, at a schedule ratio of 4.132, because its median spread at the trades is five ticks where AAPL's is three,
and the prediction above said both would fail. The pre-registered arithmetic was also off by a hundredth of a tick:
at AAPL's median trade price of 332.71 the threshold is 3.498, not the 3.508 written above, and the number is
recorded here rather than quietly corrected, because a pre-registration that can be edited after the fact is not
one.

**And the price column did not produce the new candidate it was hoped for.** Every cheap share in the list fails
the spread axis rather than the fee axis: AMC at 3.26 USD, BB at 8.52, TLRY at 4.02 and MSOS at 5.12 all have a
**median spread at the trades of one tick**, which is 0.5 of a tick a side against the 0.35 commission, and the
regulatory saving their price buys them is a fifth of a tick. The US equity tick is a cent whatever the share
costs, so a cheap share is not a wide one in ticks, it is a narrow one, and the names whose spreads are wide in
ticks are the ones the activity floor removes: JNUG, NUGT and LABU pass the spread rule at 38, 24 and 68 times the
round turn and trade 0.02, 0.04 and 0.05 times a second against the floor's 2.8. **The screen has now measured both
ends of that trade-off, and the direction it leaves is a candidate list this cache does not contain**: shares whose
spread is wide in ticks *and* whose flow clears the floor, which on this evidence is a mid-cap single name rather
than an exchange traded fund or a mega cap. That list is the next purchase, it is a few dollars of top of book, and
the rule it is judged against is the one above, written down before any of it was screened.

### Line nine, pre-registered: the mid-cap single names

**The list, and the three properties it is built from rather than picked by.** Thirty single names, none of them in
the cache the two screens above were run on: **U, DKNG, CHWY, RUN, IONQ, ACHR, JOBY, HIMS, AFRM, UPST, TOST, SMCI,
HL, CDE, AG, UEC, UUUU, RIG, VAL, NE, TDW, RXRX, CRSP, OKLO, LUNR, RGTI, QBTS, SOUN, EXK, SOFI**. A **price between
5 and 80 USD**, because the SEC fee is charged on the proceeds of a sale and is the largest single term in an
equity fill: at 20 USD it is 0.021 of a tick a sale against the 0.685 the AAPL screen was failed by and the 1.58 a
765 dollar share would pay. A **single name rather than a fund**, because a basket's spread is arbitraged down to
the tick that buys it while a mid-cap's is not. And **no name already screened**, so the answer is new rather than
a second look at a list that has already had one.

**The rule is the one above, unchanged, and four things are written down before the window is priced.**

1. **What it costs**: top of book, one two-hour window, priced by the screen's own cost mode before any download
   and bought only under a five dollar cap. The recorded rate is 0.52 USD for thirty symbols, and the credit it
   comes out of is 124.03 USD that becomes zero on 2027-03-30.
2. **What passes**: a `schedule_ratio` of at least **3.0** — the median spread at the trades against three times
   the round turn of the account's own schedule at that share's price — **and** at least **2.8 trade prints a
   second**, which is the activity floor recorded above. Both halves, or it does not pass.
3. **What a pass buys**: one ten-level two-hour window of the best-passing symbol through the pinned policy under
   the account's schedule, which is the measurement every recorded equity row is and costs about 0.20 USD.
4. **What a fail means**: **the equity quoting line is closed with its numbers**, and whatever is left of the
   credit is recorded here as forfeited **with the amount**, rather than left unspent and unnoticed.

**The stop line is the point of writing this first.** Every screen so far has produced either a symbol to buy or a
reason the axis was wrong; this one is allowed to produce only the first, because the two axes it could fail on —
spread and flow — have both already been measured at their ends and there is no third axis the screen can see.

**It has not been bought, and the reason is not the price.** The decision on 2026-11-01 was to forfeit the 0.165701
USD — the thirty names priced at 0.000263 on VAL and 0.019674 on IONQ, a seventy-five-fold spread that follows the
message count — keep the credit, and close the equity quoting line; the record has to carry the reason that actually
decides it rather than the tidier one. The tidier one would be "a pass line was registered against this screen, so
nothing was run"; that is a category error, because the line is what a *run* is judged against and the 0.17 USD is
the price of the measurement the line would judge. What closes the line is measured, twice: **the same window's
quoting is negative before any fee at all** — a gross closed P&L of -1,119.04 USD, or -1.03 ticks a unit, on the
model the tape supports — so a symbol that clears this screen at three times its round turn would still owe a
measurement of whether its gross clears zero, and **the sixth gate already took the one positive window to nothing
at one millisecond each way**. A screen answers neither question, and a screen that cannot change a decision is not
worth its 17 cents.

**The forfeit is therefore recorded as a judgement about what the answer would have been worth, not as a test of
resolve.** A rule kept for its own sake is the same error as a rule bent for seventeen cents, and writing the
tidier reason into this document would have been the third error of the three.

### The seven things that must be true before any engineering

| # | Must be true | The cheapest way to kill it | Cost |
|---|---|---|---|
| 1 | the half spread at the trades is at least three times the fee per side | `tools/study/screen_spread.py`, which prices the window before it spends and refuses to download without a stated cap — **run: AAPL and TSLA pass, thirty symbols, 0.52 USD** | 5 to 20 USD |
| 2 | capture stays positive across days once the drift is gone | eight to ten sessions, and the hours outside the open — **its first half is answered for nothing as of 2026-10-02**: the AAPL window cut into eight leaves the capture positive in all eight, and the position term is thirty times the edge, so the second half is unanswerable at a personal account's scale rather than merely unfunded | 10 to 20 USD for full sessions, or **2.30 for ten two-hour windows**, which is the unit every recorded equity row uses — see the budget section below |
| 3 | our order really rests where we measured it | pin a destination and re-measure, or record where orders actually land for a week and measure that | free, then 1 to 2 USD |
| 4 | the account may run it: day trading minimums, borrow, settlement | a channel answer, not a code change | free |
| 5 | the broker's rate limits and latency carry the pace the strategy needs | its published limits, and the latency the repository already models | free |
| 6 | the strategy survives a realistic round trip | the two latency flags the repository already has, at five and at fifty milliseconds — **run, and it failed** | free |
| 7 | the sign survives the fee tiers | **the three this account actually has, read 2026-10-02**: Tiered with the maker rebate passed through, Tiered without it, and Fixed, which bundles the exchange fee and offers `Fixed - Direct Routing`; the zero commission schedule is not available to this account | 5 to 10 USD |

**A failure on any of the seven sends the quoting line back to a research entry**, and the repository returns to
what it is best at, which is auditing the execution and the fee of a strategy developed elsewhere. That is a
result rather than a setback, and it is the reason these seven come before any live plumbing.

**The sixth gate has now been run, and it returned a failure.** The positive window loses its margin at one
millisecond each way (+262.51 to +9.65), and above that the column is the position two hours of one session
carries rather than the delay, so the tolerance is not a personal account's to control; the reflex fix of quoting
more slowly measures worse rather than better. The rows and the mechanism are in
[Performance History](../performance/history-04-the-fee-is-not-the-wall.md#the-latency-that-window-would-have-needed) and the sweep that
reproduces them is `tools/sweep/latency_sweep.sh`. That sweep's own last paragraph says the one cheap test left is a
symbol whose spread is wide enough to absorb the same delay, and that test is the screen.

**The first gate has now been run too, and it returned a pass.** AAPL and TSLA are the first symbols whose spread
and activity both clear the pre-registered rule, and the engine says what the screen said: their capture per fill
is 0.640 and 0.979 ticks at the conservative model against a fee of 0.35, the first markets in this repository
where the quoting edge covers the commission at every point of the queue model space. **And both windows still lose
money** -- 3,165.53 USD on AAPL and 1,475.53 on TSLA -- for a reason the fee table cannot see: the loss is the
position the run carries, 1,096 and 1,098 shares of it, and tightening the inventory limit lowers the loss
monotonically, because the drift is proportional to the exposure while the capture is not. So the wall moved
rather than fell, and the gate that carries the line is now the second one: does the capture stay positive, with
the position's own move separated out, across days a single session cannot speak for. Gates 3, 4 and 5 are free
and need a channel answer, and the fee tiers (7) are worth 5 to 10 USD now that a symbol exists to run them on.

**The second gate has now been taken as far as it can be taken, for nothing, and it returns a pass and then a dead
end.** Cutting the AAPL window into eight quarter hours needs no new data: `tools/study/analyze_l2_windows.py` cannot cut
these files, because it re-runs the engine per window and a converted Databento file carries one snapshot, in its
first row, so a slice has no book state to apply its deltas to. The engine already writes every fill it made, so
`out/probe_windows.py` cuts the window from its own ledger instead, and the buckets say two things the purchase
could not have said better. **The capture is positive in all eight quarters**, +0.26 to +1.38 ticks a unit, mean
+0.70, so the first half of the gate passes and the quoting edge is not a one-window artefact. **The position term
swings by 1,930 USD inside one of those quarters**, against a fee bill of 210.58 for the whole two hours, and the
single bucket of the eight that is positive is positive because its drift was +12 rather than because its capture
paid: that bucket's capture, +0.3794, is under the 0.579 the schedule charges a unit before any minimum. That
leaves the expected value: 0.6402 ticks captured against 0.940 a unit charged with the minimum per order and 0.579
without it is **-181 USD a window or +36.70**, so the floor decides the sign and nothing else does -- but a per
window position draw on the order of 3,000 USD against an edge of 37 to 100 makes the sign unobservable, needing
3,600 to 26,000 windows, or fifteen years of daily two-hour sessions at the cheap end. Smaller size does not help,
because both terms are proportional to the volume quoted, which is what makes this a dead end rather than a
funding problem. The rows are in
[Performance History](../performance/history-04-the-fee-is-not-the-wall.md#what-the-accounts-fee-schedule-adds)

**The cheap schema was priced as a way around that, and measured against the expensive one instead of assumed.**
The screen's own downloads are top of book only, under the cached files in `data/v2/screen/`, and an AAPL file for
the recorded window was already there, so the comparison cost nothing: converted and run through the same policy it
gives 1,182 fills against the ten level file's 2,324, and 67,157 submitted orders against 45,907. The mechanism is
visible in the same output -- with one level, a book whose touch is taken has nothing left, so quotes land behind
the touch 23,148 times instead of 11,596 -- which is why **the cheap schema is not a substitute for a measurement,
while a two-hour window is the measurement and costs a quarter of a full session**, and the budget below is
corrected accordingly.

### The data budget, and its deadline

One budget fact changes how the three remaining gates should be run rather than whether they can be: the
Databento account holds a **new user credit of 124.03 USD, applicable to historical data only, that expires on
2027-03-30**. Credits are consumed earliest-expiry-first and this is the only one, so there is no second balance
behind it. If the grant was 125 USD, then **about 1.85 USD of it has been spent**, and the components matter more
than the total: 0.97 on the two two-hour `mbp-10` windows the F4 results are measured on, 0.52 on the screen's
thirty symbols of top of book, and 0.36 on the two-hour `mbp-10` pair for the AAPL and TSLA windows -- of which
0.18 was spent twice, because the first pair was downloaded without `--pretty-px`, which leaves the price column
as the vendor's 1e-9 scaled integer and kills the run with `std::bad_alloc`. That is the only money this
repository has spent twice so far, and the flag is now in `tools/sweep/f4_windows.sh`'s own comment so the next reader
does not repeat it. The credit is billed on the DBN the query returns rather than on the CSV written, which is why
a re-download of the same window costs the same as the first one.

What it buys, at the rates recorded above (0.5 USD a gigabyte of DBN on the micro E-mini, 2 on an equity, and a
two-hour equity window under a dollar):

**The unit matters more than the rate, and the estimates below were written before that was noticed.** They are
priced for **full sessions of six and a half hours**; every recorded US equity row is a **two-hour window**, a
quarter of a session, and it is the window that a gate is measured on. At the measured 0.23 USD for AAPL's two
hours in ten levels, **ten sessions of two hours cost 2.30 USD and the sixty-odd sessions cost 14**, against the 10
to 20 and 40 to 95 written above. The cheap top-of-book schema is **not** the way to that number -- it was measured
on 2026-10-02 and it moves the fills and the orders by half, so it buys a different measurement rather than the same
one for less -- and the route to a cheaper campaign is the window length and the symbol count, both of which the
recorded rows already use one of.

**The multi-day campaign is now pre-registered, and its rules are written here before it runs.** The purchase is
seven more two-hour windows of MU at 13:30 to 15:30 UTC -- 2026-09-22, 09-23, 09-24, 09-25, 09-28, 09-30 and
10-01, with 09-29 already on disk, so **eight windows in ten levels for 0.608 USD**, priced a day at a time before
any of it was bought. (2026-10-02 is not among them because it is not available: the dataset's own available end is
that day at 13:20 UTC, twenty minutes before the window opens.) MU is the symbol because it is the one whose screen
ratio is 10.770 and whose quoting margin measured positive at matched exposure; AAPL is deliberately not bought,
because the account's own schedule fails it at 2.573.

Each window is run twice, at the pinned size the recorded rows use and at a notional-matched size, both with the
account's schedule. **Four rules decide what the campaign means, and they are stated before anything is measured:**

1. the quoting margin -- capture less markout less every fee including the minimum per order -- is positive in at
   least six of the eight windows: **the quoting edge is out of sample**;
2. the mean and the median of the eight net results are both positive: **the line has an expected value**;
3. the mean is positive and the median is not: **the line is carried by outliers**, it is recorded as that, and the
   exposure cap becomes mandatory rather than optional;
4. the mean is not positive: **the quoting line is closed**, with the numbers that closed it.

What the campaign cannot decide is written down with the rules: eight windows are eight draws of the position term,
which is the term that has decided every window so far, so no rule above speaks to how large the edge is against
that noise -- only to whether the edge is there.

**It has now been run, and it returns a pass on the rule that mattered and a qualified answer on the second.** The
seven windows cost 0.608 USD and the eighth was already on disk: **the quoting margin is positive in all sixteen
rows** -- eight windows at two sizes -- averaging 940.59 a window against a standard deviation of 382.24, so rule 1
holds out of sample. Rule 2 holds at the pinned size, mean +4905.77 and median +4635.13, and falls to rule 3 at the
matched size, mean +743.68 and median -542.82, and **the omission that makes both readings possible is mine**: the
pre-registration above does not say which size the four rules apply to, and it is left in place as written rather
than edited now that the answer is in.

**And the campaign measured the number this line has been carrying as an estimate: the noise is eleven times the
edge.** The quoting margin sums to 7524.69 across the eight windows with a per-window dispersion of 382.24, while
the position term sums to 31721.57 with a dispersion of 10587.96, so **about 506 windows -- two years of daily
two-hour sessions -- is what two standard deviations need** to separate one from the other, and 81 percent of the
campaign's +39246.13 is the position. That is the honest state of the line: the fee question is settled in the
quoting's favour, sixteen rows deep, and the question that remains is not a fee question.

### Where the line goes next, and why the venue is the variable

**The campaign settles the instrument and leaves the venue, and the four properties that decide a venue are written
down as a test rather than as a preference.** A retail quoter needs fees low enough to be a term rather than the
term itself; a spread wide in ticks rather than in basis points; no speed race; and a counterparty that is not a
better informed market maker. US large cap equities fail all four -- a fixed one cent tick, a commission per share
plus a regulatory fee on proceeds, microsecond competition, and Citadel on the other side -- and the eight windows
above are what that failure looks like as arithmetic: 941 USD of margin a window against a 10588 USD position draw.
The venues that pass the same four are where retail market making actually happens, and the repository already has
the plumbing for two of them: **options on a US venue, where the fee is per contract rather than per share and the
spread is a percentage of the premium rather than a tick**, and **crypto venues whose maker fee is zero or a
rebate**, where the readers, the free archives and the carry ledger already exist. Neither is a rewrite; both are
this engine pointed at a different contract, which is what it was built to be able to do.

**The hedge axis has now been measured too, and it says the hedge instrument matters more than the hedge.** On the
one day both instruments exist for, a MES hedge at a notional-matched beta removes **8 percent** of the inventory
term at the one second horizon a fill lives on -- MES explains 9.4 percent of MU's one second variance, against 50.4
percent at five minutes, where the same hedge removes 61.3 percent. So a crude cross product hedge is not a fix for
a quoting strategy, and the reason is worth keeping: **the common factor only appears at horizons longer than a
quote's life.** What that leaves is a screening question the existing tools can answer -- which instrument's returns
explain an inventory's at the horizon the inventory is actually held -- and a same industry candidate, SMH or SOXX
for a memory name and MU options for MU, is the first row of it.

**And the deliverable of this phase is the method rather than the strategy.** What the eight windows bought for
2.96 USD is not a profitable policy but a repeatable one: a pre-registered rule, an instrument priced a day at a
time before it is bought, an engine that separates a quoting margin from a position term and a fee schedule from a
commission, and four gates that re-derive every claim from the binary. Pointing that at a new venue class costs days
and a few dollars, and the record of SPY, of the micro E-mini, of AAPL and of MU is the evidence that it works: four
markets falsified with numbers rather than with opinions, which is the part of this project that is finished.

### The options venue: what has been verified, and the rule that will test it

**The fee census, with its provenance marked because two of its numbers are reported rather than read.** Cboe's own
schedule, read from the page: a **market maker adding liquidity against non-customer flow is credited 0.25 USD a
contract** (code GV), a **customer in XSP, MRUT or DJX trading under ten contracts is credited 0.30** (code XC), a
customer order under 100 contracts routed to certain venues is **free** (code TE), and the pass-through fees for
routed customer orders run **0.18 to 1.25** a contract with the penny classes the expensive ones (RF at 0.75 against
RD at 0.25). **So the sign of a per contract fee is a choice of product and order type rather than a market fact**,
which is the first time this project has had a negative fee inside an account it can actually reach. Reported by the
account holder rather than read from a page, and therefore marked as reported: the broker's commission is **0.65 USD
a contract** at the first tier, **the minimum commission is 1.00 USD an order** whatever the size, and the options
regulatory fee is about **0.02 USD a contract**. The minimum is the one that matters, because it has the same shape
as the per order floor this repository already models for equities, and it makes the clip size a decision rather than
a detail: at one contract the floor spends 1.00 to collect 0.65, and at two it spends nothing extra.

**The rule, pre-registered before any series has been measured.** A series passes when

    half_spread_usd * 100 >= 3 * (commission per order + regulatory + clearing - rebate) / contracts_per_order

evaluated at a stated clip size, because the commission has a per order floor and the rest is per contract. The
multiplier is 100, the tick is 0.05 for a non-penny class, and `tools/study/screen_spread.py` prices this with
`--fee-schedule=` plus its own multiplier, tick and clip size arguments; and the activity floor was restated rather than
repeated, because 2.8 prints a second is a stock's own tape and the screen below shows no option series reaching it.
Three consequences are written down before the answer is in: the **penny classes fail**
for the reason cheap shares fail, since a one cent spread is 0.50 USD against an 0.87 USD cost; a **five cent spread
is 2.50 USD against the same cost** and passes; and **the fraction of the premium the spread represents does not
matter to the fee**, only to the inventory, which is where the second number below comes from.

**Why this is the venue to test, in one number each.** Capture a unit divided by the notional a unit ties up is
**1.25 percent** here -- 2.50 USD on a 200 USD contract -- against **0.012 percent** for MU, and that quotient is what
decides whether an edge can be separated from a position draw at all; the option's own higher volatility takes some
of it back and the measurement is what would say how much. And the data is a rounding error, priced on 2026-10-02 for
the 2026-09-29 window: the whole SPY chain at one minute sampled top of book **0.2325 USD**, the same chain at one
second 9.32, six near the money series with each trade carrying the quote it printed against **0.00033 USD**, a single
series with the consolidated book **0.00006 USD**, and the chain's own definition free.

**Two risks are written down with the rule, because they are about the data's shape rather than its price.** OPRA
serves no `mbp-1`, so there is no venue specific ten level book for an option: the best available is a consolidated
top of book, which aggregates every venue's size at that price, and this repository has already measured what a
thinner book does to fills, because the top of book experiment halved them. And an option series is one instrument
among thousands on one underlying, so a chain is a many instrument problem for readers that take one file at a time;
a single series is inside the engine's shape, a chain is not, and a delta hedge is a second instrument again, which
the MES measurement says only works at horizons longer than a quote's life.

### Step 1 executed: the SPY chain screen, and the verdict it returns

The screen was run with one argument still missing from `tools/study/screen_spread.py`, because the ranking that decides the
verdict does not need it; what the clip size moves is the last factor of two and not the sign.

**What was bought, and for how much.** The whole SPY chain at one minute sampled top of book, 0.2325 USD: 203 MB,
1,560,172 rows, **12,626 series** over 119 minutes, with the chain's definition free. Then the trade records for the
40 series of one expiry (2026-10-01) and one strike slab (740 to 779), 0.0853 USD and priced before the purchase,
which are **6,817 prints in two hours**. Total data spend on this venue to date: **0.32 USD**.

**Two numbers came out of the chain before any series was opened.** 8,839 of the 12,626 series are quoted with a mean
spread of at least 0.05 USD in at least 100 of the 119 minutes, so **a wide spread is the rule in this venue** and not
the exception, and the ten widest are 2028 LEAPS with a 4.94 USD mean spread. That is the mirror image of equities,
where one cent of tick held every name to the fee, and it is a real structural difference between the two.

**And then the same measurement that killed JNUG killed the interesting half of it**, because prints and spread run in
opposite directions inside a single expiry.

| series | prints in two hours | prints a second | mean spread, USD |
|---|---:|---:|---:|
| C765, nearest the money | 1,147 | 0.159 | 0.0256 |
| C764 | 510 | 0.071 | 0.0344 |
| C763 | 237 | 0.033 | 0.0560 |
| C762 | 104 | 0.014 | 0.0556 |
| C750, forty strikes away | 28 | 0.004 | 0.1646 |
| C740, sixty strikes away | 3 | 0.0004 | 0.1839 |

**So the rule returns a fail, and on two axes that do not overlap.** The cost clause needs a spread of about 3.5 cents
to clear three times 0.585 USD a contract, and it is met only by series printing four to twenty times too little. The
series with the flow has a 2.56 cent spread, which misses the three times test by about 1.4. The busiest series in the
whole slab manages 1,147 prints, which at seventy-one prints a fill is sixteen fills where the target is thirty, and
**not one series in the slab reaches the restated floor of about 2,130 prints in the window.**

**What is not ruled out, stated before it is needed.** One expiry, one session, one product family. The series screened
are a penny class, so the one cent tick and the 0.75 USD pass-through are both near their worst, and the classes that
carry a rebate were deliberately left unscreened so that the rule cannot be read afterwards as an excuse built to fit
the result. The screen costs 0.32 USD and about a day; the same screen on XSP, which pays 0.30 a contract and quotes
in a different tick, is the one experiment the numbers leave standing.

### Step 2 executed: representative series, and the verdict that replaces the last one

**The verdict above was wrong, and this is the measurement that corrected it.** The screen had looked at one expiry and
one strike slab of SPY and concluded that prints and spread run in opposite directions. Picking the strikes properly
-- nearest the money, on a one day and a thirty day expiry, across four classes -- shows the opposite inside the
nearest expiry: **the series with the tightest spread also has the most flow**, 8,756 prints in two hours at a two cent
spread on an AAPL one day put, and 7,623 prints at four cents on the strike next door. What the first screen measured
was a property of a two day SPY weekly and of forty strikes chosen by hand, not a property of the venue.

**What was spent, and why the second pull cost more than the first.** The free definitions carry every strike and
expiry, so the picks cost nothing; a one day anchor close for the four underlyings cost 0.0000063 USD; the trades for
126 series across two expiries cost **0.899 USD**, three orders of magnitude more than the same call on deep out of the
money strikes, because near the money is where the volume is. Total on this venue: **1.22 USD**.

**The quotient that the screen could not see.** The trades carry the consolidated bid and ask at every print, so the
mid at print times is the contract's own path, and its standard deviation over the same window is the position term
the capture has to be separated from. Against MU's own measurement, 941 USD of capture a window over 10,588 USD of
position standard deviation, or **0.089**:

| series, one day expiry | prints in two hours | median spread, USD | premium, USD | position sigma, USD | capture in two hours, USD | capture / sigma |
|---|---:|---:|---:|---:|---:|---:|
| AAPL P327.5 | 5,276 | 0.02 | 0.57 | 0.091 | 74.3 | **8.20** |
| AAPL P330 | 8,756 | 0.02 | 1.11 | 0.164 | 123.3 | **7.54** |
| AAPL P332.5 | 7,623 | 0.04 | 1.98 | 0.294 | 214.7 | **7.30** |
| AAPL C335 | 7,791 | 0.02 | 1.19 | 0.318 | 109.7 | 3.45 |
| AAPL P335 | 1,668 | 0.15 | 3.22 | 0.533 | 176.2 | 3.31 |

**92 series were measurable, 68 of them at or above MU's ratio, the median 0.206 and the best 8.20**, which is about
ninety times MU before fees and between thirty-five and fifty-seven times after the account's 0.585 USD a contract.
The strike in the third row also clears the other two clauses written down before the answer: four cents is 2.00 USD
against three times the per contract cost, and 7,623 prints is a hundred and seven fills at the ratio the equity screen
used. **So a candidate passes, and the venue is not closed.**

**What now binds, since it is not the spread, the flow or the fee.** The fill ratio of seventy-one prints a fill is an
**equity** number, and at a two or four cent spread both sides sit at the touch: whether a retail quote gets one fill in
seventy-one prints at the touch is a queue question, and the queue is what the engine's own model measures. That is the
next measurement and it is the only one left that can move the answer, because it scales capture directly: a fill in
two hundred prints instead of seventy-one divides the quotient by three, and the margin over the fee goes with it.

**Two risks that this table does not show and the next reader should not forget.** Every winner is a **one day**
expiry, so the premium is small enough that the 1.00 USD order minimum is up to 1.75 times the entire premium, and the
sigma of the position is a lottery's sigma; the edge may be the price of standing in front of expiry flow rather than
market making, which is a different business with a different tail. And the sigma here is measured only at print
times, which understates the risk of holding continuously. Both are recorded before the engine runs rather than after.

| Spend | What it buys | Cost |
|---|---|---|
| the screen (gate 1) | top of book only, over a candidate list and a few days — **spent: 0.52 USD on thirty symbols, AAPL and TSLA pass** | 5 to 20 USD |
| two to three months of daily sessions for the surviving symbol | the out-of-sample campaign (gate 2) at a strength the roadmap's eight to ten sessions cannot reach, sixty-odd sessions instead — **now the second priority rather than the next purchase, because the probe above shows what the campaign would measure is below the noise it would measure it in** | **2.30 USD for ten two-hour windows, 14 for sixty**, and the screen's own row counts narrow it before anything is bought |
| the fee tiers (gate 7) | the same window under a zero commission tier, a rebate and the worst realistic tier | 5 to 10 USD |

**The recommendation is to front-load the evidence, and it has now been followed once.** The other reading of a
credit with a date on it is to hoard it, and that buys nothing: it is 124 USD of historical data that becomes
zero on 2027-03-30, while everything above together is less than the balance. So the screen ran first, for 0.52
USD, because it is what decides whether any of the rest is worth buying, and it produced two symbols. The
out-of-sample campaign's own question has now been answered by a free probe rather than by the campaign: the
capture does stay positive, and it stays positive below the position noise at any size this account can run. **So
the next purchase is one more screen rather than a campaign**, because the pre-registered rule has a price column
it has never been run on -- the SEC fee is charged on the proceeds of a sale, so a cheaper share with the same
spread in ticks pays a smaller regulatory fee for the same capture -- and a candidate list of cheaper, wider names
is a few dollars of top of book, judged against a rule that was written down before any of them was screened. The
campaign follows only if that screen produces a symbol whose capture clears the account's **whole** schedule rather
than its commission; and whatever is left is either spent on more days before the date or written down here as
forfeited, **with the amount**, so that the loss is a recorded decision rather than a quiet one. Two things do not
consume it at all: every replay, sweep and gate run after a download, which is free forever, and the live and paper
phases, whose data comes from our own recorded feed rather than from a vendor.

That also settles the budget rule worth pre-registering: **paid data is only ever bought for the symbol being
validated, one window before a session and one session before a week**. The recurring review that Stage D asks
for is a deliberate purchase each time rather than a standing subscription, and the zero-cost alternative —
recording our own broker's feed — is the default once a live line exists.

### The owner's three decisions, and what each one changes

Three questions were left to the owner, and the answers move work rather than opinions. They are recorded with what
each one changes in a measurement, because a decision that changes no number is a preference.

**The fee floor is per order, and a requote is a new order.** The account holder confirms it directly: partial fills
of the same order on the same trading day are charged once, while a cross-day execution and a cancel and replace each
reset the order instance and are charged their own floor. The repository had already read the broker's own note saying
exactly that, in the Chinese rendering of the page, and had already built the counter that measures how often it binds;
what is new is that the figure is confirmed rather than reported, and that the trap is a stated risk rather than a
footnote. **What it changes is the cost model rather than the market.** The options screen amortised the 1.00 USD floor
over two contracts of one order, which assumes a fill sequence belongs to the order that produced it. A market maker
requotes on every fair price move, so the floor is charged per requote cycle rather than per fill, and the term that
decides a thin margin is the floor divided by the contracts that fill before the quote is replaced. The engine already
reports that count, as filled order **instances** with a size histogram, and the AAPL window already shows the shape:
**1,222 order instances against 2,324 fills, 79 percent of the instances filling fewer than 100 shares.** The queue
measurement is therefore the measurement of two things at once, capture and the number of instances the floor is
charged on, and it remains the only step left that can move the answer.

**The options business is long dated or it is not a business.** The one day expiry series produced the best ratios
measured anywhere in this project and they are set aside deliberately: a one day option's spread is the price of
standing in front of expiry flow, so the capture is compensation for tail risk rather than a quoting edge, the premium
can be smaller than the 1.00 USD floor itself, and the position's sigma is a lottery's. **The pre-registered
restriction is a DTE above fifteen days with moderate liquidity**, which is the part of the options universe that can
be run as a market rather than as an insurance book. Paper trading may still be opened, and its purpose is re-scoped
with the restriction: it is the bench for the order API, the latency and the hedge logic, **not** a test of whether
high frequency quoting pays.

**The equity line is in hibernation, with its reasons measured.** The quoting margin was positive in sixteen of
sixteen windows and the position term drowned it, **10,588 USD of sigma against 941 of capture**, which is five
hundred and six windows to separate them at any size this account can run; the fee floor then binds on seventy-nine
percent of order instances; and no swept parameter moves the ratio by more than twenty percent. Nothing further will
be bought to widen that sample, and the line is closed as a decision rather than left open as a hope.

### Q1 and Q2 executed: the long dated options, and the verdict they return

Two questions were run for nothing, on the trades already on disk, and they close the options line rather than extend
it. The first is the critical fill ratio: with F contracts filled before the quote is replaced, the instance costs
`max(1.00, 0.65 F) + 0.02 F`, so the sign of a fill depends on the spread alone and the size of the window against the
position's own sigma depends on how many prints it takes to get one. That gives one number per series,

    R_max = prints x (half spread - cost a contract) / sigma

the largest number of prints per fill that still covers one window's own standard deviation, and it is comparable to
the 71 prints a fill the equity screen's fill model used.

**The long dated half, which is the only half the restriction allows.** 27 series at or above fifteen days have a
measurable spread and sigma, and **all 27 clear the per contract cost on their spread alone** -- margins from 0.48 to
121.48 USD a contract -- which is the first time in this project that a venue has passed the fee axis. **They then all
fail the other axis**: the largest R_max is **43**, on an AAPL 315 put with 70 prints, and the median is in the single
digits, against a reference of 71. The reason is visible in the print count: these series trade 5 to 264 times in two
hours.

**The line that decides it falls exactly on the expiry the owner excluded.** Across every expiry measured, the series
whose R_max clears 71 are the one and three day ones -- 254, 203, 113, 111, 104 prints per fill allowed -- and every
series at fifteen days or more sits below 43. So the options business that survives the tail risk restriction is
precisely the business that fails the fill ratio, and the business that passes the fill ratio is the one ruled out as
payment for insurance. The equity line's disease is unchanged and it has simply moved: **the fee is no longer what
binds, the number of prints a window contains is.**

**Two caveats, both recorded before the engine could be pointed at a rescue.** The sigma here is sampled at print
times, which understates the risk of holding a position continuously, so the failure is conservative in one direction;
and the bar assumes the position is held across the window, so an inventory path that mean reverts would raise R_max
in proportion. One measurement could still change it and it is the only one left: the engine's fill ratio on a single
long dated series, which would have to come in better than 43 prints a fill where the equity reference is 71. Until
that is run, the line is closed as a recorded decision with its numbers, which is what the owner asked for rather than
another round of tuning.

### The venue picker: what a top of book ratio can and cannot decide

`tools/study/venue_ratio.py` computes the same quotient on any top of book file the repository already owns, and reports it
as the largest number of prints per fill that still covers one window's own standard deviation -- a fill ratio, and so
comparable to the 71 prints a fill the equity screen used. It was run for nothing over the two things already on disk:
the sixty equity files in the screen cache, and the option series bought for the screens above.

| group | clip | best R_max | reading |
|---|---:|---:|---|
| 60 US equities, whole schedule plus the SEC fee and the maker rebate | 1 share | none, every one negative | 0.35 USD on one share is seventy times the half spread of a one cent market |
| the same | 100 shares | MU 1298, AMD 985, TSLA 872, AAPL 784, CCL 295 | amortising the floor makes every liquid name clear the reference |
| 128 option series, 0.65 USD a contract and a 1.00 USD floor | 1 contract | AAPL P332.5 at 254, then 203, 192, 177, 113 | a contract is large enough a unit that one instance pays its own floor |
| the same | 2 contracts | 344, then 214, 177 | the floor stops binding at two contracts |

**And the table does not settle the venue, which is the finding rather than a disappointment.** The ratio flatters the
wide ticked and heavily traded names -- MU's 1298 prints of room per fill against the options' 254 -- while the engine
measured MU as a loss over eight windows. The two sigmas are different quantities: this one is the standard deviation
of one unit's price, and the engine's 10,588 is the standard deviation of the position the policy actually builds,
which is the price move multiplied by the inventory it carried. **A one lot ratio is a necessary condition and not a
sufficient one, so it cannot rank venues against each other**, and reading it as a ranking would have repeated the
mistake the last three sections each corrected.

**What it does settle is the size of a unit.** A single contract can pay its own floor and a single share cannot, which
is the whole of the options venue's apparent advantage and the reason the one day series stood out. Once the expiry
restriction removes those, the number that decides is the print count a window contains -- 5 to 264 against 7,623 --
which is what Q1 and Q2 already measured.

### Test A executed: the high flow names at 20 to 45 days, which closes the options line

The one corner the expiry restriction allows that had never been measured was the monthly of the names where option
volume lives. It cost 0.0426 USD and it fails: **0 of 41 series clear the reference.**

| series, 2026-10-30 expiry | prints in two hours | median spread | sigma a contract | R_max |
|---|---:|---:|---:|---:|
| TSLA C355, the best of them | 184 | 0.35 | 53.88 | 56 |
| NVDA C230 | 246 | 0.20 | 48.09 | 46 |
| NVDA C235 | 237 | 0.15 | 37.72 | 41 |
| TSLA C350 | 90 | 0.35 | 61.84 | 24 |
| COIN C195 | 6 | 1.63 | 55.48 | 9 |

**The hypothesis was that a high flow name's monthly would print ten to fifty times what a quiet name's monthly does,
and the measurement says it prints the same**: 5 to 246 prints in two hours against 5 to 264 for AAPL, IWM, KO and
XSP, so the flow in these names lives in the one day expiry and nowhere else. Eight classes have now been measured at
twenty to forty five days and the best R_max in any of them is **56** against the reference of 71.

**The five series above were bought again on 2026-10-03 for 0.011938 USD and now sit in `data/v2/options/`**, because
the first copy lived in `/tmp` and nothing behind it could be rebuilt once it was gone. `tools/study/venue_ratio.py
--input 'data/v2/options/*.csv' --multiplier 100 --fee-per-unit 0.65 --min-commission 1.00 --clip 1,2` reproduces the
prints, the median spreads, the sigmas and the R_max of every row, which is what `python3 tools/study/viability.py
--derive` compares the recorded 56 against.


**So the options line is closed, and the reason is no longer the fee** -- that axis passed, first time in this project
-- **but the number of times a monthly series trades**, which is a few times a minute at best, against its own price
moving tens of dollars a contract over the same two hours. Spend on this venue to date: **1.27 USD**.

### The forced-flow family: the last unmeasured one, probed on both sides of the fee

Eight venues and families are closed above, and the one never tried was the trade everyone describes: after a forced
sell, buy the vacuum and wait for the print to come back. It costs nothing to ask here, because the trades are already on
disk, so the rule went down before the run and it is in `tools/study/cascade_probe.py` rather than in this paragraph: a
one-second bucket whose net aggressive volume is at or above the day's own 99th percentile is a burst, the entry is the
first trade at or after that bucket, the exits are 1, 5, 30 and 60 seconds later, the fee is the venue's 5 bps a side, and
the cooldown is 30 s so two entries cannot share one reversion. The tool runs the same events in both directions, because
a negative gross for the fade is a positive gross for the continuation, and only the fee chooses between them.

| one day, 2020-04-01 | fills | best net bps | at | the same trade gross | reading |
|---|---:|---:|---:|---:|---|
| BTC-PERPETUAL, fade | 80 | -12.56 | 1 s | -2.56 | the burst does not come back |
| BTC-PERPETUAL, follow | 80 | -3.91 | 30 s | +6.09 | it continues, by less than the toll |
| ETH-PERPETUAL, fade | 28 | -11.46 | 60 s | -1.46 | the same |
| ETH-PERPETUAL, follow | 26 | -3.88 | 5 s | +6.12 | the same |

<!-- toyquant:check
tool: tools/study/cascade_probe.py --instrument BTC-PERPETUAL --momentum
then: bursts=80 best_net_bps=-3.9088 gross_at_best_bps=6.0912
tool: tools/study/cascade_probe.py --instrument ETH-PERPETUAL --momentum
then: bursts=29 best_net_bps=-3.8848
-->

**Both sides are inside the fee, and that is what closes the family.** The continuation is real -- +6.1 bps gross on
both instruments, at 30 seconds on BTC and 5 on ETH -- and a taker round trip is 10 bps, so a follower is about four
basis points short before anything else is paid; the fade is worse than nothing: -1.5 to -2.6 bps gross, on a price that
never came back. No horizon on either instrument produces a positive net for either direction.

**What this does not say, and why it is a probe rather than a line.** It is one day and two instruments; ETH contributes
26 to 29 fills, below the thirty this repository treats as a conclusion. It conditions on the trade tape alone, so
"liquidity vacuum" is a burst of aggressive volume and not a measured withdrawal of depth -- the 1.5 GB book file is
there for the version of this question that conditions on depth, and that is a separate run rather than a better reading
of this one. And the entry is a taker at the next print by construction, which looks like pricing the family at the worse
of the two ways to join it -- and is not, as the next two paragraphs measure. **The one-day half of that caveat is
answered further down**, where the burst rows are re-run on the five BTC days on disk; ETH is still one day, and the
depth-conditioned version is its own section.

**Both legs passive makes it worse, and that is the mechanism rather than a detail (2026-11-25).** The tool now carries
the rule the taker-only version never needed: a passive leg is not filled because the price touched it. It fills only when
the tape prints at least one tick *through* the limit, and the fill is at the limit. With that rule, on the same day and
the same events:

| BTC 2020-04-01, momentum | fills | gross at 5 s | gross at 30 s | net at 30 s | cost a round trip |
|---|---:|---:|---:|---:|---:|
| taker entry, taker exit | 80 | +4.70 | **+6.09** | -3.91 | 10 bps |
| taker entry, passive exit | 80 | -1.11 | +2.50 | -4.73 | 7 bps |
| passive entry, passive exit | 68 | **-5.87** | **-5.23** | -9.45 | 4 bps |

The fee falls by six basis points and the gross falls by eleven, because a passive entry is filled *only when the market
comes back through the price the burst printed at* -- which is the moment the continuation has failed. P_fill is therefore
not an independent probability to be traded against a fee; it is negatively correlated with the outcome, and a reading
that prices the two passive legs as a fee saving alone overstates the result in exactly the direction a researcher wants
it to. Sixteen of the eighty-four events never fill at all, and the sixty-eight that do are the adverse selection.

**The 30 s row replicates and the 900 s row does not (2026-11-25).** Five BTC days are on disk, so both questions were put
to all of them with the same percentile, cooldown and fill rules, one command a day. The continuation is real and it is
small -- each day's own best horizon, taker both legs, against a 10 bps round trip:

| BTC-PERPETUAL, momentum, taker/taker | 04-01 | 05-01 | 06-01 | 07-01 | 08-01 |
|---|---:|---:|---:|---:|---:|
| best horizon, s | 30 | 5 | 5 | 30 | 5 |
| gross bps at it | **+6.09** | +1.33 | +3.79 | +2.92 | +1.98 |
| net bps at it | -3.91 | -8.67 | -6.21 | -7.08 | -8.02 |

So the direction holds on every day and the size does not run it: the best day is 3.91 bps short of the toll and the other
four are 6.21 to 8.67 short. Those five rows are the tool's `[CASCADE]` best-horizon line, one run a day and five runs in
all, which is why they are printed here and not carried in the gate; the command is the tool's own header, `--days` over
the five days with `--momentum --horizons 5,30`. The 900 s taker-entry/passive-exit row is the opposite case, and it is
one day:

| BTC-PERPETUAL, momentum, 900 s | 04-01 | 05-01 | 06-01 | 07-01 | 08-01 |
|---|---:|---:|---:|---:|---:|
| fills | 76 | 101 | 50 | 51 | 117 |
| gross bps | **+14.44** | -9.77 | -19.96 | -0.24 | -8.76 |
| net bps | **+7.40** | -17.16 | -27.14 | -7.83 | -16.01 |
| median net bps | -9.34 | -17.88 | -18.24 | -10.00 | -14.39 |

One cell of the fifteen the three-horizon run prints (five days at 5, 30 and 900 s) has a positive net, and it is the cell
the single-day probe landed on; the median is negative on all five, including that one. So the family is closed on its
**gross at the horizons that pay** and on its **mean at the horizon that does not**: letting the winners run produces a
number on 2020-04-01 and loses money on the four days around it, which is the shape of a day rather than of a rule.

<!-- toyquant:check
tool: tools/study/cascade_probe.py --instrument BTC-PERPETUAL --momentum --exit-mode passive --horizons 900 --patience-s 900 --tick 0.5
then: best_net_bps=7.3956 gross_at_best_bps=14.4351 fills=76
tool: tools/study/cascade_probe.py --instrument BTC-PERPETUAL --day 2020-06-01 --momentum --exit-mode passive --horizons 900 --patience-s 900 --tick 0.5
then: best_net_bps=-27.1385 gross_at_best_bps=-19.9585 fills=50
-->

<!-- toyquant:check
tool: tools/study/cascade_probe.py --instrument BTC-PERPETUAL --momentum --entry-mode passive --exit-mode passive --horizons 5,30 --tick 0.5
then: best_net_bps=-9.4493 gross_at_best_bps=-5.2287
-->

The tables above are the tool's own output, and the three blocks pin what would drift silently: the passive pair, the
900 s day the table's one positive cell belongs to, and the worst of the four days that disagree with it. The taker-only
numbers stay pinned by the block at the top of this section, which `--days` reproduces to the digit on its first day.
The other three 900 s days and the five 30 s days are one run each, which is about two minutes, so they are printed in
the tables above and not carried in the gate.

### The depth-conditioned version, which weakens the family rather than rescuing it

That separate run is done: `--condition depth` rebuilds the top of book from the incremental file (one level change a row,
a dict a side and a heap with lazy deletion, the last state of each second kept) and calls a second a vacuum when its two
top sizes together are at or below **0.2 of their trailing 300 s median**; an event is a vacuum second that also carries a
trade, entered at the next print like the rest of this section.

| 2020-04-01, vacuum | vacuum seconds | of them also a P99 burst | events | best net bps | the same trade gross |
|---|---:|---:|---:|---:|---:|
| BTC, fade | 723 | 26 | 232 | -11.20 | -1.20 |
| BTC, follow | 723 | 26 | 232 | -6.60 | +3.40 |
| ETH, fade | 788 | 2 | 52 | -8.94 | +1.06 |
| ETH, follow | 788 | 2 | 52 | -7.46 | +2.54 |

**The vacuum is not where the move lives.** The two conditions barely overlap -- 26 of 723 seconds on BTC, 2 of 788 on ETH
-- so this is a different population, and on it the continuation is **weaker** than on the trade burst: +3.40 against
+6.09 bps on BTC, +2.54 against +6.12 on ETH. Both sides of both instruments stay inside the fee, with the best net of any
horizon at -6.60. A quote resting into a thin book is not an opportunity here; it is the same six-basis-point move with
less of it.

The command is `python3 tools/study/cascade_probe.py --instrument BTC-PERPETUAL --condition depth` (and `--momentum` for
the other side), with `data/v2/crypto/deribit_incremental_book_L2_2020-04-01_{BTC,ETH}-PERPETUAL.csv[.gz]` already on disk
and **no spend**. It is a command rather than a marked block for the reason the walk-forward is: the pass reads 1.5 GB and
takes about a minute, and `fast` is the check every edit pays for -- a measurement that answers a question when the answer
is being written does not belong in it.

### The two cells the screen had never been run on (2026-11-23)

The screen above was run on equities and on options, and the two cells it had not touched were the names the equity
limit hides -- shares under twenty dollars, where one cent is two to fifty basis points -- and the perpetuals the demo
line does not trade. Both were screened against the same two pre-registered numbers: a half spread of at least three
times the fee per side where the trades print, and at least 2.8 trade prints a second.

**Small caps: the spread half passes and the floor does not, again.** Seven names, XNAS top of book, the same two hours,
**0.053662 USD** of credit: IONQ's spread at the trades is **3.494** times the round turn of the account's own schedule
and FCEL's **3.874**, so the fee half is met by two of the seven -- and they trade **1.473** and **0.594** prints a
second against the floor's 2.8. The other five (SOUN, RGTI, QBTS, PLUG, LCID) are one tick wide at 2.03 to 2.06 and fail
both halves. That is JNUG's shape exactly: the wide spread and the flow are in different names.

**Perpetuals: one instrument passes both halves, and it is the first one since AAPL and TSLA.** The venue's public
tickers route answers without a key and returns 548 two-sided swaps, of which **107 quote a half spread at or above the
2 bps it charges to rest while trading at least a million dollars a day** -- so the "19.8 half-spreads" bound is a
property of `BTC-USDT-SWAP`, not of the venue. Twenty book samples spread over ninety seconds, the real venue's own book:

| instrument | median half spread | ratio to 2 bps | prints in 13:30-15:30 UTC, per second | the floor, 2.8 |
|---|---:|---:|---:|---|
| `UP-USDT-SWAP` | 8.51 bps | 4.25 | 3.21 | **pass** |
| `DOS-USDT-SWAP` | 7.40 bps | 3.70 | 0.38 | fail |
| `SKY-USDT-SWAP` | 5.84 bps | 2.92 | 0.52 | fail |
| `BTC-USDT-SWAP` | 0.01 bps | 0.00 | 37.10 | pass |

The prints come from the free daily trade archive (`tools/fetch/fetch_okx_trades.py`, one day, one symbol per file, the
day cut on UTC+8), counted inside the window the equity screen uses so the two tables are judged by one number.

**What it says, and what it does not.** A passer is a candidate and not a result: the last two symbols to pass this
screen were AAPL and TSLA, and the measurement that followed them -- five own-side runs -- failed on the markout on four
and on the gross on all five. `UP-USDT-SWAP` is the same kind of candidate one asset class away, and the question that
decides it is unchanged: does the fill get picked off at 8.51 bps of half spread, which is a queue-model measurement and
not a screen. What this does settle is the denominator of the demo line's bound: the demo venue's own `spread_bps` is
0.101 while the real `BTC-USDT-SWAP` book quotes **0.01**, so the 19.8 half-spreads measured on the demo understates
what a quote on the real venue faces by about ten times. A demo number is about the demo.

A live snapshot is not a series, and none of the four rows above is a marked block: the book sample is taken by hand and
the trade archive is a download, so both are reproducible by a reader and neither is a number the gate can re-derive.

**The passer, taken to a session (2026-11-23).** `UP-USDT-SWAP` is registered in
`src/common/instrument_spec.h` from the venue's own metadata -- a 0.0001 tick, one contract carrying ten UP, a minimum
of one contract and the same 2/5 bps schedule -- and `l2_replay_test` pins those five numbers. Two sixty-second sessions
followed: the demo's public feed delivered **no market events at all** for it (`Error: L2 replay input contains no market
events`), and with the market leg moved to the real public stream the engine quoted 62 times and got 62 reports back
from the demo account with **no refusals** -- so the venue knows the instrument and accepts orders on it -- while
`bbo_quote_observations=0` says the book never arrived and nothing filled. That is a relay coverage gap for a symbol the
demo does not quote, not a strategy result: the queue-model question this instrument was registered for still needs a
book, either from the venue's L2 archive (portal-only) or from the relay's market leg for this symbol. `soak.sh` grew
`INST`, `INST_CODE` and `MARKET_DEMO` so that the two legs can point at different venues while the order leg stays on the
demo account.

**The book arrived, and reading it took a defect (2026-11-25).** Three sixty-second sessions on the same instrument, each
answering the previous one's question.

- **The channel was the first gap.** `books` delivered nothing for this symbol and `bbo-tbt` delivers its top of book: the
  capture's raw frames read bid 0.1893 against ask 0.1896, with 103 trades in the minute. `BOOKS_CHANNEL` is the flag that
  says which channel a session reads.
- **The book then arrived and still could not be quoted, because of a defect in the book.** The engine counted
  `crossed_book_batches=84` of 94 batches while every row of its own recording was uncrossed, which points at what the
  book holds rather than what the venue sent: `apply_incremental_batch` cleared on the *first* snapshot batch and treated
  the rest of the run as increments. That is right for a cumulative snapshot delivered in pieces and wrong for a stream
  that sends one top-of-book snapshot per change, where the superseded level must go. A snapshot that arrives at a new
  exchange timestamp now clears the book. `code` re-derives every recorded number unchanged -- the archived windows open
  with one snapshot per connect -- and the same session then reads `crossed_book_batches` absent, `bbo_quote_observations`
  2 of 14 quotes, `working_orders=2` and `total_order_lifetime_cycles=84` where it read 0, 0, 0 and 14.
- **The order leg cannot trade this symbol on the demo account, and the reason is now named rather than guessed.** Every
  order the venue answered came back `sCode=51001 Instrument ID, Instrument ID code, or Spread ID doesn't exist`: six of
  six in one session's order log and ten refusal lines in the next, and the earlier 62-order session recorded no adapter
  refusal without its answers being re-read here. The demo account does not carry the instrument, so a measurable market
  plus a demo order leg is still not a measurable instrument: what this queue-model question needs is the venue's own
  fills, which means the portal's L2 archive or an account that carries the symbol.
- **The harness had a thousand-fold sizing defect, which the same sessions exposed.** The relay's contract mapping
  defaults to BTC's and `soak.sh` passed none of its four flags, so one UP contract became a thousand: each order asked for
  1000 contracts, about 1,895 USD at 0.1895, where the strategy believed it had sent 1.90. `soak.sh` now holds a
  per-instrument mapping table (`BTC`, `ETH`, `UP`) and **refuses** an instrument it does not know, naming
  `src/common/instrument_spec.h` as the authority. That the 51001 answer blocked the order is luck about this symbol and
  not a bound: `--max-exposure-usd` checks the engine's units, not the venue's. The guard is the arithmetic itself -- the
  table is shell and the relay's `--self-test` has no case for a non-BTC mapping, which is the open half of this fix.
- **One earlier reading here was wrong and is corrected.** This page said the venue's margin check was what stood between
  the harness and a thousand-fold position; the order log shows the six orders were answered `51001`, so they never
  reached a sizing check.


**The archive answers, and the answer is no (2026-11-25).** Seven days of the venue's own L2 archive (`UP-USDT-SWAP`,
2026-11-16 to 11-22, downloaded by hand from the portal) and the same days of trades from the public endpoint are now on
disk, and two tracked tools make the measurement repeatable: `tools/study/okx_book_window.py` cuts a UTC window so a
replay opens on a snapshot and reports the *time-weighted* half-spread, and `tools/sweep/okx_maker_baseline.py` runs the
maker over one window per day per queue model. Neither number below is inside a marked block, because the archives are
140 MB of input that git does not carry; the commands that produce them are in the two tools' headers.

| day | median half spread bps | in ticks | fee / half spread |
|---|---:|---:|---:|
| 11-16 | 5.20 | 2 | 0.38 |
| 11-17 | 6.05 | 2 | 0.33 |
| 11-18 | 7.69 | 3 | 0.26 |
| 11-19 | 4.43 | 2 | 0.45 |
| 11-20 | 7.57 | 3 | 0.26 |
| 11-21 | 4.87 | 2 | 0.41 |
| 11-22 | 4.65 | 2 | 0.43 |

So the screen's 8.51 bps was optimistic by 10 to 90 percent, and the premise it stood on survives in weakened form: the
half-spread is 2.2 to 3.8 times the maker fee, not 4.25. No day carried a crossed batch.

The maker, on the same windows, `active_l2`, one contract a quote, the fee it registered with:

| day | conservative fills / net bps | prorata fills / net bps |
|---|---|---|
| 11-16 | 535 / -1.13 | 627 / -3.10 |
| 11-17 | 235 / -8.44 | 268 / -9.05 |
| 11-18 | 331 / -9.94 | 393 / -6.16 |
| 11-19 | 764 / -2.51 | 844 / -2.49 |
| 11-20 | 232 / -2.28 | 282 / +0.53 |
| 11-21 | 360 / **+0.72** | 450 / -2.46 |
| 11-22 | 744 / -0.46 | 869 / +2.35 |

Median net **-2.28 bps** under the conservative fill and **-2.49** under the optimistic one; one day of seven positive
either way; the fee is 2.00 bps on every run with no taker fill, so what the window loses is gross. The fill model moves
the answer by about 0.4 bps on the mean and by up to 3.8 bps on a single day -- it is not the variable that decides,
because 43 percent of the quotes join a level with nothing in front of them.

What the gross is made of comes from the 11-16 decomposition: `gross_closed_per_fill_usd` is +1.23 bps of notional
against a half-spread of 5.20, so the maker captures **24 percent** of what the book offers, and `breakeven_maker_rate`
is +0.87 bps -- the fee would have to be under 0.87 for this window to pay. The capture quoted at the fill
(`captured_edge_per_unit_ticks` 1.51, about 7.9 bps) is not the round trip: the exit and the inventory's path take the
rest, which is the same decomposition lesson the crypto line already carries.

Two inventory experiments followed, and both argue against the obvious fix:

| variant | median net bps | mean net bps | ending position |
|---|---:|---:|---|
| cap ten contracts (baseline) | -2.28 | -3.43 | five to ten contracts |
| cap **three** contracts | -3.22 | -4.42 | one to three |
| cap three plus a taker close-out | -3.27 | -4.42 | zero |

Tightening the cap makes it worse -- the strategy already stops quoting the adding side at its limit, so a tighter cap
buys less turnover against the same adverse drift -- and closing out at the end changes nothing, because the loss was
realised inside the window rather than carried to the mark. What is left unmeasured is the part that would raise the
capture rather than shrink the position: quote depth and refresh cadence, where 24 percent would have to become more
than 38 to cover two basis points.


**One configuration reaches the fee, and only just (2026-11-25).** A grid over the two quote-geometry flags
(`--base-spread-ticks` 1/2/3 by `--refresh-price-ticks` 1/2/4, seven days, conservative fill) says the geometry alone
cannot turn the sign: the best cell is a refresh on every tick at -0.46 bps mean against the baseline's -3.43, and no cell
has a positive median. Two gates then get close, and they are not the same rule:

| configuration | 20:00-22:00 | 02:00-04:00 | 09:30-11:30 | US 13:30-15:30 | fee |
|---|---:|---:|---:|---:|---:|
| baseline: touch quote, refresh 4 ticks | -- | -- | -- | +1.23 | 2.00 |
| `--edge-cover-ticks=1 --base-spread-ticks=3` | **+2.01** (net -0.34, 5/6) | **+2.59** (net +0.58, 3/7) | **+1.52** (net -0.48, 3/7) | **+3.12** (net +1.16, 5/7) | 2.00 |
| `--min-spread-ticks=2`, touch quote | -- | -- | -1.69 (net -4.69, 3/7) | +3.01 (net +1.35, 4/7) | 2.00 |

The cells are `closed bps` -- what the round trip earned before the fee -- and the two gate rows are the reason that
column exists: the first gate is about how far the quote sits from the fair price, the second about how wide the book is,
and only the first holds its sign in every window. Quoting three ticks deep under an edge-cover gate earns **1.5 to
3.1 bps of a 5 bps half-spread in all four windows measured**, mean **+2.3** against a 2.00 bps fee. That is break-even
with a third of a basis point to spare, and the window's net -- which carries the inventory's mark -- lands on either side
of zero by chance rather than by session: +1.16, -0.48, -0.34 and +0.58.

So the instrument is a break-even maker at the retail schedule, not a losing one, and the break-even fee this implies is
about 1.5 bps: at 1.5 the four windows read +0.51, +1.09, +0.02 and +1.62 in the table's order, and at 1.0 they read
+1.01, +1.59, +0.52 and +2.12. That is an account-tier question rather than a code one, and it is the same shape of
answer the crypto line reached from the other direction.

**The link does not decide, and the 30 ms collapse was the replay (2026-11-25).** The measurement above is a zero-latency
replay, and the demo venue's own round trip is 30-80 ms, so the replayer was asked to hold every order and cancel for that
long (`--order-latency-us` / `--cancel-latency-us`, reachable from the sweep as `--latency-us`). The first run said the
configuration did not survive: on 2026-11-16 the baseline submitted 1,275 quotes at zero latency and 31 at 30 ms, 29 of the
31 filled, the fee rose to 4.25 bps because a stale quote is marketable on arrival, and `max_quote_lifetime` went from 684
to 26,570 cycles. The deep-quote configuration behaved the same way, one to forty-four fills across the seven days against
183, and `quote_observations` equalled `submitted_orders`, so it read as the strategy's own decision.

It was not. `MatchingEngine::cancel_order` parked a cancel only when the order was already in `order_index_` and returned
silently otherwise, so a cancel issued while its order was still waiting out the *placement* latency was discarded -- and
with both legs on the same link that is the common case, not an edge. The strategy then waited for quotes it had already
tried to pull (`longest_wait_filled_cycles=26570` is a fill wait, not an age refresh). The engine now holds such a cancel
and applies it when the order lands, and the same seven days, conservative fill, one command per latency, read:

| reading | 0 ms | 10 ms | 30 ms | 50 ms | 80 ms |
|---|---:|---:|---:|---:|---:|
| rescue, fills a window | 76-358 | 64-164 | 50-91 | 37-59 | 22-46 |
| rescue, fee bps (2.00 is all maker) | 2.00 | 4.46-10.28 | 7.22-16.12 | 15.89-29.91 | 12.74-129.47 |
| rescue, capture ticks | +1.4 to +2.7 | +0.05 to +0.99 | -0.56 to +0.16 | -0.69 to -0.35 | -1.33 to -0.21 |
| baseline, fee bps | 2.02 | 3.05-7.17 | 5.10-13.28 | 8.97-16.54 | 9.68-25.17 |

**The first version of this table was five runs at 0 to 80 microseconds, and every conclusion drawn from it was about zero
latency.** The flag is `--latency-us`, a grid written in milliseconds passed 80 where it meant 80000, and the rows moved by
tenths of a basis point because there was no delay in them. What caught it was the repository's own rule that a result too
clean has to reconcile: a single day at a true 80 ms gave 41 fills where the "80 ms" row claimed 100 to 388, and the
decomposition -- capture negative, fee 31 bps, 35 of 41 fills taker -- put the flag back where it belonged. The tool now
prints a warning when the value is under one millisecond, so the same 1000x shows up in the log.

**At the true latencies the configuration is a maker only near zero.** At 10 ms the fee is already two to five times the
maker's 2.00 bps and the capture has fallen from about two ticks to +0.05 to +0.99; at 30 ms the capture is negative on
the median, and from 50 ms the fee is eight to fifteen times the maker rate. What happens is not that the flow disappears:
the quote lands after the book has moved, so it is marketable on arrival and fills as a **taker** -- the fee rate is the
taker share in the venue's own currency, and it is the column to read. The baseline shows the same ladder from its own
2.02 bps to 3 to 7 at 10 ms and 10 to 25 at 80; its net is a mark on 150 to 280 fills, so its *sign* is not a measurement
at this count, and the fee rate is the comparable number.

**With `--post-only` the same latency turns the fills off instead of making them take.** At 80 ms and one contract the flag
leaves **1 to 10 fills a window** against 22 to 46 unflagged, because the quotes that would have crossed are declined: one
day's decomposition shows `post_only_cancels=41`, 8 fills and **zero taker fills**, against 41 fills of which 35 were taker
without the flag. Both branches end the line at this latency; which one a session gets is the flag.

**The link, measured -- and it is the wall above the fee (2026-11-25).** Every latency row above is a replay setting;
the venue's own round trip from this host had never been measured, and "30-80 ms" was an assumption. It is now two paths
and a rate. `tools/live/okx_probe.py rtt` places a `post_only` buy two percent under the best bid -- so the order has to
rest and the venue's reply is the placement's -- times that reply and the cancel that follows it, and `touch` prices the
same order at the best bid, where only a book that moved inside the round trip can make it marketable. The measurement is
on `BTC-USDT-SWAP`, because `UP-USDT-SWAP` **is not tradable in the demo environment at all**: four resting and four
crossing placements, eight of eight `code=1/sCode=51001`, no `ordId` at any price.

| path | placement p50 | placement p95 | cancel p50 | cancel p95 |
|---|---:|---:|---:|---:|
| direct, one kept-alive tunnel (n=8) | **118 ms** | 123 ms | **76 ms** | 102 ms |
| direct, a fresh connection per request (n=6) | 194 ms | 2817 ms | 161 ms | 4515 ms |
| the operator's proxy, kept-alive (n=12) | 414 ms | 481 ms | 341 ms | 536 ms |
| the operator's proxy, fresh (n=12) | 632 ms | 977 ms | 584 ms | 907 ms |
| direct, one kept-alive tunnel, re-measured (n=12, 11:21 UTC) | **77.5 ms** | 81.4 ms | 78.0 ms | 91.6 ms |
| direct, a fresh connection per request, re-measured (n=12, 11:21 UTC) | 127.5 ms | 136.0 ms | 131.5 ms | 146.9 ms |
| direct, the `touch` probe's own placement leg (n=12, 11:22 UTC) | 112.9 ms | 148.1 ms | 112.7 ms | 158.4 ms |

**The direct rows are two runs of one day and they differ by 40 to 66 ms**, so the direct path is a range rather than a
constant: 77 to 118 ms kept-alive and 128 to 194 ms fresh. The proxy rows are one run each and have not been re-measured,
because the proxy is not running on this host (nothing listens on `127.0.0.1:7890`).

**Where the delay is, which is not the venue.** `curl` splits a cold request into its parts, and both paths were timed on
the same URL at the same minute:

| | connect | CONNECT + TLS | request -> first byte | total |
|---|---:|---:|---:|---:|
| direct, pinned to the venue's Cloudflare IP | 70 ms | 83 ms | **98 ms** | 181 ms |
| through the proxy | 1 ms to the proxy | 723 ms | **386 ms** | 1109 ms |

Two things fall out. The venue is fronted by Cloudflare -- `www.okx.com` is a CNAME to `www.okx.com.cdn.cloudflare.net`,
and the direct TLS handshake costs only 13 ms after the connection, so this host sits close to the edge; the 98 ms of
"request" is the edge reaching OKX's own origin. And **the proxy is the larger half**: the same proxy answers
`api.github.com` in 435 ms a request and takes 5.6 s to establish a connection to `www.google.com`, so its upstream is
slow for every host and not for this one. An order is not slower than a public read either -- 336 ms against 326 ms on
the same tunnel -- so the matching engine is not in this number at all.

**The decline rate the `touch` probe measures is a rate, and on both measured routes it is total.** Fourteen quotes at the
best bid, on a 553 ms round trip (p95 715), were **declined 14 of 14**: the venue accepts the order and then cancels it as
marketable on arrival, which is the `post_only` decline this repository counts. The 87-of-172 decline rate of the recorded
live session was a faster route than that. **On the direct route it is 12 of 12** -- twelve quotes at the best bid, a
113 ms placement p50, none rested and none filled, measured 2026-11-25 at 11:22 UTC. So at a fifth of the proxy's round
trip a `post_only` quote at the touch is still marketable on arrival every time, and the decline is not something the
proxy produces. Its through-ask version rested six of six on the same direct route, which is the same staleness read
from the other side: there the price quoted was one tick through the ask and the ask had moved up before it landed.

**The registered screen, run inside its window (2026-11-25, 13:31:01-13:40:55 UTC).** Thirty reads twenty seconds
apart, 555 instruments two-sided at least once, thresholds 1.2 bps of median half-spread, 2 ticks of median width and
500,000 USD of daily notional: **127 pass the width screen (1.2 bps), 104 clear 2.0 and 87 clear 2.5**, and 101 of 555
clear 2 bps of capture at a three-tick distance -- the maker fee. The reads are archived as
`data/v2/crypto/okx_tickers_20261125T133101Z.json`, and `--read` re-derives this table from it. From 2026-11-25 the
screen also reports **contracts a day** beside the notional, because dollars cannot separate a wide book from a busy one:
`SSV` and `NES` traded the same 1.94 M USD a day and 5,000 to 22,000 against 450,000 times. An archive written before that
column existed re-derives with the count as `n/a` and no filter on it, which is what keeps this sample checkable.

The capture ranking is where the mechanism lives, and the three instruments the owner downloaded land three different
ways:

| instrument | median half spread | median width | 3-tick capture | notional a day | verdict |
|---|---:|---:|---:|---:|---|
| DNA-USDT-SWAP | 7.75 bps | 2 ticks | **23.26 bps** | 12.4 M | rank 1, and already measured to fail the fill floor |
| UP-USDT-SWAP | 5.82 bps | 2 ticks | 17.47 bps | 3.25 M | the instrument the line is frozen on |
| NES-USDT-SWAP | 5.77 bps | 2 ticks | 17.31 bps | 1.94 M | UP's twin at rank 7 -- a candidate |
| CT-USDT-SWAP | 3.04 bps | 8 ticks | 2.28 bps | 3.62 M | clears the fee by 0.28 bps, and its tick is 0.76 bps |
| PIEVERSE-USDT-SWAP | 1.44 bps | 9 ticks | **0.96 bps** | 89.9 M | **fails**: the widest volume here cannot pay a 2 bps fee at three ticks |

So the screen separates two things a single read had run together: PIEVERSE is the most liquid instrument on the list
and still cannot pay the fee, because its book is nine ticks wide *and its tick is 0.32 bps* -- three ticks is one basis
point of capture. CT and PIEVERSE are the first candidates the frontier row asked for, the ones whose `tick_bps` is
**below 2**, where a fixed tick count is the wrong unit and `--quote-distance-bps` is the right one.

**The download ranking, in the two shapes the measured instruments define (2026-11-25).** Deciding *which* instrument is
worth the one manual download has a shape, and the archive answers it in two groups named after the instruments that
measured them. `tools/study/okx_download_rank.py --read data/v2/crypto/okx_tickers_20261125T133101Z.json` re-ranks the
sample with the screen's own medians and the same three-tick arithmetic:

| instrument | half spread bps | width ticks | tick bps | 3-tick capture bps | notional USD | depth on disk |
|---|---:|---:|---:|---:|---:|---|
| UP-USDT-SWAP | 5.82 | 2.00 | 5.82 | 17.47 | 3,248,216 | yes |
| NES-USDT-SWAP | 5.77 | 2.00 | 5.77 | 17.31 | 1,939,237 | yes |
| SSV-USDT-SWAP | 5.39 | 2.00 | 5.39 | 16.16 | 3,167,836 | |
| OL-USDT-SWAP | 8.91 | 4.00 | 4.45 | 13.36 | 1,547,216 | |

| instrument | half spread bps | width ticks | tick bps | 3-tick capture bps | notional USD | depth on disk |
|---|---:|---:|---:|---:|---:|---|
| PROS-USDT-SWAP | 3.21 | 4.50 | 1.43 | 4.28 | 16,553,709 | |
| BLUR-USDT-SWAP | 2.17 | 4.00 | 1.08 | 3.25 | 10,747,697 | |
| DGAI-USDT-SWAP | 2.48 | 5.00 | 0.99 | 2.98 | 8,544,878 | |
| CT-USDT-SWAP | 3.04 | 8.00 | 0.76 | 2.28 | 3,618,313 | yes |
| BABY-USDT-SWAP | 3.97 | 7.00 | 1.14 | 3.41 | 2,875,207 | |
| ALLO-USDT-SWAP | 1.75 | 4.00 | 0.87 | 2.62 | 2,619,783 | |
| ONE-USDT-SWAP | 2.17 | 4.00 | 1.09 | 3.26 | 2,299,142 | |
| GRVT-USDT-SWAP | 5.84 | 14.50 | 0.81 | 2.42 | 2,112,372 | |

The first group is UP's shape (two ticks wide, a tick worth 4.5 to 5.8 bps, 13 to 17 bps of capture) and the second is
CT's (four or more ticks, a tick under 2.5 bps, so a 14 bps distance stays inside forty ticks). **The pools are four and
eight, not the fifteen or twenty a reader might expect**: most of the 555 have a tick thin enough that three ticks is a
couple of basis points, which is exactly why the screen's 127 width-passers are not 127 candidates. The next two
downloads in order are **SSV** and **OL**, then **PROS** and **BLUR**.

**The screen's candidates, measured (2026-11-25).** The three-distance ladder is the first test of any candidate, because
the fill floor fails before the median does: the frozen cell minus the expected-value gate (which refuses a one-tick quote
by construction), seven days at 13:30-15:30 UTC, at one, two and three ticks -- or at seven, fourteen and twenty-eight
basis points where a tick is too narrow for a tick count to mean anything.

| instrument, distance | fills, median | closed median | days positive (closed) | what it says |
|---|---:|---:|---:|---|
| **NES, 3 ticks** | **46** | **+9.07** | **5 of 7** | clears the registered bar; net +1.25 on 4 of 7 |
| NES, 2 ticks | 57 | -0.67 | 3 of 7 | more fills, no capture |
| NES, 1 tick | 70 | -24.66 | 2 of 7 | the touch is where the flow is informed |
| SSV, 1 tick | 14 | +2.65 | 4 of 6 | 5,000 to 22,000 prints a day is not a book you can quote |
| SSV, 3 ticks | 3 | -29.74 to +31.87 | -- | three fills a window is not a measurement |
| CT, 14 bps (18 ticks) | 2 to 12 | -44.35 to +13.89 | 3 of 5 with fills | eighteen ticks out is rarely swept |
| CT, 28 bps (36 ticks) | 0 to 4 | mostly n/a | -- | dead |

**NES is the second seat the screen has produced.** At three ticks it fills 46 times a window with a positive closed
median on five of seven -- UP's bar structure at a lower capture (+9.07 against +26.18) and three fifths of its turnover
(1.94 M USD a day against 3.25). The distance ladder reads the way the DNA one did: the touch fills most and loses most
(70 fills, -24.66) and the capture only appears behind the flow.

**SSV is closed, and the reason is a rate rather than a price.** Its one-tick capture is the widest of the three (+2.65
closed on 4 of 6) and it fills **14 times a window against the thirty the bar asks**, because the tape carries 5,000 to
22,000 prints a day where NES carries 450,000. A wide book with no prints is the actions gate, not the fee gate -- the
shape the options line closed on, one market over.

**CT is closed from the other side of the same arithmetic.** A 0.76 bps tick turns 14 bps of distance into eighteen ticks
against a book eight ticks wide, so the quote rests where almost nothing sweeps: 2 to 12 fills a window at 14 bps, 0 to 4
at 28. What that does **not** close is the fine-tick branch itself, whose last two representatives are the high-turnover
ones: **PROS** (1.43 bps a tick, 14 bps is ten ticks, 16.6 M USD a day) and **BLUR** (1.08 bps a tick, thirteen ticks,
10.7 M a day). If neither of those reaches thirty fills at ten to thirteen ticks, the branch is written off rather than
extended.


**Two mechanisms, registered before their runs, and both fail the bar (2026-11-25).** The frontier's method rows had two
experiments that no data could block: the quote distance in basis points rather than ticks, and the one-sided retreat at
high inventory. Both are in the tree now behind flags that default to off, so every earlier number stands, and both ran
on the frozen cell (two layers x one contract, `--post-only`, a 40 ms asymmetric refresh, 10 ms on both legs,
`--latency-us 10000`) over the same seven days at 13:30-15:30 UTC. The bars were written before the runs: the distance
rule had to clear 30 fills a window with a positive closed median on 5 of 7 **on both instruments** and not be worse than
each instrument's own hand-tuned distance; the retreat had to keep the fills within 20 per cent and improve the median and
the worst day at twenty contracts.

| run | fills, median | closed median | net median | days positive (closed) | max inventory, contracts |
|---|---:|---:|---:|---:|---:|
| UP, `--quote-distance-bps=14`, which the rule turns into 3 ticks | 26 | +9.83 | +6.78 | 5 of 7 | 4-11 |
| DNA, `--quote-distance-bps=14`, which the rule turns into 2 ticks | 21 | +5.12 | -2.87 | 5 of 7 | 5-11 |
| UP at 20 contracts, control | 53 | +14.87 | +27.14 | 4 of 7 | 108-192 |
| UP at 20 contracts, `--onesided-inventory-frac=0.5` | 61 | +9.62 | +4.84 | 4 of 7 | 60-133 |

**The distance rule picks the right distance, and the distance is not the thing that binds.** At `D = 14` the rule reads
UP's tick (5.2 bps) as 2.66 and rounds to **3 ticks**, and DNA's (7.3 bps) as 1.92 and rounds to **2** -- exactly the two
distances the per-instrument sweeps found by hand -- and the fill counts reproduce those cells: 26 a window on UP and 21 on
DNA. What it cannot do is give DNA the thirty fills its book does not contain, so the bar fails on the instrument rather
than on the rule. On UP the fills and the day count match the frozen cell and the closed median does not (+9.83 against the
recorded +26.18); **that gap is not attributed here**, and until it is the honest statement is that the rule reproduces the
distance and not yet the cell's economics.

**The gap is attributed, and it is not in the rule (2026-11-25).** The frozen arm was re-derived with the flags the page
records for it -- `--base-spread-ticks=3`, two layers, 40 ms refresh, 10 ms link, `--post-only`, `--edge-cover-ticks=1` --
over the same seven days, and compared to the `--quote-distance-bps=14` row day for day:

| day | 3 ticks: fills, closed bps | `D = 14`: fills, closed bps | the rule's tick count that day |
|---|---:|---:|---:|
| 11-16 | 35, +37.04 | 35, +37.04 | 3 |
| 11-17 | 26, +44.84 | **40, +28.31** | **2** |
| 11-18 | 26, +3.99 | 26, +3.99 | 3 |
| 11-19 | 50, +9.83 | 50, +9.83 | 3 |
| 11-20 | 26, -8.63 | 26, -8.63 | 3 |
| 11-21 | 19, +57.91 | 19, +57.91 | 3 |
| 11-22 | 26, -69.40 | 26, -69.40 | 3 |

Six of the seven days are identical to the last digit, and the seventh is the one whose price halved: across 0.108 to 0.173
a tick is worth 9.3 to 5.8 bps, so `14 / tick_bps` falls to 1.5-2.2 and the rule reads **2 ticks** where the fixed count
holds 3. That day buys fourteen more fills and gives up 16.5 bps of capture, which is the rule doing what it was written
for: a distance in ticks is a function of the price, and a price that moved by a factor of two is exactly what a fixed
count cannot follow. The tick count per day is not a reading of the rule's intent -- it is what the rule computes, and the
sampled mid of each day's book says which bucket it lands in.

**What does not re-derive is the cell, not the rule.** Both arms read a closed median of **+9.83** on this data, so the
recorded row's **+26.18 is not reachable from the flags the page records for that row**. Four further variants were run to
look for it, with `--advance-debounce-ms=40` and `--post-only` held: without `--edge-cover-ticks=1` the two-layer arm reads
**+5.37** and the one-layer arm **+17.93** (19 fills, where the page records 17 and +12.40); at refresh 2 and 4 in the
sweep's own column the two-layer arm reads **+27.14** and **+9.59**. None is +26.18, and the one-layer row does not reproduce either. So the
statement is two-sided and both halves are worth quoting: `--quote-distance-bps=14` reproduces the frozen **geometry**
exactly, day for day, wherever the instrument's tick sits in the same rounding bucket; and the frozen **cell** is recorded
rather than re-derived -- +26.18 is quotable only as "recorded", until a run carrying the flags that produced it is found.
The one alternative this leaves open is the day set: the seven days here are 11-16 to 11-22, the depth on disk, and a cell
measured on a different seven cannot be re-derived from it.

**The one-sided retreat buys the tail and pays for it in the median.** At twenty contracts, stopping the adding side at
half the cap takes the worst day from **-215.52 to -138.01 bps** and three days' peak inventory down (1920 to 1320, 1400
to 1000, 1340 to 1230 contracts), and it takes the medians with it: closed +14.87 to +9.62, net +27.14 to +4.84. That is
what the frontier row predicted -- the prize is the handover the retreat avoids, 0.73 to 4.88 bps a day, and the
`--close-side-ticks` cousin already read lower -- and it is a registered failure rather than a finding, because the fills
stayed inside the 20 per cent (53 to 61) and the median moved the wrong way.


and places again, or amends in place. `okx_probe.py amend` runs both in one process on one kept-alive tunnel, alternating
which leg goes first, from a `post_only` buy two percent under the best bid to the same price one tick nearer, and times
each call:

| leg | p50 | p95 | what it costs |
|---|---:|---:|---|
| `POST /trade/amend-order` (n=12) | **83.2 ms** | 100.0 ms | one round trip, and the level never goes empty |
| cancel, then place (n=12) | **163.1 ms** | 175.6 ms | two round trips, with the level empty between them |

The decomposition reconciles, which is the check that makes it a measurement: the same run's cancel reads 80.2 ms and its
place 80.8, so the two-call leg should cost 161.0 ms and reads 163.1. Every amend was applied -- the price read back from
`/trade/order` moved on **12 of 12** -- with no `sCode` and no decline, so a `post_only` buy can be repriced in place.
The saving is 79.9 ms, **49 per cent of a repricing decision**, and it removes the moment the level has no quote in it.
What it does not do is shorten the reaction below the route: 83 ms is still eight times the 10 ms band where this
configuration stops being a maker, so the direction stays unbuilt until a venue answers for the instrument that matters
(`UP-USDT-SWAP` returns `51001` in the demo environment). The ruler was registered before the run -- inside 75 to 85 ms,
half a repricing decision -- and it landed at 83.2.

**The mechanism was then built into the replay and measured, and it loses on capture (2026-11-25).** With the venue's
semantics modelled -- the order keeps its id and never leaves the book, but it re-queues at the back of the level it moves
to, which is what a venue does and what a "free" amend would not be -- the frozen UP cell reads:

| arm, UP 13:30-15:30, 10 ms, `--post-only`, 40 ms refresh | fills a window, median (range) | closed bps, median | days positive (closed) | net bps, median | `zero_queue` orders |
|---|---:|---:|---:|---:|---:|
| cancel, then place (the frozen cell) | 26 (19-50) | **+9.83** | 5 of 7 | +6.78 | 21-53 |
| amend in place | **49 (29-62)** | **-5.92** | 3 of 7 | -1.82 | 239-296 |

The fills nearly double and the capture goes negative. Both halves are the same fact: an amended quote is always at the
fresh price and never spends the view the cancel-then-place pair spends being absent, so it is *reached* far more often --
and each of those extra fills is a quote that the market walked into rather than one the market came back to. The
`zero_queue` column says the same thing from the other side: 239 to 296 of the quotes were never behind anyone, against 21
to 53 in the frozen cell, because the amended order lands on a level that had just been cleared. That is the queue
position the venue's amend throws away, and at a 10 ms link it is worth more than the 79.9 ms the amend saves. So the
mechanism is **closed at this latency, built and off by default**, and the row that would reopen it is the one the route
already names: a few-millisecond round trip, where the saved 80 ms is the thing that decides whether the quote is a maker
at all and the queue given up is smaller than the fill it costs.

**And the halt, registered before its run (2026-11-25).** `ActiveL2MarketMaker`'s volatility halt is on in every recorded
run at 9.0 ticks a second and has no switch: the speed it compares is built from view gaps (8.5 ms mean on this feed)
rather than from the market, every threshold from 1.0 to 50.0 lands in the same place, and it fires `risk_pause_count` 513
times across the two hours of 11-16. Whether that helps or costs has never been measured -- only argued from half-hour
slices, where both halves lose a fill and the quiet one loses less. `--pause-ticks-per-second=D` exposes it now, with `0`
leaving the 9.0 every recorded cell used, and the run is the frozen UP cell over the same seven days with the halt off
(`D = 1e9`) against a re-derived default arm. **The bar, written here first: the halt is a tax if the fills reach 30 a
window and the closed median is not worse on five of seven days; it is doing its job if either fails, and this row closes
with that answer.** Both arms are the same configuration otherwise -- two layers, three ticks, 40 ms debounce,
`--edge-cover-ticks=1`, 10 ms, `--post-only` -- and `risk_pause_count` is read from one direct run of each arm, because a
pause count of zero in the off arm is the only thing that proves the flag moved the gate rather than the run.

**The halt is doing its job, and the row closes (2026-11-25).** The arm that ran it with the switch on reproduces the frozen
rows to the last digit, which is what says the flag defaults to the behaviour every recorded cell had; `risk_pause_count`
reads **513** with the default and **0** at `D = 1e9`, which is what says the gate and not something else moved.

| arm, UP 13:30-15:30, 10 ms, `--post-only`, 40 ms refresh | fills a window, median (range) | closed bps, median | days positive (closed) | worst day |
|---|---:|---:|---:|---:|
| halt on (the recorded cell) | 26 (19-50) | **+9.83** | 5 of 7 | -69.40 |
| halt off (`D = 1e9`) | 26 (12-44) | **+6.38** | 5 of 7 | -87.88 |

The fills do not reach thirty either way, so the bar's first condition fails and the second decides it: without the halt the
closed median is lower (**+6.38** against +9.83), lower on six of the seven days, and the worst day falls from -69.40 to
-87.88. The near-thirty half of the distribution goes the other way -- 11-20 improves from -8.63 to -3.91 -- but the
median is the registered statistic and it moves the wrong way. So the halt is not a tax on this cell: the half-hour
argument that stood in for a measurement ("both halves lose a fill and the quiet one loses less") now has its control, and
the fear was that the halt removes the cheap minutes while removing it removes more. **The row closes with the halt on**,
and the flag stays because the threshold is still built from view gaps rather than from the market: the next version of
this rule is a tick range over a fixed window, and it will need this switch to be tested against the same cell.

**And the amend is rebuilt asymmetric, with the bar written first (2026-11-25).** The measured loss had one cause: an
amended quote is always at the fresh price, so it lands at the back of the level that price just cleared and every adverse
print reaches it. The fix is to amend only when the move is a **retreat** -- a buy down, a sell up -- because that is the
case the amend exists for: one round trip, no gap at the level, and the quote out of the way before the sweep. An advance
keeps the cancel path, where the view it costs is a filter rather than a loss. A deadband of two ticks sits on top, so a
book that breathes does not re-queue a quote that was already in front. **The bar, written before the run: retreat-only has
to beat the cancel path's closed median (+9.83) and the symmetric amend's (-5.92), with fills inside 20 per cent of the
cancel path's 26 and a `zero_queue` count that falls back towards its 21-53 rather than the symmetric arm's 239-296.** The
engine gained the other half of the venue's rule for the same reason: a same-price **size reduction** keeps its queue
position, which is the amend a maker uses when inventory is heavy, and `matching_engine_test` pins it -- two orders at one
price, the first cut from ten to five, and the next trade still reaches it.

One defect came out of the first run and earns its line: the two prices were computed with `round` on the raw fraction,
and a half-tick lands on a boundary where `round` answers to even, so the target came out equal to the start -- an amend
asking for the price the order already had, reading 82.6 ms and meaning nothing. The prices are built from an integer
tick count now, and the probe refuses a target equal to its start.

**The band ends at 10 ms, not at 90.** The grid above is the whole ladder; the two measured paths sit far beyond it, and
the 100 and 181 ms rows below are the same configuration without the flag, for the record:

| latency each way | fills a window | fee bps | capture ticks | closed bps |
|---|---:|---:|---:|---|
| 10,000 us | 64 to 164 | 4.46 to 10.28 | +0.05 to +0.99 | -13.63 to +35.65 |
| 100,000 us | 25 to 44 | **26 to 89** | **-0.68 to -0.98** | +40 to +505, and -645 on one day |
| 181,000 us | 14 to 24 | 32 to 102 | -0.82 to -1.33 | -381 to +681 |

So `--latency-us` for this host is **77,500 to 118,000** on a direct kept-alive tunnel -- two runs on the same day, 40 ms
apart -- and 414,000 through the proxy, and **both are seven to forty times past the point where this configuration is a
maker** (77.5/10 and 414/10): at 10 ms it already pays 4.5 to 10 bps of fee against the maker's 2.00, and the direct
route's 77 to 118 ms brackets the 80 ms row, where the fee is 13 to 129. The row that would open
the line is an order round trip of a few milliseconds -- inside 10 ms, and the 10 ms row is already halfway to a taker, so
the target is a colocation beside the venue's matching engine rather than a Tokyo or Singapore VPS on its own.



**The capacity arithmetic, and why the fee tier cannot be bought (2026-11-25).** The tier the instrument needs is about
1.5 bps of maker fee, and the obvious question is whether this book can generate the volume that unlocks it. The inputs,
each measured on the seven windows above:

| input | value |
|---|---|
| fills in a two-hour window, rescue configuration, zero latency | 76 to 358, median 116 |
| one contract | 10 UP; the windows' mids run 0.166 to 0.228, median 0.196, so **1.96 USD** |
| level 1 of the book, time-weighted per window | bid 14 to 161 contracts (median 67), ask 27 to 143 (median 71) |
| two levels of the book | bid 130 to 555 (median 257), ask 179 to 443 (median 329) |

The depth rows are `tools/study/okx_book_depth.py` over the same seven cut windows, which is the tool
`okx_book_window.py --emit` feeds; the fills and the mids are the replay's and the archive's own.

A quote that is a tenth of the thinner level 1 is **1 to 12 contracts, median about 4** -- that is the size the book
supports without the quote becoming the touch. At one contract a window trades 116 fills, so a month of four windows a day
is `116 x 1 x 1.96 x 4 x 30` = **27,300 USD** of notional; at four contracts, **109,000 USD**, and that second number is an
upper bound because the fills are assumed not to fall as the clip grows, which they would.

Against a 5,000,000 USD thirty-day threshold (the account's own tier table, **not verified here**), one instrument of this
size is **0.5 percent** of the tier at one contract and **2.2 percent** at four: the tier would take about **46 instruments
of this depth**, each needing the seven-day treatment and the route this host does not have.

**And buying it with volume does not work either.** The 5,000,000 USD that unlocks the tier costs `5,000,000 x 2.0 bps` =
**1,000 USD** of fee a month at the retail rate, and what the tier then saves on the notional this line would trade anyway
is `27,300 x 0.5 bps` = **1.37 USD**. The ratio is five to seven hundred times the wrong way. So the tier is not something
the strategy can earn by trading -- it is something an account either already has (a balance threshold, or a rebate tier a
broker passes through) or does not, which is why the frontier map lists it as an account question with a numeric trigger
rather than as a research row.


**Two execution mechanisms, registered, measured, and one of them worth a bigger sample (2026-11-25).** With the link
measured and the maker regime ending at about 10 ms, the question the frontier map leaves is whether the *decision* can be
changed rather than the delay. Two mechanisms were registered and built, both off by default and both orthogonal to the
quote geometry: a **jump cooldown** (a mid-range of `--jump-ticks` inside `--jump-window-ms` pulls the quotes and holds
them off for `--jump-cooldown-ms`) and an **asymmetric refresh** (`--advance-debounce-ms`: the side that would move towards
the market waits until the fair has been still for that long, while the side that retreats goes out at once).

The bar was written before the runs: at 80 ms with `--post-only` the rescue configuration reads 1 to 10 fills a window at
the maker's 2.00 bps, so a mechanism that helped would raise the median to **at least 20 fills** with the fee still 2.00
and the capture at **+1.0 tick or better**, on **five of seven days**. What the runs say, at the two latencies that matter:

| configuration, `--post-only` | fills a window | fee bps | capture ticks | closed bps, median (days positive) |
|---|---:|---:|---:|---|
| 80 ms, nothing new | 1 to 10 | 2.00 | +0.79 to +3.19 | -- |
| 80 ms, jump cooldown | **0 to 1** | 2.00 | +2.00 to -2.00 | -- |
| 80 ms, asymmetric refresh | 1 to 4 | 2.00 | +0.50 to +3.50 | -- |
| 10 ms, nothing new | 38 to 75 | 2.00 | +1.49 to +1.98 | **-4.61** (1 of 7) |
| 10 ms, jump cooldown | **0 to 1** | 2.00 | +2.00 to +4.00 | -- |
| 10 ms, asymmetric refresh | **13 to 27** | 2.00 | +1.46 to +2.34 | **+12.40** (6 of 7) |

**The cooldown fails for a reason that is a property of the instrument**: its tick is 0.0001 on a 0.19 price, about half a
basis point, and the book is two to three ticks wide, so "two ticks inside forty milliseconds" is the normal state rather
than a jump -- the gate pauses almost always and the fills fall to zero or one at both latencies. That closes this variant,
not the idea: a jump threshold has to be expressed in the instrument's own volatility, and on this one it is not two ticks.

**The asymmetric refresh does the opposite at 10 ms**, and it is the one result here that is worth another sample: it
halves the fills, from 38 to 75 down to 13 to 27, and turns the closed capture from a median of -4.61 bps with one day of
seven positive into **+12.40 with six of seven positive**, at the maker's fee on every day. Refusing to chase the market
captures less often and better, which is what the mechanism was for. **It is not a conclusion**: thirteen to twenty-seven
fills is below the thirty this repository requires, so what is registered for the next run is the same mechanism with
**thirty fills or more a window and a positive closed median on five of seven days** -- which needs a larger clip or an
instrument that trades more, not a different threshold.

**Three ways to get there were tried, and the cheapest one turns out to be the wrong trade (2026-11-25).** All three runs
are at 10 ms with `--post-only` and the 40 ms asymmetric refresh, seven days, so they differ from the row above in one thing
each:

| experiment | fills a window | fee bps | closed bps, median | days positive |
|---|---:|---:|---:|---|
| the 13:30-15:30 window (the row above) | 13 to 27 | 2.00 | **+12.40** | 6 of 7 |
| 08:00-16:00, the gates deciding when to quote | **49 to 93** | 2.00 | **-1.89** | 3 of 7 |
| 16:00-24:00, one day for the shape | 2 | 2.00 | 0.00 | 0 of 1 |
| 13:30-15:30 with `--close-side-ticks=1` | 14 to 26 | 2.00 | +11.41 | 4 of 7 |

**Widening the clock buys the sample and gives up the edge.** Eight hours of the same configuration clears the thirty-fill
floor on every day (49 to 93 fills) with the fee still the maker's 2.00 bps, and its closed median is **-1.89** with three
of seven days positive, against **+12.40 and six of seven** in the two hours it was widened from. The two-hour window was
not an arbitrary lock: it is the best part of the session, and the hours around it carry the same maker fee against a worse
capture. The evening is not a candidate at all -- 16:00-24:00 UTC produced **two fills in eight hours** on the day it was
cut, so the instrument's flow is a European-and-US-morning phenomenon.

**The closing-side skew trades capture for turnover and does not pay.** Bringing the reducing side in to one tick while the
position is not flat leaves the fills where they were (14 to 26 against 13 to 27) and lowers the closed median from +12.40
to +11.41, with the positive days falling from six to four: the side that closes sooner closes worse, and the turnover it
buys is not the constraint. That is a closure of the lever at this size, not a claim about inventory management in general
-- and the existing `--inventory-shift-ticks` already moves both quotes with the position, which is the version of this
idea the recorded runs carried.

**The third one was built, and it is the one that clears the bar (2026-11-25).** Two passive layers a side, the second
one tick further out, the same size and the same asymmetric refresh: the fill *count* a window carries is bounded by the
number of orders rather than by their size, which the clip sweep showed first.

| configuration, 10 ms, `--post-only`, 40 ms refresh | fills a window, median (range) | fee bps | closed bps, median | days positive | `max_abs_inventory`, contracts |
|---|---:|---:|---:|---:|---|
| one layer, one contract | 17 (13-27) | 2.00 | +12.40 | 6 of 7 | 3 to 9 |
| one layer, two contracts | 21 (15-24) | 2.00 | +20.46 | 6 of 7 | 4 to 9 |
| one layer, four contracts | 24 (16-33) | 2.00 | +11.76 | 4 of 7 | 8 to 13 |
| **two layers, one contract** | **26 (19-37)** | 2.00 | **+26.18** | **5 of 7** | 4 to 10 |
| two layers, two contracts | 29 (22-54) | 2.00 | +4.53 | 5 of 7 | 4 to 13 |

**The size does not buy fills, and the layers do.** Doubling the clip moves the notional and leaves the count near
twenty-one; quadrupling it reaches twenty-four and costs the capture (the closed median falls from +20.46 to +11.76 and
the positive days from six to four) -- more size through the same thin book is the closing-side result again. A second
layer at the same size lifts the count to 26 with one day at 37 and raises the closed median to **+26.18**, which is the
best cell in the table, and the two capture figures beside it are recorded rather than re-derived -- the day-by-day
attribution is above, under the distance rule's closure.

**Its bar was three conditions and it meets two and a half.** Fills rose above the one-layer row (26 against 17), the
closed median stayed positive on five of seven days, and `max_abs_inventory` did **not** stay unchanged: 3 to 9 contracts
became 4 to 10, because a second resting order a side can also fill. That is not a defect of the rule but a correction to
the bar -- the condition worth registering was "inside the configured cap", and at the default ten contracts the two-layer
run reaches it exactly. On a venue where the cap is the risk budget, two layers is therefore a *sized* decision rather than
a free one.


**The half-hour profile, and it refutes the "dead wide" explanation (2026-11-25).** The eight-hour window lost its edge
and the guess was that the quiet hours carry a wide book that nobody trades -- a gate reading only the spread would quote
into it and be picked off. `tools/study/okx_halfhour_profile.py` measures the two quantities that guess is about, with no
P&L and no window chosen by outcome: the duration-weighted half spread and the prints a second, per half hour, median
across the seven days.

| bucket UTC | half spread bps | median trades/s | min trades/s | days at or above 2.5/s |
|---|---:|---:|---:|---:|
| 02:00 | 4.83 | 2.476 | 1.489 | 3 of 7 |
| 02:30 | 4.79 | 2.664 | 1.462 | 4 of 7 |
| 05:00 | 5.26 | 3.179 | 1.658 | 4 of 7 |
| 07:00 | 5.11 | 2.713 | 0.467 | 4 of 7 |
| 08:00 | 5.27 | 1.780 | 0.568 | 2 of 7 |
| 09:00 | 5.68 | 1.322 | 0.881 | 2 of 7 |
| 11:00 | 7.10 | 1.842 | 0.839 | 1 of 7 |
| 12:00 | 5.34 | 1.882 | 1.193 | 2 of 7 |
| 13:00 | 5.30 | 2.102 | 0.756 | 2 of 7 |
| 13:30 | 6.06 | 1.700 | 0.768 | 2 of 7 |
| 14:30 | 6.02 | 2.857 | 1.131 | 4 of 7 |
| 15:30 | 5.29 | 2.909 | 1.641 | 5 of 7 |
| 16:00 | 5.26 | 2.199 | **0.000** | 3 of 7 |
| 17:00 | 5.05 | 2.203 | **0.000** | 3 of 7 |
| 20:00 | 5.43 | 1.293 | **0.000** | 1 of 7 |
| 23:00 | 4.90 | 1.289 | **0.000** | 1 of 7 |

**The spread does not widen when the flow leaves.** Across all forty-eight buckets it sits between 4.78 and 7.10 bps --
5.27 at 08:00, 6.06 at 13:30, 5.43 at 20:00 -- so on this instrument a quiet book is not a wider book, and the gate cannot
be fooled by "dead width" because there is no dead width to be fooled by. That is the opposite of the guess, and it is a
property of the instrument: a 0.0001 tick on a 0.19 price is **5.2 bps**, so a two-to-three-tick book is 5 to 8 bps of half
spread whatever the hour. (The first version of this page said "one tick is half a basis point", which is 10x low; the
screen's own tickSz column measures 5.2 to 5.4 and the two agree, so the sentence was the defect and the table was right.)

**What does move is the flow, and only at the end of the day.** The median rate runs 0.93 to 3.18 prints a second all day;
the *minimum across days* is zero in every bucket from 16:00 onward and 0.38 to 1.66 before it. So the evening is genuinely
dead on at least one day of seven, which is what the eight-hour replay's two fills were telling us -- and the morning is
not: 08:00-13:00 prints at 1.3 to 2.1 a second against the good window's 1.7 to 2.9.

**And the 2.5-a-second gate the equity screen used does not transfer.** Only six of the forty-eight buckets have a *median*
at or above 2.5, two of them in the middle of the night, and 13:30 -- the middle of the window this instrument was measured
in -- clears it on two days of seven. A gate at that number would shut almost every bucket, including the two hours the
line actually earned in, while 02:30 would pass it on four days. **A density gate is therefore worth building only with a
threshold read off this profile, and this one shows that such a threshold is not a window selector at all**: 1.5 a second
clears fourteen of the sixteen buckets from 00:00 to 08:00 as well as the whole afternoon, so what it separates is the late
evening and nothing else. It is a market-open switch, not a choice between the hours the line earns in and the hours it
does not -- and the night run further down is the measurement that makes that distinction matter.

**So the eight-hour loss is not the book's state.** Same half spread, comparable flow either side of the two hours, and the
closed capture per fill fell about ten-fold. What differs is which prints the quotes met, and that is toxicity, which this
profile cannot see: a per-bucket markout is the measurement that would, and the tape supplies part of it -- the section
further down builds that tool -- while the part that needs our own fills is still open.


**The night run: the same configuration over 00:00-08:00, and it loses (2026-11-25).** The profile left two things open.
One is whether the two hours' edge belongs to those hours or to the only window anybody had run. The other it turned up
itself: the night is as dense as the afternoon. So the frozen cell -- two layers, one contract, `--post-only`, a 40 ms
refresh, 10 ms on both legs, `--edge-cover-ticks=1` -- was run over 00:00-08:00 of the same seven days, at three inventory
caps, with nothing else changed.

| inventory cap | fills a day | days net positive | median net bps |
|---|---:|---:|---:|
| ten contracts, the default | 51-152 | 2 of 7 | **-16.51** |
| three contracts | 85-153 | 2 of 7 | **-6.80** |
| one contract | 98-156 | 2 of 7 | **-7.20** |

**The fills are not what the night is short of.** 117 fills in eight hours is 14.6 an hour, against the two-hour window's
13 an hour at 26 fills -- so the density gate that reads 1.5 a second opens this block at the same rate it opens the
afternoon, and the block loses. A gate calibrated on prints a second therefore cannot be the thing that separates them.

**Most of the loss is inventory the two hours never had to carry.** Loosening the cap from three contracts to ten deepens
the median from -6.80 to -16.51: ten of the seventeen bps. What is left under a three-contract cap is the fills' own
economics, and there the night's closed capture is about zero a fill against the window's +0.71 (the median of the seven
days' closed-per-fill column). That is the quotient the profile could not produce, and it says the night is not simply
the same edge with fewer participants -- the flow does not come back to the other side.

**And a forced exit cannot be priced in this harness.** `--close-out` appended to the same eight hours returned numbers
identical to the run without it, to the last digit of every column: the replay's fills come from the tape, and an IOC at
the touch has no counterparty once the input ends, so the residual position is marked again rather than closed. The
desk's flatten step -- stop opening, then cross out of what is left -- is exactly the control this measurement cannot
test. The cap and the clock are what it can, and the cap says halving the carry halves the loss without changing its sign.


**Four windows, one configuration: the two hours are the only block that pays (2026-11-25).** The same frozen cell over
every hour the tape is alive, so the window is compared against the rest of the day rather than against itself. Two
medians are given because they answer different questions -- `net` is what a day's run banked, `closed` is the position
flattened against the last midpoint -- and mixing them in one column is how a table quotes two definitions at once:

| window UTC | hours | fills a day | days positive | median net bps | median closed bps | closed per fill bps |
|---|---:|---:|---:|---:|---:|---:|
| 00:00-08:00 | 8 | 51-152 | 2 of 7 | **-16.51** | -10.71 | -0.09 |
| 08:00-13:30 | 5.5 | 61-97 | 4 of 7 | **+4.59** | +6.59 | +0.07 |
| 13:30-15:30 | 2 | 26 | 5 of 7 | **+15.13** | **+26.18** | +0.71 |
| 16:00-24:00 | 8 | 2 | 0 of 7 | not measurable | -- | -- |

**The registered bar is what does the separating.** A block qualifies at 30 or more fills with a positive median on 5 of 7
days: the two hours clear it, the morning has the fills and not the days (4 of 7), the night has more fills than either
and loses on 5 of 7, and the evening has two fills in eight hours. So the two hours' edge is not an artefact of being the
only window anybody ran -- the same mechanism, on the same days, at the same size and fee, is worth +0.71 bps a closed
fill there, +0.07 in the morning and -0.09 at night.

**The tape's toxicity is not a constant, it is a function of how far a print reached (2026-11-25).**
`tools/study/okx_tape_toxicity.py` at a five-second horizon, run twice. Over every print the mean maker markout is negative
in forty-three of forty-eight buckets, -0.03 to -2.99 bps, with 61 to 86 per cent of the prints moving the mid: the median
print is informed, and a quote sitting on the touch is the passive side of it. Keeping only the prints that reached **two
ticks or more through the mid** -- thirteen to twenty per cent of them -- flips the sign in thirty-one of the forty
buckets measured (00:00 to 19:30): **+0.34 to +5.69 bps**, with 13:30 +1.67, 14:00 +1.21, 14:30 +1.37, 15:00 +2.11 and
15:30 +0.51, and the night mostly the same (+2.43 at 00:00, +3.30 at 02:00, -0.89 at 07:00, -2.15 at 05:00). So the maker
is not paid for the flow being uninformed; he is paid for standing where the sweeps reach and the touch does not, which is
the shape `--base-spread-ticks=3` and its passive layer already have.

**Our own fills per half hour: the night is uniformly churned, not piled up in a few buckets (2026-11-25).**
`tools/study/okx_fill_profile.py` reads the trade stream a replay already writes (`data/runtime/trades.csv`) and buckets
our fills by half hour: the net contracts a bucket moved, the share of its fills that *opened* a position, the capture
against the midpoint and the markout at five and thirty seconds. The night run of the same seven days, frozen cell:

| bucket UTC | fills a day | net contracts | opened share | capture bps | markout +5 s | markout +30 s |
|---|---:|---:|---:|---:|---:|---:|
| 00:00 | 7 | -1.00 | 62% | +2.71 | -0.000 | +2.770 |
| 01:00 | 3 | -0.50 | 53% | +2.89 | +0.000 | -5.637 |
| 02:00 | 5 | +0.00 | 57% | -2.29 | -4.118 | -0.000 |
| 02:30 | 8 | -2.00 | 45% | +0.00 | +0.000 | +7.124 |
| 03:00 | 9 | +3.00 | 47% | -0.00 | +0.000 | +8.621 |
| 04:00 | 8 | -2.00 | 37% | +2.11 | +0.000 | -4.779 |
| 05:00 | 7 | -2.00 | 46% | +1.39 | +0.000 | +0.000 |
| 05:30 | 8 | +2.00 | 57% | -0.00 | +2.307 | +4.866 |
| 06:30 | 6 | -2.00 | 50% | +1.36 | +2.148 | +4.235 |
| 07:00 | 5 | -1.00 | 49% | -2.83 | -2.607 | -0.084 |

**There is no bucket where the inventory piles up.** Across the sixteen buckets of the night, the net a bucket moves is
**-2.00 to +3.00 contracts** and the share of fills that open a position stays **37 to 62 per cent** -- near half in every
bucket, with no bucket opening three quarters of what it fills. So the night's loss is not a few bad buckets the strategy
could learn to avoid: it is the same churn spread evenly, with a capture a fill of 1.79-1.98 ticks -- 9 to 10 bps on this
instrument's 5.2 bps tick -- against a closed result of about zero a fill, so the spread taken at the fill is given back
before the position closes. A gate on the bad buckets would have nothing to switch on, and that is the answer the
four-block comparison needed.

The markout columns are noisier than the counts (a bucket holds two to nine fills a day, so fourteen to sixty-three fills
over the week) and they carry no bucket-level signal either: five seconds reads **-4.12 to +2.31 bps**, thirty seconds
**-5.64 to +9.64**, both signs present in the middle of the night and in the small hours alike.

**The screen gains the column the mechanism needs, and it re-ranks everything (2026-11-25).**
`okx_spread_screen.py` reported width, ticks and volume; a quote three ticks behind the touch needs a fourth number -- what
that distance is worth in basis points, which is `3 x tick` with a tick of `2 x half-spread / spread-in-ticks`. On the
archived reads of one off-window sample (555 instruments with a two-sided book, 06:58-07:08 UTC, so a reading and not the
registered 13:30-15:30 screen):

| instrument | half spread bps | spread ticks | 3-tick capture bps | 24h notional USD |
|---|---:|---:|---:|---:|
| DNA | 7.34 | 2.00 | **22.03** | 13.9M |
| ARX | 9.18 | 3.00 | 18.37 | 0.50M |
| LINEA | 6.05 | 2.00 | 18.15 | 0.86M |
| NES | 5.61 | 2.00 | 16.84 | 2.71M |
| **UP** | 8.11 | 3.00 | **16.21** | 3.13M |
| EGLD | 4.52 | 2.00 | 13.55 | 0.56M |

**71 of the 555 clear 2 bps of capture, and the fourteen names the width screen called passers are not among them**: BE at
1.67 bps over five ticks is 1.00 bps of capture and COST at 1.37 over thirty is 0.14. That is the reversal the column
exists to show -- wide enough in bps is not wide enough at our distance, and ranking by half-spread rewards a book whose
ticks are too cheap to pay a fee. UP sits fifth of 555, which is the sanity check: the instrument the line was measured on
is near the top of the only ranking that matches the mechanism.

**And it is a reading, not a verdict.** The sample was taken at 07:00 UTC, outside the window every other screen here uses,
so it says what the venue looked like then; capture is also not expectation -- a two-tick book pays 20 bps a fill and fills
rarely -- which is what the acceptance cell, and the depth archives it needs, is for.

**The capture screen's top instrument fails the acceptance cell (2026-11-25).** `DNA-USDT-SWAP` ranked first of 555 on the
capture column -- 22.03 bps for a quote three ticks behind the touch -- and the owner downloaded its seven days of
400-level depth from the venue's portal, because the public route has no book module. With seven trade tapes from the
public route, the instrument ran the same frozen cell as UP: two layers of one contract, `--post-only`, a 40 ms debounce,
10 ms on both legs, `--edge-cover-ticks=1`, `--settle-residual`.

| day | fills | net bps | closed bps | capture ticks | hold s | settle bps | settled bps |
|---|---:|---:|---:|---:|---:|---:|---:|
| 2026-11-16 | 7 | -100.91 | -13.45 | 1.14 | 5.75 | 6.20 | -107.11 |
| 2026-11-17 | 7 | -11.84 | -11.63 | 1.00 | 5.33 | 2.50 | -14.34 |
| 2026-11-18 | 19 | +38.04 | +39.88 | 2.21 | 1.97 | 0.73 | +37.31 |
| 2026-11-19 | 18 | +5.02 | +0.35 | 2.22 | 0.96 | 1.96 | +3.06 |
| 2026-11-20 | 13 | -49.45 | -8.61 | 0.92 | 1.22 | 7.84 | -57.29 |
| 2026-11-21 | 20 | +58.64 | +15.40 | 1.83 | 2.93 | 5.04 | +53.59 |
| 2026-11-22 | 18 | -30.25 | -1.23 | 3.14 | 1.68 | 6.74 | -36.99 |

**It fails both halves of the registered bar.** Seven to twenty fills a window against the thirty-fill floor, and a median
net of **-11.84 bps on 2 of 7 days** where UP reads 26 fills, **+15.13** and 5 of 7. The settlement deepens it to a median
of **-14.34**: a residual of one to seven contracts is a larger share of a day with eighteen fills than of one with
twenty-six, so the handover costs 0.73 to 7.84 bps here against UP's 0.73 to 4.88.

**And the reason is the column that ranked it.** DNA's tick is 0.0001 on a price near 0.136 -- **7.3 bps** -- and its book
is two ticks wide, so a quote three ticks behind the touch rests 22 bps from the mid. Almost nothing sweeps that far: the
run's own capture is 0.92 to 3.14 ticks a fill and its fill rate 1.6 to 5.4 per cent. The capture column said a two-tick
book pays well and fills rarely; the acceptance cell says how rarely, and it is too rarely to reach the fill floor.

**So the ranking is of captures, not of opportunities**, and no screen replaces the fill test. What the column buys is the
order in which to spend portal downloads: it put DNA first of 555, and DNA was rejected in an afternoon instead of after a
week of live work.

**And its best distance still misses the bar (2026-11-25).** The same seven days at three quote distances, everything else
frozen -- the parameter is the mechanism, and the bar was registered before the run:

| half width, ticks | fills a day | median net bps | days positive | closed_bps median |
|---|---:|---:|---:|---:|
| 1 (at the touch) | 28 | -2.36 | 3 of 7 | 8.82 |
| 2 | 21 | **+10.37** | 4 of 7 | 10.08 |
| 3 (the frozen cell) | 18 | -11.84 | 2 of 7 | 0.35 |

**Twenty-one fills is under the thirty-fill floor and four of seven is under the five**, so DNA does not qualify at any of
the three; two is its best compromise and the reader should not read it as a result. What the ladder does say is worth
keeping: the **touch is the worst place to quote** even though it fills most (median -2.36 against +10.37 two ticks out),
which is the tape's own finding from the other side -- a print at the touch is informed, a print two ticks through the mid
reverts -- and it says the two columns (fill count and capture) trade against each other rather than moving together.

**The first DNA run measured nothing, and the reason is a defect worth keeping.** The sweep's cut and trade-slice names
were keyed by day and window only, so the DNA run replayed *UP's* cut -- the cut file's own `instId` read `UP-USDT-SWAP` --
and every cell came back `?`. Both names now carry the instrument (`okx_<SYMBOL>_<day>_<start>.jsonl`,
`trades_<SYMBOL>_<day>.csv`) and `okx_book_window.py` names its emit the same way, so a second instrument cannot inherit
the first one's book. No recorded number came from the broken run.

**The handover priced, and the hold time that refutes the time stop (2026-11-25).** Two things were left open: what a
window's leftover position would cost a session that has to hand over, and whether the night loses to inventory that stays
too long. The first needed a flag, because `--close-out` cannot price it -- an IOC at the touch finds no counterparty once
the input ends, and the eight-hour run with the flag returned numbers identical to the run without it. `--settle-residual`
prices it instead: the far side of the touch, one tick past it, five basis points of taker fee, printed as `[SETTLE]`
behind the marked `[PNL]`. The second needed only `--metrics`, whose `[ROUND_TRIPS]` line already carries
`average_holding_seconds`.

| block UTC | median net bps | settle cost a day, bps | median settled bps | days positive | median hold, s |
|---|---:|---:|---:|---:|---:|
| 13:30-15:30 | +15.13 | 0.73 to 4.88 | **+11.22** | 5 of 7 | **2.18** |
| 00:00-08:00 | -16.51 | 0.16 to 1.41 | **-16.67** | 2 of 7 | **2.54** |

**The handover is affordable and it does not change the answer.** In the two hours it costs 0.73 to 4.88 bps a day, and the
settled median is +11.22 with the same 5 of 7 -- 74 per cent of the net median survives the worst case, so the window is
not a result that depends on being able to walk away from its last contracts. At night it costs less still (0.16 to 1.41)
because there is less maker notional behind the residual, and the block stays where it was at -16.67 on 2 of 7.

**And the time stop has nothing to act on.** The hypothesis was that the night's fills are fine at five and thirty seconds
and lose only by being held, so a "close at a touch after N seconds" rule would rescue the block. The holding times say
otherwise: completed round trips live **0.62 to 4.37 seconds in the two hours and 1.53 to 10.75 at night** -- the same
order, seconds rather than minutes, with no long-held population for a timer to reach. What the night carries is the
residual at the end (`net_pos` -3 to +9 contracts), which is exactly what the cap ladder and this settlement price, and
that residual is not a dwell. So the mechanism was not built: the measurement refuted the reason for it first, which is
cheaper than a flag nobody should run.


**What is next, and what would qualify another instrument.** The next measurement is a tiered re-screen of every swap
inside the same 13:30-15:30 UTC window -- thirty ticker reads twenty seconds apart -- with the book's median width reported
beside the half-spread, because a one-tick book cannot hold a quote three ticks behind the touch whatever its bps figure
says. A candidate has to clear **1.2, 2.0 and 2.5 bps** under two more floors: a median spread of at least **two ticks** and
a median **500,000 USD** of daily notional. Anything that passes gets the same seven-day treatment this instrument got: the
window cut by `tools/study/okx_book_window.py`, the maker run by `tools/sweep/okx_maker_baseline.py` under the conservative
fill model, and `--latency-us` at the route's own measured value -- **77,500 to 118,000** on a direct kept-alive tunnel
(two runs on 2026-11-25), 414,000 through the operator's proxy, and in microseconds always.

One measurement error belongs in this record because it produced a table of zeros: OKX cuts its trade archive on midnight
UTC+8 and its book archive on UTC, so a window starting at or after 16:00 UTC lives in the *next* trade file. Reading the
file named for the book's day gave two hours of book against zero trades, and the engine answered with zero quotes on
seven days -- which reads exactly like a gate that blocks a quiet market. `okx_maker_baseline.py` now picks the trade file
by the window rather than by the book's day.



### Line 8 executed: the low turnover rule, and the dispersion that replaces the verdict

The rule was fixed before any window was read, because the point of it is to be falsifiable rather than tuned: a one
minute mid from each file's top of book, a thirty minute simple moving average of it, an entry two ticks from the
average, an exit on the crossing back or after sixty minutes, fills at the touch both ways, and the whole schedule
charged a share on both sides. `tools/study/lowturnover_test.py` is the test.

**It fails on the median, and the reason is the same arithmetic as the other seven lines rather than a new one.** 57
symbols produced 25 whose net a share a round trip is positive, against a **median net of -0.36 cents a share and a
median gross of +0.40**: the signal is worth about four tenths of a cent a share and the round trip costs about eight.

| | symbol | round trips | mean hold | gross a share | net a share | window net at 100 shares |
|---|---|---:|---:|---:|---:|---:|
| best | AMD | 9 | 8 min | +103.78 c | +101.77 c | +915.89 USD |
| | QQQ | 7 | 7 min | +37.14 c | +34.88 c | +244.16 USD |
| | SPY | 11 | 4 min | +21.36 c | +19.05 c | +209.51 USD |
| worst | LABU | 5 | 14 min | -145.60 c | -146.86 c | -734.30 USD |
| | MU | 4 | 14 min | -92.25 c | -95.21 c | -380.85 USD |
| | MSTR | 3 | 14 min | -28.67 c | -29.73 c | -89.19 USD |

**What this run does not do is settle the rule, and the limitation is the sample rather than the arithmetic.** All 57
symbols are the *same* two hours, so they are 57 independent draws of a statistic whose median is zero and whose values
run from +102 cents a share to -147; 25 positives out of 57 is below half, and with three to eleven round trips a
symbol each figure is dominated by whichever single move its window happened to contain. **A rule like this needs many
windows per symbol, and the only instrument here with more than one is MU**, at eight, and MU is one of the largest
losers in the table. So the line was left unproven pending the replication it named, and that replication has now been run in the same file
by reading the depth shape instead: the same rule over MU's eight windows.

| MU window | round trips | mean hold | gross a share | net a share | window net at 100 shares |
|---|---:|---:|---:|---:|---:|
| 1 | 7 | 12 min | +152.71 c | +149.76 c | +1048.35 USD |
| 2 | 7 | 9 min | +43.29 c | +40.38 c | +282.65 USD |
| 3 | 11 | 7 min | +38.64 c | +35.67 c | +392.35 USD |
| 4 | 6 | 11 min | +35.50 c | +32.52 c | +195.14 USD |
| 5 | 4 | 14 min | -76.25 c | -79.21 c | -316.85 USD |
| 6 | 4 | 18 min | -129.00 c | -131.88 c | -527.51 USD |
| 7 | 4 | 20 min | -261.00 c | -263.95 c | -1055.81 USD |
| 8 | 2 | 40 min | -977.50 c | -980.38 c | -1960.77 USD |

**Four windows are positive and four are negative, so the sign does not replicate**, and the median net is -23.34 cents
a share against a median gross of -20.38. The shape of the failure is worth more than the number: the best window
earned 149.76 cents a share and the worst lost 980.38, so **the rule wins small and loses big**, which is what a
reversion rule does in a market that chops and then trends, because the exit is the crossing back or sixty minutes and
a trend is therefore held for as long as the rule allows. **Line eight is closed**, and unlike the seven before it, it
is closed by payoff asymmetry rather than by a cost, which is why the fix would be a trend filter and why that is a
different claim than the one that was tested.


### The bar every next step has to clear

The direction from the owner is that each iteration answer one question, prove basic profitability first, and only then
ask whether it can be run continuously. Written as a bar rather than an intention:

1. **One question per step**, stated before the step runs, with the measurement named that answers it and the cost
   named before it is spent.
2. **Basic profitability** means, on the measured window, net capture per fill positive after the account's whole
   schedule **including the per order floor**, and capture at least the size of one window's own position standard
   deviation. The first is already printed as fees paid and minimum commission paid; the second is the quotient
   measured above, and MU's 0.089 is the worked example of failing it.
3. **Continuity** means the same sign on at least two non-adjacent windows, which is the check that one window was not
   the market's accident.

The engine core -- market data handling, the order state machine and the gates -- is the asset that survives all of
this, and the venue and instrument decisions above are what it gets pointed at next rather than work to be redone.

### The execution order, once the seven hold

The stages below are the ones a forward reading produces as well, because the shape of a live chain does not
depend on the direction it was derived from; what the filter above changes is which market they run on, and what
it removes from the critical path.

### Stage A — live data and a recorder (one to two weeks)

The repository already says where a live feed fits: the pipeline knows `Strategy`, the engine interface and the
book, and `src/market/` holds one reader per input format, so a live feed is another reader. Concretely:

* a websocket depth-and-trades reader behind the same interface the replay readers use, so the strategy, the
  engine and the portfolio cannot tell a live session from a replayed one;
* a recorder that writes what arrives in the replay shape the readers already take, which is what makes a live
  session replayable and lets the model and the live session be compared on identical input;
* the gate gains an assertion that a recorded file replays to the summary the live run printed, so the live path
  is covered by the same four tiers as everything else.

Two of the three have landed first, because neither needs an account or a data purchase. The delivery half: a
replay can be delivered at the file's own speed or at a multiple of it, and it reports how late each delivery ran
against the schedule the timestamps define, which is the number a live session has to watch. The recorder:
`--record=PREFIX` taps the delivery callback and writes `PREFIX_trades.csv` and `PREFIX_depth.csv` in the shapes
the replay already reads, refusing anything those formats cannot express — a trade with no aggressor side, a batch
whose updates disagree about their local timestamp, a snapshot whose level count changes, or a stream that is both
snapshots and batches at once, which is why the OKX archive cannot be recorded. The gate asserts the claim rather
than describing it: tier 1 of `tools/gate/verify_l2.sh` records the 200k slice, replays the recording, and requires both
runs to report the same numbers. The reader itself has since landed too: `src/market/ws_feed.*` is a websocket
transport behind `IMarketEventReader`, and the `ws` mode is the L2 path with its readers moved onto it. So what
is left of Stage A is not a reader but a **venue** — an adapter that subscribes to one, decodes its messages,
watches the sequence for gaps and reconnects. The route to one, and why it is two steps, is in
[what is left to build by hand](#what-is-left-to-build-by-hand-in-learning-order).

**Passes when** several hours are recorded without a drop and the recorded file reproduces the live summary.
**Ends it when** the depth a retail account may legally subscribe to and read programmatically costs more than
the strategy's gross, or cannot be subscribed at all. That is a licensing question rather than a code one, and
it is one of the two places this chain can stop for reasons outside the repository.

### Stage B — orders, risk and reconciliation (one to two weeks, needs a paper account)

* an order gateway behind the same engine interface, so no strategy code changes: asynchronous submit and
  cancel, partial fills, rejects, timeouts, and idempotent client order ids so a retry cannot double a position;
* a shadow mode that takes live data, runs the strategy and places nothing, recording what it would have done —
  the cheapest way to put the model next to the market for a week;
* hard limits: daily loss, maximum position, maximum working quantity, order rate, duplicate protection, and one
  kill switch that is tested rather than assumed;
* reconciliation of every fill against the broker's positions and cash, which is the check the strategy already
  runs against the engine, extended one layer out;
* operations: one long-lived process, restart recovery that begins by asking what is still working at the
  broker, logs and metrics, alerting, and a written runbook.

**Skeleton, built without an account**: the parts of the four bullets above that a replay can host are in
`PaperBroker` (`src/lines/paper_broker.*`), a decorator over the matching engine that refuses orders,
splits fills, withholds an execution report and takes the venue away for a while, each fault a count of
orders or fills so a faulted run is reproducible from its command line (`--paper-broker` with
`--broker-reject-every=N`, `--broker-partial-every=N`, `--broker-drop-every=N`, and
`--broker-outage-after=N` with `--broker-outage-holds=M`). It keeps its own statement of what the venue
executed, and `reconcile_positions` (`src/lines/reconciliation.*`) compares that against the portfolio,
reported as a `[RECON]` line: net position and gross traded quantity per symbol, the fills the strategy was
sent against the ones it booked, and fees. The client's half of that seam has landed too: `OrderGateway`
(`src/lines/order_gateway.*`) keeps the table of orders from submission to answer, absorbs a retry under the
same client id, and times out an order the venue has not answered, re-sending it under the same id where the
venue's duplicate check makes the retry safe. Not built, because a paper account is what is missing rather
than the code: the transport itself (a socket carrying a venue's own order messages, where the gateway now
models the state rather than the wire), the hard limits and the kill switch as a venue enforces them, shadow
mode, and the operational half. Two things the faults found by being run rather than reasoned about: a cancel resolved
against an order the venue has not been given has to be answered with a `Cancelled` report, or a strategy
that keys on cancel acknowledgements stops quoting altogether — it stopped 51 orders into a 381 order run;
and a fill lost on each side leaves the position exactly where it was, so the reconciliation has to compare
gross traded quantity as well as the net position.

**Passes when** five paper sessions produce no reconciliation break and results inside a tolerance stated
before the stage starts.

### Stage C — minimum size, against real money (two to four weeks)

One share or one contract a side, with a daily table that is the entire point of the stage: live fills, fees,
capture, markout, slippage, reject rate and latency percentiles against what the model predicts for the same
window. Size grows in steps and only while the live column is not worse than the model by more than the
tolerance; three days outside it and the size steps back.

Four account constraints have to be settled first, and none of them is a coding problem: the day-trading and
minimum-equity rules a US equity account is subject to and how they meet a Hong Kong resident's account type,
settlement for a strategy that turns over inventory repeatedly, borrow and locate costs for the short side, and
the tax treatment of frequent trading, which here means no capital gains tax and a business income question
answered case by case.

**Passes when** ten sessions are net positive with live fees and capture inside the model's tolerance.

### Stage D — a measured definition of stable, then scale (months)

Stable is a number or it is a feeling. Over a rolling twenty-session window, all five: net positive with at
least fifteen positive sessions; live fees and capture inside the model tolerance; drawdown under a stated
ceiling; no reconciliation break and at most one outage; availability above 99%.

The loop that keeps it honest: every week the offline pipeline re-runs on the new data and re-derives the
recorded numbers; every quarter the candidate screen runs again, because crowding and spreads move; and any
parameter change needs out-of-sample evidence before it ships. Only then does size grow — same symbol, then
another symbol, then another market. The micro E-mini stays a candidate rather than the plan, because what
failed there was the capture and not the fee.

### What is left to build by hand, in learning order

The stages above are the plan from a positive window to a running system. Read as a learning path instead — no
profit goal, and no need for the statistical rigor the seven gates impose — the same material is a coverage
checklist. The engine already speaks historical data, the L2 book, the queue, the fee, the market-making line,
several instruments against one account, funding, the paper broker, reconciliation and the recorder. What has not been built
here by hand is the other half of a trading system, and each row points the existing book and engine at a new
workload rather than rewriting them:

**Status note.** This is the learning-path view; status and priority live in the current plan at the top.

| To build by hand | What it teaches |
|---|---|
| the order gateway and the order lifecycle behind it (Stage B) | landed: submit, cancel, partial fills, rejects and timeouts; the remaining item is timeout/query reconciliation (P1) |
| a strategy that is not market making | signal, then target position, then risk, then exit — landed as `trend_l2`: an EWMA cross, clips at the touch, a stop, a time stop, a cooldown and an exit band, on the shared pre-trade gates |
| live ingestion pointed at a real venue (Stage A) | the venue protocol, subscription, heartbeat, gap detection and reconnect |
| multi-instrument accounting and a portfolio layer | from one instrument to a portfolio and a risk budget -- landed as the two-book run: a pipeline per symbol, one account, and gross exposure against net |
| persistence and recovery | how a long-lived process keeps its state and asks the venue what is still working -- landed as the session snapshot: `--snapshot-out` writes the account and the orders, `--snapshot-in` restores the account and resolves each inherited order against the venue |

**How the live reader reaches a venue, in two steps.** The venues whose data this repository already models
publish live data over `wss://`, and the transport here speaks plaintext `ws://` with neither TLS nor a JSON
library in the tree. The first step is therefore a small Python relay, standard library only, that opens the
secure connection, subscribes, decodes the venue's messages into the replay CSV rows this repository already
reads, and re-serves them as plaintext `ws://` on localhost for the C++ feed to read -- which it already can, so
the engine is untouched and the relay is the only thing that knows the venue. It landed as
`tools/live/venue_relay.py`, standard library only, with three roles that share one decoder: it relays the venue to
the engine, it writes the decoded rows straight to `PREFIX_trades.csv` and `PREFIX_depth.csv` (the direct route,
with no engine), and it captures the venue's raw messages so a window runs again with no network. It subscribes,
decodes, keeps its own sequence accounting -- a break in the venue's `prevSeqId`/`seqId` pair is a lost update
and is counted -- and reconnects with a backoff after a drop, which the snapshot the venue re-sends on subscribe
makes safe for the book. It answers both heartbeat forms a venue can send, a text `ping` and a control ping,
which a six second run never exercises, and it bounds its hand-off to the engine: a full queue waits rather than
growing, and the wait and its high-water mark are printed with the summary. Against OKX's public `BTC-USDT-SWAP` channel a six second run handed the engine 124
trades and 60 book messages, 4807 level rows, with **no sequence gap** and no reconnect; a six second direct
recording wrote 205 trades and 4301 level rows with the same zero gap and replayed through `l2_replay` as a file.
The second step landed too: `venue_relay` (C++, in `src/lines/`) is the same tool with the same flags and the
same rows, so the two are interchangeable in front of the engine. Its decoder was checked against the Python
one on one capture and the two recorded files are byte for byte identical, and a C++ relay serving a capture to
the `ws` engine produced the same event counts. TLS is a build option: with OpenSSL's headers CMake links it and
a `wss://` venue is spoken directly, and without them the C++ relay speaks `ws://` while the Python relay stays
as the TLS terminator, which is why both are kept. OKX is the lowest-friction first target, because its public
channels need no key and its historical endpoint carries no order book.

**A thirty-minute live soak, and the replay that matches it.** The relay served OKX's public
`BTC-USDT-SWAP` channel to the engine, with the paper broker and both recorders on, for 1,800 seconds:
697,400 frames, 6,719 trades and 690,679 depth updates in 17,690 batches, with **0 sequence gaps, 0
reconnects, 0 parse errors and 0 ring waits**, and both processes' RSS flat from the first minute (relay
26.7 to 26.8 MB, engine 7.1 to 7.15 MB). The hand-off queued at most 1,159 rows of its 200,000, so
backpressure never engaged. Replaying the engine's own recording through `l2_replay` reproduced the live
summary field for field -- 323 orders, 6 fills, the same P&L and fees -- and the broker's reconciliation
stayed `status=ok` in both. The heartbeat counters read zero because the stream never went idle for 25
seconds, so that path is verified against a local stand-in that sends both a text ping and a control ping,
not against the venue.

The relay carries two hooks for exactly that: `--ping-seconds=N` sends a ping on a fixed cadence even while
data flows (0 keeps the idle-only keepalive), and `--drop-after-seconds=N` deliberately closes and reopens
the venue connection, so the reconnect path is exercised against the real venue rather than only against a
stand-in. A local stand-in that accepts twice and replies to both pings shows two connections, two pings,
two pongs received and two snapshots, which is the shape the long run has to reproduce against OKX.

**The two-hour run found a deadlock, and the bound that fixes it.** With a forced drop every 25 minutes, the
run stopped at the second drop: the engine's recorded rows froze while the relay kept producing and the
process spun at one core. The cause is the merge between the two blocking readers. It waits for the next
trade while depth keeps arriving, so a trade gap longer than the depth ring fills the ring and the receive
thread blocks on a push the merge cannot drain. The numbers line up exactly: 384 depth rows a second against
4,096 slots is 10.7 seconds, and the longest trade gap measured here is 10.2 seconds in thirty minutes and
15.7 seconds in the longer recording. The live input rings are now 131,072 rows -- about 5.7 minutes of
depth -- and the feed reports `ring_high_water`, so the margin is a measurement. The live feed now delivers
one ordered stream: the receive thread assembles a depth run, holds a trade that arrived during it, and pushes
both in order, so there is no second blocking reader for a trade gap to stall. The input ring holds 131,072
rows as a margin and reports its high-water; over two hours that was 3,917 against the old 4,096, which is why
the two-reader version could stop exactly there.

The rerun completed the two hours: 5,704,944 frames, 144,755 trades and 5,560,187 depth updates, with
**four forced reconnects and zero sequence gaps**, 355 pings answered by 355 pongs, `ring_waits=0`, and a
ring high-water of **3,917 rows** -- 179 below the old 4,096 capacity, which is the measurement that says
the old bound was not a theoretical worry. `incremental_snapshot_batches=5` says the book resynced on the
initial snapshot and on each of the four reconnects, and replaying the engine's own recording reproduced the
live summary field for field. One strategy edge is visible in the same run: `active_l2` shrank some quotes
to 5,000 units, below the instrument's 10,000-unit minimum, so the engine refused them; the reconciliation
stayed `ok` and the network result is unaffected, but a clean strategy run should set `--order-size` at or
above the minimum.

**Why UDP is not part of this yet.** The only UDP in the tree is the legacy receiver in
`src/lines/legacy/udp_feed.*`, and it is a receiver for the frozen `Tick` line: one CSV row per datagram, no
sequence, no gap detection, no recovery, and a datagram dropped silently when its ring is full. A live
`MarketEvent` feed over UDP is a different thing -- an exchange-native multicast feed needs a licensed or
colocated source, a sequenced packet header, A/B line arbitration, and a second channel that fills a gap from
a snapshot or a TCP replay. This demo has none of those, and inventing them against a public venue would
model something the account cannot legally receive. So the two representative chains stay on the transports
they can actually get: `wss://` for the push chain and files for the pull chain. A UDP chain, if it is ever
wanted, is a third, simulated one with its own header, gap detection and recovery, not a promotion of the
legacy receiver.

**The two chains, and the order of the next iterations.** The push chain is a venue's public stream, decoded
by the relay and read by `WebSocketFeed`; the pull chain is a day file read by the venue readers. Both end at
the same book, strategy, engine and portfolio, and the recorder is the bridge: a push session written back to
a replay CSV and replayed by the pull chain has to report the same numbers. The cheapest order is to close
that loop before broadening the venue list, because it reuses files already on disk and code already written.

The pull chain needs no phase of its own. It is the historical path the gate and the eight closed markets
already run on, and the recorder already proves the two chains meet: a push session written back to a replay
CSV and replayed by `l2_replay` reports the same numbers. What is left is new capability rather than more
proof, so the order below is by cost and by how much of the system each one adds.

1. **A second venue decoder.** Binance's public streams (`@depth`, `@aggTrade`) are a different JSON shape
   and a different sequence pair (`U`/`u`), which is the cheapest way to prove the decoder seam is real
   rather than an OKX-shaped special case.
2. **Multi-instrument, the one structural change.** The relay's instrument argument becomes a list on a single
   connection -- one socket and one subscription, because a connection per symbol is what rate limits punish
   -- and the engine grows from one `L2OrderBook` to one per symbol. The book is the small part: the position,
   the strategy state, the matching engine's queue bookkeeping and the portfolio are per-symbol too, so this
   is a refactor of the pipeline rather than a map in the feed. Rows route by their `symbol` column (the
   second, not the first), and an instrument with no `InstrumentSpec` still refuses the run. It is worth doing
   only when a cross-instrument use case -- a hedge, an arb -- is what the demo is for; without one it is a
   refactor that buys no new behaviour.
3. **The simulated UDP chain, only if it is wanted.** A `MarketEvent` feed with a sequenced header, gap
   detection and a snapshot recovery hook, plus a local sender that drops and reorders datagrams so the
   recovery is exercised. It teaches loss, ordering and backpressure; it is not a route any account here can
   trade on.

If the next goal is a fuller trading system rather than more ingestion, the order gateway and persistence and
recovery in the table above come before multi-instrument: they add a part of the system that no file or
socket already exercises.

**Not planned, and why.** Kernel bypass (`DPDK`, `SO_BUSY_POLL`), `O_DIRECT` and DTLS are production answers
to a licensed multicast feed this demo does not have; the recorder's buffered stream and the relay's
background socket are enough for the volumes a public stream produces. If a capture ever drops a row, the
first fix is a writer thread behind a queue, not direct I/O.

**Deferred technical bets.** The five external suggestions are folded into the current plan rather than kept
as a separate checklist: the risk pipeline landed; the other four are deferred with the trigger that would
make them worth doing.

| Bet | Status | Trigger |
|---|---|---|
| block/SIMD parse | P2 | one bounded 2M-row experiment, byte-identical, rejected if the p50 improvement is below 10 percent |
| configuration hot reload | P3 | live tuning exists; immutable snapshot behind an atomic pointer or a seqlock |
| shared-memory IPC | P3 | C++ relay/TLS first; then a measured IPC share of the event |
| binary telemetry | P3 | a few thousand orders a second; CSV remains the audit format |

### The same code, second purpose: the instrument and a live path

Everything below is live-path work or a measurement of it, so the instrument and a running system share one codebase
rather than paying for two. What the repository already has — integer prices, one parse per row, a jump table over
columns, four updates inline before any allocation, four queue models with a calibration behind them, and a four-tier
gate whose reference hash has to be justified when it moves — is what a live path would build on. The list is what a
live path adds on top.

| Iteration | C++ and low-latency surface | What it demonstrates |
|---|---|---|
| the live reader and the recorder | epoll or a busy poll socket, `std::span`, zero copy parsing, and the same parser the replay path uses | one strategy driving a replay and a live session, with the live session replayable |
| handing events between threads | a single producer lock free queue, `std::atomic` memory ordering, `alignas` against false sharing, batched wakeups | the classic low-latency question, and a live requirement rather than a decoration |
| latency reporting | `std::chrono::steady_clock`, histograms, percentiles per stage from feed to decision to order | low latency as a number rather than an adjective |
| optimisation evidence | `perf stat` instruction and cache counters, the existing libc call counter, and the documented rejection of `std::pmr` | a choice with a stated alternative beats a feature list |
| the gate itself | four tiers, a re-derived reference hash, a prose lint, and sanitizers | a repository where a documented number cannot drift |

Five small deliverables come out of it: a paper session whose recording replays to the same summary; a
feed-to-order latency table with percentiles; a benchmark of the inter-thread queue; a before and after `perf`
table for one optimisation; and a page of stated alternatives, in the style the design review already uses.
Four of the five are now in place for the replay path rather than promised: a recording replays to the same
summary; a feed thread hands events to the strategy thread over a ring whose own cost is printed as a
`[LATENCY]` line with percentiles; that hand-off is benchmarked in isolation by
`src/bench/ring_bench_main.cpp`; and the alternatives refused on the way — a dropped event instead of
backpressure, a condition variable instead of a spin, one profiler shared by two threads, an 8 slot ring
instead of 4096 — are stated where they are refused.

The benchmark measured the hand-off at 18.3 ns/message for a trivial payload and 169.9 for the event this path
carries, 1.8x the mutex and condition variable alternative, with the event's own move and destructor as the
dominant term. The table is in
[the performance reference](../PERFORMANCE.md#39-the-hand-off-between-two-threads), and the part of it no gate
asserts is exactly the part that cannot be: a time. The transport has since landed as `src/market/ws_feed.*`, so
what is left of the live reader is the venue adapter rather than the pipe, and the deliverable still outstanding
is the before and after `perf` table for one optimisation.

### What has to be done by hand

Everything here is outside a repository: an account, a licence acceptance, a download behind a login, or a
decision about money. It is written down so that code work never waits on a question nobody asked.

* **Channel answers**, before any live plumbing: whether the broker will pin an order to a named destination;
  what a depth subscription costs and at how many levels, remembering that a Level 2 service does not include
  Level 1; whether the account is subject to day trading minimum equity; borrow availability for the short side;
  and the tax treatment of frequent trading.
* **The choice between futures and shares**, which follows from those answers rather than from preference, and
  which decides the symbol screen that everything else is measured on.
* **Downloads behind a login**: none, now that the IEX route is rejected for the reasons in the data table above;
  the Databento side is already scripted and needs only the key in the environment and a cap.
* **Money and a paper account**: a funded account for the minimum size stage and a paper account for the
  reconciliation stage.
* **The two decisions only the owner can make**: how much capital, and whether the offshore venues stay a
  measurement bench or are dropped entirely.

### What would stop the chain, in the order it is likely to

| Breakpoint | Why it sits where it does |
|---|---|
| **One venue's book is not the market** | gate 3 measures it, and it is the largest single unknown in the positive row |
| **Retail latency** | Already measured on a perpetual: twenty milliseconds of cancel latency turned 0.095 ticks of capture into -0.44. If a US book behaves the same way, the quoting line is not a personal account's game |
| **Account rules** | Day-trading minimums, settlement and borrow decide whether the strategy may be run at all, before any engineering |
| **Depth data licensing** | Decides whether the live path can exist, and the repository cannot answer it |
| **Capacity** | The positive window made 262.508 USD in two hours on about 150000 USD of quotes and a peak of 841164 USD of exposure. Twenty to forty times that size is what an income would take, and at that size the fill model is gone |

### What this plan does not claim

The costs and probabilities below are the planner's stated priors, not measurements, and they are written down
so the plan can be argued with rather than trusted. The rule the rest of this document follows applies here too:
a window that is positive is not an edge, and a plan that ends in real money is worth nothing until its first
stage has tried to kill its reason for existing.

| Stage | Calendar | Cost | Prior of passing |
|---|---|---|---|
| the instrument screen, offline | two to three days | 20 to 50 USD | a symbol whose half spread is three times its fee is found about three times in five |
| the seven gates, offline | one to two days | under 40 USD of data, plus a few channel answers | gate 6 has already failed on the window it was aimed at, so the rest now decide whether anything is left of the line |
| Stage A, live data and recorder | one to two weeks | 0 to 30 USD a month | engineering succeeds about nine times in ten; the licensing question is the risk |
| Stage B, orders, risk and reconciliation | one to two weeks | a paper account | engineering succeeds about four times in five |
| Stage C, minimum size | two to four weeks | capital, and the account rules decide how much | live reproduces the offline result roughly a third to a half of the time |
| Stage D, stable then scale | three to six months | ongoing | the quoting line reaching stable positive: about one in six; the slow line, which does not depend on speed, is the better bet |



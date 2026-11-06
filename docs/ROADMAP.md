# Roadmap

The product of this repository is evidence: eight markets closed by numbers, with the [Retrospective](RETROSPECTIVE.md)
as the closing document and the [Design Review](DESIGN_REVIEW.md) as the argument for the instrument that produced them.
**This document is the plan for what is still worth building, and the boundary that stops it from expanding** — the demo
that shows the engineering is complete, a personal execution system is one possible request away rather than a
backlog item, and everything deferred is recorded here with the reason it was deferred, so that "not now" is a decision
rather than an omission.

The seam that makes more possible already exists: `Strategy` and `IMatchingEngine` are the only interfaces the pipeline
knows about, and `src/market/` holds one reader per input format. A live feed is another reader, and paper trading is
the existing book and matching engine against that feed.

## Where the project stands

| | |
|---|---|
| Delivered | the queue model, the fee model, the two latency legs, the order trace and the queue distribution, four sweeps of the strategy levers, a four-tier gate that re-derives every quoted number, the interval a mean carries, and a three-command demo path |
| The binding result | at a one-tick spread the fee is 2.57 ticks against 0.088 captured, and no lever moves that ratio |
| Closed | eight markets, each by a quotient; the inputs and the binding gate of each are in `tests/fixtures/verdicts.json` and re-derived by `tools/study/viability.py` on every gate run |
| Frozen | the pre-queue-model line is tagged `v1.0-demo` |
| Open | the conclusions rest on one day per window, and nothing has been run against a venue's own live feed; the paced replay, the recorder and the websocket reader are in place, so the seam is closed at the desk and what remains is a credential. Phase 2b is the plan from the first positive window to a running system |
| Stopped | the per-fill PnL column, a latency budget in the gate, an id that survives a restart and a fuzz tier — recorded with their verdicts below, not scheduled |

## Current iteration plan (maintained)

This is the one plan for the next iterations. It is a project-quality plan: cover the technical parts of a
trading system in a small, readable implementation, without requiring production connectivity or statistical
rigor, and stop at a stated boundary. Every deferred item is a **current scope boundary**, not a permanent
no: a later request can lift it, and the change is recorded here.

**Maintenance.** At the end of every iteration, update the statuses, the change log and the stop line in this
section, and report the diff in the chat. Do not create another planning document; this section plus the
existing design, architecture and user documents are the whole plan.

### Coverage at a glance

| Part of a trading system | State | Next useful increment | Priority |
|---|---|---|---|
| market data: replay, recorder, local websocket | done | none for the demo | done |
| L2 book, queue models, fees, latency | done; participation cap added | price impact remains out of scope | done |
| matching, partial fills, self-trade prevention, IOC/FOK | done; the venue also refuses an order that would trade on arrival when the run asked for add-liquidity-only | none: cancel/replace is **refused with a reason** under item 11, not deferred | done |
| strategy framework and market making | done | none | done |
| non-market-making line | `trend_l2`, its 27-cell grid, and the two-window scale test that closes the taker variant by non-replication | nothing while the closure stands; a new signal is a new row | closed |
| execution algorithms | `execution_l2` with three schedules, two entry modes and parent-level shortfall against both the arrival price and the window's TWAP; the schedule wins at most half a basis point, posting loses the fills it saves, and size moves the number of crossings but not the cost | nothing while the closure stands; a venue with a materially different fee would be a new row | closed |
| pre-trade risk | done: switch, fat finger, rate, exposure, position; the exposure cap is the **account's** when several books share one, so two books cannot each spend it | none | done |
| order state | done: Pending/Resting/PartiallyFilled/Filled/Cancelled/Rejected, reasons, pending release, IOC/FOK terminals; the venue also refuses an add-liquidity-only order that would trade on arrival | cancel/replace, for the reason recorded under item 11 | done |
| portfolio and PnL | several instruments in one run against one account: a book, a decision layer, a venue and a pipeline per symbol, per-instrument valuation, and `[EXPOSURE]` reporting gross against net; a book prints its own `[PNL] symbol=`, and `--close-out` prices the exit so a "net" figure that carries a position can be read as the mark it is | the maker comparison needs a larger fill count than the free days give; that is data, not code | done |
| broker and gateway simulation | paper broker plus gateway faults and timeout queries, and idempotent client order ids: a re-send under an id the venue has already seen is refused rather than filled, so a retry cannot become a second position | an id that survives a client *restart*, which is the snapshot's `next_order_id` and is a different mechanism | done |
| persistence and recovery | a session is written down and taken over: the account, the positions with their entries, the marks, the working orders and the clock, **one record per book** in a two-symbol run; a resumed run restores the account and asks the venue about each order it inherited | a venue that remembers its own fills across a client restart, which is the durable-venue line and not a persistence feature | done |
| live venue, TLS and authentication | missing; current boundary | out of the demo; lift by request | P3 |
| market impact and realistic taker fills | partial: capacity cap only; there is still no price-impact model | none inside the current boundary | done |
| out-of-sample, Sharpe and capacity | the Sharpe, volatility, drawdown and Calmar figures are measured on demand behind `--metrics`; the out-of-sample and capacity studies are still not a demo goal | the studies, not the measurement, are out of the demo | out |
| documents and the maintained plan | done: one plan, no new documents | keep it current; no interview material required | done |

### Ordered backlog

**P0 — documentation consolidation (done; no interview material).**

1. Delete `PROJECT_BRIEF.md` and the separate decision-log file; make this section the only plan.
2. Make `index.md` the map of five core documents, and add no new planning document.
3. No walkthrough or interview Q&A is planned; the project itself is the deliverable.

**P1 — current iteration (complete).**

5. **Order timeout and query visibility — done.** `OrderQuery` lets the gateway ask whether the venue still
   holds a timed-out order; `[GATEWAY]` reports `unacknowledged`, `timeouts_queried`, `timeouts_working` and
   `timeouts_unknown`. Neither query result is a fill or a cancel; the ledger stays `Pending` until a report.
6. **Participation cap only — done.** `--participation-limit=X` caps one aggressive external-BBO fill at X
   of the displayed best size, leaving the rest to rest or to the order's time in force. The execution price
   is still the touch; there is deliberately no price-impact model.
7. **IOC/FOK, not cancel/replace — done.** `TimeInForce` is carried by `StrategyOrder` and `exchange::Order`;
   an IOC cancels its immediate remainder with `CancelReason::Ioc`, and a FOK checks the external touch and
   the internal book first, then either fills completely or cancels with `CancelReason::Fok` without trading.
   `--time-in-force=gtc|ioc|fok` applies one setting to every strategy order. Cancel/replace stays deferred:
   it needs old/new id mapping, queue-priority reset, and a replace race.
8. **Demo path — done.** The ordered command list and expected lines are in the
   [User Guide](using/01-getting-started.md#a-short-demo-path) `verify_l2.sh fast`, `risk_check.sh`, then a one-cell
   `trend_sweep.sh`. It uses the existing scripts, adds no strategy, and states that the negative trend PnL
   is the measured result rather than a demo failure.

**P2 — only after P1, or when the owner asks.**

9. Multi-instrument, portfolio and netting. **Landed:** the account keys positions by symbol and reports
   `[EXPOSURE]` — one leg per symbol with its signed notional, the sum of the sizes, the size of the sum and
   their ratio, valued at the last quote each symbol showed and without moving the account. A two-leg book that
   nearly cancels reports a gross of twice its net, which is the number a hedged book is judged by and the one
   the carry line's fee bill was charged on. **And a run that trades two books at once:** `--symbol-b`,
   `--trades-b` and `--depth-b` give an `l2_replay` a second instrument, so the run builds a book, a decision
   layer, a venue and a pipeline per symbol against one `Portfolio`, merges the four readers by timestamp
   (`src/market/merged_event_reader.*`), and reports one `[EXECUTION] ... symbol=` line, one `[PNL] symbol=` line
   and one `[BOOK]` line per book, one `[PORTFOLIO]`, and one `[EXPOSURE]` over both legs. The per-book `[PNL]` is
   what says which leg lost the money: on the measured window the long BTC book lost 0.033 USD closed, at a
   break-even maker rate of −0.00025, and the short ETH book made 0.005 — and the legs add up to the account exactly,
   because the split is taken where the account's totals are written rather than recomputed from them. The account
   values each symbol with its own
   instrument (`Portfolio::set_instrument`), because a ten dollar contract face and a one dollar one are not the
   same exposure — without that the ETH leg is priced ten times too large. Measured on 2020-04-01, 200k depth
   rows per instrument: 377,024 events, one contract long in BTC and one short in ETH, `gross_usd=11` against
   `net_usd=9`, `fees_usd=0.0202` (the sum of both books' fees). The gate re-derives it on every run.
   **Still missing, each refused with a second book rather than half-applied:** the broker and the gateway (they
   wrap one venue), `--metrics` (one account's equity series sampled by two pipelines), `--parent` (one arrival
   price per line), `--record` (the replay shapes hold one book), and `--funding-rates` (one history per flag).
   A third book is a change to the two-source array and to the flags, not to the design.
   **The cap followed the account.** `--max-exposure-usd` used to be each book's own ceiling, so two books could
   each carry the full amount against the same cash and both report no violation. Each book is now told what the
   other legs already hold (`Pipeline::set_account_marks`), and the check adds it to this book's projection
   through a new `RiskContext` field, which is zero in a single-book run and therefore leaves every recorded
   number where it was. On the same two books with a ten dollar cap: the BTC book fills to ten, the ETH book is
   refused every quote because its own dollar would be the eleventh, and `[EXPOSURE]` reports `symbols=1`. The
   gate re-derives that too.
10. Persistence and restart recovery. **Landed:** a session can be written down and taken over,
   against a venue that outlives the client. `--snapshot-out=FILE` writes the account (cash, realized PnL,
   fees and both trade counts), every position with its entry price and the fee riding on it, the last
   midpoint each symbol showed, the orders the ledger still holds, and the clock and next order id.
   `--snapshot-in=FILE` restores the account, then asks the **venue** about every order the snapshot listed:
   a live one is cancelled (a queue position a new process did not stand in is not one it can keep), and a
   forgotten one is counted rather than assumed to be filled or cancelled, because the venue's query cannot
   tell those apart. The run then replays its file from the first line — the book has to be the one the
   snapshot was taken against — and holds its own orders until the snapshot's clock, so the market is
   rebuilt and the decisions are not repeated. Measured on the 200k slice: the resumed run reports
   `[RESUME] ... cash_usd=989.993 legs=1 next_order_id=382` and then a `[PORTFOLIO]` and `[EXPOSURE]`
   identical to the run it took over, field for field; the gate re-derives that on every run.
   **What it deliberately does not do:** restore the *strategy*. A new process starts with the account it was
   given and no memory of its own quotes, and the first fill after a resume reports it through
   `strategy_position_mismatches` rather than being silently absorbed. Adoption — taking a live order over
   instead of cancelling it — would need a queue position nobody can reconstruct, so it is refused by
   design. **One record per book:** the snapshot writes the account once and each book's own clock, id sequence and
   working orders, so a two-symbol run resumes both legs at their own venues; the single-book format written first
   is still read, because a restart reads a file an older build left behind. **Still missing:** a venue that
   remembers its own fills across a client restart (the simulator's engine is created per run, so a resumed run
   compares against nothing; the test uses one broker for both halves to show a fill missed while down being
   *caught* by `reconcile_positions`).
11. Full venue semantics. **First quarter landed: add-liquidity-only.** `--post-only` makes the venue refuse an
    order that would trade on arrival: it is cancelled with reason `post_only` rather than filled, because a quote
    that arrives after the market went through it is a maker paying the taker rate. The check is the one FOK
    already used, so a venue that refuses to cross cannot disagree with the venue that fills. Measured on the
    200k window: `active_l2` is unchanged, because none of its quotes arrived crossed; `trend_l2` clips at the
    touch, so **all 55 of its fills were taker fills** (taker fees 0.275 USD against maker fees 0) and with the flag
    it makes 1,814 crossing attempts, all cancelled, and trades nothing. That is the reversion line's cost explained
    one level down: a clip at the touch is not a quote, it is a market order.
    **Cancel/replace is deliberately not built**, and the reason is in the engine: the queue an order joins is
    reconstructed from the displayed size at the moment it joins and then shrinks with the display, so an amend at
    the same price lands behind the same queue a re-entry would. Its only measurable gain is one round trip instead
    of two and no window in which the quote is absent, and those need the latency model (`--order-latency-us`,
    `--cancel-latency-us`) to price; a version that reports "amends saved nothing" would be a claim about a lever
    this engine does not have.
    **Still deferred (P3, out of the demo):** live venue/TLS/authentication, a price-impact model, out-of-sample
    studies, and the performance experiments (block parsing, shared memory, binary telemetry, configuration hot
    reload).

**P3 — current boundaries, not permanent.**

Everything in P2 is deferred for this stage. Lifting one requires a new request; add it as a new row here
rather than pretending the boundary was never there.

### Gaps that were on no list (recorded 2026-11-02)

Everything above was chosen and written down. The rows below were **not**: they were absent from this plan, from
the Design Review's "what it deliberately does not do", and from the retro's list of closed lines. Several of them
are gaps in the *evidence* rather than in the simulator, which is why they matter more than their size suggests: a
claim can be honestly produced and still be under-powered.

| Gap | Why it matters | Verdict |
|---|---|---|
| **No interval on any number** | Almost every closed line rests on a point estimate over few fills. The rule "fill counts below thirty are not conclusions" was applied by hand, one row at a time; nothing computed a standard error and no line printed one, and the maker break-even gap between BTC and ETH was quoted as −5.65 against +1.26 bps from one window each | **done**: `tools/study/pnl_interval.py` (a bootstrap over a series, a window mode that replays the windows itself, a self-test in the tier-1 tool block) and a five-month series in `docs/PERFORMANCE_HISTORY.md`, where the mean is −1.87 bps with an interval of −2.84 to −0.90 and the five draws span 2.75 -- so the month moves one instrument by 40% of the 6.9 bps gap. Still open, and recorded as such: a **per-fill** series does not exist on disk (the trade report carries price, quantity, side, fee and role), so adding a PnL column to it is the remaining half |
| **The strongest gate tier never runs** | `tools/gate/verify_l2.sh full` compares a 1.5 GB replay's output against a stored hash, and the reference lived in `/tmp`, so in a clean checkout it printed `SKIP (no reference yet)`. The Design Review's claim that tier 2 "rules out every explanation except cost" was therefore true of the script and not of the tree. **Fixed in this session:** the hash drops the mode line, which carries absolute paths, and the reference is kept at `tests/fixtures/l2_full_ref.sha256`, so the tier runs and passes on a checkout | **done** |
| **No calendar and no contract lifecycle** | The engine has no opening or closing auction, no halt, no half day, and no expiry or roll: a future position never expires and a perp's funding is only applied when a file is named. Everything measured here is a continuous event stream, which is exactly what a crypto perpetual is and is not what an equity or a future is | **record only**: the corpus is crypto and the equity rows are measured from daily bars, so closing this would be a different data line rather than an extension |
| **"Portfolio" means several books sharing a wallet** | There is no covariance, no factor model, no beta, no cross-sectional ranking and no margin: `[EXPOSURE]` sums signed notional in USD and that is the whole risk model. Two books are two independent books, not a hedged portfolio with an optimum | **record only**: a risk model needs a return history the free days do not provide, and any number it produced would be a model of a model |
| **The kill switch stops quoting; it does not flatten** | A run at its loss limit stopped sending orders and kept whatever it was holding, so the control that exists to cap loss left the exposure open until the input ended. Two things had to be fixed and only one was obvious: the switch refused *every* order, including the one that would take the position down, and the account was never marked during a run, so `unrealised_pnl` stayed at zero and the loss it reads sat inside the position instead of in its input. **Fixed in this session:** the switch exits the position (narrower than `is_reducing`, because a flip is not an exit), the account is marked each cycle while it is armed, and the halt is priced — 5.39 bps on the measured window, with `[EXPOSURE] symbols=0` afterwards | **done** |
| **The venue is a process, not a service** | Client and venue share one process and one clock, so a restart can never miss a fill and a reconnect never loses a sequence number. This is why the venue is not given its own persisted statement: it would only prove the simulator agrees with itself | **deliberately not** (P3 by nature): a durable venue is the live-venue line, not a persistence feature |
| **No adversarial input for the readers** | Four venue formats are parsed by hand-written readers, and the tests fed them fixed files in the shape the venue publishes. Nothing asserted what happens to a row that is truncated, that says `nan`, that carries a negative quantity or a side in the wrong case -- so "the reader rejects bad input" was a claim about code nobody had run against bad input. The readers turned out to be stricter than the test suite knew (they refused all of it), but four things were missing: the messages named no row, a `nan` price was refused by the *validator* rather than by the reader that could locate it, a crossed incremental batch was skipped without being counted, and an empty depth file produced a formatted run of zeros with exit code 0 | **done**: `tests/malformed_input_test.cpp` (hostile rows per family plus the tolerant shapes that must still read), `LineReader::lines_read()` for row numbers, `csv_fields::parse_price`, `crossed_book_batches` on `[L2 DATA]`, and both silent-zero paths refused |
| **No latency budget in the gate** | `[LATENCY]` and the stage profiler report numbers, and `ring_bench` reports a throughput figure, but nothing fails when any of them regresses by a factor. A performance claim in a document is re-derived only if someone re-runs it | **later**: an assertion needs a stable machine, and this project has one that changes |
| **The recorded L2 windows mix two time spans** | A 200k-row depth slice is a quarter of an hour of book against a whole day of prints, so 117,889 of the 119,237 prints arrive with a midpoint older than a second. No fill happens there -- the last one is 1.2 seconds before the depth ends -- so the measurement stands, but the window is not a window of one market | **record only**, and now printed: `stale_trades` on `[L2 DATA]` says which case a run is in. Cutting both inputs to the same span would re-derive every number in `docs/PERFORMANCE_HISTORY.md` to a different window, which is a new study rather than a fix |
| **The two paths disagree about a print far from the mid** | On the L1 replay a print more than five basis points from the best bid and offer is **dropped** before the strategy or the venue sees it (`filtered_market_trades`), on the reasoning that a BBO sitting that far from a print is the one that is stale. The L2 path **counts** the same rule and delivers the print anyway, because a book is a better reference than a BBO and a print far from it is a sweep rather than a bad tick. Two rules, one phrase in the code | **record only**: unifying them means choosing one of the two behaviours for both paths, and either choice moves every recorded fill on the path that changes -- the count is the honest gain, and it is printed |
| **No machine-readable record of which run produced which number** | The marked blocks re-run a command and compare fields, which catches a number that moved; nothing records the commit that produced a historical figure, and the change log does that by hand | **deliberately not**: the documents and their blocks are the register, and a second one would be maintained by hand too |

### Stop line

Stop when the P1 items that buy a new technical point are done. The current iteration is complete when:

1. `ctest` is 31/31 and `tools/gate/verify_l2.sh fast` is tier 0 and tier 1 OK;
2. one L2 event, one risk refusal/backoff and one order-state path can be traced end to end;
3. one short replay and `tools/sweep/risk_check.sh` can be shown if asked, without claiming a profitable strategy;
4. this plan and the five core documents still describe the binary;
5. no known bug on the demo path is hidden.

**Not accepted during the freeze:** a new strategy, venue, risk control, performance experiment or document.
Accepted: a bug fix, a test or explanation of existing behaviour, or a demo script.

**Lifted on 2026-10-30, for one requested item.** The freeze was there to stop the market-making line from
expanding without bound; both it and the signal line are now closed by measurement, so the boundary moves rather
than disappears. The request was an execution scheduler: `execution_l2` works one parent order and takes no view
on direction, so it is a new strategy name without being a new claim about the market. Everything else stays
frozen, and a next item needs the same kind of request and a row here.

**The owner's stop, recorded 2026-11-02.** Four items were candidates and are not being built: a per-fill PnL column
in the trade report (the remaining half of the interval work), a latency budget asserted in the gate, a client order
id that survives a process restart, and a randomized fuzz tier for the readers. Each has its verdict in the gaps
table or in the item that named it; none is scheduled, and lifting one takes a new request like any other row.

### Change log

**Scope decisions only.** What changed in the code, release by release, is
[CHANGELOG.md](https://github.com/12mango/ToyQuant/blob/main/CHANGELOG.md) — this table
records the decisions about what the project will and will not build, which is the thing a plan has to remember. A row
here that also appears there is the same event seen twice: once as work, once as scope.

| Date | Change | Commit |
|---|---|---|
| 2026-10-28 | Consolidated the plan here; removed `PROJECT_BRIEF` and the separate decision log; reset the boundary to "deferred, can be lifted" | `2d23d5d` |
| 2026-10-28 | Removed the interview walkthrough/Q&A from P0; the plan is project-quality only | `2078958` |
| 2026-10-28 | P1-1 done: gateway timeout query/reconciliation; P1-2 is a participation cap only; P1-3 is IOC/FOK, cancel/replace deferred | this commit |
| 2026-10-28 | P1-2 and P1-3 done: external-BBO participation cap, `TimeInForce` on orders, IOC/FOK terminal cancels, `--participation-limit` and `--time-in-force` | `f832873` |
| 2026-10-28 | P1-4 done: the three-command demo path and expected lines in the User Guide; P1 is complete | `95d3216` |
| 2026-10-29 | Internal refactor: one side-generic matcher, one external-touch predicate, and a single-order submission step; no behavior change and gate green | `87be6ba` |
| 2026-10-29 | Internal refactor: report fan-out named as `handle_report`; quote observation separated from generic order submission | `b05898e` |
| 2026-10-29 | Internal refactor: named `handle_event` overloads replace the `if constexpr` market-event chain | `7ba3927` |
| 2026-10-29 | Internal refactor: fill accounting extracted from `handle_report` into `apply_market_maker_fill` | `3ac6994` |
| 2026-10-29 | Internal refactor: fill state moved into `FillAccounting`; Pipeline now reads position/reservations/counters through it | `0526993` |
| 2026-10-30 | Internal refactor: shared event clock/mid/queue helpers, one risk-context builder and one order-settlement step; no behavior change | `96f652f` |
| 2026-10-30 | Metrics batch 1: the equity series sampled once per 60-second bucket behind `--metrics`, and a `[PERFORMANCE]` line with drawdown, its duration, volatility, Sharpe, Sortino and Calmar; default output unchanged | `f0a7b9d` |
| 2026-10-30 | Metrics batch 2: every closing fill booked as a round trip (side, entry and exit price and time, PnL, both halves of the fee), with `[ROUND_TRIPS]` reporting win rate, profit factor, expectancy, average win and loss, holding time, taker and total notional, and turnover | `8b259fb` |
| 2026-10-30 | Metrics batch 3: the decision midpoint kept per order, `[TAKER]` reporting arrival slippage, the drift after a crossing fill at 5/20/60 cycles, and the implementation shortfall of the IOC/FOK orders; `--markout-horizon=N` parameterises the hardcoded five cycles | `66b0484` |
| 2026-10-30 | Metrics line closed: the architecture table documents `[PERFORMANCE]`, `[ROUND_TRIPS]` and `[TAKER]` with their definitions, the coverage table states what is measured and what is still out of scope, and two accessors nothing read are gone | `934ee97` |
| 2026-10-30 | Non-market-making line closed by non-replication: the EWMA cross re-measured at four horizons of 1–64 seconds on two non-adjacent windows, break-even taker rate 0.74–6.72 bps against the venue's 5, one net-positive cell in April and none in June | `9582550` |
| 2026-10-30 | Metrics batch 4: the round-trip distribution split by the side the position was opened on, one `[ROUND_TRIPS_SIDE]` line per side, sharing the aggregate's arithmetic | `110f8aa` |
| 2026-10-30 | Boundary lifted for one requested item: the freeze no longer covers an execution scheduler that takes no view on direction, and everything else stays frozen | `69eaa83` |
| 2026-10-30 | Execution line: `execution_l2` works one parent order with `market`, `twap:N` or `pov:X` children, and `[EXECUTION_ALGO]` reports the shortfall in basis points. First measurement: no schedule beats one crossing by 2 bps on two windows, so the schedule is not the lever and the fee is | `69eaa83` |
| 2026-10-30 | Execution benchmark: the parent is also measured against the time-weighted midpoint of its own window. `twap` lands within 3.2 bps of it in every cell while a single crossing sits at −11.92 and +7.79, so the crossing is a drift bet and the schedule pays a stable half point | `bbbe087` |
| 2026-10-30 | Execution entry: `--algo-entry=post:N` rests each slice and crosses the remainder on timeout. Four timeouts on two windows: the fee falls with patience (5.00 → 1.80 bps) but fills fall with it (100 % → 60 %) and the opportunity cost rises faster (0.00 → 10.85 bps), so crossing stays the entry | `509e811` |
| 2026-10-30 | Execution cleanup: two dead fields removed, `--algo-entry=cross` refuses a parameter it would ignore, `complete` documents that a truncated window makes the shortfall a lower bound, and the architecture's file and family counts now include the execution line | `605af3f` |
| 2026-10-30 | Capacity ladder 1,000–100,000 contracts on both windows: the shortfall in basis points is flat (every term is proportional to quantity) while the crossings needed go from one to six for `market` and to 97 for `twap`; no size reorders the schedules, so the capacity question closes | `6c6aa7a` |
| 2026-10-30 | Pair pre-check before building a multi-symbol pipeline: Tardis serves ETH-PERPETUAL in the same schema without a key, and a fixed z-score rule captures 2–10 bps of reversion against a 28 bps round trip, so the pair closes by measurement rather than by missing data | `53f7f63` |
| 2026-10-30 | Second instrument: `ETH-PERPETUAL` registered (five-cent tick, one dollar of face value, same fee), its account numbers separated from the BTC contract's, and the crossing floor measured at 6.88 against 5.39 bps for an identical fee | `432dc18` |
| 2026-10-30 | Maker break-even is tick-dependent: −5.65 bps on the BTC contract against +1.26 on the ETH one, same venue and fee; recorded as a hint, because the free days give too few fills to settle it | `0b79452` |
| 2026-10-31 | The campaign after the tick curve, each with its verdict in the sections above: the dated and calendar basis, the CME calendar over 23 quarterly holds, the straddle measured three times (the first two wrong), the maker revival test that falsifies "execution rescues a closed line", and the funding extreme that fails all three gates | `8df8777` … `86be0ce` |
| 2026-11-02 | P2-9 completed: a run that trades two books at once. `--symbol-b`/`--trades-b`/`--depth-b` build a book, a decision layer, a venue and a pipeline per symbol against one `Portfolio`; the four readers are merged by timestamp; `Portfolio::set_instrument` values each symbol at its own face value; the summary prints one `[EXECUTION] ... symbol=` and one `[BOOK]` per book and the account once. Measured: 377,024 events, gross 11 against net 9, fees 0.0202, re-derived by the gate | `bf9ef28` |
| 2026-11-02 | Pre-trade risk follows the account: the hard exposure cap counted each book's own notional against the same cash, so two books could carry twice the cap while both reported no violation. Each book is told what the other legs hold and the check adds it, which is zero in a single-book run. With a ten dollar cap on the same two books the second book is refused and the account never passes ten | `5a63c58` |
| 2026-11-02 | P2-10 landed: a session is written down and taken over. `--snapshot-out` writes the account, the positions with their entries, the marks, the working orders and the clock; `--snapshot-in` restores the account, asks the venue about every inherited order (a live one is cancelled, a forgotten one counted) and holds its own orders until the snapshot's clock while it rebuilds the book. The resumed run's `[PORTFOLIO]` and `[EXPOSURE]` are identical to the run it took over, and the gate re-derives that. Also fixed: a `Pipeline` left the venue holding a callback into itself, which a restart is the first thing to notice | `195839f` |
| 2026-11-02 | P2-11 opened with add-liquidity-only: `--post-only` makes the venue cancel an order that would trade on arrival instead of filling it. `active_l2` is unchanged on the 200k window; `trend_l2`'s 55 fills on it were *all* taker fills (taker fees 0.275 against maker fees 0), and with the flag it trades nothing. Cancel/replace is recorded as deliberately unbuilt, with the engine reason: the queue is reconstructed on entry, so an amend's only gain is the round trip, which needs the latency model | `0112968` |
| 2026-11-02 | Two loose ends closed. A two-book run now reports `[PNL] symbol=..` per book — the account's line could not say which leg lost the money — and the split is taken where the account's totals are written, so the legs add up to it exactly. And the session snapshot holds one record per book (its own clock, id sequence and working orders), so a two-symbol run resumes both legs at their own venues; the single-book format written first is still read, because a restart reads a file an older build left behind | `d5ab624` |
| 2026-11-02 | The end of the session, priced: `--close-out` flattens what the run is holding at the touch and prints `[WIND-DOWN]`. Closing a carried BTC position costs **5.39 bps** (5.00 taker + 0.39 half tick) — the same floor the crossing measurements found, arrived at from the other end of the trade — which is the amount every `net_pnl_usd` that carries a position was optimistic by. A close the touch cannot fill reports `no_liquidity=1` with the position as `residual` instead of a flat account. Also decided: the simulator's venue does not persist its own fills across a client restart, because that would only prove the simulator is self-consistent; a fill a client misses is already reachable in one run (`--broker-drop-every`) | `6a6893a` |
| 2026-11-02 | Recorded the gaps that were on **no** list — no interval on any number, no calendar or contract lifecycle, "portfolio" with no risk model, a kill switch that stops quoting but does not flatten, a venue that is a process rather than a service, no adversarial input for the readers, no latency budget, no machine-readable provenance — each with a verdict, because an unrecorded gap is rediscovered as a feature request. One of them was then closed: tier 2 compared against a reference in `/tmp`, so it never ran on a checkout; the hash now drops the path-dependent mode line and the reference lives in `tests/fixtures/` | `445a4cc` |
| 2026-11-02 | The halt path completed. The kill switch used to refuse every order — including the one that would close the position — and the account was never marked during a run, so its `realised + unrealised` reading had the loss sitting in the position rather than in the PnL. It now exits only (a flip is not an exit, which is what its magnitude-based neighbour would have allowed), marks the book each cycle while it is armed, closes at the touch when it latches, and prices that exit: 5.39 bps on the 200k window, with `[EXPOSURE] symbols=0` afterwards where it used to leave a leg open. A limit the run never reaches changes nothing, so every recorded number stands | `355b215` |
| 2026-11-02 | Every number this project quotes is a mean over a handful of events, and the "below thirty fills is not a conclusion" rule had only ever been applied by hand. `tools/study/pnl_interval.py` now resamples the mean and prints the interval, the median, the range and the thin-sample flag, with its own hand-checked self-test in the gate. Applied to one snapshot slice a month for five months, the maker break-even rate moves from −3.19 to −0.44 bps: the interval around the mean is −2.84 to −0.90, narrower than the 2.75 bps the five draws span, which is the difference between "this mean is pinned" and "this population is one thing". The paired same-day difference is negative on both windows available, so the BTC/ETH gap keeps its sign and loses its size | this commit |
| 2026-11-02 | The L2 path's data-quality accounting was inert: the validator's `stale_trades`/`dislocated_trades` need a best bid and offer, and the three depth readers produce none, so on the path every recorded number comes from those checks never fired and `[L2 DATA]` did not print them either. The prints are now scored against the book's own midpoint in `Pipeline` (correct on the threaded path too, where validation runs on another thread), split into `stale_trades` -- no midpoint, or one more than a second old -- and `dislocated_trades` -- a fresh midpoint the print was more than five basis points from. Measured: 117,889 and 9 on the 200k window, 0 and 554 on the full day, both printed only when non-zero. The tier-2 reference moved by exactly one token and the re-record is justified by stripping it (`270d8085` → `d66e88e0`) | this commit |
| 2026-11-02 | The last two long documents were split, which is where the tiering stopped being enough. `ARCHITECTURE.md` (1,574 lines) is an index over six parts in `docs/architecture/`, and `USER_GUIDE.md` (1,048) an index over seven in `docs/using/`. The field list in the L2 replay part was the remaining overlap with the Learning Path: five fields stay because a run is judged by them, the rest are the conditional groups a flag turns on, and the benchmark's CSV columns are pointed at the tool that documents them. Splitting broke 29 links and one heading's slug had been wrong for weeks, which produced the thing this repository was missing: `tools/gate/check_links.py`, which resolves every relative link and every `#anchor` against the headings of the file it names, counts the number of link openings so a malformed one cannot hide from it, and runs in tier 1 and in CI | this commit |
| 2026-11-02 | Twelve documents at one level, three of them longer than a thousand lines, with the plan carrying 1,700 lines of finished research and two change logs saying the same thing. The documents are now in three tiers -- the answer and the argument, the reference, the evidence -- with `docs/index.md` as the map and a "who owns what" table that says what each document owns and what it must not contain. `PERFORMANCE_HISTORY.md` is an index over three parts in `docs/performance/` (the engine, the L2 mainline, the studies); `LATENCY_DESIGN.md` became `docs/performance/design.md`; `ROADMAP.md` dropped from 1,971 to 290 lines by moving the record of finished studies to `docs/archive/studies.md`; and `RESEARCH_NOTES.md` joined it in `docs/archive/`. Every intra-document link was rewritten, the gate and CI now lint and re-derive `docs/*.md docs/*/*.md`, and a paragraph claiming there is no persistence or recovery -- both of which exist -- is corrected | `0dbfbb4` |
| 2026-11-02 | The documents still introduced the project as a toy simulator for learning, with the measurement work as a second half bolted on. They now say what it is: a measurement project with an instrument inside it, eight markets closed by quotients, and every quoted number re-derived by the build. The intro of the README, the documentation index, the Design Review, Architecture, the Main Line, the Learning Path, the User Guide and the Roadmap's opening and status table all say it the same way; the "interview versions" section became "the questions a reader asks", because the plan removed interview material a week ago; and the README's non-goals were rewritten, since the old list claimed there was no persistence and no L2 reconstruction, both of which exist | `a4f0bf9` |
| 2026-11-02 | The tools directory was one flat list of forty-eight files, and nothing in it said which tool answered which question. It is now seven directories by kind -- `gate/`, `sweep/`, `fetch/`, `study/`, `live/`, `bench/`, `legacy/` -- with `tools/README.md` as the map, and every path in the documents, the source comments, the gate and the CI workflow moved with it. Two files became one entry point (`slice.py`, whose `window` and `snapshot` subcommands are the two shapes a slice takes), the marked-block emission moved into `lib.sh` so four sweeps no longer each spell the marker, and one file was deleted: `lf_windows.sh`, named by nothing and asking the question `equity_schedule_sweep.sh` already asks. The stale `site/` build of the documents went too | `47c6849` |
| 2026-11-02 | A review of the recent code found three things worth changing and two worth recording. Changed: the OKX depth reader's levels and the live feed's prices went through `parse_double`, so a `nan` level price reached the ladder on the two paths the CSV families had already been hardened against; the OKX depth errors now carry the line number as well. Recorded: the L1 path drops a print far from the BBO while the L2 path counts it, and the recorded windows mix quarter-hour depth with day-long prints -- both in the gaps table, because either fix moves recorded numbers | `7fb6133` |
| 2026-11-02 | The readers were asked what they do with input nobody promised them, in a test rather than in a review. They already refused every hostile row — truncated, `nan`, negative quantity, wrong-case side — which was better than the test suite knew, and four things were still missing: no message said *which* row, a `nan` price was caught by the validator instead of the reader that could locate it, a batch that left the book crossed was skipped and not counted, and an empty depth file produced a formatted run of zeros with exit code 0. All four are closed; the row numbers come from `LineReader::lines_read()` and the count rides on `[L2 DATA]` only when it is non-zero, so no recorded line moved | `7c62be3` |
| 2026-11-06 | The two notes that lived outside `docs/` came in: `data/README.md` and `tools/README.md` are now `docs/DATA.md` and `docs/TOOLS.md`, with a six-line pointer left at each old path so that a link into `data/` or `tools/` still lands on something that says where the answer is. The reason is mechanical rather than tidy: a document outside `docs_dir` cannot be linked relatively -- `mkdocs build --strict` refuses the link and `check_links.py` cannot follow it -- so the seven links that reached those files were absolute GitHub URLs on a repository name that has not been renamed yet, which is a 404 in the window between the two. They are relative and checked again. The eighth absolute link, to `CHANGELOG.md`, stays absolute: a release log belongs at the root of a repository and not inside its site. The prose lint now covers 31 documents instead of 29, which is how the fetchers' `--verify`, `--sample` and `--self-test` were found missing from its list of flags that belong to the tools rather than to the engine | `this commit` |
| 2026-11-06 | The two longest pages were split once more: `architecture/05-strategy.md` (663 lines) is an index over two parts and `using/03-l2-replay.md` (618) an index over three, both keeping their paths so nothing that linked them had to move, and each part keeping the heading text it had so every anchor survived the cut. Splitting them exposed a gate bug rather than a document one: `lint_prose.py` and `check_docs.py` were called with `docs/*.md docs/*/*.md`, which is exactly two levels deep, so the five new pages were outside both checks and nine documented flags read as "not mentioned in any document" while every tier stayed green. Both tools now take a directory and walk it, the gate and CI pass `docs`, and a section that grows a subdirectory can no longer fall out of the lint silently | `this commit` |
| 2026-11-06 | The threaded path became measurable per stage, which it could not be: `--profile-stages` with `--feed-thread` was refused because one profiler would be written from two threads. Each thread now has its own instance and the run prints two labelled blocks — the feed's `read+parse` and `validate`, then the consumer's six — so the mode the ring exists for can be profiled at all, which matters because `[LATENCY]` counts in microseconds and the hand-off it would have to show is 170 ns. The two-book path keeps the refusal with its reason: there one profiler would be written by two pipelines as well. With the measurement available, the event's shape is recorded as **not worth changing** — 119 ns per message of move-and-destructor against a trivially copyable payload of the same size, on a path no recorded number uses, and not a rename: `MarketDepthSnapshot` carries two `std::vector<DepthLevel>`, so making the variant trivially copyable means bounding a snapshot's levels or a pooled-buffer protocol | `this commit` |
| 2026-11-07 | The reading the previous row made possible was taken and written down: three runs each way on the W1 slice, median of three, now in §3.5 of the performance reference. It says the split is what threading buys — the feed thread carried 57–68% of the direct block's per-event work, so most of an event's cost is the reader's and it runs beside the strategy instead of in front of it — and it says every stage is *slower* than the same stage done serially (`book-apply` +53%, `market-view` +28%, `strategy` +23%, `engine` +13%, and even the feed's `validate` +27%), with the ordering stable across the three runs and the magnitudes not. That follows from the hand-off costing more than the 119 ns `ring_bench` measures: the bench hands over a value the consumer already has in cache, while a real event is written on one core and read on another. It is a second reason not to restructure the event type to recover that 119 ns | `this commit` |



| 2026-10-28 | Order state machine, reject/cancel reasons, risk pipeline, non-market-making line and measurements | `9a691be` … `3b0a180` |


### For an AI reviewing this plan

Read this section, then the [Design Review](DESIGN_REVIEW.md) and [Architecture](ARCHITECTURE.md). Review it
against the goal above: evidence that stays true, produced by a bounded instrument, not a production system. Check for
scope creep, missing technical coverage, duplicate documents, a clear stop line, and any deferred item written as
permanent. Do not propose production features unless they buy a concrete project improvement.

## The studies behind the answers

This document used to be 1,971 lines, most of it the record of studies that have already been run: the CME calendar, the
large aggressive prints, the stale quote, the options lines, lead-lag, the funding extreme, the venues that fit a
personal user, the carry line's gates, and the plan from a positive window to a running system, stage by stage.

All of it is now [The studies](archive/studies.md) under `docs/archive/`. Nothing was deleted: a study that closed a line
is the evidence for the closure, and the ones that did not close anything are the reason nobody has to run them again. It
is out of this document because the plan is short by design and a record of finished research is not a plan.

Read it when you want to know *how* a line was closed or *why* a venue was rejected. Read this document when you want to
know what is still open.

## Deliberately not planned

* **Order-by-order (L3) data.** The bounds experiment shows the conclusions do not move between the
  conservative and the optimistic queue models, so a true queue in between cannot move them either. Finer
  data would buy precision on secondary numbers, at the price of a different instrument and the measured
  history that goes with it. If it is ever worth doing, it is one day of data used to calibrate the existing
  model offline, not a new runtime path.
* **More strategies and more knobs.** The binding constraint is the fee, so a better quote rule changes how
  much is lost rather than whether anything is.
* **Cosmetic repository work.** No demo recordings, no badges. The documents are the interface.

## Why the seams are where they are

Every choice below was made for a measurement that had to be possible, and each one is also what keeps a live path
possible. The table is the reason the instrument is shaped the way it is rather than a promise about later.

| Choice made now | What it keeps open later |
|---|---|
| every input format behind one reader in `src/market/` | a live feed is another reader, not a second pipeline |
| the strategy talking to interfaces only | the same code can be driven by a replay or by a live book |
| execution reports as the only input to accounting | a real venue's fills update the portfolio unchanged |
| determinism and replay as pinned properties | a recorded live session can be replayed and compared |

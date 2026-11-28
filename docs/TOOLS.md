# The tools directory

*Part of the reference tier. The map of the documents is [`MAP.md`](MAP.md).*

Everything here exists to turn a claim into a number that somebody else can reproduce. The directory names say which
kind of tool a file is:

| directory | what lives there | the gate runs |
|---|---|---|
| `gate/` | the gate, the six lints it calls, `lib.sh` (the shared shell helpers) and `detach.sh` (one long command, started detached) | the lints, on every tier 1 run; `check_docs.py` on the pages a docs run names |
| `sweep/` | the measurements that vary one thing over a window and print a table | `risk_check.sh` |
| `fetch/` | getting data in and cutting it down: five venue fetchers, `slice.py`, `inspect_source.py` | `slice.py window`, `fetch_iex_hist.py --self-test` |
| `study/` | the studies: one question each, and the drivers that turn a window into a row | six `--self-test`s, and `viability.py` |
| `live/` | the seven tools that speak to a socket or a live config: relay, config reader, sender, soak, tracer, resume, and the account probe | `venue_relay.py --self-test`, `live_config.py --self-test` |
| `export/` | the bundle a reader that cannot clone the tree is given | no |
| `bench/` | two harnesses for the performance line: the paired A/B runner and the libc call counter | no |
| `legacy/` | the CSV and UDP generators for the L1 path that the demo and `data/scenarios/` still use | no |

> The relay exists twice. `tools/live/venue_relay.py` is the implementation the engine's order session was ported from
> (ADR 0006) and stays in the tree as the reference a reader can diff the port against -- kept for review, not the route a
> run uses; `src/lines/venue_relay_main.cpp` is the one a session runs. The C++ relay's order leg also serves the two
> questions the engine cannot answer alone -- the venue's working orders and the account's balance -- over the same engine
> socket (`PENDING`, `EQUITY`), and `--venue-demo` points its market leg at the demo public stream.

Two things a reader can rely on, because both are counted rather than promised. A tool the gate does not run is quoted
only through a marked block that `check_docs.py` re-runs, so its arithmetic has a witness even when nobody runs it by
hand; and every file under `tools/` has a line in the inventory below, because `lint_prose.py` fails a build when one
does not.

/// details | The inventory: every file under `tools/`, one line each

| tool | what it prints | run by |
|---|---|---|
| `tools/gate/verify_l2.sh` | one line per tier, and the field that moved | is the gate |
| `tools/gate/quiet.sh` | the first and last few lines of a command's output, and the log that holds the rest | hand |
| `tools/study/okx_download_rank.py` | the archived screen re-ranked for the download decision, in the two shapes the measured instruments define | hand |
| `tools/gate/check_docs.py` | `checked N recorded run(s), 0 mismatch(es)` | gate |
| `tools/gate/check_written_fields.py` | `checked N metric fields, 0 unwritten` | gate |
| `tools/gate/check_layering.py` | the include that crosses a layer boundary | gate |
| `tools/gate/check_links.py` | `checked N link(s), 0 problem(s)` | gate |
| `tools/gate/check_adr.py` | a decision and its index row that disagree | gate |
| `tools/gate/check_deploy.py` | eight couplings between `deploy/`, `.env.example`, `config/live.example.yaml` and `deploy.yml` that a host would otherwise discover: the env names the relay reads, the template's keys, the engine's endpoints against the relay's listeners, the units' ordering, the stop window against the engine's deadline, the package step against the units' paths, `RELAY_ORDER_LIVE` against its venue, and `OnFailure=` against a shipped notifier | gate |
| `tools/fetch/okx_depth_align.py` | validate a downloaded 400-level book archive and align it with another instrument: the flat `{instId,action,asks,bids,ts}` shape, an opening snapshot, at least 100 levels a side, a first timestamp inside the named UTC day, the two ranges printed, and `--watch N` for an overnight drop. `--self-test` builds four archives and pins the pass plus three refusals | hand |
| `tools/study/portfolio_stress.py` | two seats merged into one account: the joint per-day series, `sweep_summary.py`'s gate applied to it in USD, the correlation and worst-day overlap, and the margin both seats fully loaded need at each leverage (peak position from `max_inv`, priced at the archive's own print) | hand |
| `tools/sweep/up_ten_contract_cell.sh` | the ten-contract cell and its state arms in one script: the control, `--jump-ticks=2` and the registered `--jump-ticks=3`, with the bar and the three measured rows in the header. Exists because the cell was recorded by reference and the flags that rebuild it (`--extra-layers=1`, `--settle-slippage-ticks=1`) were in a `/tmp` driver; `--symbol`/`--tick` run the same cell on the other seat, which is what the two-seat ranking needs | hand |
| `tools/ops/jobs.py` | the two views of a run that `dispatch_deploy.sh logs` needs: `list` prints each job and the steps that did not succeed, `failed` prints the job ids that did not, and `tail N` prints the last N lines of the job log zip the API hands back. Split out because a bash function that both parsed JSON and unpacked an archive was the third quoting layer in one line | hand |
| `tools/ops/dispatch_deploy.sh` | the deploy loop without a browser: `run` dispatches `deploy.yml` (or `ops.yml` with `--ops <action>`), and `--key`/`--value`/`--add` carry `set-key`'s inputs zipped in order, so a setting is applied without the browser; `wait` blocks to the conclusion, `status` prints the newest run of each, `logs` names the job and step that failed. Reads `secrets/gh.token` -- a fine-grained token with Actions: Read and write and nothing else -- and refuses a dirty tree, a second concurrent dispatch, `data_gate=true`, and a setting named for a workflow that carries none | hand |
| `tools/fetch/data_inventory.py` | classify a `data/` tree: every file as a symbol, a kind (400-level depth, trade tape, other) and a day, with sizes per symbol, and the findings that cost a run when they are found late -- partial downloads, a day with only one of the two files, the same day in two layouts, an unreadable gzip, days older than a retention. `--prune` deletes only under an explicit rule and prints each path | hand |
| `deploy/notify.py` | one mail when a unit fails: the unit, the host, the four properties that name the ending and the journal tail; unconfigured-safe, `--dry-run` prints it, `--self-test` pins both paths | host |
| `deploy/activate.sh` | the host activation a deploy performs, as one tracked script instead of a single-quoted ssh argument: `daemon-reload`, `reset-failed`, a bounded start of the relay that prints the attempt it succeeded on, the port wait, the preflight, and a session report that never touches a running session. A start that will not come up is a warning, because `--ops restart` and the 13:30 window are the proofs that work | hand |
| `tools/ops/predeploy.sh` | everything a deploy can be told locally before any SSH exists, in under a second: both workflows parse, `deploy.yml`'s remote heredoc parses, every `deploy/*.sh` and `tools/ops/*.sh` passes `bash -n`, `check_deploy` and both self-tests pass, `lint_prose`/`check_links` agree with the tree, and a dirty tree is refused. `dispatch_deploy.sh run` calls it unless `--no-predeploy` | hand |
| `tools/ops/window_ledger.py` | the window series as a series: `--table` one line a window (fills, fees, net, gross, residual, equity, markout), `--progress N` the cumulative fills against a target and the weeks left at the current rate, `--check` monotone windows with one row each, `--emit` a two-line SVG. It exists for the live sample's own question -- 300 to 500 fills before its markout can be compared with the replay's, whose 28 days read a median of +0.416 and a worst of -0.808 | hand |
| `tools/gate/check_leaks.py` | a secret, a machine path or a private file that a commit would publish; `--audit` reports the deny-list's remaining public surface without failing | gate |
| `tools/gate/check_sweep_columns.py` | `sweep columns: ok` -- the sweep's CSV against the two one-minute runs the gate makes | gate |
| `tools/gate/lint_prose.py` | a flag, model, path or size figure the documents get wrong | gate |
| `tools/gate/lib.sh` | `field_of` and `emit_block`, sourced by the shell tools | sourced |
| `tools/gate/detach.sh` | the log path; the status lands in `/tmp/<name>.exit` beside it | hand |
| `tools/sweep/fee_sweep.sh` | PnL before fees, what closed, what the schedule kept | hand |
| `tools/sweep/latency_sweep.sh` | one row per order and cancel latency, 1 to 50 ms | hand |
| `tools/sweep/limit_sweep.sh` | what the kill switch saves, and what it costs | hand |
| `tools/sweep/trend_sweep.sh` | one row per `--trend-params` cell on one slice | hand |
| `tools/sweep/equity_schedule_sweep.sh` | which queue model the equity tape supports | hand |
| `tools/sweep/multi_day_check.sh` | one measured row per Deribit day | hand |
| `tools/sweep/workload_matrix.sh` | the four fill levers around one window | hand |
| `tools/sweep/risk_check.sh` | four runs and the invariant each one holds | gate |
| `tools/sweep/compare_runs.sh` | one row per field, one column per variant | hand |
| `tools/sweep/f4_windows.sh` | the F4 and equity windows, re-run | hand |
| `tools/sweep/okx_maker_baseline.py` | one row per day per queue model for one L2 strategy inside a UTC window -- fills, fees, gross, net, breakeven, and the inventory the window ended holding -- with `--strategy` choosing the line (the maker by default, `trend_l2` and `execution_l2` on the same cut and columns) | hand |
| `tools/fetch/fetch_databento.py` | a downloaded day, split into a depth CSV and a trade CSV | hand |
| `tools/fetch/fetch_deribit_options.py` | one straddle a month and its implied volatility | hand |
| `tools/fetch/fetch_funding_rates.py` | one instrument's funding series as a CSV `carry` reads | hand |
| `tools/fetch/fetch_iex_hist.py` | the container walk over a pcap, and its refusal | both |
| `tools/fetch/fetch_okx_trades.py` | one UTC+8 day of OKX trades cut to one symbol, or a window: `--from/--to` with `--pause` (a rapid loop answers HTTP 429), `--skip-existing` to resume, `--output-dir` for the per-instrument folder; each venue-wide zip (166-615 MB) is deleted after slicing | hand |
| `tools/fetch/prefetch_windows.sh` | the days the next questions will need, three blocks with the question written above each: UP's sixty-day sample (2026-09-28..2026-10-29, the second month the "one month is one regime" argument asks for), NES on the same 28 days as UP (a seat comparison needs one sample), and the first live window's own day. `--block N` runs one; both fetchers take `--skip-existing`, so a re-run costs nothing and the script is safe on a timer | hand |
| `tools/fetch/fetch_okx_depth.py` | OKX's 400-level book archive for one instrument, one day or a window, at the portal's own URL shape (`/cdn/okx/match/orderbook/pro/L2/400lv/daily/<stamp>/<sym>-L2orderbook-400lv-<day>.tar.gz`, measured 2026-11-27 after eight guessed paths answered 404); each file is refused unless its single `.data` member opens with a snapshot for that symbol stamped inside the named UTC day, and a 404 (a day before the listing) is named instead of stored | hand |
| `tools/fetch/fetch_okx_index.py` | the venue's **index series** for one instrument, one minute bars, one day or a window: the number a swap's mark price is built from, so a perp-index basis has a series even where no spot book is listed. `/api/v5/market/index-candles` serves the recent bars and `/history-index-candles` the older ones, both `code 0` on 2026-11-28 (a live bar at 0.1997-0.2001, a thirty-day-old one at 0.1388), a hundred bars a call and no key; an empty page is the end of the history rather than a failure, a refusal is retried, and a day with no bars is written as a header only | hand |
| `tools/study/fill_basis_asymmetry.py` | the perp-index basis at each maker fill, bucketed into deciles with the buy share and the one-second markout beside each bucket: reads the replay's own record, the window cut's mid series and the index bars above, and `--split DAY` prints the bottom decile's lean in two halves, which is how Tier 2 #5's sign-stability bar is read as a line instead of an argument | hand |
| `deploy/release_path.py` | the release-path classifier: a diff is `light` only when every changed path is one of the card's six (three directories, `*.service`, `*.timer`, `*.env.example`) and none names a program; empty is `full`, because a classifier told nothing must not answer "light". `--self-test` pins twelve paths, a mixed list and the empty one; `check_deploy`'s nineteenth rule holds its allowlist to the card | hand |
| `tools/study/fill_decomposition.py` | the window's P&L as `spread + drift - fees`, reconciled against the sweep CSV's `net_usd`: spread is each maker fill's edge at the instant, drift is that fill marked at the window's last mid, fees come from the record. It prints the gap per day and **refuses to quote the split when the identity does not reconcile** -- three of twenty-one days reconcile today, so what it delivers is the method and the open question (the residual's valuation), not a table to cite | hand |
| `tools/fetch/slice.py` | a bounded window, streamed, from a file too large to replay | both |
| `tools/fetch/inspect_source.py` | delimiter, types, timestamp columns, implied top of book | hand |
| `tools/study/book_lead_lag.py` | does the leader's mid move before the follower's: two mid series on one cadence from the two 400-level archives, returns in bps, the cross-correlation at every lag inside ±N ms, and the peak printed beside the correlation at zero and at our own 78/118 ms round trip. Measured 2026-11-28 on BTC to UP over the seven overlapping days: the peak sits at 0 ms with corr 0.013, so there is no lead a 76-118 ms reaction could use | hand |
| `tools/study/fill_markout_by_state.py` | the same question asked the way a gate would use it: the frozen cell replayed with `--record-orders`, each **maker** execution's 1 s markout against the follower's mid, bucketed by the leader's 3 s return at the fill's stamp. Measured 2026-11-28 over the seven days: the flat bucket keeps a +2.17 bps median on 77 percent of the fills while the two moving buckets sit at 0.00 and mean -3.2 and -3.7, so the state worth gating is the absolute return and not its sign -- a rise is as bad for a passive fill as a fall | hand |
| `tools/study/window_regimes.py` | the windows bucketed by their own physics -- A: absolute move at or below 100 bps, B: 100 to 200, C: above 200 -- and joined with one arm's per-day rows (`--rows`, `--folder`). Measured 2026-11-28 over twenty-eight days: the label separates the **worst** day by 45 times (A +5.5 bps, C -250.5) and nothing else, since the medians fall +19.6, +9.5, +6.4 as the move grows; no C-day has efficiency above 0.5, so the month holds "wide two-way range" and no one-way day at all; and the half hour before 13:30 does **not** separate the buckets (C's pre-window move is the smallest of the three). Reproduced on the sixty-day arm (59 days, 2026-10-17 skipped as `n/a`): +19.5, +10.7, +8.4 closed -- the ordering and the size within 2 bps, efficiency still under 0.5 on all 26 C-days, the same single -250.5 worst day (2026-11-22, one event rather than a regime) -- and A's worst day is -28.6 there, so "A never loses" was the nine-day sample and not the bucket | hand |
| `tools/study/leader_state_out_of_sample.py` | prepares the cuts `fill_markout_by_state.py` needs for a month the gate was never measured on, then runs it: `--days` (default 2026-10-30..2026-11-26), one two-hour cut plus one trade slice per day, the seven days the gate was found on skipped by name rather than counted twice, the sweep tool's lock and free-space refusal reused, one flushed line per day. `--prepare-only` stops after the cuts. Written before its data landed so the twenty-one-day counterfactual is one command, not a recipe remembered from a session | hand |
| `tools/study/live_knobs_ab.py` | the live file's advance debounce, A/B on one window with the live flags: 40,000 ms (what `live.yaml` and the recorded ten-contract cell both carry) reads 31 buy / 40 sell quotes, 3 fills, -0.2317 USD, against 40 ms 52 / 25, 6 fills, +0.1066. The arm that shows the first live window's 2,547-to-44 split is not the strategy's shape -- the engine is near-balanced -- and that 40 ms is a new setting to register, not a cell to go back to | hand |
| `tools/study/analyze_l2_windows.py` | one row per time window per strategy | hand |
| `tools/study/benchmark_l2_strategies.py` | the strategy set on one window, marking thin rows; `--include-legacy` adds `inventory_aware_l2`, which lives under `src/strategy/maker/legacy/` and has no pinned run | hand |
| `tools/study/benchmark_replay_windows.py` | the replay benchmark per time window | hand |
| `tools/study/calibrate_queue_model.py` | how a level decrease splits into trades and cancels | hand |
| `tools/study/capacity_table.py` | what a book costs to run, by capital, breadth and tier | both |
| `tools/study/cascade_probe.py` | a forced-flow burst's forward move, from the trade tape or from the book thinning; `--days` is the multi-day form | hand |
| `tools/study/check_okx_book.py` | a rebuilt book against the archive's own snapshots | hand |
| `tools/study/figure_charge.py` | the quickstart's cost figure as inline SVG | hand |
| `tools/study/figure_path.py` | the two path figures as inline SVG | both |
| `tools/study/funding_extreme_test.py` | whether a funding extreme predicts the next eight hours | hand |
| `tools/study/lowturnover_test.py` | the pre-registered rule on sixty equity windows | hand |
| `tools/study/measure_l2_baseline.py` | replay seconds, and the hash of the output | hand |
| `tools/lib/okx_data.py` | where an OKX depth archive and trade tape live: `<root>/<SYMBOL>/` or the flat root, so one module owns the two layouts every study tool reads | hand |
| `tools/study/okx_book_window.py` | a day archive's time-weighted half-spread inside a UTC window, and the window cut that opens on a snapshot | hand |
| `tools/study/okx_book_depth.py` | the same cut's top two levels, time-weighted: the size a quote would stand behind, and the USD it is worth | hand |
| `tools/study/okx_halfhour_profile.py` | a day's half-hour profile -- duration-weighted half spread and trades a second -- with no P&L, for deciding a window on the book rather than on a run | hand |
| `tools/study/okx_tape_toxicity.py` | the tape's own adverse selection per half hour: each print's signed mid move over a stated horizon, as the passive side's markout -- mean, median and the share the mid moved on, with no strategy and no P&L | hand |
| `tools/study/okx_quote_share.py` | how much of the book one quote is: the window's market turnover, the time-weighted USD resting at the level the quote joins, and the two ratios that say whether a size is invisible -- with `--order-units` so a size ladder reads on one scale | hand |
| `tools/study/okx_fill_profile.py` | our own fills per half hour, from a run's trade stream: capture against the mid, markout at two horizons, the net contracts and the share of fills that opened a position -- the subset the quotes actually got | hand |
| `tools/study/okx_spread_screen.py` | every swap's median half-spread and tick width over repeated ticker reads, against three thresholds, archiving its reads; `--quote-ticks` adds the basis points a quote that far behind the touch captures a fill, which is the column the fee is actually paid out of | hand |
| `tools/study/plot_report.py` | quotes, distance, inventory and equity as one PNG | hand |
| `tools/study/pnl_interval.py` | a mean beside the interval it carries | both |
| `tools/study/ring_bench_table.py` | the ring hand-off over several sessions | hand |
| `tools/study/screen_spread.py` | symbols whose half spread could absorb a fee | hand |
| `tools/study/slice_pnl.py` | the same maker per slice, as two joinable tables | both |
| `tools/study/venue_ratio.py` | instruments ranked by capture against their own move | hand |
| `tools/study/fee_gate_table.py` | both gates in one row: the fee against the spread, and R_max against its reference, imported from the two tools above rather than recomputed | hand |
| `tools/study/viability.py` | the eight closed lines, re-derived | gate |
| `tools/study/volatility_window.py` | a windowed true range and its median split | both |
| `tools/study/walk_forward.py` | chosen on the past, scored on the next window | both |
| `tools/study/sweep_summary.py` | one sweep CSV in, the median **and its spread** out: closed min/max, positive days, fills, fee range, the absolute residual, the leave-one-out median and the best day's dominance. `--gate` applies the owner's two dispersion conditions (leave-one-out median positive, worst day within `--worst-multiple` of the median), which archived the BLUR size ladder | gate |
| `tools/live/venue_relay.py` | the relay the deployed units ship: market data to the engine and the order leg both ways, on the venue's grid, with the engine's three queries (`PENDING`, `EQUITY`, `POSITION`) answered from signed REST reads and `CLOSE` placed as a `reduceOnly` market order. `SIGTERM` refuses new orders, cancels what the relay placed and leaves only once nothing is live | both |
| `tools/live/live_config.py` | turns `config/live.yaml` into the engine's arguments -- the positionals first, then one `flag=value` a line -- and refuses a key it does not know, so the ignored live file is a contract rather than a second copy of the flags | gate |
| `tools/live/websocket_sender.py` | a local venue streaming two recorded files | hand |
| `tools/live/soak_push.sh` | a session's ping, reconnect and sequence counters | hand |
| `tools/live/trace_order.sh` | the order that waited longest, stage by stage | hand |
| `tools/live/resume_live.sh` | a snapshot written, resumed, and either break -- the file's or the venue's | hand |
| `tools/live/okx_probe.py` | the account's own side: USDT equity, the position in contracts, pending orders, and one contract placed for a run that needs a position the ledger does not know about; `rtt`/`warm`/`touch` measure the link's round trip and the post-only decline rate, `amend` times an amend against cancel+place on the same tunnel, and `spec <inst>` prints the venue's own `tickSz`, `lotSz`, `minSz`, `ctVal` and `instIdCode` plus what one contract is in engine units | hand |
| `tools/live/soak.sh` | one live `ws` session of a fixed shape, `post-only` or `baseline`, with both legs recorded so the window can be replayed; started as `bash .run.sh soak <mode>`, because a session typed into the tool's terminal once wedged it | hand |
| `tools/export/bundle.py` | two bundles, a manifest and a diff, each checked | hand |
| `tools/export/push_windows.sh` | the copied names with remote size and sha256 | hand |
| `tools/export/install_post_commit.sh` | the hook it wrote, or the one it repaired | hand |
| `tools/bench/ab_bench.sh` | paired differences, their mean and t statistic | hand |
| `tools/bench/count_libc_calls.c` | libc call counts, byte totals, a size histogram | hand |
| `tools/legacy/gen_ticks.py` | a reproducible synthetic tick CSV | hand |
| `tools/legacy/udp_sender.py` | the rows it sent, at the CSV's own intervals | hand |

///

## The four conventions

1. **A script knows where the repository is.** A shell script starts with `cd "$(dirname "$0")/../.." || exit 1`, so
   paths inside it are written relative to the root and a tool started from anywhere behaves the same. A Python tool
   resolves its data from `Path(__file__).resolve().parents[2]` where it can, and otherwise is documented as being run
   from the root — the gate and every command in the documents do that.
2. **Shell helpers are sourced, never copied.** `. tools/gate/lib.sh` gives `field_of`, which reads a
   `name=value` out of a run's output with the same rule `check_docs.py` uses, and `emit_block`, which prints a
   marked block. A tool that prints a table usually also prints that block, so pasting its output into a document is
   the whole job of documenting a number — and `tools/gate/check_docs.py` then fails the build when the number moves.
3. **A tool that cannot run says so.** The fetchers and sweeps need files that are not in the repository (paid
   Databento windows, the free sample days); a missing input is an error naming the file, never a table of zeros.
4. **Nothing here is a build step.** The build is CMake and `CMakeLists.txt`; no tool is on the path from source to
   binary. Deleting a tool breaks a document, not the program.

## Where to start

- To accept a change: `tools/gate/verify_l2.sh all`.
- To see what a gate tier actually checks: the comment block at the top of `tools/gate/verify_l2.sh`.
- To pick the cheapest check that can see your mistake: `smoke` (two replays, about a second), `code` (tier 0 and the
  invariants, about fifteen seconds) and `docs <pages>` (the documents, page by page) are the halves of tier 1 that a
  change pays for separately. `fast` is the whole of tier 1 and belongs once per task, not once per edit: the document
  half is about two minutes and almost all of it is one page whose blocks replay Databento equity windows.
- To reproduce a number a document quotes: the marked block next to it names the command, and `check_docs.py` runs
  exactly that command.
- To add a measurement: put the script in `sweep/` if it prints a table for a document, in `study/` if it answers one
  question once, and in `fetch/` if it is about getting or cutting data -- then add its line to the inventory above,
  because `lint_prose.py` counts the directory and a tool nobody named fails a build.
- To hand the repository to a reader that cannot clone it:
  `TOYQUANT_DRIVE_DIR=<the folder your client syncs> python3 tools/export/bundle.py --sync` writes two bundles, a
  manifest that carries the commit and a sha256 per file, and `diff.patch` since the commit the previous manifest
  carried -- then checks the lot. That diff is what a read-only reviewer is given: a review needs the change, not the
  tree, and the manifest is how it says "this is stale" (whole tree 3.35 MB, manifest plus the three front doors
  47.6 KB, a typical diff a few KB). A dump without the check is a copy with no age, which is what a ten-day-old
  snapshot is: the one this tool was written for described a README three times the length of the real one, and nothing
  in it could say so.

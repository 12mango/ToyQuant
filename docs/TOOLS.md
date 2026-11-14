# The tools directory

*Part of the reference tier. The map of the documents is [`MAP.md`](MAP.md).*

Everything here exists to turn a claim into a number that somebody else can reproduce. The directory names say which
kind of tool a file is:

| directory | what lives there | the gate runs |
|---|---|---|
| `gate/` | the gate, the six lints it calls, and `lib.sh`, the shared shell helpers | the lints, on every tier 1 run; `check_docs.py` on the pages a docs run names |
| `sweep/` | the measurements that vary one thing over a window and print a table | `risk_check.sh` |
| `fetch/` | getting data in and cutting it down: five venue fetchers, `slice.py`, `inspect_source.py` | `slice.py window`, `fetch_iex_hist.py --self-test` |
| `study/` | the studies: one question each, and the drivers that turn a window into a row | five `--self-test`s, and `viability.py` |
| `live/` | the four tools that speak to a socket: relay, sender, soak, tracer | `venue_relay.py --self-test` |
| `export/` | the bundle a reader that cannot clone the tree is given | no |
| `bench/` | two harnesses for the performance line: the paired A/B runner and the libc call counter | no |
| `legacy/` | the CSV and UDP generators for the L1 path that the demo and `data/scenarios/` still use | no |

Two things a reader can rely on, because both are counted rather than promised. A tool the gate does not run is quoted
only through a marked block that `check_docs.py` re-runs, so its arithmetic has a witness even when nobody runs it by
hand; and every file under `tools/` has a line in the inventory below, because `lint_prose.py` fails a build when one
does not.

/// details | The inventory: every file under `tools/`, one line each

| tool | what it prints | run by |
|---|---|---|
| `tools/gate/verify_l2.sh` | one line per tier, and the field that moved | is the gate |
| `tools/gate/check_docs.py` | `checked N recorded run(s), 0 mismatch(es)` | gate |
| `tools/gate/check_written_fields.py` | `checked N metric fields, 0 unwritten` | gate |
| `tools/gate/check_layering.py` | the include that crosses a layer boundary | gate |
| `tools/gate/check_links.py` | `checked N link(s), 0 problem(s)` | gate |
| `tools/gate/check_adr.py` | a decision and its index row that disagree | gate |
| `tools/gate/lint_prose.py` | a flag, model, path or size figure the documents get wrong | gate |
| `tools/gate/lib.sh` | `field_of` and `emit_block`, sourced by the shell tools | sourced |
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
| `tools/fetch/fetch_databento.py` | a downloaded day, split into a depth CSV and a trade CSV | hand |
| `tools/fetch/fetch_deribit_options.py` | one straddle a month and its implied volatility | hand |
| `tools/fetch/fetch_funding_rates.py` | one instrument's funding series as a CSV `carry` reads | hand |
| `tools/fetch/fetch_iex_hist.py` | the container walk over a pcap, and its refusal | both |
| `tools/fetch/fetch_okx_trades.py` | one UTC+8 day of OKX trades, cut to one symbol | hand |
| `tools/fetch/slice.py` | a bounded window, streamed, from a file too large to replay | both |
| `tools/fetch/inspect_source.py` | delimiter, types, timestamp columns, implied top of book | hand |
| `tools/study/analyze_l2_windows.py` | one row per time window per strategy | hand |
| `tools/study/benchmark_l2_strategies.py` | the strategy set on one window, marking thin rows | hand |
| `tools/study/benchmark_replay_windows.py` | the replay benchmark per time window | hand |
| `tools/study/calibrate_queue_model.py` | how a level decrease splits into trades and cancels | hand |
| `tools/study/capacity_table.py` | what a book costs to run, by capital, breadth and tier | both |
| `tools/study/check_okx_book.py` | a rebuilt book against the archive's own snapshots | hand |
| `tools/study/figure_charge.py` | the quickstart's cost figure as inline SVG | hand |
| `tools/study/figure_path.py` | the two path figures as inline SVG | both |
| `tools/study/funding_extreme_test.py` | whether a funding extreme predicts the next eight hours | hand |
| `tools/study/lowturnover_test.py` | the pre-registered rule on sixty equity windows | hand |
| `tools/study/measure_l2_baseline.py` | replay seconds, and the hash of the output | hand |
| `tools/study/plot_report.py` | quotes, distance, inventory and equity as one PNG | hand |
| `tools/study/pnl_interval.py` | a mean beside the interval it carries | both |
| `tools/study/ring_bench_table.py` | the ring hand-off over several sessions | hand |
| `tools/study/screen_spread.py` | symbols whose half spread could absorb a fee | hand |
| `tools/study/slice_pnl.py` | the same maker per slice, as two joinable tables | both |
| `tools/study/venue_ratio.py` | instruments ranked by capture against their own move | hand |
| `tools/study/viability.py` | the eight closed lines, re-derived | gate |
| `tools/study/volatility_window.py` | a windowed true range and its median split | both |
| `tools/study/walk_forward.py` | chosen on the past, scored on the next window | both |
| `tools/live/venue_relay.py` | market data to the engine, and the order leg both ways on the venue's grid | both |
| `tools/live/websocket_sender.py` | a local venue streaming two recorded files | hand |
| `tools/live/soak_push.sh` | a session's ping, reconnect and sequence counters | hand |
| `tools/live/trace_order.sh` | the order that waited longest, stage by stage | hand |
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

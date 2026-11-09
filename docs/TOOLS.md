# The tools directory

*Part of the reference tier. The map of the documents is [`MAP.md`](MAP.md).*

Everything here exists to turn a claim into a number that somebody else can reproduce. There are three kinds of
file, and the directory names say which kind a tool is:

| directory | what lives there | does the gate run it? |
|---|---|---|
| `gate/` | the gate itself (`verify_l2.sh`), the six lints it calls, and `lib.sh` — the shared shell helpers | yes; this is the gate |
| `sweep/` | the measurements that vary one thing and print a table: fee, latency, limits, the signal grid, the equity schedule, the day-by-day check, the workload matrix, the risk controls | one of them (`risk_check.sh`); the rest are run by hand and their output is pasted into a document |
| `fetch/` | getting data in and cutting it down: the five venue fetchers, `slice.py`, and `inspect_source.py` for a file nobody has parsed before | two of them, as self-tests |
| `study/` | the studies: capacity, the instrument screen, the venue ratio, viability, the ring table, the queue calibration, the interval, and the benchmarks that drive a window at a time | three of them, as self-tests |
| `live/` | the things that speak to a socket: the venue relay, the WebSocket sender, the push-chain soak, the order tracer | no |
| `bench/` | two harnesses for the performance line: the paired A/B runner and the libc call counter | no |
| `legacy/` | the CSV and UDP generators for the L1 path that the demo and `data/scenarios/` still use | no |

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
  question once, and in `fetch/` if it is about getting or cutting data.

# User Guide

ToyQuant is a measurement project with a small C++20 market-making simulator inside it — the eight markets it closed
are in the [Retrospective](RETROSPECTIVE.md) and the argument for the instrument is in the
[Design Review](DESIGN_REVIEW.md). **This document is the how-to-run reference**: modes, flags, inputs, outputs and
troubleshooting. The mainline is `l2_replay` on Deribit trades and incremental depth, which is the only path that models
the queue a quote waits behind; a Binance Trade+BBO replay and the legacy CSV and UDP modes remain for compatibility and
for the regression invariants that pin them. It does not explain *why* the software is shaped the way it is — that is
[Design Review](DESIGN_REVIEW.md) — and it is not the module-by-module implementation reference, which is
[Architecture](ARCHITECTURE.md).


## Which page do I need?

| I want to... | Read this | Skip |
|---|---|---|
| see it work once | [1. Quickstart](using/01-getting-started.md) | everything else, until you want more |
| understand what a run prints | [6. Reading the output](using/06-reading-the-output.md) + [the glossary](GLOSSARY.md) | -- |
| look up a flag | [the L2 replay flags](L2_FLAGS.md) | the guide's prose |
| pick a mode (CSV, UDP, Binance, L2, ws, carry) | [2. Modes](using/02-modes.md) | -- |
| replay the mainline properly | [3. The L2 replay](using/03-l2-replay.md), part 1 | parts 2 and 3 unless the venue or funding is your question |
| connect to a live feed | [4. A live session and the carry line](using/04-live-and-carry.md) | -- |
| explain a number that surprised me | [6. Reading the output](using/06-reading-the-output.md), then [the glossary](GLOSSARY.md) | -- |
| fix a failure | [7. Troubleshooting](using/07-troubleshooting.md) | -- |
| write a strategy | [8. Writing your own strategy](using/08-writing-a-strategy.md) | the rest of the guide |

## The guide, in eight parts

Eight parts, in the order a session actually goes: get it built, pick a mode, replay the mainline, run it live, read what
came out, fix what went wrong -- and, once you want a strategy of your own, write one.

**Only part 1 is needed to see the project work.** It ends with a replay of the mainline, which is the thing every number
in this repository came from. Parts 2 and 3 are the reference for that path -- flags, inputs, and what each one does to a
run -- and parts 4, 5, 7 and 8 are for when you need them: a live session, the synthetic scenarios, a failure, or a
strategy of your own.

| part | what it covers |
|---|---|
| [1. Quickstart](using/01-getting-started.md) | the whole system on one page: one diagram, one scoreboard of the eight closures, three commands, and the flag dictionary folded away |
| [2. Modes](using/02-modes.md) | the four input modes, what each is for, and the legacy CSV and UDP commands |
| [3. The L2 replay](using/03-l2-replay.md), in three parts | the mainline: the two inputs, one run, the venue side, funding and the benchmark -- with every flag in [the flag reference](L2_FLAGS.md) |
| [4. A live session and the carry line](using/04-live-and-carry.md) | the WebSocket session over a local sender, and the two-legged funding carry |
| [5. Scenarios and reports](using/05-scenarios-and-reports.md) | the synthetic tick files, and the static report with a plot |
| [6. Reading the output](using/06-reading-the-output.md) | what each line reports, and how sure a number is |
| [7. Troubleshooting](using/07-troubleshooting.md) | the failures that waste an afternoon, and what each one means |
| [8. Writing your own strategy](using/08-writing-a-strategy.md) | the interface, the two seams, how a name reaches the binary, and what a strategy may not own |

What a *field* measures is in the [Learning Path](LEARNING.md) field reference and the terms behind it are in
[the glossary](GLOSSARY.md) -- this guide says how to produce a number and where it appears.


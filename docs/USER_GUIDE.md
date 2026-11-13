# User Guide

*How to run it: modes, inputs, outputs and the failures that waste an afternoon. What the instrument found is
[the retrospective](RETROSPECTIVE.md); why it is built this way is the [design review](DESIGN_REVIEW.md).*

The mainline is `l2_replay` on venue trades and incremental depth -- the only path that models the queue a quote waits
behind. `ws` is that path with a socket where the file was, and `carry` is a closed line of its own; the Binance
Trade+BBO replay and the legacy CSV and UDP inputs are a compatibility path, folded away on the modes page.

**One page is enough to start.** [The quickstart](using/01-getting-started.md) is one diagram, the eight closures and
three commands; run it and stop. The seven pages below answer specific questions rather than forming a course, and each
opens with the question it answers.

| # | page | what it covers | open it when |
|---|---|---|---|
| 1 | [Quickstart](using/01-getting-started.md) | the whole system on one page: the path, the eight closures, three commands, the flag dictionary folded away | you have not built it yet |
| 2 | [Modes](using/02-modes.md) | the two L2 paths and the closing carry line, with the legacy modes folded away | your data is not the Deribit L2 pair |
| 3 | [The L2 replay](using/03-l2-replay.md), in three parts | the two inputs, one run, the venue side, funding and the benchmark | you are running the mainline properly |
| 4 | [A live session and the carry line](using/04-live-and-carry.md) | the WebSocket session over a local sender, and the two-legged funding carry | your input is a socket, or your question is funding |
| 5 | [Scenarios and reports](using/05-scenarios-and-reports.md) | the synthetic tick files and the static report with a plot | you want a file you can check against a known shape |
| 6 | [Reading the output](using/06-reading-the-output.md) | what each line reports, and how sure a number is | a number surprised you |
| 7 | [Troubleshooting](using/07-troubleshooting.md) | the failures that waste an afternoon, and what each one means | a run died or printed nothing |
| 8 | [Writing your own strategy](using/08-writing-a-strategy.md) | the interface, the two seams, how a name reaches the binary, what a strategy may not own | you want a decision layer of your own |

Every flag lives in [the flag reference](L2_FLAGS.md); what a *field* measures is in the [Learning Path](LEARNING.md)
and what a term means is in [the glossary](GLOSSARY.md). This guide says how to produce a number and where it appears.


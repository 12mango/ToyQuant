# Performance History

This is the lab notebook: every measurement taken, in the order it was taken, superseded ones included. It is not the
place to read conclusions -- [Design Review](DESIGN_REVIEW.md) carries those and [Retrospective](RETROSPECTIVE.md) says
which markets they closed -- but it is the place to find out *how a number was obtained*.

**The four parts' numbers are retained outside the public tree.** A part is a trading result as much as a measurement --
sizes, fills and money on named instruments -- so the full text lives privately (`private/docs/performance/`) and each page
here is its public face: the question it asked, the method it used, and a pointer to the public page that documents that
method. The split is enforced by `tools/gate/check_leaks.py`, which refuses a tracked secret, a machine path or a private
file, and reports the count of what is still public.

It is four parts, split by what the measurements were about rather than by date, because that is how a reader arrives:
with a question about the engine, about the mainline strategy, about one of the studies, or about a venue that is not a
crypto perpetual.

| part | what it holds |
|---|---|
| [1. The engine and the L1 line](performance/history-01-engine.md) | the workload and host, the pre-queue-model L1 line, quote width against the fee, the levers and the invariant they do not move, and the five-day out-of-sample check |
| [2. The L2 mainline](performance/history-02-l2.md) | the L2 replay path itself: the workload, the gprof preparation, the paired baseline, the 27-cell strategy regression matrix, the long-window profile, the buffered line reader and the stage timings that decided the container |
| [3. The studies](performance/history-03-studies.md) | everything the instrument was pointed at after the mainline worked, on the crypto slice: the queue distribution, funding as an input, the acceptance number, the maker fee each window needed, the decision layer, the queue model and the fee floor, the volatility floor and why it is not built, the non-market-making signal, the execution line, the pair trade and the pre-trade controls |
| [4. Where the fee is not the wall](performance/history-04-the-fee-is-not-the-wall.md) | the venues outside the perpetuals: the SPY, AAPL and TSLA windows, the latency sweep, the screen that found the symbols, what a fill was worth later, the account's fee schedule, and the MU sweeps that closed the quote-placement axis |

Two things about reading it:

- **A number that has been superseded is still here.** That is the point of a notebook: the row that changed is the
  evidence that something was learned, and deleting it would make the current row look like it was always obvious.
- **Every part starts with the question it answers.** If you want "what did the fee do to the maker line", the answers
  are in part 3; if you want "how did the engine get 2x faster", they are in part 2. The
  [Low-Latency Design](performance/design.md) document holds the *why* of each optimisation; this one holds the
  measurement, and the two are meant to be read together.

The workload and the host are described at the top of part 1, and the machinery that keeps all of this honest is described
in [Design Review](DESIGN_REVIEW.md) section 4.

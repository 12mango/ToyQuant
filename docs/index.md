# ToyQuant Documentation

ToyQuant is a measurement project with a small trading simulator inside it. The question it asks is whether a personal
account's programmatic quote can pay for itself, and the answer it recorded is no in eight markets — each closure a
quotient rather than an opinion, from the fee against the tick to the inventory's own move against the edge. The
simulator is the instrument that made those answers computable, not the product: it has no live venue connectivity and
claims nothing about future profitability.

What makes this one project rather than a notebook is that **most of its claims are re-derived by the build**. A
document carries the command behind a number, `tools/gate/verify_l2.sh` re-runs that command on every edit, and a figure
that moves fails the gate.

## Three tiers

Twelve documents at one level was too many to read in any order, so they are in three tiers. The tier is the whole of
the reading advice: **read tier 1, look things up in tier 2, and open tier 3 only when you want the evidence for one
specific claim.**

| Tier | What it is | Documents | How to read it |
|---|---|---|---|
| **1. The answer and the argument** | what the project found, and why the instrument that found it is shaped the way it is | [Retrospective](RETROSPECTIVE.md), [Design Review](DESIGN_REVIEW.md), [User Guide](USER_GUIDE.md) | in that order; about ninety minutes |
| **2. The reference** | what exists, how to read the code, what each field means, what is left to do | [Architecture](ARCHITECTURE.md), [Main Line](MAINLINE.md), [Learning Path](LEARNING.md), [Performance](PERFORMANCE.md), [Roadmap](ROADMAP.md), [Data Files](../data/README.md), [Tools](../tools/README.md) | open the one that matches the question; none is meant to be read whole |
| **3. The evidence** | the measurements in the order they were taken, and the research record behind the closures | [Performance History](PERFORMANCE_HISTORY.md) and its three parts, [Low-Latency Design](performance/design.md), [The studies](archive/studies.md), [Research Notes](archive/research-notes.md) | search it; every part opens with the question it answers |


## Pick the path

| If you want to... | Read | Then |
|---|---|---|
| see what the project found | [Retrospective](RETROSPECTIVE.md) | [Design Review](DESIGN_REVIEW.md) for how each closure was measured |
| evaluate the design | [Design Review](DESIGN_REVIEW.md) | [Architecture](ARCHITECTURE.md) for what exists |
| understand market making | [Learning Path](LEARNING.md) | [Design Review](DESIGN_REVIEW.md) section 4 for the gate |
| read the code | [Main Line](MAINLINE.md) | the module reference in [Architecture](ARCHITECTURE.md) |
| run it | [User Guide](USER_GUIDE.md) | the [Data Files](../data/README.md) note for the input contracts |
| review the plan | [Roadmap](ROADMAP.md#current-iteration-plan-maintained) | [The studies](archive/studies.md) for why each line is closed |
| find the tool behind a number | [tools/README.md](../tools/README.md) | the marked block next to the number, which names the exact command |
| know what it costs | [Performance](PERFORMANCE.md) | [Low-Latency Design](performance/design.md) for why each change is faster |
| see how a number was obtained | [Performance History](PERFORMANCE_HISTORY.md) | the part named in whichever document quoted it |

`docs/` holds tiers 1 and 2 at the top level, and two directories for tier 3: `performance/` for the measurement of the
engine and `archive/` for the research record. Nothing in tier 3 is unfinished or disowned — it is the material that made
the closures in tier 1 possible, kept because a reason that is not written down is a reason nobody can check.

## Who owns what

Each document owns one kind of claim, and the fourth column is the part that keeps them from growing into each other. A
document that starts explaining something in another document's row is the failure this table exists to prevent.

| Document | Tier | What it owns | Not the place for | Kept honest by |
|---|---|---:|---|---|
| [Retrospective](RETROSPECTIVE.md) | 1 | the eight closures, the number that closed each, the rules that came out of them | what exists in the code, or how to run it | prose linted |
| [Design Review](DESIGN_REVIEW.md) | 1 | why the instrument is built this way, the C++ decisions with anchors, the headline results, the questions a reader asks, the limitations | the module reference, or the raw measurement records | prose linted; the size, test count, flags and tool paths it states are re-counted |
| [User Guide](USER_GUIDE.md) | 1 | every mode, every flag, the data contracts, and the failure modes that waste an afternoon: an index over seven parts in `docs/using/` | why anything is designed the way it is, or what a field means | prose linted, so a flag it forgets or invents fails the build |
| [Architecture](ARCHITECTURE.md) | 2 | what each module is, the contracts between them, the matching rules: an index over six parts in `docs/architecture/` | the rationale and the trade-offs | marked blocks re-run on every gate run |
| [Main Line](MAINLINE.md) | 2 | the order to read the source in, the measured result each step produced, the verdict on what to keep, freeze or delete | how to run anything, or the mechanism behind a result | prose linted |
| [Learning Path](LEARNING.md) | 2 | the mechanisms the instrument has to model, and the field reference for each output line | engineering decisions, or how to run anything | prose linted |
| [Performance](PERFORMANCE.md) | 2 | workloads, how to measure on this host, the current stage breakdown, and every optimisation attempted | the mechanism of a change, or the whole history | its ledger is paired measurements from `tools/study/measure_l2_baseline.py` |
| [Roadmap](ROADMAP.md) | 2 | the maintained plan: coverage, backlog, gaps, stop line, what is deliberately not planned | what exists today, or the record of finished studies | prose linted |
| [Data Files](../data/README.md) | 2 | the tick CSV format, the runtime outputs, the workflow that produces them | anything about the strategy or the design | the data directory it describes is the one the gate reads |
| [Tools](../tools/README.md) | 2 | which script answers which question, and which of them the gate runs | the results themselves | the documents that cite a tool are prose linted, so a moved script fails the build |

## What the project covers, and what it deliberately does not

The loop is feed adapters, an order book, a strategy, a matching engine with an explicit queue model, execution reports,
a portfolio, and a session that can be written down and taken over, in C++20 with CMake and a Python 3 toolchain for the
measurement tools.

It is not a production exchange stack, a connectivity layer, or a live-trading system. There is no exchange connectivity
or authentication, no market impact, no queue-position feedback from the strategy's own resting size, no calendar or
contract lifecycle, and no portfolio risk model beyond signed notional. The pre-trade risk pipeline is in-process (kill
switch, fat finger, rate, exposure, position), not a broker or exchange risk gateway. Each of those is a limitation in
the [Design Review](DESIGN_REVIEW.md) and a boundary with a reason in the
[Roadmap](ROADMAP.md#deliberately-not-planned), rather than a gap that nobody wrote down.

What keeps this table true is mechanical, which is the only reason to trust it: `tools/gate/lint_prose.py` re-counts every
flag, queue model, tool path and size figure a document states, `tools/gate/check_docs.py` re-runs the command behind
every quoted number, and `tools/gate/check_links.py` follows every link and anchor in the three tiers.

| [Performance History](PERFORMANCE_HISTORY.md) | 3 | the measurements in the order they were taken, superseded ones included: one index and three parts | the current conclusions | every marked block re-run on every gate run |
| [Low-Latency Design](performance/design.md) | 3 | why each low-latency change is faster: the problem, the mechanism, the designs that were rejected | the numbers themselves | prose linted |
| [The studies](archive/studies.md) | 3 | the research record: the phases, the venues, the carry gates, the options line, the plan from a positive window to a running system | what is still open | prose linted |
| [Research Notes](archive/research-notes.md) | 3 | external cases of individual quantitative trading, and what a personal account can do about the fee | any measurement of this repository, or a plan | every external claim carries a `[read]`, `[reported]` or `[not verified]` mark |


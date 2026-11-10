# The documentation map

*The three tiers, the path to take for a question, and which document owns which claim. The front door is
[`index.md`](index.md); this page is the map behind it.*

## How the navigation groups these

The site's navigation puts the same documents on four shelves, because that is how a reader walks in: **The Core** --
how to run it and what each module is; **Reference** -- the numbers, the data and the tools; **The Vault** -- the
retrospectives, the histories and the decisions; **Project meta** -- the plan, the design review and this map.

That is a different cut from the three tiers below, and the difference is worth stating rather than hiding. A **tier** is
what a document may claim -- an answer, a reference, or evidence -- while a **shelf** is where a reader will look for it.
So [Architecture](ARCHITECTURE.md) and [Roadmap](ROADMAP.md) are tier 2 and sit on different shelves, and
[Design Review](DESIGN_REVIEW.md) is tier 1 but sits under Project meta, because it is where a reader who asks "what does
this project deliberately not do?" arrives. The tier table is the authority on what a document owns; the navigation is
the route to it.

## Three tiers

Twelve documents at one level was too many to read in any order, so they are in three tiers. The tier is the whole of the
reading advice: **read tier 1, look things up in tier 2, and open tier 3 only when you want the evidence for one specific
claim.**

| Tier | What it is | Documents | How to read it |
|---|---|---|---|
| **1. The answer and the argument** | what the project found, why the instrument that found it is shaped the way it is, and what that line of work did not measure | [Retrospective](RETROSPECTIVE.md), [Low-Latency Retrospective](LOW_LATENCY.md), [Design Review](DESIGN_REVIEW.md), [User Guide](USER_GUIDE.md) | in that order; about ninety minutes |
| **2. The reference** | what exists, how to read the code, what each field means, what is left to do | [Architecture](ARCHITECTURE.md), [Main Line](MAINLINE.md), [Learning Path](LEARNING.md), [Performance](PERFORMANCE.md), [Roadmap](ROADMAP.md), [Data Files](DATA.md), [Tools](TOOLS.md) | open the one that matches the question; none is meant to be read whole |
| **3. The evidence** | the measurements in the order they were taken, the research record behind the closures, and one file per technical decision | [Performance History](PERFORMANCE_HISTORY.md) and its three parts, [Low-Latency Design](performance/design.md), [The studies](archive/studies.md), [Research Notes](archive/research-notes.md), [Decisions](adr/index.md) | search it; every part opens with the question it answers, and a decision is one file you can send as a link |

## Pick the path

| If you want to... | Read | Then |
|---|---|---|
| see what the project found | [Retrospective](RETROSPECTIVE.md) | [Design Review](DESIGN_REVIEW.md) for how each closure was measured |
| see what the engine work returned, and why it stopped | [Low-Latency Retrospective](LOW_LATENCY.md) | [Performance](PERFORMANCE.md#5-optimization-ledger) for the rows |
| evaluate the design | [Design Review](DESIGN_REVIEW.md) | [Architecture](ARCHITECTURE.md) for what exists |
| understand market making | [Learning Path](LEARNING.md) | [Design Review](DESIGN_REVIEW.md) section 4 for the gate |
| read the code | [Main Line](MAINLINE.md) | the module reference in [Architecture](ARCHITECTURE.md) |
| run it | [User Guide](USER_GUIDE.md) | the [Data Files](DATA.md) note for the input contracts |
| review the plan | [Roadmap](ROADMAP.md#current-iteration-plan-maintained) | [The studies](archive/studies.md) for why each line is closed |
| find the tool behind a number | [Tools](TOOLS.md) | the marked block next to the number, which names the exact command |
| know what it costs | [Performance](PERFORMANCE.md) | [Low-Latency Design](performance/design.md) for why each change is faster |
| see how a number was obtained | [Performance History](PERFORMANCE_HISTORY.md) | the part named in whichever document quoted it |
| send someone one decision | [Decisions](adr/index.md) | the record's evidence section |
| find the meaning of a term in a table | [Glossary](GLOSSARY.md) | the field that prints it, in the [Learning Path](LEARNING.md) |
| write your own strategy | [Writing your own strategy](using/08-writing-a-strategy.md) | [the strategy layer](architecture/05-strategy.md) for how the existing ones decide |

## Who owns what

Each document owns one kind of claim, and the last column is the part that keeps them from growing into each other. A
document that starts explaining something in another document's row is the failure this table exists to prevent.

| Document | Tier | What it owns | Not the place for | Kept honest by |
|---|---|---:|---|---|
| [Retrospective](RETROSPECTIVE.md) | 1 | the eight closures, the number that closed each, the rules that came out of them | what exists in the code, or how to run it | prose linted |
| [Low-Latency Retrospective](LOW_LATENCY.md) | 1 | the engine line as a whole: what it was for, the order the attempts came in, what returned, the condition that stopped it, and what it never measured | a number, the mechanism of a change, or the measurement record | prose linted; every figure it needs is a link to a block the gate re-runs |
| [Design Review](DESIGN_REVIEW.md) | 1 | why the instrument is built this way, the C++ decisions with anchors, the headline results, the questions a reader asks, the limitations | the module reference, or the raw measurement records | prose linted; the size, test count, flags and tool paths it states are re-counted |
| [User Guide](USER_GUIDE.md) | 1 | every mode, every flag, the data contracts, and the failure modes that waste an afternoon: an index over seven parts in `docs/using/` | why anything is designed the way it is, or what a field means | prose linted, so a flag it forgets or invents fails the build |
| [Architecture](ARCHITECTURE.md) | 2 | what each module is, the contracts between them, the matching rules: an index over six parts in `docs/architecture/` | the rationale and the trade-offs | marked blocks re-run on every gate run |
| [Main Line](MAINLINE.md) | 2 | the order to read the source in, the measured result each step produced, the verdict on what to keep, freeze or delete | how to run anything, or the mechanism behind a result | prose linted |
| [Learning Path](LEARNING.md) | 2 | the mechanisms the instrument has to model, and the field reference for each output line | engineering decisions, or how to run anything | prose linted |
| [Performance](PERFORMANCE.md) | 2 | workloads, how to measure on this host, the current stage breakdown, and every optimisation attempted | the mechanism of a change, or the whole history | its ledger is paired measurements from `tools/study/measure_l2_baseline.py` |
| [Roadmap](ROADMAP.md) | 2 | the maintained plan: coverage, backlog, gaps, stop line, what is deliberately not planned | what exists today, or the record of finished studies | prose linted |
| [Data Files](DATA.md) | 2 | the tick CSV format, the runtime outputs, the workflow that produces them | anything about the strategy or the design | the data directory it describes is the one the gate reads |
| [Tools](TOOLS.md) | 2 | which script answers which question, and which of them the gate runs | the results themselves | the documents that cite a tool are prose linted, so a moved script fails the build |

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

What keeps these tables true is mechanical, which is the only reason to trust them: `tools/gate/lint_prose.py` re-counts
every flag, queue model, tool path and size figure a document states, `tools/gate/check_docs.py` re-runs the command
behind every quoted number, `tools/gate/check_links.py` follows every link and anchor in the three tiers, and
`tools/gate/check_adr.py` holds each decision's status to the table that lists it.

## How the tree is laid out

`docs/` holds tiers 1 and 2 at the top level. `using/` and `architecture/` hold the parts of the two longest references --
the guide and the module reference -- and a part is split into smaller pages where it grew long. `performance/` and `archive/` hold tier 3: the measurement of the engine, and the research record.
`adr/` holds one file per decision. Nothing in tier 3 is unfinished or disowned -- it is the material that made the
closures in tier 1 possible, kept because a reason that is not written down is a reason nobody can check.

| [Performance History](PERFORMANCE_HISTORY.md) | 3 | the measurements in the order they were taken, superseded ones included: one index and three parts | the current conclusions | every marked block re-run on every gate run |
| [Low-Latency Design](performance/design.md) | 3 | why each low-latency change is faster: the problem, the mechanism, the designs that were rejected | the numbers themselves, or the line's narrative | prose linted |
| [The studies](archive/studies.md) | 3 | the research record: the phases, the venues, the carry gates, the options line, the plan from a positive window to a running system | what is still open | prose linted |
| [Research Notes](archive/research-notes.md) | 3 | external cases of individual quantitative trading, and what a personal account can do about the fee | any measurement of this repository, or a plan | every external claim carries a `[read]`, `[reported]` or `[not verified]` mark |
| [Decisions](adr/index.md) | 3 | one file per technical decision that closed a question: the context, the decision, what it costs, and the two or three links that measure it | the numbers themselves, or the research behind a market | a record quotes no figure it does not own, so every number in it is a link to a block the gate re-runs and the record cannot drift |
| [Glossary](GLOSSARY.md) | 2 | the concepts behind the numbers: one formula per term, the sign convention, and the file that computes it | the field list, which the learning path owns, or the measurement record | prose linted; every formula names the file it comes from |
| [Writing your own strategy](using/08-writing-a-strategy.md) | 1 | how a strategy reaches the binary: the interface, the two seams, registration, and what a strategy may not own | the strategy layer's design, which the architecture owns | prose linted, so a flag or a class it forgets or invents fails the build |

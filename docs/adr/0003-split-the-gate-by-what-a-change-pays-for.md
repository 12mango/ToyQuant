# 0003. Split the gate by what a change pays for

- **Status**: accepted
- **Date**: 2026-11-10
- **Supersedes**: none

## Context

Tier 1 was one undivided hundred-plus seconds of checks, so anything that called the gate paid for all of it: a
documentation edit paid to re-derive every quoted number, and a one-line change under `src/` paid the same. The
[working rules](https://github.com/12mango/ToyQuant/blob/main/.clinerules) had made that mandatory for every edit, which
turned the gate into a tax on the edit loop rather than a check on it.

The costs were measured before anything was cut, and they are not the costs the header had been claiming: nearly all of
tier 1's time is one document page whose blocks replay vendor windows at roughly a hundred times the cost of the
repository's own slices, while the invariants over the code cost a fraction of that. The measured split is in the
[gate's own header](https://github.com/12mango/ToyQuant/blob/main/tools/gate/verify_l2.sh), which is where a cost claim about the gate belongs and where it is
now checked by reading rather than remembered.

## Decision

Tier 1 is two halves with separate verbs, and the whole tier is a commit-time cost:

- `smoke` — two replays and the six fields that catch a broken line, about a second;
- `code` — tier 0 plus the invariants and the source-level checks, for a change under `src/` or `tests/`;
- `docs [pages]` — the document half, on the whole set or on the pages a change touched;
- `fast` — both halves, once per task; `all` stays the acceptance gate.

## Consequences

- **What it forbids**: "run the gate after every edit". The loop for a source change is a build plus `smoke` or `code`;
  for a document change it is `docs` on the pages that moved.
- A page argument is not a path to a program. The verb that takes pages gets its binary from an environment variable, and
  the dispatch says why — that bug surfaced on the verb's first run.
- The halves can disagree about a change: `code` green and `docs` red is the normal shape of a change that moved a number
  in a document and not in the code, and it is the state `fast` exists to catch once.
- Cost claims about the gate are now read from the header rather than remembered, because the previous one had been wrong
  by an order of magnitude for months and nothing was checking it.
- **To revisit**: if the document half's cost moves into a page that a change does not touch, the fix is to split that
  page, not to widen the verb.

## Evidence

- [`tools/gate/verify_l2.sh`](https://github.com/12mango/ToyQuant/blob/main/tools/gate/verify_l2.sh) — the header's measured table and the dispatch.
- [Tools: where to start](../TOOLS.md) — which verb a reader should use.
- [Roadmap: change log](../ROADMAP.md#change-log) — the decision as a scope row, for the row's own audience.

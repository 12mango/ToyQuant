# 0004. The document tree keeps its names; what it gains is addressability

- **Status**: accepted
- **Date**: 2026-11-10
- **Supersedes**: none

## Context

A proposal to reorganise the documents along the usual four-layer scheme arrived twice: rename `docs/archive/` to
`docs/research/`, add a `QUICKSTART.md` beside the getting-started guide, slim `PERFORMANCE.md` by moving its optimisation
ledger out, and introduce decision records. The proposal's diagnosis was right — the documents were long and the layers
were implicit — and its prescription was half already in place under other names: `docs/index.md` had a three-tier map
with a "what this document is not" column, the guide and the module reference were already split into parts, and the
research record was already isolated in `docs/archive/`.

Measured before deciding: the three longest files carry two fifths of all document lines, and they are all vault material;
`PERFORMANCE.md#5-optimization-ledger` has fifteen structural inbound links and `docs/archive/studies.md` has seven.

## Decision

1. **The tree keeps its names.** `archive/` stays; a rename buys vocabulary and costs seven inbound links plus the
   navigation. The layer a reader needs is not a directory name, it is a file they can send.
2. **Add the layer that was actually missing: `docs/adr/`**, one file per technical decision, append-only, with a status
   line and consequences — see [the index](index.md).
3. **Do not add a second quick-start.** The four run modes, the recorder and the two-thread path already have their own
   pages; a `QUICKSTART.md` would be the second copy of a command sequence, which is the drift this repository spends its
   gates on.
4. **Do not split the optimisation ledger.** Fifteen structural links and a `Ledger row:` convention in the low-latency
   design make it load-bearing; a stub page would keep the anchors alive at the cost of a permanent second home.
5. **Do shorten the front door.** `README.md` carried a closures table that duplicated the retrospective and a quick
   start that duplicated the guide, so it now carries the pitch, one command and the hook into the vault.
6. **Check the records mechanically**: `tools/gate/check_adr.py` holds each status line to the index table and refuses a
   supersede that only one end states.

## Consequences

- The vault is entered through [the decisions index](index.md) and the retrospective, not through a renamed directory.
- A future agent will propose the rename again; this record is the answer, and pointing at it costs one link.
- The document set is longer-lived than any file layout: `index.md`'s three tiers are the map, so a file can move without
  the reader's route changing.
- **What this forbids**: reorganising for the sake of the vocabulary, and any second copy of a command sequence.
- **To revisit**: if `docs/` ever exceeds what one map page can describe, the fix is a fourth tier page, not a rename.

## Evidence

- [The documentation map](../index.md) — the three tiers and what each document must not contain.
- [Roadmap: change log](../ROADMAP.md#change-log) — the same decision as a scope row.
- [`tools/gate/check_adr.py`](https://github.com/12mango/ToyQuant/blob/main/tools/gate/check_adr.py) — the status/index
  agreement, with its own four-case self-test.

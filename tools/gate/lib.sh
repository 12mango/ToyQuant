#!/usr/bin/env bash
# Shared shell helpers for the tools that read a run's output. Sourced, never executed: every script that
# needs these changes to the repository root first, so `. tools/gate/lib.sh` resolves from wherever it was started.

# `field_of <output> <name>` returns the first value of a `name=value` pair in a run's output.
#
# The boundary is the whole of it. A field name can only begin where the character before it is not a word
# character, and that is not a stylistic choice: `tools/gate/check_docs.py` reads recorded blocks with the Python
# rule `([A-Za-z_][A-Za-z0-9_]*)=`, whose name group is greedy, so the only place a name can start is a
# position the longer name did not already consume. A single L2 run's output contains eight field names that
# are the tails of longer ones -- `gross_closed_pnl_usd` ends in `closed_pnl_usd`, `buy_fill_count` in
# `fill_count` -- so a rule that matched without this boundary would read the gross figure as the closed one.
#
# Four of the tools that needed this carried a `(^| )` variant instead, which is narrower: a field following
# a `]` or a `,` would be missed. Measured on a 115-field run the two agree on every field, so neither had
# produced a wrong number, but only one of them is the rule the Python side actually applies.
field_of() {
  printf '%s' "$1" | grep -oE "(^|[^A-Za-z0-9_])$2=[^[:space:]]+" | head -n 1 |
    sed 's/^[^A-Za-z0-9_]//; s/^[A-Za-z_][A-Za-z0-9_]*=//'
}

# `emit_block <lines>` prints a marked block: the marker, the `run:`/`then:` lines, and the close.
#
# The marker text is matched exactly by `tools/gate/check_docs.py`, so a script that printed the block by hand could
# drift from the checker by one character and its numbers would silently stop being re-derived -- which is the one
# failure a recorded number cannot afford. The four sweeps that emit blocks call this instead of repeating it, and
# the argument is the whole middle of the block, newline-terminated.
emit_block() {
  printf '<!-- toyquant:check\n%s-->\n' "$1"
}

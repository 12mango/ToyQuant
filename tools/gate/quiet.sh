#!/usr/bin/env bash
# Run one command and print a bounded slice of its output, because a terminal command whose output is large loses the
# shell-integration marker that tells the tool the command finished.
#
# Measured 2026-11-25 on this host, deliberately, after a session lost 25 completions: `seq 1 1000 | sed ...` -- about
# 10 KB of short lines -- lost its completion and the tool opened a new terminal, while a single 4000-character line,
# `sleep 3; echo` and `sleep 12; echo` all reported normally. The trigger is therefore the volume, not the delay and not
# the line length, and the shape that costs a round trip is a `cat` of a CSV, an `ls` of twenty-five names or a `ps` of
# thirty rows. The whole output goes to its own file, and only the first and last few lines reach the terminal.
#
#   bash tools/gate/quiet.sh -- bash .run.sh tail fast
#   bash tools/gate/quiet.sh 5 -- grep -n 'fills=' /tmp/tq/lad_nes.log
#
# The exit status is the command's own, so a gate can be wrapped without changing what a caller sees.
set -u
# The guard every tool opens with, enforced by verify_l2.sh: a script that only works from the repository root is a
# script a reader meets broken.
cd "$(dirname "$0")/../.." || exit 1
lines=10
if [ "${1:-}" != "--" ]; then
    case ${1:-} in
        '' | *[!0-9]*)
            echo "usage: $0 [lines] -- <command...>" >&2
            exit 2
            ;;
    esac
    lines=$1
    shift
fi
if [ "${1:-}" != "--" ]; then
    echo "usage: $0 [lines] -- <command...>" >&2
    exit 2
fi
shift
if [ $# -eq 0 ]; then
    echo "$0: no command" >&2
    exit 2
fi
mkdir -p /tmp/tq
log=$(mktemp /tmp/tq/quiet.XXXXXX) || exit 2
"$@" > "$log" 2>&1
status=$?
total=$(wc -l < "$log")
printf '%s\n' "quiet: $total line(s), status $status, log $log"
if [ "$total" -le $((2 * lines)) ]; then
    cat "$log"
else
    head -n "$lines" "$log"
    printf '%s\n' "... $((total - 2 * lines)) line(s) only in the log ..."
    tail -n "$lines" "$log"
fi
exit "$status"

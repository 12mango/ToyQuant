#!/usr/bin/env bash
# Start one long command so a terminal that loses completion cannot lose the command.
#
# The failure this exists for: a foreground command that outlives the tool's budget is killed when the next one is
# typed, and that is how a post-commit sync was cut off between its manifest and its copy. Nothing here waits: the
# command is started detached from this shell, its output goes to one log, and its exit status lands in a marker
# beside it -- so "is it finished" is a file read rather than a question to a single-threaded shell.
#
#   tools/gate/detach.sh fast -- bash tools/gate/verify_l2.sh fast
#   tools/gate/detach.sh sync 120 -- bash tools/export/push_windows.sh
#
# It prints the log path. When the command is over, /tmp/<name>.exit holds its status (124 when the timeout fired).
set -u
if [ $# -lt 3 ]; then
    echo "usage: $0 <name> [timeout-seconds] -- <command...>" >&2
    exit 2
fi
name=$1
case $name in
    *[!a-zA-Z0-9_.-]* | '')
        echo "the name is one word of letters, digits, dot, dash or underscore" >&2
        exit 2
        ;;
esac
shift
limit=900
if [ "${1:-}" != "--" ]; then
    limit=$1
    case $limit in
        *[!0-9]* | '')
            echo "the timeout is whole seconds" >&2
            exit 2
            ;;
    esac
    shift
fi
if [ "${1:-}" != "--" ] || [ $# -eq 1 ]; then
    echo "usage: $0 <name> [timeout-seconds] -- <command...>" >&2
    exit 2
fi
shift

log="/tmp/${name}.log"
status="/tmp/${name}.exit"
rm -f "$log" "$status"
# `setsid`, so the child is not in this shell's process group: closing the terminal, or typing the next command,
# cannot signal it. stdin from /dev/null for the same reason -- a child that reads the terminal would hold it.
setsid bash -c 'log=$1; shift; limit=$1; shift; timeout "$limit" "$@" > "$log" 2>&1; echo "$?" > "${log%.log}.exit"' \
    _ "$log" "$limit" "$@" >/dev/null 2>&1 </dev/null &
printf '%s\n' "$log"

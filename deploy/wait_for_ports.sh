#!/bin/bash
# Wait until every address given on the command line is listening, and fail naming the one that never opened.
#
#   bash deploy/wait_for_ports.sh 127.0.0.1:8765 127.0.0.1:8766
#
# It is a file rather than an `ExecStartPre=/bin/bash -c '...'` for a reason measured on 2026-11-27: systemd parses the
# Exec line for escapes and drops the ones it does not know, so the `\.` in a regex reached bash as a bare dot and every
# start logged `Ignoring unknown escape sequences`, while the `$1` and `$)` of a shell function inside that line depend on
# two parsers agreeing about quoting. A path and two arguments have neither problem, and `bash -n` and a local run can
# check this file where a unit line cannot be checked at all.
#
# `ss` prints the local address in column 4; an exact match on that column is what "is this address listening" means, and
# a substring match would answer yes for 127.0.0.1:8766 when asked about 127.0.0.1:876.
set -u

limit=${WAIT_PORT_SECONDS:-30}
status=0
for endpoint in "$@"; do
  waited=0
  while :; do
    if ss -ltn 2>/dev/null | awk 'NR > 1 {print $4}' | grep -qx -- "$endpoint"; then
      echo "ready: $endpoint after ${waited}s"
      break
    fi
    if [ "$waited" -ge "$limit" ]; then
      echo "waited ${limit}s and nothing listens on $endpoint" >&2
      status=1
      break
    fi
    sleep 1
    waited=$((waited + 1))
  done
done
exit "$status"

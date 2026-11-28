#!/usr/bin/env bash
# Put the previous program back, restart, and say whether the session came up. The deploy copies the running binary to
# `toy_quant.prev` before it unpacks a new one, so the rollback is one `install` and one `restart` -- the version that was
# running a minute ago, which is the only version anybody has evidence for.
#
#   sudo bash deploy/rollback.sh                      # put the previous program back and check it
#   sudo bash deploy/rollback.sh --refuse <sha>       # the same, and leave the latch the next deploy reads
#
# **The latch is the deploy card's Cut 1.** A release that a running session was rolled back from is refused on the host
# until somebody changes something: `reverted_release` names it, and the deploy's remote script compares that name with the
# release it just unpacked and stops -- because a third dispatch of the unchanged thing is a change (`.clinerules`), and a
# revert that is immediately re-deployed is a loop with extra steps.
set -euo pipefail

PROGRAM=${PROGRAM:-/opt/toyquant/toy_quant}
SERVICE_NAME=${SERVICE_NAME:-toyquant-live}
LATCH=${LATCH:-/opt/toyquant/data/runtime/reverted_release}
HERE=$(cd "$(dirname "$0")" && pwd)

REFUSE=""
while [ $# -gt 0 ]; do
  case "$1" in
    --refuse) REFUSE=${2:?--refuse needs the release being refused}; shift 2 ;;
    *) echo "rollback: unknown argument $1" >&2; exit 2 ;;
  esac
done

if [ ! -x "$PROGRAM.prev" ]; then
  echo "rollback: no $PROGRAM.prev -- this host has not had a successful deploy yet" >&2
  exit 2
fi

echo "rollback: restoring $PROGRAM.prev -> $PROGRAM"
install -m 0755 "$PROGRAM.prev" "$PROGRAM"
systemctl restart "$SERVICE_NAME"
# The unit gives the session twelve seconds to cancel what it holds; three is enough for the process to be up and is what
# the deploy itself waits before the health check reads the journal.
sleep 3

# The latch is written **before** the health check, so a rollback whose own check is red still leaves the record of what
# was refused: the next reader learns the release was reverted even though the host is not healthy yet.
if [ -n "$REFUSE" ]; then
  mkdir -p "$(dirname "$LATCH")"
  printf '%s %s\n' "$REFUSE" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$LATCH"
  echo "rollback: reverted_release <- $REFUSE ($(cut -d' ' -f2- "$LATCH"))"
fi

exec bash "$HERE/check_health.sh"

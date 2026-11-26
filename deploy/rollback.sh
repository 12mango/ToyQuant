#!/usr/bin/env bash
# Put the previous program back, restart, and say whether the session came up. The deploy copies the running binary to
# `toy_quant.prev` before it unpacks a new one, so the rollback is one `install` and one `restart` -- the version that was
# running a minute ago, which is the only version anybody has evidence for.
#
#   sudo bash deploy/rollback.sh
set -euo pipefail

PROGRAM=${PROGRAM:-/opt/toyquant/toy_quant}
SERVICE_NAME=${SERVICE_NAME:-toyquant-live}
HERE=$(cd "$(dirname "$0")" && pwd)

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
exec bash "$HERE/check_health.sh"

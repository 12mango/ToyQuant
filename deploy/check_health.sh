#!/usr/bin/env bash
# Is the session alive, alone, and holding nothing it does not know about? Exit 0 only when all three answers are yes.
#
#   bash deploy/check_health.sh            # on the host
#   ssh ... 'bash /opt/toyquant/deploy/check_health.sh'   # what the deploy runs after the restart
#
# The checks are chosen so that each one names a failure the deploy can cause or the engine can have:
#
#   1. the unit is active                          -- a restart that did not take
#   2. exactly one session process                 -- two sessions on one account is a position neither sees
#   3. no error-severity journal line since boot   -- the relay's or the engine's own complaint
#   4. the binary is where the unit expects it     -- a bundle unpacked without the program
#   5. the snapshot parses and is not stale        -- a session that is running and not recording its own state
#
# The snapshot's *content* is reported, not judged: whether a position is the right one is an accounting question that
# `[RECONCILE]` in the run's own log answers, and a health check that guessed it would fail every time the market moved.
set -euo pipefail

ETC_DIR=${ETC_DIR:-/etc/toyquant}
SERVICE_NAME=${SERVICE_NAME:-toyquant-live}
PROGRAM=${PROGRAM:-/opt/toyquant/toy_quant}
SNAPSHOT=${SNAPSHOT:-/opt/toyquant/data/runtime/live-snapshot.json}
MAX_SNAPSHOT_AGE_S=${MAX_SNAPSHOT_AGE_S:-600}
failures=0

fail() {
  echo "health: FAIL $*" >&2
  failures=$((failures + 1))
}
ok() { echo "health: ok $*"; }

if ! systemctl is-active --quiet "$SERVICE_NAME"; then
  fail "$SERVICE_NAME is not active: $(systemctl is-active "$SERVICE_NAME" 2>&1 || true)"
else
  ok "$SERVICE_NAME is active"
fi

# One process, not one per deploy. `pgrep -c -f` counts the program path, which the unit's ExecStart names.
sessions=$(pgrep -c -f "$PROGRAM" 2>/dev/null || true)
sessions=${sessions:-0}
if [ "$sessions" -ne 1 ]; then
  fail "$sessions session process(es) run $PROGRAM; one host may run one"
else
  ok "one session process"
fi

if [ ! -x "$PROGRAM" ]; then
  fail "$PROGRAM is missing or not executable -- the bundle was unpacked without the program"
else
  ok "$PROGRAM present ($(stat -c '%s bytes, %y' "$PROGRAM"))"
fi

# The journal is where a refused order or a rejected key lands. `-p err` is severity 3 and above, and the window is this
# boot, because a line from last week is not this deploy's failure.
if command -v journalctl > /dev/null 2>&1; then
  errors=$(journalctl -u "$SERVICE_NAME" -b -p err --no-pager -q 2>/dev/null | wc -l)
  if [ "${errors:-0}" -gt 0 ]; then
    fail "$errors error-severity journal line(s) since boot:"
    journalctl -u "$SERVICE_NAME" -b -p err --no-pager -q 2>/dev/null | tail -5 | sed 's/^/health:   /' >&2
  else
    ok "no error-severity journal line since boot"
  fi
fi

if [ -f "$SNAPSHOT" ]; then
  if python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "$SNAPSHOT" 2>/dev/null; then
    age=$(( $(date +%s) - $(stat -c %Y "$SNAPSHOT") ))
    summary=$(python3 - "$SNAPSHOT" <<'PY'
import json, sys
state = json.load(open(sys.argv[1]))
orders = state.get("open_orders") or state.get("orders") or []
positions = state.get("positions") or {}
print(f"open_orders={len(orders)} positions={len(positions)} keys={','.join(sorted(state)[:6])}")
PY
)
    if [ "$age" -gt "$MAX_SNAPSHOT_AGE_S" ]; then
      fail "the snapshot is ${age}s old (limit ${MAX_SNAPSHOT_AGE_S}s): $SNAPSHOT"
    else
      ok "snapshot is ${age}s old; $summary"
    fi
  else
    fail "the snapshot does not parse as JSON: $SNAPSHOT"
  fi
else
  ok "no snapshot yet at $SNAPSHOT (a session that has not traded writes none)"
fi

# The two operator files, by existence and mode only: the check never reads a credential.
for file in "$ETC_DIR/live.yaml" "$ETC_DIR/live.env"; do
  if [ ! -f "$file" ]; then
    fail "$file is missing -- see deploy/bootstrap_server.sh for the templates"
  else
    mode=$(stat -c '%a' "$file")
    if [ "${mode: -1}" != "0" ]; then
      fail "$file is mode $mode; a credential file must not be readable by others"
    else
      ok "$file present (mode $mode)"
    fi
  fi
done

if [ "$failures" -ne 0 ]; then
  echo "health: $failures failure(s)" >&2
  exit 1
fi
echo "health: OK"

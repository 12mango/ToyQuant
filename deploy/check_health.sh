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
RELAY_SERVICE_NAME=${RELAY_SERVICE_NAME:-toyquant-relay}
PROGRAM=${PROGRAM:-/opt/toyquant/toy_quant}
RELAY_PROGRAM=${RELAY_PROGRAM:-/opt/toyquant/tools/live/venue_relay.py}
SNAPSHOT=${SNAPSHOT:-/opt/toyquant/data/runtime/live-snapshot.json}
MAX_SNAPSHOT_AGE_S=${MAX_SNAPSHOT_AGE_S:-600}
# How long the check waits for a unit that has just been restarted. A fixed `sleep 3` in front of this script was the
# first version, and it is the wrong shape: three seconds is either too short (the unit is still `activating`) or wasted
# (it came up in one). This waits for the state and then judges it, bounded so a unit that never comes up fails the check
# instead of hanging it.
WAIT_ACTIVE_S=${WAIT_ACTIVE_S:-15}
# How far back the journal questions look, and how much of the two units' own output a failure attaches. The systemd state
# is what a unit *is*; its stdout is what it *said*, and the first three deploys each cost a round trip because the report
# quoted the first and not the second (the sentence that explained them was one `journalctl` away).
HEALTH_WINDOW_S=${HEALTH_WINDOW_S:-600}
HEALTH_JOURNAL_LINES=${HEALTH_JOURNAL_LINES:-60}
failures=0

wait_active() {  # wait_active <unit>
  local unit=$1 waited=0
  while [ "$waited" -lt "$WAIT_ACTIVE_S" ]; do
    [ "$(systemctl is-active "$unit" 2>/dev/null || true)" = active ] && return 0
    sleep 1
    waited=$((waited + 1))
  done
  return 1
}

fail() {
  echo "health: FAIL $*" >&2
  failures=$((failures + 1))
}
ok() { echo "health: ok $*"; }

if ! systemctl is-active --quiet "$SERVICE_NAME"; then
  if wait_active "$SERVICE_NAME"; then
    ok "$SERVICE_NAME is active (after $WAIT_ACTIVE_S s or less)"
  else
    fail "$SERVICE_NAME is still $(systemctl is-active "$SERVICE_NAME" 2>&1 || true) after ${WAIT_ACTIVE_S}s"
  fi
else
  ok "$SERVICE_NAME is active"
fi

# Why a unit is not running is a different question from whether it is, and systemd keeps the answer. `NRestarts` above
# zero means it has restarted since the unit was last started by hand; `ExecMainStatus` is the number `journalctl` shows
# as `status=N` (4 is SIGILL, 139 a segfault); `Result` names the class. One property a line, because `systemctl show`
# prints several `--value`s in its *own* order and a single line of them cannot be labelled honestly.
for property in ActiveState SubState Result NRestarts ExecMainStatus; do
  printf 'health: ok %s %s=%s\n' "$SERVICE_NAME" "$property" \
    "$(systemctl show -p "$property" --value "$SERVICE_NAME" 2>/dev/null)"
done
restarts=$(systemctl show -p NRestarts --value "$SERVICE_NAME" 2>/dev/null || echo 0)
if [ "${restarts:-0}" -gt 0 ]; then
  fail "$SERVICE_NAME has restarted $restarts time(s) since it was last started by hand: a unit that is up now may be up between crashes"
fi
# A crash is not always an *error-severity* journal line -- systemd writes "Failed with result 'core-dump'" at its own
# severity, and the first real deploy produced 35 of them while `-p err` showed none. The window is the last
# `HEALTH_WINDOW_S` seconds rather than "since boot": a counter that only a reboot clears would keep a repaired unit red
# forever, which is the same failure as a check that cannot go green.
if command -v journalctl > /dev/null 2>&1; then
  crashed=$(journalctl -u "$SERVICE_NAME" --since "-${HEALTH_WINDOW_S}s" --no-pager -q 2>/dev/null | grep -c "Failed with result" || true)
  if [ "${crashed:-0}" -gt 0 ]; then
    fail "$crashed 'Failed with result' line(s) in the last ${HEALTH_WINDOW_S}s; last: $(journalctl -u "$SERVICE_NAME" --since "-${HEALTH_WINDOW_S}s" --no-pager -q 2>/dev/null | grep "Failed with result" | tail -1)"
  else
    ok "no 'Failed with result' line in the last ${HEALTH_WINDOW_S}s"
  fi
fi

# One process, not one per deploy -- and the unit's own MainPID rather than a `/proc` scan: `pgrep` samples between two
# restarts and reads zero, which is what the first real deploy's report said ("0 session process(es)") while the unit was
# cycling on a refused connection. MainPID is systemd's answer to the same question.
mainpid=$(systemctl show -p MainPID --value "$SERVICE_NAME" 2>/dev/null || echo 0)
if [ "${mainpid:-0}" -gt 0 ] && ps -o args= -p "$mainpid" 2>/dev/null | grep -q 'toy_quant'; then
  ok "one session process (pid $mainpid)"
else
  fail "the unit reports MainPID ${mainpid:-0}, which is not a running $PROGRAM"
fi
# Two of them on one account is a position neither sees, so the scan stays as the duplicate guard it is good at.
duplicates=$(pgrep -c -f "$PROGRAM" 2>/dev/null || true)
if [ "${duplicates:-0}" -gt 1 ]; then
  fail "$duplicates processes run $PROGRAM; one host may run one"
fi

# The relay is the other half: without it the engine reads a closed port and its summary says so, but the failure reads
# as an engine failure. Two questions, because the process can be up while its socket is not.
if systemctl is-active --quiet "$RELAY_SERVICE_NAME" || wait_active "$RELAY_SERVICE_NAME"; then
  ok "$RELAY_SERVICE_NAME is active"
else
  fail "$RELAY_SERVICE_NAME is still $(systemctl is-active "$RELAY_SERVICE_NAME" 2>&1 || true) after ${WAIT_ACTIVE_S}s"
fi
relay_restarts=$(systemctl show -p NRestarts --value "$RELAY_SERVICE_NAME" 2>/dev/null || echo 0)
if [ "${relay_restarts:-0}" -gt 0 ]; then
  fail "$RELAY_SERVICE_NAME has restarted $relay_restarts time(s): check its journal for the reason"
fi
relays=$(pgrep -c -f "$RELAY_PROGRAM" 2>/dev/null || true)
relays=${relays:-0}
if [ "$relays" -ne 1 ]; then
  fail "$relays relay process(es) run $RELAY_PROGRAM; one host may run one"
else
  ok "one relay process"
fi
if command -v ss > /dev/null 2>&1; then
  if ss -ltn 2>/dev/null | grep -q '127.0.0.1:8765'; then
    ok "the market-data socket is listening on 127.0.0.1:8765"
  else
    fail "nothing listens on 127.0.0.1:8765 -- the engine will read a closed port"
  fi
  if ss -ltn 2>/dev/null | grep -q '127.0.0.1:8766'; then
    ok "the order socket is listening on 127.0.0.1:8766"
  else
    fail "nothing listens on 127.0.0.1:8766 -- an order would have nowhere to go"
  fi
fi

if [ ! -x "$PROGRAM" ]; then
  fail "$PROGRAM is missing or not executable -- the bundle was unpacked without the program"
else
  ok "$PROGRAM present ($(stat -c '%s bytes, %y' "$PROGRAM"))"
fi

# The journal is where a refused order or a rejected key lands. `-p err` is severity 3 and above, and the window is this
# boot, because a line from last week is not this deploy's failure.
if command -v journalctl > /dev/null 2>&1; then
  errors=$(journalctl -u "$SERVICE_NAME" --since "-${HEALTH_WINDOW_S}s" -p err --no-pager -q 2>/dev/null | wc -l)
  if [ "${errors:-0}" -gt 0 ]; then
    fail "$errors error-severity journal line(s) in the last ${HEALTH_WINDOW_S}s:"
    journalctl -u "$SERVICE_NAME" --since "-${HEALTH_WINDOW_S}s" -p err --no-pager -q 2>/dev/null | tail -5 | sed 's/^/health:   /' >&2
  else
    ok "no error-severity journal line in the last ${HEALTH_WINDOW_S}s"
  fi
fi

# The engine's own first line, which is the only thing that proves a *session* started: a `ws` run prints `[Mode: ...]`
# before it connects. The snapshot file is not this signal -- it is an l2_replay artifact, and a live config cannot name it
# (measured 2026-11-27: `--snapshot-out` is refused in `ws`, which is what the fourth deploy's report was about).
if command -v journalctl > /dev/null 2>&1; then
  if journalctl -u "$SERVICE_NAME" --since "-${HEALTH_WINDOW_S}s" --no-pager -q 2>/dev/null | grep -q '\[Mode'; then
    ok "the session reported its [Mode] line"
  else
    fail "no [Mode] line from $SERVICE_NAME in the last ${HEALTH_WINDOW_S}s: the engine has not started a session (its journal tail is below)"
  fi
fi
if [ -f "$SNAPSHOT" ]; then
  # Kept because a file that exists and does not parse is worth a red whatever wrote it; a live session writes none.
  if python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "$SNAPSHOT" 2>/dev/null; then
    ok "the snapshot parses ($SNAPSHOT, $(($(date +%s) - $(stat -c %Y "$SNAPSHOT")))s old)"
  else
    fail "the snapshot does not parse as JSON: $SNAPSHOT"
  fi
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
  # The program's own words, which is the part a systemd summary cannot carry: an argument the parser refused, a venue
  # answer, a traceback. Attached only on failure, because a green report with fifty journal lines under it is a report
  # nobody reads to the end.
  if command -v journalctl > /dev/null 2>&1; then
    for unit in "$RELAY_SERVICE_NAME" "$SERVICE_NAME"; do
      echo "health: --- last ${HEALTH_JOURNAL_LINES} lines of $unit" >&2
      journalctl -u "$unit" -n "$HEALTH_JOURNAL_LINES" --no-pager -q 2>/dev/null | sed 's/^/health:   /' >&2
    done
  fi
  echo "health: $failures failure(s)" >&2
  # Both units carry a start limit, so a crash loop that has been fixed still refuses to start until the counter is
  # cleared -- and a refusal reads exactly like a fresh fault. Naming the command is cheaper than the reader's hour.
  echo "health: if this was a crash loop and the cause is fixed, clear the limit first:" >&2
  echo "health:   sudo systemctl reset-failed $RELAY_SERVICE_NAME $SERVICE_NAME" >&2
  exit 1
fi
echo "health: OK"

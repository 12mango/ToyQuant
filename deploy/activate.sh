#!/usr/bin/env bash
# Bring the host's units to the state a deploy promises, in one auditable sequence -- and nothing more.
#
# **Why this is a script and not four lines in the workflow's SSH string.** A shell line that travels as a
# single-quoted `ssh` argument is parsed twice (by the runner and by the host), so one apostrophe in an echoed
# sentence closed the argument and the step died with `unexpected EOF while looking for matching quote` *after* its
# diagnostics -- which read like a systemd fault (measured 2026-11-27). A tracked script is parsed once, here, by
# `bash -n` in the local preflight and by `check_deploy.py`'s rules.
#
# **What a deploy claims, and what it only reports.** The files landed, the manager loaded the units, the artifact
# runs, the config translates: those are the claim. A relay that will not start is a warning, because the two proofs
# of startability that work are elsewhere (`--ops restart`, and the 13:30 window, whose session `Requires=` the relay).
# A session that is running is the operator's and is never touched: a restart is a SIGTERM to quotes that may be at
# the venue (measured 2026-11-27: 13 s, `stop-sigterm` timed out, SIGKILL, quotes left resting).
#
# Usage: activate.sh [--repo-dir /opt/toyquant] [--relay toyquant-relay] [--session toyquant-live] [--attempts 15]
set -uo pipefail

REPO_DIR=/opt/toyquant
RELAY=toyquant-relay
SESSION=toyquant-live
ATTEMPTS=15
while [ $# -gt 0 ]; do
  case "$1" in
    --repo-dir) REPO_DIR=${2:?}; shift 2 ;;
    --relay) RELAY=${2:?}; shift 2 ;;
    --session) SESSION=${2:?}; shift 2 ;;
    --attempts) ATTEMPTS=${2:?}; shift 2 ;;
    *) echo "activate: unknown argument $1" >&2; exit 2 ;;
  esac
done

echo "activate: reloading the manager and clearing unit state for $RELAY and $SESSION"
systemctl daemon-reload
systemctl reset-failed "$RELAY" "$SESSION" 2>&1 || true

# A start that answers `Unit ... not found` is the manager saying it has no fragment to load: measured 2026-11-27,
# `LoadState=not-found`, `FragmentPath=` empty and `No files found for <unit>` while `show -p LoadState` had answered
# `loaded` a moment before. A reload takes a moment to land, so the start is retried inside a bound and the attempt it
# succeeded on is printed rather than assumed.
started=0
for attempt in $(seq 1 "$ATTEMPTS"); do
  if systemctl start "$RELAY"; then
    echo "activate: $RELAY started on attempt $attempt"
    started=1
    break
  fi
  echo "activate: start answered not-found (attempt $attempt/$ATTEMPTS); reloading and waiting" >&2
  systemctl daemon-reload
  systemctl reset-failed "$RELAY" 2>&1 || true
  sleep 1
done

if [ "$started" = 0 ]; then
  echo "::warning::$RELAY did not start from this session; the files landed and the window starts it"
  echo "activate: the manager's view follows, for the next round"
  ls -l "/etc/systemd/system/$RELAY.service" 2>&1 || true
  systemctl show -p LoadState -p FragmentPath -p ActiveState "$RELAY" 2>&1 || true
  systemctl cat "$RELAY" 2>&1 | head -6 || true
  journalctl -u "$RELAY" -n 8 --no-pager -q 2>&1 || true
else
  bash "$REPO_DIR/deploy/wait_for_ports.sh" 127.0.0.1:8765 127.0.0.1:8766
  # The preflight answers "would this start" in seconds, and points both of the engine's endpoints at 127.0.0.1:1 for
  # its own run, because the relay's order channel serves exactly one client and a check that took it would leave the
  # session without one (measured 2026-11-27).
  bash "$REPO_DIR/deploy/preflight.sh"
fi

# The session is reported, never touched. Running: the operator's, and the health check follows. Not running: the
# normal state between windows, and the window timer starts this binary.
if systemctl is-active --quiet "$SESSION"; then
  echo "activate: $SESSION is running and was not touched; the health check follows"
  bash "$REPO_DIR/deploy/check_health.sh"
else
  echo "activate: $SESSION is not running, which is the state between windows; the window timer starts it"
  systemctl --no-pager --lines=0 status "$SESSION" 2>&1 | head -3 || true
fi

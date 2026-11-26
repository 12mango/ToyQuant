#!/usr/bin/env bash
# Answer one question in about a second: would this binary start on this host, with this configuration? It touches no unit
# and restarts nothing.
#
# Six deploys in a row failed for six different reasons, and every one of them was visible in the first second of a run:
# an illegal instruction (a `-march=native` build on another CPU), a market socket nobody opened, a flag the engine refuses
# (`--snapshot-out` in `ws`), an argument list the config reader could not produce, and a log directory that resolved to the
# *build* tree (the runner's checkout, which the host cannot write). None of them needed the restart, the crash loop, the
# journal or the report; all of them needed this script, which is why it is now run before the deploy restarts anything.
#
#   sudo bash /opt/toyquant/deploy/preflight.sh
set -euo pipefail

REPO_DIR=${REPO_DIR:-/opt/toyquant}
ETC_DIR=${ETC_DIR:-/etc/toyquant}
SERVICE_USER=${SERVICE_USER:-toyquant}
CONFIG=$ETC_DIR/live.yaml
PROGRAM=$REPO_DIR/toy_quant
failures=0

fail() { echo "preflight: FAIL $*" >&2; failures=$((failures + 1)); }
ok() { echo "preflight: ok $*"; }

# 1. The binary starts at all. An artifact built for another CPU dies here, before anything that could explain it.
if start=$(("$PROGRAM" --help 2>&1 | head -1)); then
  ok "the binary starts: $(printf '%.70s' "$start")"
else
  fail "the binary does not start (an illegal instruction reads as exit 132)"
fi

# 2. The configuration translates into the engine's arguments, and a live file must yield at least the three positionals.
flags=$(python3 "$REPO_DIR/tools/live/live_config.py" --flags "$CONFIG" 2>&1) || {
  fail "live_config.py refused $CONFIG:"
  printf '%s\n' "$flags" | sed 's/^/preflight:   /' >&2
  flags=""
}
if [ -n "$flags" ]; then
  count=$(printf '%s\n' "$flags" | grep -c . || true)
  if [ "${count:-0}" -lt 3 ]; then
    fail "live_config.py emitted $count argument(s); ws needs the endpoint, the symbol and the strategy"
  else
    ok "the configuration emits $count argument(s)"
  fi
fi

# 3. The engine accepts them and gets past its own startup -- the log directory, the mode line, everything that happens
#    before the first connect. `127.0.0.1:1` is a port nothing listens on, so the run is expected to end there; the `[Mode]`
#    line is what proves it got that far. Runs as the service user, because a path the operator can write and the service
#    cannot is one of the failures this exists to catch.
if [ -n "$flags" ]; then
  mapfile -t argv <<<"$flags"
  if [ "$(id -u)" = 0 ] && id "$SERVICE_USER" > /dev/null 2>&1; then
    runner=(sudo -u "$SERVICE_USER" -H)
  else
    runner=()
  fi
  startup=$("${runner[@]}" bash -c "cd '$REPO_DIR' && exec '$PROGRAM' ws 127.0.0.1:1 \"\$@\" --fast-validation" \
            _ "${argv[@]:1}" 2>&1 | head -8) || true
  if printf '%s\n' "$startup" | grep -q '\[Mode'; then
    ok "the engine reported its [Mode] line and reached the connect"
  else
    fail "the engine did not get past startup:"
    printf '%s\n' "$startup" | sed 's/^/preflight:   /' >&2
  fi
fi

# 4. The relay's two sockets, which the engine dials at startup and which `After=` alone does not guarantee are open.
for port in "${RELAY_MARKET_PORT:-8765}" "${RELAY_ORDER_PORT:-8766}"; do
  if ss -ltn 2>/dev/null | grep -q "127.0.0.1:$port"; then
    ok "something listens on 127.0.0.1:$port"
  else
    fail "nothing listens on 127.0.0.1:$port"
  fi
done

if [ "$failures" -eq 0 ]; then
  echo "preflight: OK -- this host would start a session"
  exit 0
fi
echo "preflight: $failures failure(s); nothing was restarted" >&2
exit 1

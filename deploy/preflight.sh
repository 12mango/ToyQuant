#!/usr/bin/env bash
# Answer one question in about a second: would this binary start on this host, with this configuration? It touches no unit
# and restarts nothing -- and it therefore runs **after** the deploy has restarted the relay with the code it just unpacked,
# because check 6 asks whether the relay's two sockets are open. A host the operator has stopped has no sockets to check,
# and demanding them before anything starts made every deploy of a stopped host red (measured 2026-11-27, two `FAIL
# nothing listens on 127.0.0.1:8765`); check 3 runs the engine against `127.0.0.1:1`, so nothing here can disturb a running
# relay's single-client order channel.
#
# Seven deploys failed in a row for seven different reasons, and every one of them was visible in the first second of a run:
# an illegal instruction (a `-march=native` build on another CPU), a market socket nobody opened, a flag the engine refuses
# (`--snapshot-out` in `ws`), an argument list the config reader could not produce, a log directory that resolved to the
# *build* tree (the runner's checkout, which the host cannot write), and a relay whose credentials were still a template's.
# None of them needed the restart, the crash loop, the journal or the report; all of them needed this script, which is why
# it runs before the deploy restarts the engine.
#
#   sudo bash /opt/toyquant/deploy/preflight.sh
set -euo pipefail

# `set -e` exits with no message, and a preflight that dies silently after its third ok line is a check whose failure
# cannot be acted on (measured 2026-11-27, twice: the deploy job showed three ok lines and `exit code 1`). The trap
# names the line and the command; the step lines below say which check was running when it happened.
trap 'status=$?; echo "preflight: FAIL at line $LINENO: $BASH_COMMAND (exit $status)" >&2; exit $status' ERR

REPO_DIR=${REPO_DIR:-/opt/toyquant}
ETC_DIR=${ETC_DIR:-/etc/toyquant}
SERVICE_USER=${SERVICE_USER:-toyquant}
CONFIG=$ETC_DIR/live.yaml
PROGRAM=$REPO_DIR/toy_quant
failures=0

fail() { echo "preflight: FAIL $*" >&2; failures=$((failures + 1)); }
ok() { echo "preflight: ok $*"; }

echo "preflight: .. check 1/6"
# 1. The binary starts at all. An artifact built for another CPU dies here, before anything that could explain it.
#    The first version of this check was `start=$(($PROGRAM --help ...))`: `$(( ))` is arithmetic, not a command
#    substitution, so bash printed `syntax error: operand expected` and the pipeline swallowed it into an `ok`.
#    Measured 2026-11-27 on the first host to run this file.
help_rc=0
help_out=$("$PROGRAM" --help 2>&1) || help_rc=$?
if [ "$help_rc" -eq 0 ]; then
  ok "the binary starts: $(printf '%s\n' "$help_out" | head -1 | cut -c1-70)"
else
  fail "the binary does not start (exit $help_rc; an illegal instruction reads as 132)"
  printf '%s\n' "$help_out" | head -3 | sed 's/^/preflight:   /' >&2
fi

echo "preflight: .. check 2/6"
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

# The two addresses the engine will actually dial, taken from the file it is handed rather than from this script's
# defaults: `link.market_endpoint` is the first positional and `link.venue_endpoint` the `--venue-endpoint=` token. A
# preflight that hardcoded 8765 while the file named 9000 would pass on a host whose session connects to nothing.
engine_market=""
engine_order=""
if [ -n "$flags" ]; then
  engine_market=$(printf '%s\n' "$flags" | head -1)
  engine_order=$(printf '%s\n' "$flags" | sed -n 's/^--venue-endpoint=//p' | head -1)
fi

echo "preflight: .. check 3/6"
# 3. The engine accepts them and gets past its own startup -- the log directory, the mode line, everything that happens
#    before the first connect. `127.0.0.1:1` is a port nothing listens on, so the run is expected to end there; the `[Mode]`
#    line is what proves it got that far. Runs as the service user, because a path the operator can write and the service
#    cannot is one of the failures this exists to catch.
#
#    **Both endpoints are pointed at that dead port.** The relay's order channel serves exactly one client, so an engine
#    that dials the real one here takes the slot and the session's own engine then never gets it: measured 2026-11-27, the
#    relay was up on both sockets, this check ran, and thirty seconds later `toyquant-live` failed its `ExecStartPre` with
#    `waited 30s and nothing listens on 127.0.0.1:8766`. Reachability is check 6's question, and agreement between the two
#    files is its other half; this check is about the program starting at all.
if [ -n "$flags" ]; then
  mapfile -t argv <<<"$flags"
  if [ "$(id -u)" = 0 ] && id "$SERVICE_USER" > /dev/null 2>&1; then
    runner=(sudo -u "$SERVICE_USER" -H)
  else
    runner=()
  fi
  engine_flags=()
  for argument in "${argv[@]:1}"; do
    case "$argument" in
      --venue-endpoint=*) engine_flags+=("--venue-endpoint=127.0.0.1:1") ;;
      *) engine_flags+=("$argument") ;;
    esac
  done
  startup=$("${runner[@]}" bash -c "cd '$REPO_DIR' && exec '$PROGRAM' ws 127.0.0.1:1 \"\$@\" --fast-validation" \
            _ "${engine_flags[@]}" 2>&1 | head -8) || true
  if printf '%s\n' "$startup" | grep -q '\[Mode'; then
    ok "the engine reported its [Mode] line and reached the connect"
  else
    fail "the engine did not get past startup:"
    printf '%s\n' "$startup" | sed 's/^/preflight:   /' >&2
  fi
fi

echo "preflight: .. check 4/6"

# Read one KEY=value from the operator's env file WITHOUT sourcing it. Sourcing a file executes it: a line that is not an
# assignment returns non-zero, `set -e` inherits into the command substitution, and the whole check dies with nothing to
# read -- measured 2026-11-27 on the host, `preflight: FAIL at line 109: unset_creds=$(...) (exit 1)`. An awk read has no
# such path: it cannot run the file, it strips a CRLF or a BOM without being told, and its own failure is guarded.
read_env_value() {  # read_env_value <file> <key>
  awk -v key="$2" '
    {
      line = $0
      sub(/\r$/, "", line)
      sub(/^[^A-Za-z_]+/, "", line)
      equal = index(line, "=")
      if (equal > 0 && substr(line, 1, equal - 1) == key) value = substr(line, equal + 1)
    }
    END { print value }
  ' "$1" 2>/dev/null || true
}

# 4. The credentials the relay logs in with, and whether they are still a template's. A placeholder passes every check
#    above -- the binary starts, the engine reaches `[Mode]`, the sockets listen -- and then the venue refuses the login and
#    every reconciliation reads `n/a` / `status=unknown`, which looks like a venue answer rather than a missing key. Names
#    are printed, values never: this output ends up in a deploy log.
if [ -r "$ETC_DIR/live.env" ]; then
  unset_creds=""
  for name in TOYQUANT_API_KEY TOYQUANT_API_SECRET TOYQUANT_PASSPHRASE; do
    value=$(read_env_value "$ETC_DIR/live.env" "$name")
    case "$value" in
      ''|*replace*|*REPLACE*|*changeme*|*CHANGEME*|*your*|*YOUR*|'<'*) unset_creds="$unset_creds $name" ;;
    esac
  done
  if [ -z "$unset_creds" ]; then
    ok "the three credentials are set in $ETC_DIR/live.env"
  else
    fail "unset or still a template's in $ETC_DIR/live.env:$unset_creds"
  fi
else
  fail "$ETC_DIR/live.env is not readable, and the relay logs in from it"
fi

echo "preflight: .. check 5/6"
# 5. The two files as the account that will read them, not as root. `root:root 0600` passes every root-side check and is
#    a file systemd cannot read as `toyquant`: the relay's `EnvironmentFile=` then fails the unit outright, and the
#    engine's `-` prefix turns it into silence.
if id "$SERVICE_USER" > /dev/null 2>&1 && command -v runuser > /dev/null 2>&1; then
  for file in "$CONFIG" "$ETC_DIR/live.env"; do
    if [ ! -f "$file" ]; then
      fail "$file is missing: the operator's file was never seeded (deploy/bootstrap_server.sh does it from the template)"
    elif runuser -u "$SERVICE_USER" -- test -r "$file" 2>/dev/null; then
      ok "$SERVICE_USER can read $file"
    else
      fail "$SERVICE_USER cannot read $file ($(stat -c '%a %U:%G' "$file")): the unit runs as $SERVICE_USER"
    fi
  done
fi

echo "preflight: .. check 6/6"
# 6. The two addresses agree, and the one the engine dials is open. The relay's side comes from its own environment file,
#    because that is what systemd will hand it: an engine pointed at a port the relay does not serve waits forever, and the
#    only symptom is a session that starts and does nothing -- the failure this whole script exists to name.
relay_market=""
relay_order=""
if [ -r "$ETC_DIR/live.env" ]; then
  relay_market=$(read_env_value "$ETC_DIR/live.env" RELAY_MARKET_LISTEN)
  relay_order=$(read_env_value "$ETC_DIR/live.env" RELAY_ORDER_LISTEN)
fi

check_endpoint() {  # check_endpoint <name> <the engine's address> <the relay's address>
  local name=$1 engine=$2 relay=$3 port
  if [ -z "$engine" ]; then return 0; fi
  if [ "$engine" = "$relay" ]; then
    ok "the engine's $name endpoint is the relay's listener ($engine)"
  else
    fail "the engine dials $engine for $name and the relay listens on $relay: live.yaml and live.env disagree"
  fi
  port=${engine##*:}
  if ss -ltn 2>/dev/null | grep -qE "127\.0\.0\.1:$port([[:space:]]|$)"; then
    ok "something listens on $engine"
  else
    fail "nothing listens on $engine, which is where the engine will look"
  fi
}

check_endpoint market "$engine_market" "${relay_market:-127.0.0.1:8765}"
check_endpoint order "$engine_order" "${relay_order:-127.0.0.1:8766}"

if [ "$failures" -eq 0 ]; then
  echo "preflight: OK -- this host would start a session"
  exit 0
fi
echo "preflight: $failures failure(s); nothing was restarted" >&2
exit 1


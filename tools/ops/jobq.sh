#!/usr/bin/env bash
#
# A serial queue for the heavy measurements: registered, resumable, and read as files rather than remembered.
#
# The four failures this exists for are all measured in this tree. A terminal whose shell integration loses a command's
# end takes the command with it, and every command typed afterwards is dropped (three times on 2026-11-23); a reboot wipes
# `/tmp/tq` and every detached run's log (2026-11-28, five jobs at once); the 7.7 GB tmpfs filled by a fourteen-symbol
# sweep and the last two symbols died with **exit 120 and an empty log**, which is the tell for a full `/tmp` rather than a
# traceback (2026-11-26); and "three fetches and two sweeps running, each with a log, none of them with a named consumer"
# is the state `.clinerules` was written from. So a job is a file with four cells -- the command, the artifact it writes,
# the tool that reads that artifact, and the bar that closes it -- the queue runs one at a time, and the state lives in the
# tree (`out/jobs/`), which survives a reboot, rather than in `/tmp`, which does not.
#
#   tools/ops/jobq.sh add nes28 --artifact /tmp/tq/nes28/f1_control.csv \
#       --consumer "python3 tools/study/sweep_summary.py --read /tmp/tq/nes28/f1_control.csv" \
#       --bar "median fills >= 25, closed median positive, <= 15% dead windows" \
#       -- bash tools/sweep/up_ten_contract_cell.sh --arm f1_control --symbol NES-USDT-SWAP --days ... \
#            --latency-us 10000 --out /tmp/tq/nes28
#   tools/ops/jobq.sh run        # start the runner detached (refused if one is alive)
#   tools/ops/jobq.sh status     # the registry: state, elapsed, exit, artifact, bar
#   tools/ops/jobq.sh tail <name> [lines]
#   tools/ops/jobq.sh stop       # finish the current job, then stop
#
# **The constraints are this machine's, measured 2026-11-29**: 8 cores, 15 GB of memory, and 6.0 GB free on `/tmp`'s
# 7.7 GB tmpfs -- so the binding resource is the tmpfs, not the CPU, and one symbol's seven-day cut set is ~1.5 GB by the
# gate's own note. Hence: **one job at a time** (a serial queue is the only shape that keeps two sweeps out of one cuts
# directory, and `okx_maker_baseline.py` already refuses a second writer by name), **nice 10** so a foreground `fast` is
# not starved, and a **disk guard** that refuses a job needing more free space than `/tmp` has and marks it `blocked`
# instead of filling the slice. A foreground gate may run beside the queue; if tier0 goes red with only
# `venue_order_session_test` (#23) named while a job is running, that is the load-sensitive flake the handoff records --
# re-run tier0 alone, do not debug it.
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
STATE=${JOBQ_ROOT:-$ROOT/out/jobs}
NICE_LEVEL=${JOBQ_NICE:-10}
DISK_PATH=${JOBQ_DISK:-/tmp}

usage() { sed -n '2,42p' "$0"; }

sanitize() { printf '%s' "$1" | tr -c 'a-zA-Z0-9_.-' '_'; }

free_gb() {
  # `df` in kilobytes and integer arithmetic: no `du`, no `bc`, nothing that a full disk can fail to start.
  local avail
  avail=$(df -Pk "$DISK_PATH" 2>/dev/null | awk 'NR==2 {print $4}')
  [ -n "$avail" ] || { echo 0; return; }
  echo $((avail / 1024 / 1024))
}

header_of() { sed -n 's/^# jobq //p' "$1"; }
field_of() { header_of "$1" | sed -n "s/^$2=//p" | head -1; }

cmd_add() {
  local name="" artifact="" consumer="" bar="" needs="1.5"
  [ $# -ge 1 ] || { echo "jobq: add needs a name" >&2; return 2; }
  name=$(sanitize "$1"); shift
  while [ $# -gt 0 ]; do
    case "$1" in
      --artifact) artifact=${2:-}; shift 2 ;;
      --consumer) consumer=${2:-}; shift 2 ;;
      --bar) bar=${2:-}; shift 2 ;;
      --needs-gb) needs=${2:-}; shift 2 ;;
      --) shift; break ;;
      *) echo "jobq: add does not know '$1'" >&2; return 2 ;;
    esac
  done
  if [ $# -eq 0 ]; then
    echo "jobq: add needs a command after '--', and a job without one is a row with no work in it" >&2
    return 2
  fi
  case "$needs" in *[!0-9.]*|'') echo "jobq: --needs-gb takes a number" >&2; return 2 ;; esac
  mkdir -p "$STATE"/pending "$STATE"/running "$STATE"/done
  # A name is a key: a second job with it would make the registry ambiguous the way two rows for one window make a ledger
  # double-count. Any state counts, because a finished job's log is still the answer to "what did that one do".
  if [ -n "$(find "$STATE" -name "[0-9]*-$name.job" -print -quit 2>/dev/null)" ]; then
    echo "jobq: '$name' is already registered (pending, running or done) -- pick another name" >&2
    return 1
  fi
  local seq file part
  seq=$(find "$STATE" -name '*.job' 2>/dev/null | wc -l)
  file=$(printf '%s/pending/%04d-%s.job' "$STATE" "$((seq + 1))" "$name")
  {
    printf '# jobq name=%s\n' "$name"
    printf '# jobq artifact=%s\n' "$artifact"
    printf '# jobq consumer=%s\n' "$consumer"
    printf '# jobq bar=%s\n' "$bar"
    printf '# jobq needs_gb=%s\n' "$needs"
    printf '# jobq added=%s\n' "$(date -u +%Y-%m-%dT%H:%MZ)"
    printf 'cd %q || exit 1\n' "$ROOT"
    printf 'exec'
    for part in "$@"; do printf ' %q' "$part"; done
    printf '\n'
  } > "$file"
  echo "$file"
}

cmd_run() {
  local foreground=0
  [ "${1:-}" = "--foreground" ] && foreground=1
  mkdir -p "$STATE"/pending "$STATE"/running "$STATE"/done
  if [ "$foreground" = 0 ] && [ -f "$STATE/runner.pid" ] &&
     kill -0 "$(cat "$STATE/runner.pid" 2>/dev/null)" 2>/dev/null; then
    echo "jobq: the runner is already alive (pid $(cat "$STATE/runner.pid")) -- one queue per tree" >&2
    return 1
  fi
  if [ "$foreground" = 0 ]; then
    rm -f "$STATE/runner.exit" "$STATE/stop"
    setsid bash "$0" run --foreground > "$STATE/runner.log" 2>&1 < /dev/null &
    echo "$STATE/runner.log"
    return 0
  fi
  echo $$ > "$STATE/runner.pid"
  rm -f "$STATE/stop"
  local job name needs exit_code
  while true; do
    if [ -f "$STATE/stop" ]; then
      echo "jobq: stop requested after the current job"
      break
    fi
    job=$(ls "$STATE"/pending/*.job 2>/dev/null | sort | head -1)
    [ -n "$job" ] || break
    name=$(field_of "$job" name)
    if [ -z "$name" ]; then
      echo "jobq: $(basename "$job") has no name header -- moved aside rather than run unnamed" >&2
      mv "$job" "$STATE/malformed.job" 2>/dev/null || rm -f "$job"
      continue
    fi
    needs=$(field_of "$job" needs_gb); needs=${needs:-1.5}
    # The guard is the one thing that must happen before the work: a job that fills /tmp dies with exit 120 and an empty
    # log, which reads as a traceback nobody can find.
    if ! awk -v free="$(free_gb)" -v need="$needs" 'BEGIN { exit !(free + 0 >= need + 0) }'; then
      echo "jobq: blocked -- $name needs ${needs} GB and $(free_gb) GB is free on $DISK_PATH"
      mv "$job" "$STATE/pending/$(basename "$job" .job).blocked" 2>/dev/null || true
      break
    fi
    mv "$job" "$STATE/running/$(basename "$job")"
    job="$STATE/running/$(basename "$job")"
    date -u +%Y-%m-%dT%H:%MZ > "$STATE/$name.started"
    echo "jobq: $name start $(date -u +%H:%M:%SZ) needs=${needs}GB free=$(free_gb)GB"
    nice -n "$NICE_LEVEL" bash "$job" > "$STATE/$name.log" 2>&1
    exit_code=$?
    echo "$exit_code" > "$STATE/$name.exit"
    date -u +%Y-%m-%dT%H:%MZ > "$STATE/$name.finished"
    mv "$job" "$STATE/done/$(basename "$job")" 2>/dev/null || true
    echo "jobq: $name done exit=$exit_code $(date -u +%H:%M:%SZ)"
    # A failed job does not stop the queue -- one bad day in a sweep must not hide the nine behind it -- but it is loud in
    # the log and in the status row, which is where a reader looks.
  done
  rm -f "$STATE/runner.pid"
  echo "jobq: queue empty"
}

cmd_status() {
  mkdir -p "$STATE"/pending "$STATE"/running "$STATE"/done
  printf '%-10s %-9s %-11s %-6s %-9s %s\n' name state started exit artifact bar
  local job base name state started artifact bar size exit_text
  {
    ls "$STATE"/pending/*.job "$STATE"/running/*.job "$STATE"/done/*.job 2>/dev/null
    ls "$STATE"/pending/*.blocked 2>/dev/null
  } | sort | while read -r job; do
    [ -n "$job" ] || continue
    base=$(basename "$job"); base=${base%.job}; base=${base%.blocked}
    name=${base#*-}
    exit_text=$(cat "$STATE/$name.exit" 2>/dev/null || echo -)
    if [ "$exit_text" != "-" ]; then
      state="done($exit_text)"
    elif [ -e "$STATE/$name.started" ]; then
      state="running"
    elif [ "${job%.blocked}" != "$job" ]; then
      state="blocked"
    else
      state="pending"
    fi
    started=$(cat "$STATE/$name.started" 2>/dev/null || echo "-")
    artifact=$(field_of "$job" artifact)
    bar=$(field_of "$job" bar)
    if [ -n "$artifact" ] && [ -e "$artifact" ]; then
      size=$(du -h "$artifact" 2>/dev/null | cut -f1)
      artifact="ok ${size}"
    else
      artifact="absent"
    fi
    printf '%-10s %-9s %-11s %-6s %-9s %s\n' "$name" "$state" "${started#*T}" "$exit_text" "$artifact" "$bar"
  done
}

cmd_tail() {
  local name
  name=$(sanitize "${1:-}")
  [ -n "$name" ] || { echo "jobq: tail needs a name" >&2; return 2; }
  [ -f "$STATE/$name.log" ] || { echo "jobq: no log for '$name' in $STATE" >&2; return 1; }
  tail -n "${2:-20}" "$STATE/$name.log"
}

cmd_stop() {
  mkdir -p "$STATE"
  touch "$STATE/stop"
  echo "jobq: the runner will finish the job it is on and stop; 'rm $STATE/stop' cancels that"
}

cmd_self_test() {
  local scratch status needle
  scratch=$(mktemp -d)
  JOBQ_ROOT=$scratch JOBQ_NICE=0 bash "$0" add one --artifact "$scratch/a.txt" \
    --bar "a trivial artefact" -- sh -c "echo hi > $scratch/a.txt" > /dev/null
  JOBQ_ROOT=$scratch JOBQ_NICE=0 bash "$0" add two --bar "fails loudly" -- sh -c 'exit 7' > /dev/null
  JOBQ_ROOT=$scratch JOBQ_NICE=0 bash "$0" add three --needs-gb 99999 --bar "blocked by the disk guard" -- true > /dev/null
  if JOBQ_ROOT=$scratch JOBQ_NICE=0 bash "$0" add one -- true > /dev/null 2>&1; then
    echo "jobq self-test: a duplicate name was accepted" >&2
    rm -rf "$scratch"
    return 1
  fi
  JOBQ_ROOT=$scratch JOBQ_NICE=0 bash "$0" run --foreground > "$scratch/selftest.log" 2>&1
  status=$(JOBQ_ROOT=$scratch bash "$0" status)
  for needle in "done(0)" "done(7)" "blocked" "ok "; do
    printf '%s' "$status" | grep -q -- "$needle" || {
      echo "jobq self-test: '$needle' is not in the table:" >&2
      printf '%s\n' "$status" >&2
      rm -rf "$scratch"
      return 1
    }
  done
  # The artefact cell is the row's second cell and has to say something the state cannot: a job that exits 0 without
  # writing its file is the "artifact present but empty" failure read from the other side.
  printf '%s' "$status" | grep -q 'three *blocked' || {
    echo "jobq self-test: the blocked row is not rendered:" >&2
    printf '%s\n' "$status" >&2
    rm -rf "$scratch"
    return 1
  }
  rm -rf "$scratch"
  echo "jobq self-test OK (duplicate refused, exits 0 and 7 recorded, the disk guard blocking, the table rendered)"
  return 0
}

main() {
  local command=${1:-status}
  shift || true
  case "$command" in
    add) cmd_add "$@" ;;
    run) cmd_run "$@" ;;
    status) cmd_status ;;
    tail) cmd_tail "$@" ;;
    stop) cmd_stop ;;
    self-test) cmd_self_test ;;
    -h|--help|help) usage ;;
    *) echo "jobq: unknown command $command (add|run|status|tail|stop|self-test)" >&2; return 2 ;;
  esac
}

main "$@"


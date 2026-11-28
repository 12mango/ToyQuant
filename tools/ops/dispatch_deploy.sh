#!/bin/bash
# Dispatch, watch and read this repository's workflow runs -- the deploy loop without a browser.
#
#   tools/ops/dispatch_deploy.sh run              # dispatch deploy.yml (data_gate stays false)
#   tools/ops/dispatch_deploy.sh run --ops stop   # dispatch ops.yml with an action
#   tools/ops/dispatch_deploy.sh run --ops report --at 2026-11-27T15:36:00Z   # one window's report, no mail
#   tools/ops/dispatch_deploy.sh run --ops set-key \
#     --key engine.max_orders_per_second,engine.order_burst --value 20,4 --add   # one setting, or a zipped list
#   tools/ops/dispatch_deploy.sh run --base-sha 0e05ee4   # Cut 2: the diff the release-path classifier reads
#   tools/ops/dispatch_deploy.sh check            # is the token the right shape, and does GitHub accept it
#   tools/ops/dispatch_deploy.sh status           # the last few runs and their state
#   tools/ops/dispatch_deploy.sh logs [--failed]  # the newest run's log (failed steps only with --failed)
#   tools/ops/dispatch_deploy.sh logs ops.yml     # the same for another workflow: **`logs` and `wait` default to
#                                                 # deploy.yml**, so reading an ops run means naming it -- measured
#                                                 # 2026-11-28, five reads of the wrong run before the default was noticed
#   tools/ops/dispatch_deploy.sh wait             # block until the newest run finishes
#   tools/ops/dispatch_deploy.sh window           # dispatch `--ops ledger`, wait, and save the series into
#                                                 # out/runtime/window_ledger.csv, then print it -- the window's
#                                                 # numbers reaching this tree without a journal pasted by hand
#
# The credential is one file, ignored, mode 600: `secrets/gh.token`, a fine-grained token for this repository with
# **Actions: Read and write** and nothing else. It is not a push token and it cannot be: a token without `Contents: write`
# cannot create the `v*` tag that also triggers a deploy, so the only restart it can cause is a dispatch.
#
#   github.com/settings/tokens?type=beta -> Repository access: only 12mango/ToyQuant -> Permissions: Actions: Read and write
#   install -m 600 /dev/null secrets/gh.token && sudoedit secrets/gh.token
#
# What this tool refuses, on purpose: a dirty working tree (a deploy of something the gate has not seen), a dispatch while
# a run of the same workflow is in progress (the workflow's own `concurrency` would queue it, but a queue nobody reads is
# how two deploys look like one), and `data_gate=true` (that job needs a self-hosted runner this host does not have).
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TOKEN_FILE="${GH_TOKEN_FILE:-$ROOT/secrets/gh.token}"
API=${TOYQUANT_API:-https://api.github.com}
DRY_RUN=0

usage() { sed -n '2,27p' "$0"; }

repo_slug() {
  local url
  url=$(git -C "$ROOT" remote get-url origin 2>/dev/null) || return 1
  printf '%s' "$url" | sed -e 's#^git@github.com:##' -e 's#^https://github.com/##' -e 's#\.git$##'
}

read_token() {
  if [ -n "${GH_TOKEN:-}" ]; then printf '%s' "$GH_TOKEN"; return 0; fi
  if [ ! -r "$TOKEN_FILE" ]; then
    echo "dispatch_deploy: no token -- expected $TOKEN_FILE (a fine-grained token, Actions: Read and write) or GH_TOKEN" >&2
    return 1
  fi
  tr -d '\r\n' < "$TOKEN_FILE"
}

api() {  # api <method> <path> [body]
  local method=$1 path=$2 body=${3:-} token
  token=$(read_token) || return 2
  if [ "$DRY_RUN" = 1 ]; then
    echo "would: curl -X $method $API$path (Authorization: token ***) ${body:+with body $body}"
    return 0
  fi
  if [ -n "$body" ]; then
    curl -sS -X "$method" -H "Authorization: token $token" -H 'Accept: application/vnd.github+json' \
      -H 'X-GitHub-Api-Version: 2022-11-28' -d "$body" "$API$path"
  else
    curl -sS -X "$method" -H "Authorization: token $token" -H 'Accept: application/vnd.github+json' \
      -H 'X-GitHub-Api-Version: 2022-11-28' "$API$path"
  fi
}

newest_run() {  # newest_run <workflow-file>
  api GET "/repos/$SLUG/actions/workflows/$1/runs?per_page=1" |
    python3 -c 'import json,sys
raw = sys.stdin.read().strip()
if not raw:
    print(0, "unknown", "-", "")
    raise SystemExit
d = json.loads(raw); r=(d.get("workflow_runs") or [{}])[0];
print(r.get("id",0), r.get("status","none"), r.get("conclusion") or "-", r.get("html_url",""))'
}

require_clean_tree() {
  local dirty
  dirty=$(git -C "$ROOT" status --porcelain)
  if [ -n "$dirty" ]; then
    echo "dispatch_deploy: the working tree is dirty -- a deploy would carry something the gate has not seen:" >&2
    printf '%s\n' "$dirty" | head -5 | sed 's/^/  /' >&2
    return 1
  fi
}

in_progress() {  # in_progress <workflow-file> -> prints the run id when one is queued or running
  newest_run "$1" | awk '$2 == "queued" || $2 == "in_progress" {print $1}'
}

cmd_run() {
  local workflow=deploy.yml body action="" at="" lines="" key="" value="" add=0 base_sha=""
  while [ $# -gt 0 ]; do
    case "$1" in
      --ops) workflow=ops.yml; action=${2:-}; shift 2 ;;
      --at) at=${2:-}; shift 2 ;;
      --lines) lines=${2:-}; shift 2 ;;
      --key) key=${2:-}; shift 2 ;;
      --value) value=${2:-}; shift 2 ;;
      --add) add=1; shift ;;
      # Cut 2: the release-path classifier diffs this commit against the one being deployed. A dispatch has no `before`,
      # so the base is named here or the classifier answers `full` -- which is the safe default.
      --base-sha) base_sha=${2:-}; shift 2 ;;
      --no-predeploy) SKIP_PREDEPLOY=1; shift ;;
      --allow-dirty) ALLOW_DIRTY=1; shift ;;
      *) echo "dispatch_deploy: unknown argument $1" >&2; return 2 ;;
    esac
  done
  if [ -n "$base_sha" ]; then
    case "$base_sha" in *[!0-9a-fA-F]*) echo "dispatch_deploy: --base-sha takes a commit hash, not '$base_sha'" >&2; return 2 ;; esac
    if [ "$workflow" != deploy.yml ]; then
      echo "dispatch_deploy: --base-sha is deploy.yml's (the release-path classifier), not $workflow's" >&2
      return 2
    fi
  fi
  # The three `set-key` inputs. `ops.yml` declares them and this dispatcher has to carry them, or the only way to apply a
  # setting is the browser -- which is the loop this tool exists to close. They are one action's inputs, so naming them
  # for another workflow is refused here rather than arriving at a step that ignores them and reports a run that changed
  # nothing. Measured 2026-11-28: the card's four budget values were one dispatch away and the body had no room for them.
  if [ -n "$key$value" ] || [ "$add" = 1 ]; then
    if [ "$workflow" != ops.yml ] || [ "${action:-}" != set-key ]; then
      echo "dispatch_deploy: --key/--value/--add are '--ops set-key' inputs, not $workflow's (action '${action:-none}')" >&2
      return 2
    fi
  fi
  if [ "$add" = 1 ] && [ -z "$key" ]; then
    echo "dispatch_deploy: --add says 'append this key', and no --key was named" >&2
    return 2
  fi
  if [ -n "$key" ] || [ -n "$value" ]; then
    if [ -z "$key" ] || [ -z "$value" ]; then
      echo "dispatch_deploy: --key and --value go together; one without the other is a setting nobody named" >&2
      return 2
    fi
    # Both are zipped by `ops.yml`, so a character that is not in a key, a number or a comma is refused here, and a list
    # whose halves differ in length is refused before the runner starts rather than in the job. Same reason `--at` and
    # `--lines` are checked: these characters go into a JSON body and a local refusal costs a second, a host one a round trip.
    case "$key" in *[!0-9A-Za-z_.,-]*) echo "dispatch_deploy: --key takes section.name[,section.name], not '$key'" >&2; return 2 ;; esac
    # `/` is allowed in a value because `engine.record_orders` takes an absolute path, and `live_config.py` is the reader that
    # refuses a relative one -- the guard here is about what can reach a JSON body, not about what a path may look like.
    case "$value" in *[!0-9A-Za-z_.,/-]*) echo "dispatch_deploy: --value takes a value[,value], not '$value'" >&2; return 2 ;; esac
    local keys=() values=()
    IFS=',' read -r -a keys <<< "$key"
    IFS=',' read -r -a values <<< "$value"
    if [ "${#keys[@]}" -ne "${#values[@]}" ]; then
      echo "dispatch_deploy: ${#keys[@]} key(s) against ${#values[@]} value(s) -- they are zipped, so they must match" >&2
      return 2
    fi
  fi
  if [ "${ALLOW_DIRTY:-0}" != 1 ]; then require_clean_tree || return 1; fi
  # Fail-fast, locally, in about three seconds: a fault found on the host costs a runner start, a build, an upload and a
  # journal round trip (measured 2026-11-27, several of four to five minutes each).
  if [ "${SKIP_PREDEPLOY:-0}" != 1 ] && [ "$DRY_RUN" = 0 ]; then
    if ! bash tools/ops/predeploy.sh; then
      echo "dispatch_deploy: predeploy refused; nothing was dispatched" >&2
      return 1
    fi
  fi
  local running=""
  # Without a token there is nothing to ask, so the dry run does not pretend to know whether a run is in flight.
  if [ "$DRY_RUN" = 0 ]; then running=$(in_progress "$workflow"); fi
  if [ -n "$running" ]; then
    echo "dispatch_deploy: $workflow already has run $running queued or running; waiting is cheaper than queueing a second" >&2
    return 1
  fi
  if [ "$workflow" = ops.yml ]; then
    # `--at` is an ISO instant and `--lines` a count: both go into a JSON body, so a character that does not belong in
    # either is refused here rather than escaped there.
    if [ -n "$at" ]; then
      case "$at" in *[!0-9A-Za-z:.+-]*) echo "dispatch_deploy: --at takes an ISO instant, not '$at'" >&2; return 2 ;; esac
    fi
    if [ -n "$lines" ]; then
      case "$lines" in *[!0-9]*) echo "dispatch_deploy: --lines takes a number, not '$lines'" >&2; return 2 ;; esac
    fi
    body="{\"ref\":\"main\",\"inputs\":{\"action\":\"${action:-status}\""
    if [ -n "$lines" ]; then body="$body,\"lines\":\"$lines\""; fi
    if [ -n "$at" ]; then body="$body,\"at\":\"$at\""; fi
    if [ -n "$key" ]; then body="$body,\"key\":\"$key\",\"value\":\"$value\""; fi
    if [ "$add" = 1 ]; then body="$body,\"add\":true"; fi
    body="$body}}"
  else
    # `data_gate` is deliberately absent: its runner does not exist on this host, and a queued job nobody serves is how a
    # green deploy read as a hung one (measured 2026-11-27, deploy #8). `base_sha` is the one input a dispatch may carry,
    # because the release-path classifier needs a diff and a dispatch has no `before` to diff against.
    if [ -n "$base_sha" ]; then
      body="{\"ref\":\"main\",\"inputs\":{\"base_sha\":\"${base_sha}\"}}"
    else
      body='{"ref":"main"}'
    fi
  fi
  if [ "$DRY_RUN" = 1 ]; then
    echo "would: POST /repos/$SLUG/actions/workflows/$workflow/dispatches $body"
    return 0
  fi
  local status
  status=$(curl -sS -o /dev/null -w '%{http_code}' -X POST \
    -H "Authorization: token $(read_token)" -H 'Accept: application/vnd.github+json' \
    -H 'X-GitHub-Api-Version: 2022-11-28' -d "$body" \
    "$API/repos/$SLUG/actions/workflows/$workflow/dispatches")
  case "$status" in
    204) echo "dispatched $workflow; run it with: $0 wait" ;;
    401|403) echo "dispatch_deploy: the token was refused ($status) -- it needs Actions: Read and write on $SLUG" >&2; return 1 ;;
    *) echo "dispatch_deploy: dispatch answered $status" >&2; return 1 ;;
  esac
}

cmd_status() {
  local workflow run
  for workflow in deploy.yml ops.yml; do
    run=$(newest_run "$workflow")
    printf '%-12s %s\n' "$workflow" "$run"
  done
}

cmd_wait() {
  local workflow=${1:-deploy.yml} id="" state="" new timeout=${WAIT_SECONDS:-1800} waited=0
  while [ "$waited" -lt "$timeout" ]; do
    new=$(newest_run "$workflow")
    set -- $new
    if [ "$1" != "$id" ] || [ "$2" != "$state" ]; then
      id=$1; state=$2
      echo "$workflow run $id: $state ${3:-(no conclusion yet)} ${4:-}"
    fi
    if [ "$state" = completed ]; then
      [ "$3" = success ] && return 0
      echo "dispatch_deploy: run $id finished ${3}" >&2
      return 1
    fi
    sleep 5
    waited=$((waited + 5))
  done
  echo "dispatch_deploy: gave up after ${timeout}s with run $id in $state" >&2
  return 1
}

cmd_logs() {
  # The point of this verb is the *text* a job printed: the ops actions report their result there, and a deploy's failure
  # is a sentence in it. The API hands a job's log as a redirect to a zip, so it is fetched and unpacked -- a list of steps
  # says which step failed and never why.
  local workflow=${1:-deploy.yml} lines=${TAIL_LINES:-40} id jobs job
  read -r id _ _ html <<<"$(newest_run "$workflow")"
  echo "run $id: $html"
  jobs=$(api GET "/repos/$SLUG/actions/runs/$id/jobs")
  printf '%s' "$jobs" | python3 "$ROOT/tools/ops/jobs.py" list
  # A failed job first; with none, the **last** job, because an ops action reports its result in a job that succeeds and
  # that result is the whole point of asking (measured 2026-11-27: `--ops stop` printed the position it left and the tool
  # showed only `completed success`).
  local targets
  targets=$(printf '%s' "$jobs" | python3 "$ROOT/tools/ops/jobs.py" failed)
  [ -z "$targets" ] && targets=$(printf '%s' "$jobs" | python3 "$ROOT/tools/ops/jobs.py" last)
  for job in $targets; do
    echo "--- log tail of job $job"
    curl -sSL -H "Authorization: token $(read_token)" -H 'Accept: application/vnd.github+json' \
      "$API/repos/$SLUG/actions/jobs/$job/logs" -o /tmp/tq/joblog.zip 2>/dev/null || true
    python3 "$ROOT/tools/ops/jobs.py" tail "$lines"
  done
}

cmd_check() {
  # A credential that is wrong must say *how* it is wrong: shape first (a fine-grained token is `github_pat_` + 22 + `_` +
  # 59 = 93 characters), then whether GitHub accepts it, then whether it can see this repository. Measured 2026-11-27: a
  # token pasted with one extra character answered 401 with no hint, and the shape is what identified it.
  local token
  token=$(read_token) || return 2
  local shape
  shape=$(printf '%s' "$token" | awk -F_ '{ out = ""; for (i = 1; i <= NF; i++) out = out length($i) "/"; print out }')
  echo "token: ${#token} bytes, prefix $(printf '%s' "$token" | cut -c1-11), segments $shape"
  case "$token" in
    github_pat_*)
      if [ "${#token}" -ne 93 ]; then
        echo "  a fine-grained token is 93 characters (github_pat_ + 22 + _ + 59); this one is ${#token} -- re-copy it" >&2
      fi ;;
    *) [ "${#token}" -eq 40 ] || echo "  a classic token is 40 characters; this one is ${#token}" >&2 ;;
  esac
  local user repo
  user=$(curl -sS -o /dev/null -w '%{http_code}' -H "Authorization: token $token" "$API/user")
  echo "  GET /user -> $user"
  repo=$(curl -sS -o /dev/null -w '%{http_code}' -H "Authorization: token $token" "$API/repos/$SLUG")
  echo "  GET /repos/$SLUG -> $repo"
  if [ "$user" != 200 ]; then
    echo "  the token is not accepted at all: re-copy it (a fine-grained token is shown once, and is 93 characters)" >&2
    return 1
  fi
  if [ "$repo" != 200 ]; then
    echo "  the token is valid but cannot see $SLUG: give it Repository access for that repository" >&2
    return 1
  fi
  echo "  ok -- Actions: Read and write is the last thing to confirm, and \`status\` proves it"
}

cmd_window() {
  # One command from the host's window series to a file in this tree. The host is the only machine that can see the
  # ledger, and until this verb existed the numbers were copied out of a journal by hand -- which is how a window came to
  # be read from two lines pasted into a conversation while its own row carried neither. The file it saves is under
  # `out/`, which is generated output and stays out of the commit, so the series travels with the tree without becoming a
  # tracked copy of the host's state.
  local save=${1:-$ROOT/out/runtime/window_ledger.csv} target=${2:-300} id job jobs
  cmd_run --ops ledger || return 1
  cmd_wait ops.yml || return 1
  read -r id _ _ html <<<"$(newest_run ops.yml)"
  echo "run $id: $html"
  jobs=$(api GET "/repos/$SLUG/actions/runs/$id/jobs")
  job=$(printf '%s' "$jobs" | python3 "$ROOT/tools/ops/jobs.py" last)
  if [ -z "$job" ]; then
    echo "dispatch_deploy: run $id has no job to read -- nothing was pulled, and no file was written" >&2
    return 1
  fi
  curl -sSL -H "Authorization: token $(read_token)" -H 'Accept: application/vnd.github+json' \
    "$API/repos/$SLUG/actions/jobs/$job/logs" -o /tmp/tq/joblog.zip || {
      echo "dispatch_deploy: could not fetch the log of job $job -- nothing was pulled" >&2
      return 1
    }
  python3 "$ROOT/tools/ops/jobs.py" lines > /tmp/tq/joblog.txt || return 1
  # `--from-log` refuses a log with no CSV in it rather than writing an empty file: a pull that reports success over
  # nothing is the failure this whole path exists to prevent.
  python3 "$ROOT/tools/ops/window_ledger.py" --from-log /tmp/tq/joblog.txt --save "$save" \
    --table --progress "$target"
}

main() {
  local command=${1:-status}
  shift || true
  case "$command" in
    run) [ "${1:-}" = "--dry-run" ] && { DRY_RUN=1; shift; }; cmd_run "$@" ;;
    status) [ "${1:-}" = "--dry-run" ] && DRY_RUN=1; cmd_status ;;
    wait) cmd_wait "$@" ;;
    logs) cmd_logs "$@" ;;
    window) cmd_window "$@" ;;
    check) cmd_check ;;
    -h|--help|help) usage ;;
    *) echo "dispatch_deploy: unknown command $command (check|run|status|wait|logs|window)" >&2; return 2 ;;
  esac
}

SLUG=${TOYQUANT_REPO:-$(repo_slug)}
if [ -z "$SLUG" ]; then
  echo "dispatch_deploy: no origin remote -- set TOYQUANT_REPO=owner/name" >&2
  exit 2
fi
main "$@"

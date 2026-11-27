#!/usr/bin/env bash
# Everything a deploy can be told locally, in about three seconds, before any SSH connection exists.
#
# The rule this exists for: a fault found on the host costs a runner start, a build, an upload and a round trip through
# the journal -- measured 2026-11-27, several rounds of four to five minutes each, every one of them ending on something a
# local parse or a rule could have refused. So the order is: parse the workflows, parse every shell script a deploy
# carries, run the rules a host would otherwise discover, and only then dispatch.
#
# It is deliberately not `verify_l2.sh fast`: that is the commit-time gate and it costs one to two minutes. This is the
# deploy-time question -- "is the thing about to travel well-formed and self-consistent" -- and it stays under five
# seconds so that running it before every dispatch is not a decision.
set -uo pipefail

cd "$(dirname "$0")/../.." || exit 1
failed=0
start=$(date +%s)

check() {
  local name=$1; shift
  local began=$(date +%s%N)
  if "$@" > /tmp/predeploy-step.log 2>&1; then
    local ms=$(( ($(date +%s%N) - began) / 1000000 ))
    printf 'predeploy: %-46s ok (%d ms)\n' "$name" "$ms"
  else
    printf 'predeploy: %-46s FAILED\n' "$name"
    sed -n '1,12p' /tmp/predeploy-step.log | sed 's/^/    /'
    failed=1
  fi
}

parse_workflow() {
  python3 -c 'import sys, yaml; yaml.safe_load(open(sys.argv[1]))' "$1"
}

parse_shell() {
  local path
  for path in "$@"; do
    bash -n "$path" || return 1
  done
}

# The remote script in deploy.yml is a heredoc; the runner hands it to bash on the host, so it is parsed here the same
# way. An apostrophe in an echoed sentence once closed a single-quoted ssh argument and killed the step after its
# diagnostics (measured 2026-11-27).
parse_remote() {
  python3 - <<'PY'
import subprocess, sys
text = open('.github/workflows/deploy.yml').read()
start = text.find("<<'REMOTE'")
if start < 0:
    print('deploy.yml has no REMOTE heredoc'); sys.exit(1)
start = text.index('\n', start) + 1
end = text.find("          REMOTE", start)
if end < 0:
    print('deploy.yml has no REMOTE terminator'); sys.exit(1)
body = text[start:end]
open('/tmp/predeploy-remote.sh', 'w').write(body)
sys.exit(subprocess.call(['bash', '-n', '/tmp/predeploy-remote.sh']))
PY
}

check "deploy.yml parses"            parse_workflow .github/workflows/deploy.yml
check "ops.yml parses"               parse_workflow .github/workflows/ops.yml
check "deploy.yml's remote script"   parse_remote
check "shell scripts parse"          parse_shell deploy/*.sh tools/ops/*.sh
check "check_deploy rules"           python3 tools/gate/check_deploy.py
check "check_deploy self-test"       python3 tools/gate/check_deploy.py --self-test
check "daily_report self-test"       python3 deploy/daily_report.py --self-test

if [ -n "$(git status --porcelain)" ]; then
  echo "predeploy: the working tree is dirty -- a deploy would carry something the gate has not seen"
  failed=1
fi

elapsed=$(( $(date +%s) - start ))
if [ "$failed" = 0 ]; then
  echo "predeploy: OK (${elapsed}s) -- safe to dispatch"
  exit 0
fi
echo "predeploy: refusing the dispatch; fix the check above first" >&2
exit 1

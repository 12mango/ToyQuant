#!/usr/bin/env bash
# Make a fresh host able to run a session, and make a running host agree with this file again. Idempotent: every step is
# a create-if-absent or an install, so the second run of this script is a no-op with a report.
#
#   sudo bash deploy/bootstrap_server.sh                 # do the work
#   sudo bash deploy/bootstrap_server.sh --dry-run       # print what it would do
#
# What it does *not* do: it never writes a credential and never writes a live configuration. Those are the operator's two
# files (`/etc/toyquant/live.env` and `/etc/toyquant/live.yaml`), they are created from the tracked templates with
# `install -m 0640` and edited in place, and this script refuses to start a session without them.
#
# The script is written for Ubuntu 24.04 (the host the deploy names) and checks for `apt-get` and `systemctl` rather than
# assuming them, because a bootstrap that half-runs on the wrong distribution is worse than one that refuses.
set -euo pipefail

REPO_DIR=/opt/toyquant
ETC_DIR=/etc/toyquant
SERVICE_USER=toyquant
SERVICE_NAME=toyquant-live
DRY_RUN=0

while [ $# -gt 0 ]; do
  case "$1" in
    --repo-dir) REPO_DIR=$2; shift 2 ;;
    --etc-dir) ETC_DIR=$2; shift 2 ;;
    --user) SERVICE_USER=$2; shift 2 ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
    *) echo "bootstrap: unknown argument $1" >&2; exit 2 ;;
  esac
done

run() {  # run <description> <command...>
  local what=$1
  shift
  if [ "$DRY_RUN" = 1 ]; then
    echo "would: $what"
    return 0
  fi
  echo "doing: $what"
  "$@"
}

[ "$(id -u)" = 0 ] || [ "$DRY_RUN" = 1 ] || { echo "bootstrap: needs root (or --dry-run)" >&2; exit 2; }
command -v systemctl > /dev/null || { echo "bootstrap: no systemctl; this is not a systemd host" >&2; exit 2; }
[ -d "$REPO_DIR" ] || { echo "bootstrap: $REPO_DIR does not exist -- unpack the deploy bundle there first" >&2; exit 2; }

# The packages a session needs: the interpreter the relay and the config reader run on, the CA store a TLS call needs,
# and the log rotator that keeps a week of journal-adjacent files from filling the disk.
if command -v apt-get > /dev/null; then
  run "install the runtime packages" apt-get install -y --no-install-recommends python3 ca-certificates logrotate
else
  echo "bootstrap: no apt-get; install python3, ca-certificates and logrotate yourself" >&2
fi

# An unprivileged service account. A session that runs as root turns a bug in the quote loop into a bug in the host.
if ! id "$SERVICE_USER" > /dev/null 2>&1; then
  run "create the service account $SERVICE_USER" \
    useradd --system --create-home --home-dir "/var/lib/$SERVICE_USER" --shell /usr/sbin/nologin "$SERVICE_USER"
fi

# The two directories have different lives: the program is replaced by every deploy, the configuration outlives them.
run "create the program directory" install -d -m 0755 "$REPO_DIR"
for sub in data/runtime logs config deploy; do
  run "create $REPO_DIR/$sub" install -d -m 0755 "$REPO_DIR/$sub"
done
run "create the configuration directory" install -d -m 0750 -g "$SERVICE_USER" "$ETC_DIR"
run "hand the writable paths to $SERVICE_USER" chown -R "$SERVICE_USER:$SERVICE_USER" \
  "$REPO_DIR/data/runtime" "$REPO_DIR/logs"

# Rotation, so that a session left running for a month does not fill the disk with its own log.
if [ -f "$REPO_DIR/deploy/logrotate/toyquant" ]; then
  run "install the logrotate rule" install -m 0644 "$REPO_DIR/deploy/logrotate/toyquant" /etc/logrotate.d/toyquant
else
  echo "bootstrap: $REPO_DIR/deploy/logrotate/toyquant is missing from the bundle" >&2
fi
# The journal's own ceiling. logrotate bounds the engine's files and nothing bounded systemd's journal, which is the log a
# 20 GB disk loses to first -- and the drop-in is what makes it 500 MB rather than the distribution's 10% of the volume.
if [ -f "$REPO_DIR/deploy/journald/toyquant.conf" ]; then
  run "create the journald drop-in directory" install -d -m 0755 /etc/systemd/journald.conf.d
  run "install the journald ceiling" install -m 0644 "$REPO_DIR/deploy/journald/toyquant.conf" \
    /etc/systemd/journald.conf.d/toyquant.conf
  run "reload journald" systemctl restart systemd-journald
else
  echo "bootstrap: $REPO_DIR/deploy/journald/toyquant.conf is missing from the bundle" >&2
fi

# The units are installed here rather than by the workflow so that a host brought up by hand and a host brought up by CI
# end up identical. Two of them: the relay talks to the venue and holds the only key, the engine reads its sockets.
SERVICE_NAME=${SERVICE_NAME:-toyquant-live}
RELAY_SERVICE_NAME=${RELAY_SERVICE_NAME:-toyquant-relay}
units=""
# The notifier is a template unit (`name@.service`), so it is installed under the name it is instantiated from and never
# enabled: `OnFailure=` in the two units above is its only caller.
for unit in "$RELAY_SERVICE_NAME" "$SERVICE_NAME" toyquant-notify@ toyquant-report; do
  if [ -f "$REPO_DIR/deploy/$unit.service" ]; then
    run "install $unit" install -m 0644 "$REPO_DIR/deploy/$unit.service" "/etc/systemd/system/$unit.service"
    units="$units $unit"
  else
    echo "bootstrap: $REPO_DIR/deploy/$unit.service is missing from the bundle" >&2
  fi
done
if [ -n "$units" ]; then
  run "reload systemd" systemctl daemon-reload
  for unit in $units; do
    case "$unit" in
      *@) : ;;  # a template is not enabled: nothing starts it but `OnFailure=`
      *)
        # Only a unit with an `[Install]` section can be enabled, and systemd answers the others with a four-line
        # explanation that reads like an error in a deploy log ("The unit files have no installation config..."). The
        # report unit is the timer's, so it is started by `toyquant-report.timer` and never enabled itself.
        if grep -q '^\[Install\]' "/etc/systemd/system/$unit.service"; then
          run "enable $unit at boot" systemctl enable "$unit"
        else
          echo "bootstrap: $unit has no [Install] section; something else starts it (a timer or a unit), so it is not enabled"
        fi ;;
    esac
  done
fi
# The daily report is a timer, not a unit: enabled and started, so `systemctl list-timers` can answer "when is the next
# one" and `journalctl -u toyquant-report` answers "what did the last one say".
if [ -f "$REPO_DIR/deploy/toyquant-report.timer" ]; then
  run "install the daily report timer" install -m 0644 "$REPO_DIR/deploy/toyquant-report.timer" \
    /etc/systemd/system/toyquant-report.timer
  run "start the daily report timer" systemctl enable --now toyquant-report.timer
else
  echo "bootstrap: $REPO_DIR/deploy/toyquant-report.timer is missing from the bundle" >&2
fi

# The two operator files. This script creates them *from the templates* when they are absent, so that the first deploy
# has something to read, and refuses to touch them afterwards: a rewrite of a live configuration is an action a person
# takes, not one a bootstrap repeats.
if [ ! -f "$ETC_DIR/live.yaml" ]; then
  if [ -f "$REPO_DIR/config/live.example.yaml" ]; then
    run "seed $ETC_DIR/live.yaml from the template" install -m 0640 -g "$SERVICE_USER" \
      "$REPO_DIR/config/live.example.yaml" "$ETC_DIR/live.yaml"
    echo "bootstrap: $ETC_DIR/live.yaml is the template -- edit it before the first real session"
  fi
fi
if [ ! -f "$ETC_DIR/live.env" ]; then
  if [ -f "$REPO_DIR/.env.example" ]; then
    run "seed $ETC_DIR/live.env from the template" install -m 0640 -g "$SERVICE_USER" \
      "$REPO_DIR/.env.example" "$ETC_DIR/live.env"
    echo "bootstrap: $ETC_DIR/live.env carries placeholders -- fill them in before the first real session"
  fi
fi
# Notifications are optional, and seeding the template is what makes the notifier's first failure a mail rather than a
# journal line about a missing file. Created like the other two and never rewritten: it holds an SMTP password.
if [ ! -f "$ETC_DIR/notify.env" ] && [ -f "$REPO_DIR/deploy/notify.env.example" ]; then
  run "seed $ETC_DIR/notify.env from the template" install -m 0640 -g "$SERVICE_USER" \
    "$REPO_DIR/deploy/notify.env.example" "$ETC_DIR/notify.env"
  echo "bootstrap: $ETC_DIR/notify.env carries placeholders -- fill them in to receive a mail when a unit fails"
fi

if [ "$DRY_RUN" = 0 ]; then
  echo
  echo "bootstrap: done. The two files a session reads are the operator's:"
  echo "  $ETC_DIR/live.yaml   the instrument, the size and the quoting shape (0640 root:$SERVICE_USER)"
  echo "  $ETC_DIR/live.env    the three venue credentials (0640 root:$SERVICE_USER)"
  echo "Start it with:  systemctl reset-failed $RELAY_SERVICE_NAME $SERVICE_NAME || true"
  echo "                systemctl restart $RELAY_SERVICE_NAME $SERVICE_NAME"
  echo "                bash $REPO_DIR/deploy/check_health.sh"
fi

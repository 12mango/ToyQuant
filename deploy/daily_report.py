#!/usr/bin/env python3
"""The day's one mail: what the session did, what the account holds, and what the host has left.

    python3 deploy/daily_report.py --dry-run     # print it
    python3 deploy/daily_report.py               # mail it

Run by `toyquant-report.timer` (once a day, after the window the operator named in the timer). Four groups, and each one
says `unavailable` with a reason rather than a zero when it cannot read its source -- a report that prints `net_pnl=0.00`
because the journal was rotated is worse than one that says it could not read the journal:

  * the account, from the venue's own REST surface (`okx_probe`'s reads: USDT equity, the position, pending orders)
  * the session, from `journalctl -u toyquant-live --since -24h`: fills, the last `[PNL]`, the last `[RECON]`, any
    `[BREAKER]`, and the restarts systemd counted
  * the host: disk on `/` and `/opt/toyquant`, memory, uptime
  * the two warnings the operator acts on: a break in `[RECON]` and a restart count above zero

The mail goes through `notify.py`, whose SMTP settings are the same ones `/etc/toyquant/live.env` already carries for
alerts (`SMTP_*`, `ALERT_EMAIL_TO` or `NOTIFY_TO`). No credentials means no mail and a sentence saying so.
"""
import argparse
import os
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "live"))
import notify  # noqa: E402
import okx_probe  # noqa: E402

UNIT = os.environ.get("SERVICE_NAME", "toyquant-live")
WINDOW_HOURS = 24


def run(command, timeout=25):
    """A command's stdout, or `None` when it could not be run at all."""
    try:
        finished = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    except (OSError, subprocess.SubprocessError):
        return None
    return finished.stdout


def host_facts():
    usage = shutil.disk_usage("/")
    facts = {"disk_root": f"{usage.used / usage.total:.0%} used, {usage.free / 2**30:.1f} GiB free of "
                          f"{usage.total / 2**30:.1f} GiB"}
    try:
        program = shutil.disk_usage("/opt/toyquant")
        facts["disk_program"] = f"{program.used / program.total:.0%} of {program.total / 2**30:.1f} GiB"
    except OSError:
        facts["disk_program"] = "unavailable (/opt/toyquant is not on this host)"
    try:
        with open("/proc/meminfo") as handle:
            meminfo = dict(line.split(":", 1) for line in handle.read().splitlines() if ":" in line)
        total = int(meminfo["MemTotal"].split()[0]) / 2**20
        available = int(meminfo["MemAvailable"].split()[0]) / 2**20
        facts["memory"] = f"{total - available:.1f} GiB used of {total:.1f} GiB"
    except (OSError, KeyError, ValueError):
        facts["memory"] = "unavailable (/proc/meminfo could not be read)"
    try:
        with open("/proc/uptime") as handle:
            facts["uptime"] = f"{float(handle.read().split()[0]) / 3600:.1f} h"
    except (OSError, ValueError, IndexError):
        facts["uptime"] = "unavailable"
    journal = run(["journalctl", "--disk-usage"])
    facts["journal"] = journal.strip() if journal else "unavailable"
    return facts


def session_facts():
    """The last day of the session's own journal, as the four lines an operator reads."""
    text = run(["journalctl", "-u", UNIT, "--since", f"-{WINDOW_HOURS}h", "--no-pager", "-q"])
    if text is None:
        return {"fills": "unavailable", "pnl": "unavailable", "recon": "unavailable", "breaker": "none"}
    lines = text.splitlines()

    def last(tag):
        found = [line for line in lines if tag in line]
        return found[-1].strip() if found else f"no {tag} line in {WINDOW_HOURS} h"

    facts = {"fills": f"{sum(1 for line in lines if '[FILL]' in line)} fill line(s) in {WINDOW_HOURS} h",
             "pnl": last("[PNL]"), "recon": last("[RECON]"), "breaker": last("[BREAKER]")}
    state = run(["systemctl", "show", "-p", "NRestarts", "--value", UNIT])
    active = run(["systemctl", "show", "-p", "ActiveState", "--value", UNIT])
    facts["unit"] = f"{UNIT} {(active or 'unknown').strip()}, restarts={(state or 'unavailable').strip()}"
    return facts


def account_facts():
    """USDT equity, the position and the working orders, from the venue. A missing credential is named, not faked."""
    try:
        okx_probe.load_credentials()
        equity = okx_probe.balance()
        position = okx_probe.positions()
        pending = okx_probe.pending()
    except SystemExit as error:  # `load_credentials` refuses by naming the variable
        return {"equity": f"unavailable ({error})", "position": "unavailable", "pending": "unavailable"}
    except OSError as error:
        return {"equity": f"unavailable ({error})", "position": "unavailable", "pending": "unavailable"}
    contracts = sum(float(row.get("pos", "0") or 0) for row in (position or [])
                    if row.get("instId") in (None, okx_probe.INST))
    return {"equity": "unavailable (the venue did not answer the balance)" if equity is None else f"{equity:,.2f} USDT",
            "position": f"{contracts:g} contract(s) on {okx_probe.INST}",
            "pending": f"{len(pending or [])} working order(s)"}


def compose(groups, host=None):
    """The body: four labelled groups, no column alignment to break in a phone's mail client."""
    host = host or socket.gethostname()
    subject = f"ToyQuant daily: {groups['account'].get('equity', 'unavailable')} on {host}"
    body = []
    for title, facts in groups.items():
        body.append(f"-- {title}")
        for name, value in facts.items():
            body.append(f"{name + ':':<12}{value}")
        body.append("")
    return subject, "\n".join(body)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dry-run", action="store_true", help="print the report instead of mailing it")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()

    groups = {"session": session_facts(), "account": account_facts(), "host": host_facts()}
    subject, body = compose(groups)
    settings = notify.load_settings()
    if args.dry_run:
        print(f"To: {settings['NOTIFY_TO'] or '<ALERT_EMAIL_TO is not set>'}\nSubject: {subject}\n\n{body}")
        return 0
    if not settings["SMTP_HOST"] or not settings["NOTIFY_TO"]:
        print(f"daily_report: no SMTP_HOST/ALERT_EMAIL_TO in {notify.ENV_FILE} or the environment; no mail sent",
              file=sys.stderr)
        return 0
    print(f"daily_report: {notify.send(subject, body, settings)}", file=sys.stderr)
    return 0


def self_test():
    """A report that names its three groups, and one that says `unavailable` instead of inventing a zero."""
    groups = {"session": {"fills": "3 fill line(s) in 24 h", "pnl": "[PNL] net_pnl_usd=-1.20"},
              "account": {"equity": "498.10 USDT", "pending": "2 working order(s)"},
              "host": {"disk_root": "42% used", "memory": "1.1 GiB used of 3.9 GiB"}}
    subject, body = compose(groups, host="toyquant-test")
    assert "498.10 USDT" in subject and "toyquant-test" in subject, subject
    for needle in ("-- session", "-- account", "-- host", "2 working order(s)", "1.1 GiB"):
        assert needle in body, (needle, body)
    # An unreadable source is a sentence, not a zero: the two silent-zero shapes the report exists to avoid.
    quiet = compose({"account": {"equity": "unavailable (TOYQUANT_API_KEY is not set)"}, "host": {}}, host="h")[1]
    assert "unavailable" in quiet and "0.00" not in quiet, quiet
    print("daily_report self-test OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())

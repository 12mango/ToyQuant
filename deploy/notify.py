#!/usr/bin/env python3
"""One mail when a unit fails, because a crash loop's only witness is a journal nobody is watching.

`deploy/toyquant-live.service` and `deploy/toyquant-relay.service` carry `OnFailure=toyquant-notify@%n.service`, so the
instantiation names the unit that failed:

    systemctl status 'toyquant-notify@toyquant-live.service'   # what the last attempt said
    python3 deploy/notify.py --unit toyquant-live --dry-run     # the same mail, printed instead of sent

The configuration is one ignored file, `/etc/toyquant/notify.env`, because the SMTP password is a credential like the
venue's and lives where the others do:

    SMTP_HOST=smtp.example.com
    SMTP_PORT=587
    SMTP_USER=alerts@example.com
    SMTP_PASS=...
    SMTP_FROM=toyquant@example.com
    NOTIFY_TO=operator@example.com

Without that file the tool says so in the journal and exits 0. A notifier that fails because it is unconfigured is a
second failure on top of the one it was meant to report -- and the unit it runs in is `oneshot`, so a non-zero exit there
is a red unit nobody asked for.

The mail is plain text and deliberately short: the unit, the host, the four systemd properties that name the ending, and
the tail of the failed unit's own journal. Numbers are not judged -- this file's job is to get a human to the host, not to
decide whether the session was right.
"""
import argparse
import os
import socket
import subprocess
import sys

ENV_FILE = os.environ.get("TOYQUANT_NOTIFY_ENV") or "/etc/toyquant/notify.env"
PROPERTIES = ("ActiveState", "SubState", "Result", "NRestarts", "ExecMainStatus")


def load_settings():
    """The SMTP settings from the environment, then from the file. A name already set is never overridden."""
    try:
        with open(ENV_FILE) as handle:
            for line in handle:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                name, _, value = line.partition("=")
                os.environ.setdefault(name.strip(), value.strip().strip('"').strip("'"))
    except FileNotFoundError:
        pass
    except PermissionError:
        print(f"notify: {ENV_FILE} is not readable by this user", file=sys.stderr)
    return {name: os.environ.get(name, "") for name in
            ("SMTP_HOST", "SMTP_PORT", "SMTP_USER", "SMTP_PASS", "SMTP_FROM", "NOTIFY_TO")}


def unit_state(unit, lines=30):
    """The properties that name how a unit ended, and the tail of its journal. Missing systemd is not an error here."""
    state = {}
    for prop in PROPERTIES:
        try:
            state[prop] = subprocess.run(["systemctl", "show", "-p", prop, "--value", unit],
                                         capture_output=True, text=True, timeout=10).stdout.strip()
        except (OSError, subprocess.SubprocessError):
            state[prop] = "unavailable"
    try:
        tail = subprocess.run(["journalctl", "-u", unit, "-n", str(lines), "--no-pager", "-q"],
                              capture_output=True, text=True, timeout=20).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        tail = "journalctl is unavailable"
    return state, tail

def compose(unit, state, tail, lines=30):
    """The message: who, where, how it ended, and the unit's own last words."""
    host = socket.gethostname()
    subject = f"ToyQuant: {unit} failed on {host}"
    body = [f"unit:      {unit}", f"host:      {host}"]
    for prop in PROPERTIES:
        body.append(f"{prop + ':':<13}{state.get(prop, 'unknown')}")
    body.append("")
    body.append(f"last {lines} journal line(s):")
    body.extend(tail.splitlines()[-lines:])
    return subject, "\n".join(body) + "\n"


def send(subject, body, settings):
    """One SMTP message. Returns a sentence describing what happened, never raises."""
    import smtplib
    from email.message import EmailMessage
    message = EmailMessage()
    message["Subject"] = subject
    message["From"] = settings["SMTP_FROM"] or settings["SMTP_USER"]
    message["To"] = settings["NOTIFY_TO"]
    message.set_content(body)
    port = int(settings["SMTP_PORT"] or 587)
    try:
        if port == 465:
            server = smtplib.SMTP_SSL(settings["SMTP_HOST"], port, timeout=20)
        else:
            server = smtplib.SMTP(settings["SMTP_HOST"], port, timeout=20)
        with server:
            if port != 465:
                server.starttls()
            if settings["SMTP_USER"]:
                server.login(settings["SMTP_USER"], settings["SMTP_PASS"])
            server.send_message(message)
    except (OSError, smtplib.SMTPException) as error:
        return f"the mail could not be sent ({error})"
    return f"mailed {settings['NOTIFY_TO']} via {settings['SMTP_HOST']}:{port}"


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--unit", default=os.environ.get("NOTIFY_UNIT", ""),
                        help="the unit that failed; the systemd instance name when the template fires (%%i)")
    parser.add_argument("--lines", type=int, default=30, help="journal lines to attach (default 30)")
    parser.add_argument("--dry-run", action="store_true", help="print the mail instead of sending it")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()

    if not args.unit:
        print("notify: no --unit: nothing names what failed", file=sys.stderr)
        return 2
    settings = load_settings()
    state, tail = unit_state(args.unit, args.lines)
    subject, body = compose(args.unit, state, tail, args.lines)
    if args.dry_run:
        print(f"To: {settings['NOTIFY_TO'] or '<NOTIFY_TO is not set>'}\nSubject: {subject}\n\n{body}")
        return 0
    if not settings["SMTP_HOST"] or not settings["NOTIFY_TO"]:
        print(f"notify: {ENV_FILE} does not set SMTP_HOST and NOTIFY_TO; no mail sent for {args.unit}",
              file=sys.stderr)
        return 0
    print(f"notify: {send(subject, body, settings)}", file=sys.stderr)
    return 0


def self_test():
    """The two paths a notifier has to get right: a message that names the failure, and no failure of its own."""
    state = {"ActiveState": "failed", "SubState": "failed", "Result": "exit-code", "NRestarts": "5",
             "ExecMainStatus": "1"}
    subject, body = compose("toyquant-live.service", state, "line one\nline two", lines=2)
    assert "toyquant-live.service" in subject, subject
    assert "ExecMainStatus" in body and "NRestarts" in body and "line two" in body, body
    # The properties come from systemd when it exists and are named `unavailable` when it does not: a notifier that
    # raised here would be a second failure on top of the one it reports.
    quiet_state, quiet_tail = unit_state("toyquant-no-such-unit.service")
    assert set(PROPERTIES) <= set(quiet_state), quiet_state
    assert compose("toyquant-no-such-unit.service", quiet_state, quiet_tail)[1]
    # A mail with no recipient is a message nobody reads, and a failed SMTP connection is reported rather than raised.
    assert "could not be sent" in send("s", "b", {"SMTP_HOST": "127.0.0.1", "SMTP_PORT": "1",
                                                  "SMTP_USER": "", "SMTP_PASS": "",
                                                  "SMTP_FROM": "a@b", "NOTIFY_TO": "c@d"})
    print("notify self-test OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())

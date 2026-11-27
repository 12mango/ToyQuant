#!/usr/bin/env python3
"""The day's one mail: what the session did, what the account holds, and what the host has left.

    python3 deploy/daily_report.py --dry-run     # print it
    python3 deploy/daily_report.py               # mail it

Run by `toyquant-report.timer` (once a day, after the trading window). Four groups, and each one says `unavailable` with
a reason rather than a zero when it cannot read its source -- a report that prints `net_pnl=0.00` because the journal was
rotated is worse than one that says it could not read the journal:

  * the window, stated three ways: UTC (the clock the engine and the archive use), HKT and JST (the two clocks the people
    involved read) -- because this host is in Tokyo, the operator is in Hong Kong and the session is named in UTC
  * the account, from the venue's own REST surface (`okx_probe`'s reads: USDT equity, the position, pending orders)
  * the session, from `journalctl -u toyquant-live` over **the window's own interval**, in UTC: fills, the last `[PNL]`,
    the last `[RECON]`, any `[BREAKER]`, and the restarts systemd counted
  * the host: disk on `/` and `/opt/toyquant`, memory, uptime, and which clock the host keeps

**The interval is the most recently completed window, not a rolling 24 h.** A rolling day was wrong on this host in the
silent way time zones are wrong: the window 13:30-15:30 UTC is 21:30-23:30 in Hong Kong and 22:30-00:30 in Tokyo, the
host's *location* is Tokyo while its *clock* is `Etc/UTC`, and a mail that fired at 00:15 host-local reported 00:15 UTC --
nine hours after the close, with the window split across two of its own reports. The window is named once, in UTC, by
`--window-start/--window-end` (defaults `13:30`/`15:30`), and `journalctl` is given epoch bounds so the host's zone cannot
reinterpret them.

The mail goes through `notify.py`, whose SMTP settings are the same ones `/etc/toyquant/live.env` already carries for
alerts (`SMTP_*`, `ALERT_EMAIL_TO` or `NOTIFY_TO`). No credentials means no mail and a sentence saying so.
"""
import argparse
import os
import shutil
import socket
import subprocess
import sys
from datetime import datetime, time, timedelta, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "live"))
import notify  # noqa: E402
import okx_probe  # noqa: E402

UNIT = os.environ.get("SERVICE_NAME", "toyquant-live")


def instrument_of_the_seat():
    """The instrument the account trades: the relay's own name, before `okx_probe`'s default.

    `okx_probe` defaults to `BTC-USDT-SWAP`; this report runs with `/etc/toyquant/live.env` loaded, where the relay names
    its instrument. Measured 2026-11-27, the first working dry run said `0 contract(s) on BTC-USDT-SWAP` about an account
    that trades `UP-USDT-SWAP` -- a position line about another contract, which reads as agreement.
    """
    return os.environ.get("INST_ID") or os.environ.get("RELAY_INSTRUMENT") or okx_probe.INST


okx_probe.INST = instrument_of_the_seat()
# The window, in UTC, named once. The engine's own session clock is UTC (`--wind-down-end` is "minutes of the UTC day"),
# the archive's timestamps are UTC, and this report reads the journal between two UTC instants.
WINDOW_START = os.environ.get("TOYQUANT_WINDOW_START_UTC", "13:30")
WINDOW_END = os.environ.get("TOYQUANT_WINDOW_END_UTC", "15:30")
# The two clocks a reader uses. Neither zone has daylight saving, so a fixed offset is exact and needs no tzdata.
ZONES = (("UTC", timezone.utc), ("HKT", timezone(timedelta(hours=8))), ("JST", timezone(timedelta(hours=9))))


def clock(text):
    """`HH:MM` as a `time`, or a sentence naming what was given instead."""
    try:
        hour, minute = text.split(":")
        return time(int(hour), int(minute))
    except (AttributeError, ValueError):
        raise SystemExit(f"daily_report: '{text}' is not HH:MM (the window is named in UTC)")


def window_at(now, start=WINDOW_START, end=WINDOW_END):
    """The most recently completed window, as two UTC instants.

    A window that ends before it starts crosses UTC midnight (`23:00` to `01:00`), so the start belongs to the previous
    day. A window that has not closed yet at `now` means yesterday's is the last complete one.
    """
    start_at = clock(start)
    end_at = clock(end)
    end_utc = datetime.combine(now.date(), end_at, tzinfo=timezone.utc)
    start_utc = datetime.combine(now.date(), start_at, tzinfo=timezone.utc)
    if start_utc >= end_utc:
        start_utc -= timedelta(days=1)
    if now < end_utc:
        start_utc -= timedelta(days=1)
        end_utc -= timedelta(days=1)
    return start_utc, end_utc


def zone_range(start_utc, end_utc):
    """The same interval in each clock, as one line: the three readings of one session."""
    return "  ".join(f"{name} {start_utc.astimezone(offset):%H:%M}-{end_utc.astimezone(offset):%H:%M}"
                     for name, offset in ZONES)


def host_zone():
    local = datetime.now().astimezone()
    return f"{local.tzname() or 'unknown'} (UTC{local:%z})"


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
    facts["clock"] = host_zone()
    return facts


def session_facts(start_utc, end_utc):
    """The window's own journal, as the lines an operator reads.

    Epoch bounds (`@<seconds>`) and `--utc`, so neither this host's zone nor its locale can move the interval or the
    stamps: the host's clock is `Etc/UTC`, the operator is in Hong Kong, and the window is named in UTC.

    **There is no `[FILL]` line to count.** The engine prints one summary per run, not a line per fill, so the previous
    version's `sum(1 for line if '[FILL]' in line)` was a zero that could never be anything else -- a report saying "0
    fill line(s)" on a session that traded, which is exactly the silent zero this file exists to refuse. The evidence for
    what traded is the last run's own counters: `[PORTFOLIO]` carries `maker_trade_count`/`taker_trade_count` and the
    fees, `[EXECUTION]` the submissions and the venue's reports, and `[PNL]` the money.
    """
    text = run(["journalctl", "-u", UNIT, "--since", f"@{int(start_utc.timestamp())}",
                "--until", f"@{int(end_utc.timestamp())}", "--utc", "--no-pager", "-q"])
    if text is None:
        return {"trades": "unavailable", "pnl": "unavailable", "execution": "unavailable",
                "recon": "unavailable", "breaker": "none"}
    lines = text.splitlines()

    def last(tag):
        found = [line for line in lines if tag in line]
        return found[-1].strip() if found else f"no {tag} line in the window"

    facts = {"trades": last("[PORTFOLIO]"), "pnl": last("[PNL]"), "execution": last("[EXECUTION]"),
             "recon": last("[RECON]"), "breaker": last("[BREAKER]")}
    state = run(["systemctl", "show", "-p", "NRestarts", "--value", UNIT])
    active = run(["systemctl", "show", "-p", "ActiveState", "--value", UNIT])
    facts["unit"] = f"{UNIT} {(active or 'unknown').strip()}, restarts={(state or 'unavailable').strip()}"
    return facts


def account_facts():
    """USDT equity, the position and the working orders, from the venue. A missing credential is named, not faked."""
    try:
        okx_probe.load_credentials()
        # `load_credentials()` also puts every `KEY=value` of the operator's file into the environment (it uses
        # `setdefault` for all of them, not only the three credentials), so the instrument is known *after* it -- which is
        # why this is read here and not only at import: a manual `--dry-run` run has no EnvironmentFile, and the first one
        # reported `0 contract(s) on BTC-USDT-SWAP` for an account that trades `UP-USDT-SWAP`.
        okx_probe.INST = instrument_of_the_seat()
        equity = okx_probe.balance()
        position = okx_probe.positions()
        pending = okx_probe.pending()
    except SystemExit as error:  # `load_credentials` refuses by naming the variable
        return {"equity": f"unavailable ({error})", "position": "unavailable", "pending": "unavailable"}
    except OSError as error:
        return {"equity": f"unavailable ({error})", "position": "unavailable", "pending": "unavailable"}
    contracts = sum(float(row.get("pos", "0") or 0) for row in (position or [])
                    if row.get("instId") in (None, okx_probe.INST))
    # `okx_probe.balance()` hands back the venue's own text (`details[ccy=USDT].eq`), not a float -- measured 2026-11-27
    # on the host: `f"{equity:,.2f}"` raised `Unknown format code 'f' for object of type 'str'`, which would have been a
    # traceback in tonight's mail instead of a report. Parse it, and say so if it does not parse.
    try:
        amount = float(equity) if equity not in (None, "") else None
    except (TypeError, ValueError):
        return {"equity": f"unavailable (the venue's balance was not a number: {equity!r})", "position": "unavailable",
                "pending": "unavailable"}
    # The reads happen now, after the window, so the line says when rather than leaving the reader to assume a moment.
    at = datetime.now(timezone.utc)
    return {"equity": "unavailable (the venue did not answer the balance)" if amount is None else f"{amount:,.2f} USDT",
            "position": f"{contracts:g} contract(s) on {okx_probe.INST}",
            "pending": f"{len(pending or [])} working order(s)",
            "read_at": f"{at:%Y-%m-%dT%H:%MZ}"}


def window_note(now, start=WINDOW_START, end=WINDOW_END):
    """Which window this mail is about, in one sentence the reader cannot misread.

    `window_at` returns the most recently **completed** window. A mail fired inside a window therefore reports the
    previous one, and a reader who assumed "today's" would be reading the wrong day -- so the mail says which interval it
    is and, when it fired inside a window, that today's is still running.
    """
    today_start, today_end = window_at(now.replace(hour=23, minute=59, second=59), start, end)
    if today_start <= now < today_end:
        return "this mail fired inside the window, so it reports the previous one, which is the last complete interval"
    return "the most recently completed window; the account is read at report time"


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
    parser.add_argument("--window-start", default=WINDOW_START, metavar="HH:MM", help="window open, UTC")
    parser.add_argument("--window-end", default=WINDOW_END, metavar="HH:MM", help="window close, UTC")
    parser.add_argument("--at", metavar="ISO8601", help="pretend it is this UTC instant (for a dry run)")
    args = parser.parse_args()
    if args.self_test:
        return self_test()

    now = datetime.fromisoformat(args.at).replace(tzinfo=timezone.utc) if args.at else datetime.now(timezone.utc)
    start_utc, end_utc = window_at(now, args.window_start, args.window_end)
    groups = {
        "window": {"utc": f"{start_utc:%Y-%m-%dT%H:%MZ} .. {end_utc:%H:%MZ}",
                   "clocks": zone_range(start_utc, end_utc),
                   "note": window_note(now, args.window_start, args.window_end)},
        "session": session_facts(start_utc, end_utc),
        "account": account_facts(),
        "host": host_facts(),
    }
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
    """A report that names its four groups, and one that says `unavailable` instead of inventing a zero."""
    groups = {"window": {"clocks": zone_range(*window_at(datetime(2026, 11, 28, 1, 0, tzinfo=timezone.utc)))},
              "session": {"trades": "[PORTFOLIO] maker_trade_count=44 taker_trade_count=1",
                          "pnl": "[PNL] net_pnl_usd=-1.20"},
              "account": {"equity": "498.10 USDT", "pending": "2 working order(s)"},
              "host": {"disk_root": "42% used", "memory": "1.1 GiB used of 3.9 GiB"}}
    subject, body = compose(groups, host="toyquant-test")
    assert "498.10 USDT" in subject and "toyquant-test" in subject, subject
    for needle in ("-- window", "-- session", "-- account", "-- host", "2 working order(s)", "1.1 GiB",
                   "maker_trade_count=44"):
        assert needle in body, (needle, body)
    # The three readings of one interval: the window is 13:30-15:30 UTC, which is 21:30-23:30 in Hong Kong and
    # 22:30-00:30 in Tokyo -- the crossing of local midnight is the point, and the reason the mail names the clock.
    start, end = window_at(datetime(2026, 11, 27, 16, 0, tzinfo=timezone.utc))
    assert (start.hour, start.minute) == (13, 30) and (end.hour, end.minute) == (15, 30), (start, end)
    assert zone_range(start, end) == "UTC 13:30-15:30  HKT 21:30-23:30  JST 22:30-00:30", zone_range(start, end)
    # Before the window has closed, the last complete one is yesterday's: 15:29Z is inside the window itself.
    start, end = window_at(datetime(2026, 11, 27, 15, 29, tzinfo=timezone.utc))
    assert start.date().isoformat() == "2026-11-26" and end.date().isoformat() == "2026-11-26", (start, end)
    # A window that crosses UTC midnight keeps its start on the previous day.
    start, end = window_at(datetime(2026, 11, 27, 2, 0, tzinfo=timezone.utc), "23:00", "01:00")
    assert start.date().isoformat() == "2026-11-26" and end.date().isoformat() == "2026-11-27", (start, end)
    # An unreadable source is a sentence, not a zero: the two silent-zero shapes the report exists to avoid.
    quiet = compose({"account": {"equity": "unavailable (TOYQUANT_API_KEY is not set)"}, "host": {}}, host="h")[1]
    assert "unavailable" in quiet and "0.00" not in quiet, quiet
    # The venue's balance is text, not a float: `okx_probe.balance()` returns `details[ccy=USDT].eq` verbatim. Measured on
    # the host 2026-11-27, formatting it raised `Unknown format code 'f' for object of type 'str'` -- a traceback where the
    # mail should be. Stub the four reads and pin both the number and the refusal.
    saved = (okx_probe.load_credentials, okx_probe.balance, okx_probe.positions, okx_probe.pending)
    try:
        okx_probe.load_credentials = lambda: None
        okx_probe.balance = lambda: "99.43355303805785"
        okx_probe.positions = lambda: [{"instId": okx_probe.INST, "pos": "-10"}]
        okx_probe.pending = lambda: []
        facts = account_facts()
        assert facts["equity"] == "99.43 USDT", facts
        assert "10 contract(s)" in facts["position"], facts
        okx_probe.balance = lambda: "not-a-number"
        assert "unavailable" in account_facts()["equity"]
    finally:
        (okx_probe.load_credentials, okx_probe.balance, okx_probe.positions, okx_probe.pending) = saved
    # A window that has not closed keeps the *previous* one as the report's subject, and the mail has to say so.
    assert "previous one" in window_note(datetime(2026, 11, 27, 14, 0, tzinfo=timezone.utc)), "inside the window"
    assert "previous one" not in window_note(datetime(2026, 11, 27, 15, 35, tzinfo=timezone.utc)), "after the close"
    # The seat's instrument comes from the relay's own name, not from `okx_probe`'s BTC default.
    saved_env = {name: os.environ.pop(name, None) for name in ("INST_ID", "RELAY_INSTRUMENT")}
    try:
        os.environ["RELAY_INSTRUMENT"] = "UP-USDT-SWAP"
        assert instrument_of_the_seat() == "UP-USDT-SWAP", instrument_of_the_seat()
        os.environ["INST_ID"] = "NES-USDT-SWAP"
        assert instrument_of_the_seat() == "NES-USDT-SWAP", instrument_of_the_seat()
    finally:
        for name, value in saved_env.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value
    print("daily_report self-test OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())

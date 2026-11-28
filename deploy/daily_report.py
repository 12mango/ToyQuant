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
    the last `[RECON]`, any `[BREAKER]`, and the restarts systemd counted -- scanned to a grace past the window's edge,
    because the session's own summary is printed there (see `SCAN_GRACE_S`)
  * the host: disk on `/` and `/opt/toyquant`, memory, uptime, and which clock the host keeps
  * the ledger: one row per window, the same numbers as numbers, appended to `/opt/toyquant/data/runtime/window_ledger.csv`
    -- and the mail's last group says whether that row was written, because a row lost to a sandbox (measured
    2026-11-27) is a series that silently starts empty

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
import csv
import os
import shutil
import socket
import subprocess
import sys
import tempfile
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
# How far past the window's last second the journal is read. The session's own summary is printed where its wind-down
# runs, which is *after* the window edge: measured 2026-11-27, `[WIND-DOWN]` through `[WS]` at 15:30:02, two seconds past
# `--window-end=15:30`. A scan bounded at the edge therefore reported "no [PORTFOLIO] line in the window" about a session
# whose accounting was in the journal, and the ledger row for that day was empty. Three minutes covers the wind-down, the
# stop timer's backstop (15:32) and systemd's own stop lines, and nothing else runs in a closed window.
SCAN_GRACE_S = 180
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


def journal_text(start_utc, end_utc):
    """The window's journal, once, as the text both the mail and the ledger read.

    Epoch bounds (`@<seconds>`) and `--utc`, so neither the host's zone nor its locale can move the interval or the
    stamps: the host's clock is `Etc/UTC`, the operator is in Hong Kong, and the window is named in UTC.

    The scan runs `SCAN_GRACE_S` past the end, and that is not slack: the session's own summary is printed *after* its
    last second. Measured 2026-11-27 (the first live window), the engine's wind-down wrote `[WIND-DOWN]`, `[L2 DATA]`,
    `[EXECUTION]`, `[PORTFOLIO]`, `[PNL]`, `[RISK]`, `[VENUE]`, `[RECON] at=end` and `[WS]` at `15:30:02`, and a scan
    bounded at `--window-end=15:30` reported "no [PORTFOLIO] line in the window" while six of those lines sat in the
    journal two seconds outside it -- so the mail said nothing traded and the ledger row for the day was empty. The
    stamps are what the mail prints, so the window is still named by its own two ends; only the *read* is wider.
    """
    return run(["journalctl", "-u", UNIT, "--since", f"@{int(start_utc.timestamp())}",
                "--until", f"@{int(end_utc.timestamp()) + SCAN_GRACE_S}", "--utc", "--no-pager", "-q"])


def field_of(line, name):
    """One `name=value` out of a summary line, or an empty cell when the line does not carry it.

    An empty cell, never a zero: a `net_pnl_usd` of 0.00 and a line that was not there are different facts.
    """
    marker = name + "="
    start = line.find(marker)
    if start < 0:
        return ""
    rest = line[start + len(marker):]
    end = rest.find(" ")
    return rest if end < 0 else rest[:end]


def ledger_row(start_utc, end_utc, lines, groups):
    """One row per window, for the long series the mail cannot show.

    The columns are the numbers the mail already quotes, so the ledger cannot say anything the journal did not: the
    window, how many runs it held, the last run's trades and money, the residual its close left, and the account at
    report time. A column the journal did not carry is empty.
    """
    portfolios = [line for line in lines if "[PORTFOLIO]" in line]
    pnls = [line for line in lines if "[PNL]" in line]
    wind_downs = [line for line in lines if "[WIND-DOWN]" in line]
    executions = [line for line in lines if "[EXECUTION]" in line]
    last_portfolio = portfolios[-1] if portfolios else ""
    last_pnl = pnls[-1] if pnls else ""
    last_wind_down = wind_downs[-1] if wind_downs else ""
    # The fill markout, which is the one number the live sample has to compare against the replay's: 300 to 500 fills is
    # what pins it, and `[EXECUTION]` carries it per unit of quantity (`markout_per_unit_ticks`). Empty when the line is
    # absent, as every other cell is.
    last_execution = executions[-1] if executions else ""
    account = groups.get("account", {})
    return {
        "window_start_utc": f"{start_utc:%Y-%m-%dT%H:%MZ}",
        "window_end_utc": f"{end_utc:%H:%MZ}",
        "sessions": str(len(pnls)),
        "maker_trades": field_of(last_portfolio, "maker_trade_count"),
        "taker_trades": field_of(last_portfolio, "taker_trade_count"),
        "fees_paid": field_of(last_portfolio, "fees_paid"),
        "net_pnl_usd": field_of(last_pnl, "net_pnl_usd"),
        "gross_pnl_usd": field_of(last_pnl, "gross_pnl_usd"),
        "residual_units": field_of(last_wind_down, "residual"),
        "markout_ticks": field_of(last_execution, "markout_per_unit_ticks"),
        "equity_usd": (account.get("equity", "").replace(" USDT", "").replace(",", "")
                       if account.get("equity", "").endswith(" USDT") else ""),
        "read_at": account.get("read_at", ""),
        "source": "journalctl",
    }


# The registered bar for a live window (ROADMAP, fixed before the 2026-11-28 window): five clauses, each read from the
# engine's own journal line rather than from a file, because the window's evidence is the journal.
BAR_REFUSED_SHARE = 0.20   # `refused_by_adapter` against the attempts it refused
BAR_RATE_LIMITED = 2000
BAR_FILLS = 20


def verdict_of(lines):
    """The bar as one line: PASS or FAIL, the clause that decided it, and every clause's number beside its limit.

    A verdict nobody can trace is a verdict nobody reads, so a clause the journal did not carry prints `missing` instead of
    passing quietly -- an absent number is not a met one, which is the same rule the empty cells above follow.
    """
    def number(tag, name):
        found = [line for line in lines if tag in line]
        if not found:
            return None
        try:
            return float(field_of(found[-1], name))
        except (TypeError, ValueError):
            return None

    orders = number("[VENUE]", "orders")
    refused = number("[VENUE]", "refused_by_adapter")
    attempts = (orders or 0.0) + (refused or 0.0)
    share = (refused / attempts) if (refused is not None and attempts > 0.0) else None
    rate_limited = number("[RISK]", "rate_limited_orders")
    fills = number("[PORTFOLIO]", "maker_trade_count")
    capture = number("[EXECUTION]", "captured_edge_per_unit_ticks")
    markout = number("[EXECUTION]", "markout_per_unit_ticks")
    residual = number("[WIND-DOWN]", "residual")
    reconciled = any("[RECON]" in line and "at=end" in line and "status=ok" in line for line in lines)
    clauses = [
        ("refused", share is not None and share < BAR_REFUSED_SHARE,
         f"{'missing' if share is None else f'{share:.1%}'} < {BAR_REFUSED_SHARE:.0%}"),
        ("rate_limited", rate_limited is not None and rate_limited < BAR_RATE_LIMITED,
         f"{'missing' if rate_limited is None else f'{rate_limited:.0f}'} < {BAR_RATE_LIMITED}"),
        ("fills", fills is not None and fills >= BAR_FILLS,
         f"{'missing' if fills is None else f'{fills:.0f}'} >= {BAR_FILLS}"),
        ("capture", capture is not None and capture > 0.0,
         f"{'missing' if capture is None else f'{capture:+.3f}'} > 0, markout "
         f"{'missing' if markout is None else f'{markout:+.3f}'} ticks beside it"),
        ("residual", residual is not None and abs(residual) < 0.5 and reconciled,
         f"{'missing' if residual is None else f'{residual:.0f}'} units and [RECON] at=end "
         f"{'ok' if reconciled else 'missing or not ok'}"),
    ]
    failed = [name for name, passed, _ in clauses if not passed]
    head = "verdict PASS" if not failed else f"verdict FAIL (failed: {', '.join(failed)})"
    return f"{head} -- " + "; ".join(f"{name}: {shown}" for name, _, shown in clauses)


def append_ledger(path, row):
    """One row per window: append it, replacing the row that window already has.

    A rewrite rather than a blind append, because a row is keyed by its window and two rows for one window are a series
    that double-counts: the timer can fire twice (`Persistent=true` after a suspend) and a backfill deliberately re-runs a
    window (`--ops report --at`), and re-deriving a recorded number is the only way this file is allowed to change one.
    Older rows keep the current columns, so a row written before a column was added reads as empty in it.

    Never fails the mail for it -- a ledger that breaks the report is a report lost.
    """
    try:
        target = Path(path)
        target.parent.mkdir(parents=True, exist_ok=True)
        rows = []
        replaced = False
        if target.exists():
            with target.open(encoding="utf-8", newline="") as handle:
                for existing in csv.DictReader(handle):
                    if existing.get("window_start_utc") == row["window_start_utc"]:
                        replaced = True
                        continue
                    rows.append(existing)
        rows.append(row)
        # A series is read in window order, and a backfill of an older window belongs where it belongs rather than at the
        # end: the key is an ISO date, so the sort is chronological.
        rows.sort(key=lambda item: item.get("window_start_utc", ""))
        with target.open("w", encoding="utf-8", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(row))
            writer.writeheader()
            for existing in rows:
                writer.writerow({name: existing.get(name, "") for name in row})
        return f"{'replaced the row in' if replaced else 'appended to'} {target}"
    except OSError as error:
        return f"unavailable ({error})"


def session_facts(start_utc, end_utc, text=None):
    """The window's own journal, as the lines an operator reads.

    **There is no `[FILL]` line to count.** The engine prints one summary per run, not a line per fill, so the previous
    version's `sum(1 for line if '[FILL]' in line)` was a zero that could never be anything else -- a report saying "0
    fill line(s)" on a session that traded, which is exactly the silent zero this file exists to refuse. The evidence for
    what traded is the last run's own counters: `[PORTFOLIO]` carries `maker_trade_count`/`taker_trade_count` and the
    fees, `[EXECUTION]` the submissions and the venue's reports, and `[PNL]` the money.
    """
    if text is None:
        text = journal_text(start_utc, end_utc)
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
    parser.add_argument("--no-mail", action="store_true",
                        help="write the ledger row and print the mail, but send nothing (a backfill or a check)")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--window-start", default=WINDOW_START, metavar="HH:MM", help="window open, UTC")
    parser.add_argument("--window-end", default=WINDOW_END, metavar="HH:MM", help="window close, UTC")
    parser.add_argument("--at", metavar="ISO8601", help="pretend it is this UTC instant (for a dry run)")
    parser.add_argument("--ledger", default=os.environ.get("TOYQUANT_LEDGER",
                                                           "/opt/toyquant/data/runtime/window_ledger.csv"),
                        metavar="PATH", help="one row per window, appended after the mail")
    args = parser.parse_args()
    if args.self_test:
        return self_test()

    now = datetime.fromisoformat(args.at).replace(tzinfo=timezone.utc) if args.at else datetime.now(timezone.utc)
    start_utc, end_utc = window_at(now, args.window_start, args.window_end)
    text = journal_text(start_utc, end_utc)
    lines = (text or "").splitlines()
    verdict = verdict_of(lines)
    groups = {
        "window": {"utc": f"{start_utc:%Y-%m-%dT%H:%MZ} .. {end_utc:%H:%MZ}",
                   "clocks": zone_range(start_utc, end_utc),
                   "note": window_note(now, args.window_start, args.window_end)},
        "session": session_facts(start_utc, end_utc, text),
        "account": account_facts(),
        "host": host_facts(),
    }
    subject, body = compose(groups)
    row = ledger_row(start_utc, end_utc, (text or "").splitlines(), groups)
    row["verdict"] = "PASS" if verdict.startswith("verdict PASS") else "FAIL"
    settings = notify.load_settings()
    if args.dry_run:
        print(f"To: {settings['NOTIFY_TO'] or '<ALERT_EMAIL_TO is not set>'}\nSubject: {subject}\n\n{verdict}\n\n{body}")
        print(f"ledger: would append to {args.ledger}: {row}")
        return 0
    # The row is written before the mail, and the mail carries the outcome, because a ledger failure has to be visible
    # where the operator looks. Measured 2026-11-27, the first live window: the append hit `Read-only file system`
    # (`ProtectSystem=strict` in the report unit, no `ReadWritePaths=`), `append_ledger` caught it as designed, the mail
    # went out unchanged, and the only trace was one stderr line in `journalctl -u toyquant-report` -- so the series the
    # ledger exists for started empty and nobody could tell from the mail. A row is a record even when the mail cannot be
    # sent, which is why this comes before the SMTP check.
    ledger_status = append_ledger(args.ledger, row)
    groups["ledger"] = {"file": args.ledger, "result": ledger_status,
                        "row": " ".join(f"{name}={value}" for name, value in row.items() if value)}
    subject, body = compose(groups)
    body = f"{verdict}\n\n{body}"
    if args.no_mail:
        print(f"Subject: {subject}\n\n{body}")
        print(f"daily_report: ledger {ledger_status}; no mail sent (--no-mail)")
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
    # The ledger's columns come from lines that exist, and a line that is absent is an empty cell rather than a zero.
    lines = ["Nov 27 15:30:01 host toy_quant[1]: [PORTFOLIO] fees_paid=0.02 maker_trade_count=44 taker_trade_count=1",
             "Nov 27 15:30:01 host toy_quant[1]: [PNL] gross_pnl_usd=0.5 net_pnl_usd=-0.25 shares=1000",
             "Nov 27 15:30:01 host toy_quant[1]: [EXECUTION] trade_reports=45 markout_per_unit_ticks=-0.590909",
             "Nov 27 15:30:02 host toy_quant[1]: [WIND-DOWN] position=0 closed=10000 residual=0 total_bps=3.2"]
    start, end = window_at(datetime(2026, 11, 27, 16, 0, tzinfo=timezone.utc))
    row = ledger_row(start, end, lines, {"account": {"equity": "99.45 USDT", "read_at": "2026-11-27T16:00Z"}})
    assert row["maker_trades"] == "44" and row["taker_trades"] == "1", row
    assert row["net_pnl_usd"] == "-0.25" and row["residual_units"] == "0", row
    assert row["sessions"] == "1" and row["equity_usd"] == "99.45", row
    # The markout rides the row from `[EXECUTION]`, because the live sample's markout is the number the replay's has to be
    # compared with; a window whose line is absent leaves the cell empty rather than zero.
    assert row["markout_ticks"] == "-0.590909", row
    assert ledger_row(start, end, lines[:2], {"account": {}})["markout_ticks"] == "", "no [EXECUTION] line"
    empty = ledger_row(start, end, [], {"account": {}})
    assert empty["net_pnl_usd"] == "" and empty["sessions"] == "0" and empty["equity_usd"] == "", empty
    # The mail writes `4,797.31 USDT` for a reader; the ledger is parsed by a tool, so the cell is a number.
    assert ledger_row(start, end, [], {"account": {"equity": "4,797.31 USDT"}})["equity_usd"] == "4797.31"
    # One row per window, even when a window is reported twice: a backfill replaces the row rather than adding a second
    # one, because two rows for one window are a series that double-counts (measured 2026-11-27: the first backfill ran
    # against a journal the account could not read, and re-deriving it must not leave both rows behind).
    with tempfile.TemporaryDirectory() as scratch:
        ledger = os.path.join(scratch, "window_ledger.csv")
        assert "appended" in append_ledger(ledger, dict(row, maker_trades="1")), "first write"
        assert "replaced" in append_ledger(ledger, dict(row, maker_trades="44")), "the same window again"
        append_ledger(ledger, dict(row, window_start_utc="2026-11-26T13:30Z", maker_trades="7"))
        with open(ledger, encoding="utf-8") as handle:
            written = list(csv.DictReader(handle))
        assert len(written) == 2, written
        assert written[0]["window_start_utc"] == "2026-11-26T13:30Z", written
        assert written[1]["window_start_utc"] == row["window_start_utc"] and written[1]["maker_trades"] == "44", written
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
    # The verdict: the registered bar's five clauses, each read from the engine's own line, and a clause the journal did not
    # carry is `missing` rather than a pass -- the same rule the empty cells follow.
    passing = [
        "[VENUE] endpoint=a orders=100 cancels=50 refused_by_adapter=5 write_failures=0",
        "[RISK] fat_finger_bps=50 rate_limited_orders=3",
        "[PORTFOLIO] maker_trade_count=25 taker_trade_count=0 fees_paid=0.1",
        "[EXECUTION] captured_edge_per_unit_ticks=0.5 markout_per_unit_ticks=0.4",
        "[WIND-DOWN] position=0 closed=10000 residual=0 total_bps=3.2",
        "[RECON] at=end venue_equity_usd=99.3 ledger_equity_usd=99.3 status=ok",
    ]
    said = verdict_of(passing)
    assert said.startswith("verdict PASS"), said
    thin = [line.replace("maker_trade_count=25", "maker_trade_count=7") for line in passing]
    said = verdict_of(thin)
    assert said.startswith("verdict FAIL (failed: fills)"), said
    assert verdict_of([]).startswith("verdict FAIL"), verdict_of([])
    print("daily_report self-test OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())

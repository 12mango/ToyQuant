#!/usr/bin/env python3
"""The deploy's own invariants, checked without a host.

Every failure this file's rules describe cost a deploy round trip, and none of them needed one: they are mismatches
between two files in this repository, and a reader with both files in front of them can see them. This is the audit of
2026-11-27 written down, so the next edit to `deploy/` or `.github/workflows/deploy.yml` is held to it:

  1. `env_names`      every name the relay reads is in the template, and every template name is read -- a typo in either
                      direction is a variable that is silently absent on the host
  2. `template_keys`  every key in `config/live.example.yaml` is one `live_config.py` can translate, because the reader
                      refuses the rest by name and on a host that is a unit that exits 1
  3. `endpoints`      the engine's two endpoints in the template are the relay's two listeners in the template
  4. `ordering`       the relay is `Before=` the engine, and the engine `After=` and `Requires=` the relay
  5. `stop_window`    the unit's `TimeoutStopSec` outlasts the engine's own shutdown deadline
  6. `bundled`        every program and file a unit names is in the deploy's package step
  7. `coupling`       `RELAY_ORDER_LIVE` and the order venue name the same environment, in both directions
  8. `notify_refs`    `OnFailure=` names the notifier template, and the bundle ships it
  9. `deploy_order`   the relay is restarted (and waited for) before the preflight, and the engine after it, because the
                      preflight's socket check has nothing to read on a host the operator has stopped
 10. `unit_reload`    the bootstrap asks the manager whether it loaded the units it just installed, and names the one it
                      could not, because a file written immediately before `daemon-reload` can be missed by its scan
 11. `ssh_quoting`    a remote script passed to `ssh` in single quotes contains no single quote of its own, because one
                      ends the argument and the step then exits with a bare code and no output
 12. `preflight_isolation`  the preflight's own engine run points both endpoints at a dead port, because the relay's order
                      channel serves exactly one client and a check that took it leaves the session without a venue

    python3 tools/gate/check_deploy.py             # this repository
    python3 tools/gate/check_deploy.py --self-test  # every rule on a defect and on a clean copy
"""
import argparse
import re
import sys
from pathlib import Path

TOOLS_LIVE = Path(__file__).resolve().parents[1] / "live"
sys.path.insert(0, str(TOOLS_LIVE))
import live_config  # noqa: E402  (the reader is the authority on which keys exist)

# The relay's environment surface: a quoted name of this shape, in the module *without* its own `self_test()` -- the tests
# deliberately name a variable that must be absent (`TOYQUANT_SURELY_ABSENT`), and counting that as a read would make this
# rule fire on a correct file. The function body is removed up to the next top-level `def`; the parser in `main()` keeps
# its `env_default("RELAY_...")` calls, and a name followed by `[` is a regex rather than a name and does not match.
RELAY_NAME = re.compile(r'"((?:RELAY|TOYQUANT)_[A-Z0-9_]+)"')
SELF_TEST_BODY = re.compile(r"^def self_test\(.*?(?=^def )", re.S | re.M)
UNIT_PATH = re.compile(r"/opt/toyquant/([A-Za-z0-9_./@-]+)")
# Directories the bootstrap creates and no bundle carries: `ReadWritePaths` names them, and a package step that shipped
# a directory would be shipping an empty one over the operator's files.
NOT_PACKAGED = {"data/runtime", "logs"}
LIVE_PRIVATE = "ws.okx.com:8443"
DEMO_PRIVATE = "wspap.okx.com"


def assignments_of(text):
    """The `NAME=value` pairs of an env file, comments and quotes stripped. `export` is allowed, silently ignored."""
    found = {}
    for line in text.splitlines():
        match = re.match(r"^\s*(?:export\s+)?([A-Z][A-Z0-9_]*)=(.*)$", line)
        if match:
            found[match.group(1)] = match.group(2).split("#", 1)[0].strip().strip('"').strip("'")
    return found


def yaml_pairs(text):
    """The two-level `section: key: value` pairs of a live config, as `live_config.py` reads them."""
    values, section = {}, ""
    for line in text.splitlines():
        line = line.split("#", 1)[0].rstrip()
        if not line.strip():
            continue
        if not line.startswith(" "):
            section = line.split(":", 1)[0].strip()
            continue
        match = re.match(r"^\s+([a-z_]+):\s*(\S+)", line)
        if match:
            values[(section, match.group(1))] = match.group(2).strip('"').strip("'")
    return values


def rule_env_names(texts):
    """The relay's names and the template's are the same set, in both directions."""
    program = SELF_TEST_BODY.sub("", texts["relay_py"])
    relay = {name for name in RELAY_NAME.findall(program)}
    template = {name for name in assignments_of(texts["env"]) if name.startswith(("RELAY_", "TOYQUANT_"))}
    problems = [f".env.example does not define {name}, which venue_relay.py reads" for name in sorted(relay - template)]
    problems += [f".env.example defines {name}, which venue_relay.py never reads" for name in sorted(template - relay)]
    return problems


def rule_template_keys(texts):
    """Every key in the template is one the reader can translate."""
    try:
        live_config.validate(yaml_pairs(texts["live_yaml"]), "config/live.example.yaml")
    except ValueError as error:
        return [str(error)]
    values = yaml_pairs(texts["live_yaml"])
    problems = [f"config/live.example.yaml: {section}.{key} is not in live_config.py's map"
                for section, key in sorted(values)
                if (section, key) not in live_config.FLAGS and (section, key) not in live_config.POSITIONAL
                and (section, key) not in live_config.POSITIONAL_OPTIONAL]
    if not values:
        problems.append("config/live.example.yaml parsed to no settings at all")
    return problems


def rule_endpoints(texts):
    """The engine's endpoints are the relay's listeners: anything else waits on a port nobody serves."""
    engine = yaml_pairs(texts["live_yaml"])
    relay = assignments_of(texts["env"])
    problems = []
    for key, relay_key in (("market_endpoint", "RELAY_MARKET_LISTEN"), ("venue_endpoint", "RELAY_ORDER_LISTEN")):
        address = engine.get(("link", key))
        listener = relay.get(relay_key)
        if not address or not listener:
            problems.append(f"both config/live.example.yaml link.{key} and .env.example {relay_key} must be named")
        elif address != listener:
            problems.append(f"the engine's link.{key} is {address} and the relay's {relay_key} is {listener}")
    return problems


def rule_ordering(texts):
    """The relay before the engine, and the engine requiring it: a session with no relay dials two closed ports."""
    problems = []
    if "Before=toyquant-live.service" not in texts["relay_unit"]:
        problems.append("deploy/toyquant-relay.service: no `Before=toyquant-live.service`")
    if "After=toyquant-relay.service" not in texts["live_unit"]:
        problems.append("deploy/toyquant-live.service: no `After=toyquant-relay.service`")
    if "Requires=toyquant-relay.service" not in texts["live_unit"]:
        problems.append("deploy/toyquant-live.service: no `Requires=toyquant-relay.service`")
    return problems


def rule_stop_window(texts):
    """`TimeoutStopSec` has to outlast the engine's cancel sweep **and** the close-out that runs after it.

    On SIGTERM the session cancels its working orders (bounded by `deadline_ms`) and then `--close-out` runs on the same
    signal: `send_close_order` cancels what is left, drains the cancels and waits up to a second for the exit's own fill
    report. A unit that stops waiting at the engine's own deadline therefore SIGKILLs a session between the cancels and
    the close -- measured class, and the reason the shipped value moved from 12 to 25.
    """
    stop = re.search(r"^TimeoutStopSec=(\d+)", texts["live_unit"], re.MULTILINE)
    deadline = re.search(r"deadline_ms\{(\d+)", texts["shutdown_h"].replace("'", ""))
    if not stop or not deadline:
        return ["neither TimeoutStopSec nor deadline_ms was found to compare"]
    seconds, engine = int(stop.group(1)), int(deadline.group(1)) // 1000
    allowance = 10  # the close-out: its cancel drain and its fill wait are each bounded by a second, plus the venue's
    if seconds < engine + allowance:
        return [f"TimeoutStopSec={seconds}s does not outlast the engine's {engine}s cancel sweep plus a {allowance}s "
                f"close-out (`--close-out` runs after the sweep, on the same signal)"]
    return []


def rule_bundled(texts):
    """Every program a unit names is in the package step, or the host gets a unit for a file it never received."""
    workflow = texts["workflow"]
    problems = []
    for unit in ("live_unit", "relay_unit", "notify_unit"):
        for path in sorted(set(UNIT_PATH.findall(texts[unit]))):
            if path in NOT_PACKAGED:
                continue
            name = path.rsplit("/", 1)[-1]
            if name not in workflow:
                problems.append(f"{name} is named by {unit} and never packaged by deploy.yml")
    for script in ("preflight.sh", "check_health.sh", "bootstrap_server.sh", "rollback.sh", "notify.py",
                   "wait_for_ports.sh", "daily_report.py", "toyquant-report.service", "toyquant-report.timer",
                   "toyquant-notify@.service", "journald/toyquant.conf", "logrotate/toyquant",
                   "toyquant-window-start.timer", "toyquant-window-stop.service", "toyquant-window-stop.timer"):
        name = script.rsplit("/", 1)[-1]
        if script not in workflow and name not in workflow:
            problems.append(f"{script} is not in the deploy's package step")
    return problems


def rule_coupling(texts):
    """`RELAY_ORDER_LIVE=1` with the demo host is a 401, and `=0` with the live host is a real order."""
    assignments = assignments_of(texts["env"])
    live = str(assignments.get("RELAY_ORDER_LIVE", "0")).lower() in ("1", "true", "yes")
    venue = assignments.get("RELAY_ORDER_VENUE", "")
    problems = []
    if live and DEMO_PRIVATE in venue:
        problems.append(f"RELAY_ORDER_LIVE=1 places real orders but RELAY_ORDER_VENUE is the demo host ({venue})")
    if not live and LIVE_PRIVATE in venue:
        problems.append(f"RELAY_ORDER_LIVE=0 is a simulated order but RELAY_ORDER_VENUE is the live host ({venue})")
    return problems


def rule_notify_refs(texts):
    """An `OnFailure=` naming a unit that is not shipped is a failure nobody hears about."""
    problems = []
    for unit in ("live_unit", "relay_unit"):
        for target in re.findall(r"^OnFailure=(\S+)", texts[unit], re.MULTILINE):
            if target != "toyquant-notify@%n.service":
                problems.append(f"{unit}: OnFailure={target} is not the notifier template")
            elif "toyquant-notify@.service" not in texts["workflow"]:
                problems.append(f"{unit}: OnFailure names the notifier and deploy.yml does not ship it")
    if "notify.py" not in texts["workflow"]:
        problems.append("deploy/notify.py is not in the deploy's package step")
    return problems


def rule_deploy_order(texts):
    """The deploy ships and verifies without restarting a session; the restart is the ops action's job.

    Two measurements made this rule. The preflight's socket check has nothing to read on a stopped host, so the deploy
    **starts** the relay (idempotent) and waits for its ports before the check. And a restart is a SIGTERM to a session
    that may hold quotes, which a redeploy inside a window cannot afford -- measured 2026-11-27: `stop-sigterm` timed out
    after 13 s, SIGKILL, and the quotes stayed at the venue with nothing to cancel them. The window timer starts the new
    binary at 13:30, so a deploy needs no restart at all; a hotfix is `ops.yml`'s `restart)` branch, and that branch keeps
    the order: relay, wait, preflight, engine.
    """
    problems = []
    deploy = texts["workflow"]
    preflight = deploy.find("/opt/toyquant/deploy/preflight.sh")
    if preflight < 0:
        problems.append("deploy.yml never runs /opt/toyquant/deploy/preflight.sh")
    else:
        relay = deploy.rfind("systemctl start toyquant-relay", 0, preflight)
        if relay < 0:
            problems.append("deploy.yml runs the preflight without starting the relay: its socket check fails on a host "
                            "that is stopped, and nothing can bring it up")
        elif "wait_for_ports.sh" not in deploy[relay:preflight]:
            problems.append("deploy.yml starts the relay and runs the preflight without waiting for the sockets: "
                            "`Type=simple` reports the unit started before it binds")
    for verb in ("restart toyquant-relay", "restart toyquant-live"):
        if f"systemctl {verb}" in deploy:
            problems.append(f"deploy.yml runs `systemctl {verb}`: a redeploy must not restart a session that may hold "
                            f"quotes -- the window timer starts the new binary, and a hotfix is `--ops restart`")
    whole = texts["ops_workflow"]
    start = whole.find("            restart)")
    end = whole.find("            ;;", start) if start >= 0 else -1
    if start < 0 or end < 0:
        problems.append("ops.yml has no `restart)` branch to hold to the order")
        return problems
    text = whole[start:end]
    branch = text.find("/opt/toyquant/deploy/preflight.sh")
    if branch < 0:
        problems.append("ops.yml's `restart)` branch never runs the preflight")
        return problems
    relay = text.rfind("systemctl restart toyquant-relay", 0, branch)
    if relay < 0:
        problems.append("ops.yml's `restart)` branch runs the preflight before restarting the relay: its socket check "
                        "fails on a host that is stopped")
    elif "wait_for_ports.sh" not in text[relay:branch]:
        problems.append("ops.yml's `restart)` branch restarts the relay and runs the preflight without waiting for the "
                        "sockets")
    if text.find("systemctl restart toyquant-live", branch) < 0:
        problems.append("ops.yml's `restart)` branch never restarts toyquant-live after the preflight")
    return problems


def rule_unit_reload(texts):
    """The bootstrap asks the manager whether it loaded what it was handed, instead of trusting one `daemon-reload`.

    Measured 2026-11-27: four unit files written, one `daemon-reload`, `enable` accepted the names, and the next
    `systemctl restart` answered `Unit toyquant-relay.service not found` -- while the same query minutes later reported
    `LoadState=loaded`. A reload that missed a file it was just given has to be retried by name, not read as a missing
    unit by the deploy's third command.
    """
    bootstrap = texts["bootstrap"]
    # `find`, not `rfind`: the failure message quotes `systemctl daemon-reload` as the operator's next step, and the
    # question is what happens after the *reload*, not after the sentence that names it.
    reload_at = bootstrap.find("systemctl daemon-reload")
    if reload_at < 0:
        return ["deploy/bootstrap_server.sh never runs systemctl daemon-reload"]
    problems = []
    if "LoadState" not in bootstrap[reload_at:]:
        problems.append("deploy/bootstrap_server.sh reloads systemd and never asks whether the manager loaded the units")
    if "did not load" not in bootstrap[reload_at:]:
        problems.append("deploy/bootstrap_server.sh's reload check does not name the unit it could not load")
    if "*@)" not in bootstrap[reload_at:]:
        problems.append("deploy/bootstrap_server.sh asks `LoadState` of a template unit, which has none of its own: "
                        "skip the `*@` names, or the bootstrap fails on `toyquant-notify@` for ever")
    if "LoadState" not in texts["workflow"]:
        problems.append("deploy.yml restarts the units without asking whether the manager loaded them: the bootstrap's "
                        "`loaded` and the next systemctl call's `not found` were one second apart on 2026-11-27")
    return problems


def rule_ssh_quoting(texts):
    """A remote script handed to `ssh` inside single quotes may not contain a single quote of its own.

    The quote ends the argument, so ssh receives a truncated command plus new words, and the step exits with a bare code
    and **no output at all** -- the exact signature measured 2026-11-27, when `echo "reports '$states'"` inside the
    deploy's single-quoted script failed a whole deploy in 0.3 s. The fix is double quotes inside the script; this rule is
    the one that says so before a dispatch does.
    """
    problems = []
    for name in ("workflow", "ops_workflow"):
        for match in re.finditer(r"ssh .*?'\n(.*?)\n\s*'", texts[name], re.S):
            body = match.group(1)
            if "'" in body:
                line = body[:body.index("'")].count("\n") + 2
                where = FILES[name].rsplit("/", 1)[-1]
                problems.append(f"{where}: the single-quoted ssh script has a single quote on its script line {line}, "
                                f"which ends the quoting: use double quotes inside it")
    return problems


def rule_preflight_isolation(texts):
    """The preflight's own engine run must not dial the relay's real order endpoint.

    The relay's order channel serves exactly one client: measured 2026-11-27, the preflight ran its engine against the
    real `--venue-endpoint`, the relay accepted it, the check's engine exited, the order channel ended with it, and thirty
    seconds later the session failed `waited 30s and nothing listens on 127.0.0.1:8766`. Reachability is the sixth check's
    question; this one is about the program starting.
    """
    preflight = texts["preflight"]
    problems = []
    if "ws 127.0.0.1:1" not in preflight:
        problems.append("deploy/preflight.sh no longer runs the engine against 127.0.0.1:1")
    if "--venue-endpoint=127.0.0.1:1" not in preflight:
        problems.append("deploy/preflight.sh runs the engine with the relay's real order endpoint: that takes the order "
                        "channel's single client and leaves the session without one")
    return problems


def rule_report_clock(texts):
    """Every timer names its zone, and the daily mail's interval is the window rather than a rolling day.

    The window is 13:30-15:30 UTC; the operator is in Hong Kong (21:30-23:30) and the host is located in Tokyo
    (22:30-00:30, which crosses midnight) while its clock is `Etc/UTC` (measured 2026-11-27). A host-local `OnCalendar`
    therefore means whatever the host's clock happens to say -- and a rolling `--since -24h` reports a day that is not the
    session's. The zone in the line is what makes the file mean one moment wherever it runs.
    """
    problems = []
    timers = (("report_timer", "deploy/toyquant-report.timer"), ("window_start", "deploy/toyquant-window-start.timer"),
              ("window_stop", "deploy/toyquant-window-stop.timer"))
    for name, where in timers:
        calendar = [line for line in texts[name].splitlines() if line.startswith("OnCalendar=")]
        if not calendar:
            problems.append(f"{where} has no OnCalendar line")
        elif not calendar[0].rstrip().endswith((" UTC", " GMT")):
            problems.append(f"{where} fires on the host's own clock: name the zone (e.g. "
                            f"`OnCalendar=*-*-* 16:15:00 UTC`), because the window crosses the host's midnight")
    report = texts["daily_report"]
    if '"--utc"' not in report:
        problems.append("deploy/daily_report.py reads the journal without `--utc`: its lines then carry the host's clock")
    if '"--until"' not in report:
        problems.append("deploy/daily_report.py has no `--until`: a rolling `--since -24h` is not the window")
    return problems


def rule_timer_install(texts):
    """Every timer the package ships is installed by the bootstrap, by name.

    A unit loop that appends `.service` cannot install `x.timer`: measured 2026-11-27, the window timers were in the list,
    the bootstrap looked for `toyquant-window-start.timer.service`, printed "missing from the bundle", `enable --now`
    answered `Unit file ... does not exist`, and `list-timers` was empty -- the window would not have opened itself. The
    rule reads the bootstrap's `install` lines, so a timer that is only *named* somewhere does not count.
    """
    installs = "\n".join(line for line in texts["bootstrap"].splitlines() if "install " in line)
    problems = []
    for name in ("toyquant-report.timer", "toyquant-window-start.timer", "toyquant-window-stop.timer"):
        if name not in installs:
            problems.append(f"deploy/bootstrap_server.sh has no `install` line for {name}: a packaged timer the bootstrap "
                            f"does not install is a moment the host does not have")
    return problems


# The directory `daily_report.py` writes its one row into, named once: the report's `--ledger` default and the unit's
# `ReadWritePaths=` are the two ends of one coupling, and the rule below reads both.
LEDGER_DIR = "/opt/toyquant/data/runtime"


def rule_report_sandbox(texts):
    """A unit that writes a file has to name the path, because `ProtectSystem=strict` makes everything else read-only.

    Measured 2026-11-27, the first live window's mail: `daily_report.py` appended its ledger row, the write failed with
    `Read-only file system` (the unit sandboxes the whole filesystem and named no `ReadWritePaths=`), `append_ledger`
    caught it as it was written to, and the mail went out unchanged -- the ledger the series is built on started empty
    and the only trace was a stderr line in a timer's journal, which nobody reads. The rule holds both ends of the
    coupling: the directory the report writes, and the line the unit allows.
    """
    problems = []
    unit = texts["report_unit"]
    if "ProtectSystem=strict" not in unit and "ProtectSystem=full" not in unit:
        return problems
    writable = [line for line in unit.splitlines() if line.startswith("ReadWritePaths=")]
    if not writable:
        problems.append("deploy/toyquant-report.service protects the filesystem and names no `ReadWritePaths=`: "
                        f"the ledger row `daily_report.py` appends under {LEDGER_DIR} cannot be written")
    elif LEDGER_DIR not in writable[0]:
        problems.append(f"deploy/toyquant-report.service allows `{writable[0].strip()}` and not {LEDGER_DIR}: "
                        "the report's ledger row is written under that directory")
    if LEDGER_DIR not in texts["daily_report"]:
        problems.append(f"deploy/daily_report.py no longer names {LEDGER_DIR}: one of the two moved and the other "
                        "did not, so the unit's `ReadWritePaths=` and the report's ledger disagree")
    return problems


RULES = (("env_names", rule_env_names), ("template_keys", rule_template_keys), ("endpoints", rule_endpoints),
         ("ordering", rule_ordering), ("stop_window", rule_stop_window), ("bundled", rule_bundled),
         ("coupling", rule_coupling), ("notify_refs", rule_notify_refs), ("deploy_order", rule_deploy_order),
         ("unit_reload", rule_unit_reload), ("ssh_quoting", rule_ssh_quoting),
         ("preflight_isolation", rule_preflight_isolation), ("report_clock", rule_report_clock),
         ("timer_install", rule_timer_install), ("report_sandbox", rule_report_sandbox))

FILES = {"env": ".env.example",
         "live_yaml": "config/live.example.yaml",
         "relay_py": "tools/live/venue_relay.py",
         "live_unit": "deploy/toyquant-live.service",
         "relay_unit": "deploy/toyquant-relay.service",
         "notify_unit": "deploy/toyquant-notify@.service",
         "workflow": ".github/workflows/deploy.yml",
         "ops_workflow": ".github/workflows/ops.yml",
         "bootstrap": "deploy/bootstrap_server.sh",
         "preflight": "deploy/preflight.sh",
         "report_timer": "deploy/toyquant-report.timer",
         "window_start": "deploy/toyquant-window-start.timer",
         "window_stop": "deploy/toyquant-window-stop.timer",
         "daily_report": "deploy/daily_report.py",
         "report_unit": "deploy/toyquant-report.service",
         "shutdown_h": "src/lines/session_shutdown.h"}


def read_tree(root):
    """The files the rules read, by logical name. A missing one is an error naming it, never a silent skip."""
    texts = {}
    for name, relative in FILES.items():
        path = Path(root) / relative
        if not path.is_file():
            raise SystemExit(f"check_deploy: {relative} is missing")
        texts[name] = path.read_text()
    return texts


def evaluate(texts):
    problems = []
    for name, rule in RULES:
        problems.extend(f"{name}: {problem}" for problem in rule(texts))
    return problems


CLEAN = {
    "env": "TOYQUANT_API_KEY=k\nTOYQUANT_API_SECRET=s\nTOYQUANT_PASSPHRASE=p\nRELAY_INSTRUMENT=BTC-USDT-SWAP\n"
           "RELAY_MARKET_LISTEN=127.0.0.1:8765\nRELAY_ORDER_LISTEN=127.0.0.1:8766\n"
           "RELAY_ORDER_VENUE=wss://wspap.okx.com:443/ws/v5/private?brokerId=9999\nRELAY_ORDER_LIVE=0\n",
    "live_yaml": "engine:\n  symbol: BTC-USDT-SWAP\n  strategy: active_l2\n  live_orders: true\n"
                 "link:\n  market_endpoint: 127.0.0.1:8765\n  venue_endpoint: 127.0.0.1:8766\n"
                 "  venue_max_orders: 20\n  venue_max_notional: 500\n  venue_orders_per_second: 5\n",
    "relay_py": "\n".join(f'os.environ["{name}"]' for name in (
        "TOYQUANT_API_KEY", "TOYQUANT_API_SECRET", "TOYQUANT_PASSPHRASE", "RELAY_INSTRUMENT",
        "RELAY_MARKET_LISTEN", "RELAY_ORDER_LISTEN", "RELAY_ORDER_VENUE", "RELAY_ORDER_LIVE")) + "\n",
    "live_unit": "[Unit]\nAfter=toyquant-relay.service\nRequires=toyquant-relay.service\n"
                 "OnFailure=toyquant-notify@%n.service\n[Service]\nTimeoutStopSec=25\n"
                 "ExecStartPre=/bin/bash -c 'python3 /opt/toyquant/tools/live/live_config.py --check x'\n"
                 "ExecStart=/bin/bash -c 'exec /opt/toyquant/toy_quant ws'\n",
    "relay_unit": "[Unit]\nBefore=toyquant-live.service\nOnFailure=toyquant-notify@%n.service\n[Service]\n"
                  "ExecStart=/usr/bin/python3 /opt/toyquant/tools/live/venue_relay.py\n",
    "notify_unit": "[Service]\nExecStart=/usr/bin/python3 /opt/toyquant/deploy/notify.py --unit %i\n",
    "workflow": "install -m 0755 toy_quant; install tools/live/live_config.py tools/live/venue_relay.py; "
                "install deploy/toyquant-notify@.service deploy/toyquant-report.service deploy/toyquant-report.timer "
                "deploy/toyquant-window-start.timer deploy/toyquant-window-stop.service deploy/toyquant-window-stop.timer; "
                "install deploy/notify.py deploy/daily_report.py deploy/preflight.sh deploy/check_health.sh "
                "deploy/wait_for_ports.sh deploy/bootstrap_server.sh deploy/rollback.sh deploy/journald/toyquant.conf "
                "deploy/logrotate/toyquant\n"
                "states=$(systemctl show -p LoadState --value toyquant-relay toyquant-live)\n"
                "sudo systemctl start toyquant-relay\n"
                "sudo bash /opt/toyquant/deploy/wait_for_ports.sh 127.0.0.1:8765 127.0.0.1:8766\n"
                "sudo bash /opt/toyquant/deploy/preflight.sh\n"
                "if systemctl is-active --quiet toyquant-live; then sudo bash /opt/toyquant/deploy/check_health.sh; fi\n"
                "ssh host '\nset -euo pipefail\necho hello\n'\n",
    "ops_workflow": "            restart)\n"
                    "sudo systemctl restart toyquant-relay\n"
                    "sudo bash /opt/toyquant/deploy/wait_for_ports.sh 127.0.0.1:8765 127.0.0.1:8766\n"
                    "sudo bash /opt/toyquant/deploy/preflight.sh\n"
                    "sudo systemctl restart toyquant-live\n"
                    "            ;;\n",
    "bootstrap": "systemctl daemon-reload\n"
                 "install -m 0644 deploy/toyquant-report.timer /etc/systemd/system/\n"
                 "install -m 0644 deploy/toyquant-window-start.timer /etc/systemd/system/\n"
                 "install -m 0644 deploy/toyquant-window-stop.timer /etc/systemd/system/\n"
                 "state=$(systemctl show -p LoadState --value toyquant-relay)\n"
                 "case \"$unit\" in *@) continue ;; esac\n"
                 "echo \"bootstrap: systemd did not load toyquant-relay\"\n",
    "preflight": "startup=$(engine ws 127.0.0.1:1 --venue-endpoint=127.0.0.1:1)\n",
    "report_timer": "[Timer]\nOnCalendar=*-*-* 16:15:00 UTC\nPersistent=true\n",
    "report_unit": "[Service]\nUser=toyquant\nProtectSystem=strict\nReadWritePaths=-/opt/toyquant/data/runtime\n",
    "window_start": "[Timer]\nOnCalendar=*-*-* 13:30:00 UTC\nUnit=toyquant-live.service\n",
    "window_stop": "[Timer]\nOnCalendar=*-*-* 15:32:00 UTC\nUnit=toyquant-window-stop.service\n",
    "daily_report": "run([\"journalctl\", \"-u\", UNIT, \"--since\", start, \"--until\", end, \"--utc\"])\n"
                    "LEDGER = \"/opt/toyquant/data/runtime/window_ledger.csv\"\n",
    "shutdown_h": "std::uint64_t deadline_ms{10'000};\n",
}


def self_test():
    """Every rule fires on its own defect and stays quiet on the clean copy: a check nobody has seen fail is a wish."""
    assert evaluate(CLEAN) == [], evaluate(CLEAN)
    defects = {
        "env_names": ("env", "RELAY_TYPO_NAME=1\n"),
        "template_keys": ("live_yaml", "  not_a_setting: 1\n"),
        "endpoints": ("live_yaml", None),  # handled below: a value, not an appended line
        "ordering": ("live_unit", ""),
        "stop_window": ("live_unit", ""),
        "bundled": ("workflow", ""),
        "coupling": ("env", "RELAY_ORDER_LIVE=1\n"),
        "notify_refs": ("live_unit", ""),
    }
    def check(rule_name, mutate, needle):
        texts = dict(CLEAN)
        mutate(texts)
        problems = dict(RULES)[rule_name](texts)
        assert any(needle in problem for problem in problems), (rule_name, problems)

    check("env_names", lambda t: t.update(env=t["env"] + "RELAY_TYPO_NAME=1\n"), "RELAY_TYPO_NAME")
    check("template_keys", lambda t: t.update(live_yaml=t["live_yaml"] + "  not_a_setting: 1\n"),
          "not_a_setting")
    check("endpoints", lambda t: t.update(live_yaml=t["live_yaml"].replace("8766", "9000")), "link.venue_endpoint")
    check("ordering", lambda t: t.update(live_unit=t["live_unit"].replace("Requires=", "X=")), "Requires=")
    check("stop_window", lambda t: t.update(live_unit=t["live_unit"].replace("TimeoutStopSec=25",
                                                                              "TimeoutStopSec=5")), "close-out")
    check("bundled", lambda t: t.update(workflow=t["workflow"].replace("preflight.sh", "gone.sh")), "preflight.sh")
    check("coupling", lambda t: t.update(env=t["env"].replace("RELAY_ORDER_LIVE=0", "RELAY_ORDER_LIVE=1")), "demo host")
    check("notify_refs", lambda t: t.update(live_unit=t["live_unit"].replace(
        "toyquant-notify@%n.service", "somebody-else@%n.service")), "not the notifier template")
    # The ordering defect this rule was written for: the preflight before the relay's restart, which is what a stopped
    # host fails on. Dropping the restart line is the smallest edit that reproduces it.
    check("deploy_order", lambda t: t.update(workflow=t["workflow"].replace(
        "sudo systemctl start toyquant-relay\n", "", 1)), "without starting the relay")
    # The other half: a redeploy that restarts the engine is the thing the rule refuses.
    check("deploy_order", lambda t: t.update(workflow=t["workflow"] + "sudo systemctl restart toyquant-live\n"),
          "must not restart a session that may hold")
    # The ops action had the same defect, and it is the way an operator starts a stopped host: the rule reads both.
    check("deploy_order", lambda t: t.update(ops_workflow=t["ops_workflow"].replace(
        "sudo systemctl restart toyquant-relay\n", "", 1)), "runs the preflight before restarting the relay")
    # A reload that is trusted rather than asked: the bootstrap drops its `LoadState` question.
    check("unit_reload", lambda t: t.update(bootstrap=t["bootstrap"].replace(
        "LoadState", "SomethingElse")), "never asks whether the manager loaded")
    # A quote inside the single-quoted script: the deploy that exited 1 with no output at all.
    check("ssh_quoting", lambda t: t.update(workflow=t["workflow"].replace(
        "echo hello", "echo it's broken")), "single quote")
    # The preflight's own engine run dialling the real order endpoint: it takes the relay's single client.
    check("preflight_isolation", lambda t: t.update(preflight=t["preflight"].replace(
        "--venue-endpoint=127.0.0.1:1", "--venue-endpoint=127.0.0.1:8766")), "single client")
    # The report's clock: a host-local timer fires inside a window that crosses the host's midnight.
    check("report_clock", lambda t: t.update(report_timer=t["report_timer"].replace("16:15:00 UTC", "00:15:00")),
          "host's own clock")
    check("report_clock", lambda t: t.update(daily_report=t["daily_report"].replace('"--until"', '"-n"')),
          "rolling `--since -24h`")
    check("report_clock", lambda t: t.update(window_start=t["window_start"].replace("13:30:00 UTC", "13:30:00")),
          "deploy/toyquant-window-start.timer fires on the host's own clock")
    check("report_clock", lambda t: t.update(window_stop=t["window_stop"].replace("15:32:00 UTC", "15:32:00")),
          "deploy/toyquant-window-stop.timer fires on the host's own clock")
    # A timer that is packaged and named but never installed: the bootstrap's unit loop looked for `.timer.service`.
    check("timer_install", lambda t: t.update(bootstrap=t["bootstrap"].replace(
        "install -m 0644 deploy/toyquant-window-start.timer /etc/systemd/system/\n", "")),
        "no `install` line for toyquant-window-start.timer")
    # The sandbox: a report unit that writes a file behind `ProtectSystem=strict` and does not name it. Measured
    # 2026-11-27, the first live window's ledger row was never written for exactly this reason.
    check("report_sandbox", lambda t: t.update(report_unit=t["report_unit"].replace("ReadWritePaths", "X")),
          "no `ReadWritePaths=`")
    check("report_sandbox", lambda t: t.update(report_unit=t["report_unit"].replace("data/runtime", "data/elsewhere")),
          "and not /opt/toyquant/data/runtime")
    check("report_sandbox", lambda t: t.update(daily_report=t["daily_report"].replace("data/runtime", "data/x")),
          "one of the two moved")
    del defects
    print(f"check_deploy self-test OK ({len(RULES)} rule(s), each on a defect and on the clean copy)")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("root", nargs="?", default=str(Path(__file__).resolve().parents[2]),
                        help="the repository root (default: the tree this file is in)")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    problems = evaluate(read_tree(args.root))
    for problem in problems:
        print(f"check_deploy: {problem}", file=sys.stderr)
    if problems:
        print(f"check_deploy: {len(problems)} problem(s)", file=sys.stderr)
        return 1
    print(f"check_deploy: OK ({len(RULES)} rule(s) over {len(FILES)} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

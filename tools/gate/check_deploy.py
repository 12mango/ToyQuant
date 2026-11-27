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
    """`TimeoutStopSec` has to outlast the engine's own cancel sweep, or systemd SIGKILLs a session mid-cancel."""
    stop = re.search(r"^TimeoutStopSec=(\d+)", texts["live_unit"], re.MULTILINE)
    deadline = re.search(r"deadline_ms\{(\d+)", texts["shutdown_h"].replace("'", ""))
    if not stop or not deadline:
        return ["neither TimeoutStopSec nor deadline_ms was found to compare"]
    seconds, engine = int(stop.group(1)), int(deadline.group(1)) // 1000
    if seconds <= engine:
        return [f"TimeoutStopSec={seconds}s does not outlast the engine's {engine}s cancel sweep "
                f"(src/lines/session_shutdown.h deadline_ms)"]
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
                   "toyquant-notify@.service", "journald/toyquant.conf", "logrotate/toyquant"):
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
    """The relay is restarted before the preflight, and the engine after it: a stopped host has no sockets to check.

    The finding this rule is: the preflight's sixth check reads `ss`, and on a host the operator has stopped there is
    nothing listening -- so a deploy of a stopped host failed before it could start anything. Measured 2026-11-27. The
    same order is required of the ops workflow's own `restart` action, which is how an operator starts a stopped host.
    """
    problems = []
    for name in ("workflow", "ops_workflow"):
        workflow = texts[name]
        where = FILES[name].rsplit("/", 1)[-1]
        preflight = workflow.find("/opt/toyquant/deploy/preflight.sh")
        if preflight < 0:
            if name == "workflow":
                problems.append(f"{where} never runs /opt/toyquant/deploy/preflight.sh")
            continue
        relay = workflow.rfind("systemctl restart toyquant-relay", 0, preflight)
        if relay < 0:
            problems.append(f"{where} runs the preflight before restarting the relay: its socket check fails on a host "
                            f"that is stopped, and nothing can start it")
        elif "wait_for_ports.sh" not in workflow[relay:preflight]:
            problems.append(f"{where} restarts the relay and runs the preflight without waiting for the sockets: "
                            f"`Type=simple` reports the unit started before it binds")
        if workflow.find("systemctl restart toyquant-live", preflight) < 0:
            problems.append(f"{where} never restarts toyquant-live after the preflight")
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
    return problems


RULES = (("env_names", rule_env_names), ("template_keys", rule_template_keys), ("endpoints", rule_endpoints),
         ("ordering", rule_ordering), ("stop_window", rule_stop_window), ("bundled", rule_bundled),
         ("coupling", rule_coupling), ("notify_refs", rule_notify_refs), ("deploy_order", rule_deploy_order),
         ("unit_reload", rule_unit_reload))

FILES = {"env": ".env.example",
         "live_yaml": "config/live.example.yaml",
         "relay_py": "tools/live/venue_relay.py",
         "live_unit": "deploy/toyquant-live.service",
         "relay_unit": "deploy/toyquant-relay.service",
         "notify_unit": "deploy/toyquant-notify@.service",
         "workflow": ".github/workflows/deploy.yml",
         "ops_workflow": ".github/workflows/ops.yml",
         "bootstrap": "deploy/bootstrap_server.sh",
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
                 "OnFailure=toyquant-notify@%n.service\n[Service]\nTimeoutStopSec=12\n"
                 "ExecStartPre=/bin/bash -c 'python3 /opt/toyquant/tools/live/live_config.py --check x'\n"
                 "ExecStart=/bin/bash -c 'exec /opt/toyquant/toy_quant ws'\n",
    "relay_unit": "[Unit]\nBefore=toyquant-live.service\nOnFailure=toyquant-notify@%n.service\n[Service]\n"
                  "ExecStart=/usr/bin/python3 /opt/toyquant/tools/live/venue_relay.py\n",
    "notify_unit": "[Service]\nExecStart=/usr/bin/python3 /opt/toyquant/deploy/notify.py --unit %i\n",
    "workflow": "install -m 0755 toy_quant; install tools/live/live_config.py tools/live/venue_relay.py; "
                "install deploy/toyquant-notify@.service deploy/toyquant-report.service deploy/toyquant-report.timer "
                "deploy/notify.py deploy/daily_report.py deploy/preflight.sh deploy/check_health.sh "
                "deploy/wait_for_ports.sh deploy/bootstrap_server.sh deploy/rollback.sh deploy/journald/toyquant.conf "
                "deploy/logrotate/toyquant\n"
                "sudo systemctl restart toyquant-relay\n"
                "sudo bash /opt/toyquant/deploy/wait_for_ports.sh 127.0.0.1:8765 127.0.0.1:8766\n"
                "sudo bash /opt/toyquant/deploy/preflight.sh\n"
                "sudo systemctl restart toyquant-live\n",
    "ops_workflow": "sudo systemctl restart toyquant-relay\n"
                    "sudo bash /opt/toyquant/deploy/wait_for_ports.sh 127.0.0.1:8765 127.0.0.1:8766\n"
                    "sudo bash /opt/toyquant/deploy/preflight.sh\n"
                    "sudo systemctl restart toyquant-live\n",
    "bootstrap": "systemctl daemon-reload\n"
                 "state=$(systemctl show -p LoadState --value toyquant-relay)\n"
                 "case \"$unit\" in *@) continue ;; esac\n"
                 "echo \"bootstrap: systemd did not load toyquant-relay\"\n",
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
    check("stop_window", lambda t: t.update(live_unit=t["live_unit"].replace("TimeoutStopSec=12",
                                                                              "TimeoutStopSec=5")), "outlast")
    check("bundled", lambda t: t.update(workflow=t["workflow"].replace("preflight.sh", "gone.sh")), "preflight.sh")
    check("coupling", lambda t: t.update(env=t["env"].replace("RELAY_ORDER_LIVE=0", "RELAY_ORDER_LIVE=1")), "demo host")
    check("notify_refs", lambda t: t.update(live_unit=t["live_unit"].replace(
        "toyquant-notify@%n.service", "somebody-else@%n.service")), "not the notifier template")
    # The ordering defect this rule was written for: the preflight before the relay's restart, which is what a stopped
    # host fails on. Dropping the restart line is the smallest edit that reproduces it.
    check("deploy_order", lambda t: t.update(workflow=t["workflow"].replace(
        "sudo systemctl restart toyquant-relay\n", "", 1)), "before restarting the relay")
    # The ops action had the same defect, and it is the way an operator starts a stopped host: the rule reads both.
    check("deploy_order", lambda t: t.update(ops_workflow=t["ops_workflow"].replace(
        "sudo systemctl restart toyquant-relay\n", "", 1)), "ops.yml runs the preflight")
    # A reload that is trusted rather than asked: the bootstrap drops its `LoadState` question.
    check("unit_reload", lambda t: t.update(bootstrap=t["bootstrap"].replace(
        "LoadState", "SomethingElse")), "never asks whether the manager loaded")
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

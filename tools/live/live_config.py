#!/usr/bin/env python3
"""Turn the live configuration into the engine's command line, and refuse anything it does not know.

The live settings live in `config/live.yaml`, which is ignored: a symbol list and a size are a trading decision, and a
public repository has no business carrying one. `config/live.example.yaml` is the tracked template, and it holds nothing
but the demo endpoint and a size that costs almost nothing.

This tool exists so the template is not a second copy of the flags that nothing reads. It parses the small subset of
YAML the file uses -- `key: value` pairs, one level of `section:` nesting, `#` comments -- and prints the flags the engine
takes, refusing an unknown key by name rather than ignoring it: a typo in a live config that is silently dropped is a run
that trades something other than what its file says.

Usage:
    python3 tools/live/live_config.py --flags config/live.yaml
    python3 tools/live/live_config.py --check config/live.example.yaml
"""

import argparse
import os
import shutil
import sys
from datetime import datetime, timezone

# Every key the engine's live path reads, and the flag it becomes. The mapping is deliberately explicit: a key that is not
# here is refused, which is what makes the template a contract rather than a suggestion.
FLAGS = {
    # Positional in `ws` mode, listed here so the parser knows the keys; `POSITIONAL` below fixes their order.
    ("link", "market_endpoint"): "",
    ("engine", "symbol"): "",
    ("engine", "strategy"): "",
    # Optional positionals, after the strategy: the clip scale and the queue model.
    ("engine", "quantity_scale"): "",
    ("engine", "queue_model"): "",
    # The session's own bound: a live window is two hours, so a session started at the open with `ws_seconds: 7200` ends
    # itself at the close and exits 0 -- `Restart=on-failure` then leaves it down, which is what "the window closed" has
    # to mean on a host nobody is watching. The engine's wind-down (`wind_down_*`) is the gentler half of the same end.
    ("engine", "ws_seconds"): "--ws-seconds",
    ("engine", "base_spread_ticks"): "--base-spread-ticks",
    ("engine", "quote_distance_bps"): "--quote-distance-bps",
    ("engine", "refresh_price_ticks"): "--refresh-price-ticks",
    ("engine", "order_size"): "--order-size",
    ("engine", "inventory_limit"): "--inventory-limit",
    ("engine", "inventory_shift_ticks"): "--inventory-shift-ticks",
    ("engine", "onesided_inventory_frac"): "--onesided-inventory-frac",
    ("engine", "extra_layers"): "--extra-layers",
    ("engine", "layer_step_bps"): "--layer-step-bps",
    ("engine", "edge_cover_ticks"): "--edge-cover-ticks",
    ("engine", "min_spread_ticks"): "--min-spread-ticks",
    ("engine", "max_depth_ratio"): "--max-depth-ratio",
    ("engine", "depth_ladder_max"): "--depth-ladder-max",
    ("engine", "post_only"): "--post-only",
    ("engine", "advance_debounce_ms"): "--advance-debounce-ms",
    ("engine", "pause_ticks_per_second"): "--pause-ticks-per-second",
    ("engine", "amend_on_toxicity"): "--amend-on-toxicity",
    ("engine", "amend_deadband_ticks"): "--amend-deadband-ticks",
    ("engine", "markout_cover"): "--markout-cover",
    ("engine", "wind_down_skew_minutes"): "--wind-down-skew-minutes",
    ("engine", "wind_down_end"): "--wind-down-end",
    ("engine", "wind_down_skew_ticks"): "--wind-down-skew-ticks",
    ("engine", "wind_down_adverse_bps"): "--wind-down-adverse-bps",
    ("engine", "settle_residual"): "--settle-residual",
    # The opt-in that makes the venue a socket instead of the in-process matcher, and the four `venue_*` keys below are
    # part of it: the engine refuses them without it (`--venue-endpoint ... part of --live-orders`). Missing this key was
    # the third deploy's failure -- the unit started, exited 1 on that message and was restarted, with both relay sockets
    # listening and nothing to place an order with.
    ("engine", "live_orders"): "--live-orders",
    ("engine", "close_out"): "--close-out",
    ("engine", "close_out_cross_bps"): "--close-out-cross-bps",
    ("engine", "starting_cash"): "--starting-cash",
    ("engine", "max_exposure_usd"): "--max-exposure-usd",
    ("engine", "kill_switch_loss_usd"): "--kill-switch-loss-usd",
    ("engine", "fat_finger_bps"): "--fat-finger-bps",
    ("engine", "max_orders_per_second"): "--max-orders-per-second",
    # The burst bucket that sits beside the rate. It became reachable from this file on 2026-11-28, when the order-budget
    # card named it as one of the four values the next window needs: until then it existed only as a command-line flag, and
    # a session started by a unit cannot pass one.
    ("engine", "order_burst"): "--order-burst",
    ("link", "order_latency_us"): "--order-latency-us",
    ("link", "cancel_latency_us"): "--cancel-latency-us",
    ("link", "venue_endpoint"): "--venue-endpoint",
    ("link", "venue_max_orders"): "--venue-max-orders",
    ("link", "venue_max_notional"): "--venue-max-notional",
    ("link", "venue_orders_per_second"): "--venue-orders-per-second",
    ("link", "snapshot_out"): "--snapshot-out",
}

# Keys the *instrument* owns. An agent may set these with `--set`: they change how a measurement is taken -- a debounce,
# a ladder cap, a latency, the session's own length -- and not how much money is at risk.
#
# Everything else in `FLAGS` is the operator's, and `--set` refuses it by name: the seat (`symbol`, `strategy`), the size
# (`order_size`, `inventory_limit`), the two loss limits, the exit (`close_out`, `close_out_cross_bps`), the opening
# balance, the order kind (`post_only`), and the two endpoints. The list is deliberately explicit rather than derived from
# "not in AGENT_KEYS": a key added later is denied until someone decides it is the instrument's.
AGENT_KEYS = {
    "engine.advance_debounce_ms",
    "engine.amend_deadband_ticks",
    "engine.depth_ladder_max",
    "engine.edge_cover_ticks",
    "engine.extra_layers",
    "engine.layer_step_bps",
    "engine.markout_cover",
    "engine.max_depth_ratio",
    "engine.min_spread_ticks",
    "engine.pause_ticks_per_second",
    "engine.quote_distance_bps",
    "engine.refresh_price_ticks",
    "engine.settle_residual",
    "engine.ws_seconds",
    "link.cancel_latency_us",
    "link.order_latency_us",
    "link.venue_max_notional",
    "link.venue_orders_per_second",
}

# Keys the owner handed to the agent on 2026-11-28, in one message that confirmed the order-budget card's values and said
# the operator's parameters are the agent's to manage. They are the three order-budget limits -- a rate, its burst bucket,
# and the adapter's own cap -- and they are a set of their own rather than extra members of `AGENT_KEYS` so that a reader
# can see exactly what was handed over, and undo it in one place, without re-reading a comment about the instrument's keys.
OWNER_DELEGATED = {
    "engine.max_orders_per_second",
    "engine.order_burst",
    "link.venue_max_orders",
}

# What `--set` may change: the instrument's own keys and the ones the owner delegated. Anything else is refused by name.
SETTABLE = AGENT_KEYS | OWNER_DELEGATED

# Keys whose value is a switch rather than an argument: written as a bare flag when true and left out when false.
SWITCHES = {
    ("engine", "post_only"),
    ("engine", "settle_residual"),
    ("engine", "live_orders"),
    ("engine", "close_out"),
    ("engine", "markout_cover"),
    ("engine", "amend_on_toxicity"),
}

# The keys that are positional in `ws` mode, in the order the engine reads them: the relay's market-data endpoint, the
# symbol, then the strategy, and -- only when the file names them -- the clip scale and the queue model. They are not
# flags, and emitting them as flags would be a command line the parser refuses.
POSITIONAL = (("link", "market_endpoint"), ("engine", "symbol"), ("engine", "strategy"))
POSITIONAL_OPTIONAL = (("engine", "quantity_scale"), ("engine", "queue_model"))

# A value that has to be present for the file to be a live configuration at all.
REQUIRED = POSITIONAL


def parse(text, source="<text>"):
    """The (section, key) -> value mapping, or a `ValueError` naming the line."""
    values = {}
    section = ""
    for number, raw in enumerate(text.splitlines(), start=1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        key, colon, value = line.partition(":")
        if not colon:
            raise ValueError(f"{source}:{number}: '{line}' is not `key: value`")
        key = key.strip()
        value = value.strip().strip('"').strip("'")
        if not value:
            section = key
            continue
        name = (section, key)
        if name not in FLAGS:
            where = f"{section}.{key}" if section else key
            raise ValueError(f"{source}:{number}: '{where}' is not a live setting this engine takes")
        values[name] = value
    missing = [f"{section_name}.{key}" for section_name, key in REQUIRED if (section_name, key) not in values]
    if missing:
        raise ValueError(f"{source}: missing {', '.join(missing)}")
    return values


VENUE_KEYS = (("link", "venue_endpoint"), ("link", "venue_max_orders"), ("link", "venue_orders_per_second"),
              ("link", "venue_max_notional"))

# Keys this reader can emit that the engine refuses in `ws` mode, with the engine's own reason. The reader always emits a
# `ws` command line (`ws` is the only live mode), so a key here can never be part of a valid file -- and finding that out on
# a host costs a unit that exits 1 and a round trip: measured 2026-11-27, `link.snapshot_out`, whose sentence is
# `--snapshot-out ... a session is written and resumed against a replay's own clock`. A live session reconciles against the
# venue (`[RECON]`), not against a snapshot file.
WS_REFUSED = {
    ("link", "snapshot_out"): "--snapshot-out is l2_replay only: a session is written and resumed against a replay's own "
                              "clock, and a live session reconciles against the venue",
}


def validate(values, source="<text>"):
    """The couplings the engine enforces, checked where the file is read rather than on the host.

    `--venue-endpoint` and its three neighbours are *part of* `--live-orders`: the engine refuses them without it, and on a
    host that refusal is a unit that exits 1 and is restarted every five seconds. Measured 2026-11-27, the third deploy:
    both relay sockets listening, `ExecMainStatus=1`, `MainPID=0`, and the sentence that explained it sitting in the
    journal unread because nothing in the report quoted the program's own words. One refusal here is cheaper than that.
    """
    live = str(values.get(("engine", "live_orders"), "false")).strip().lower() in ("true", "yes", "on", "1")
    refused = [f"{section}.{key}" for section, key in WS_REFUSED if (section, key) in values]
    if refused:
        section, key = next(name for name in WS_REFUSED if f"{name[0]}.{name[1]}" in refused)
        raise ValueError(f"{source}: {', '.join(refused)} cannot be part of a live file: {WS_REFUSED[(section, key)]}")
    named = [f"{section}.{key}" for section, key in VENUE_KEYS if (section, key) in values]
    if named and not live:
        raise ValueError(f"{source}: {', '.join(named)} is part of --live-orders: add `engine.live_orders: true`, or "
                         f"remove the venue keys (the engine refuses an endpoint that reaches no venue)")
    if live:
        missing = [f"{section}.{key}" for section, key in VENUE_KEYS[:2] if (section, key) not in values]
        if missing:
            raise ValueError(f"{source}: engine.live_orders needs {', '.join(missing)}: the relay's order channel is "
                             f"named, not guessed, and the adapter's own cap is the limit ADR 0005 decision 4 asks for")
    # The opening balance is US dollars, or the one word `venue` -- which makes the engine read it off the account at
    # start instead of from this file. Refused here rather than on the host, where it is a unit that exits 1 and is
    # restarted: the engine's own sentence would be the first line of a journal nobody reads.
    cash = values.get(("engine", "starting_cash"))
    if cash is not None:
        word = cash.strip().lower()
        if word == "venue":
            if not live:
                raise ValueError(f"{source}: engine.starting_cash is `venue`, which reads the account's equity -- and "
                                 f"that needs engine.live_orders: true (a rehearsal has no account to ask)")
        else:
            try:
                positive = float(word) > 0.0
            except ValueError:
                positive = False
            if not positive:
                raise ValueError(f"{source}: engine.starting_cash is US dollars or `venue` (the account's own equity "
                                 f"at start); '{cash}' is neither")
    return values


def flags(values):
    """The engine's arguments, one a line: the positionals first, then `--name=value` -- the two forms the parser reads.
    A switch is a bare flag when true and absent when false."""
    validate(values)
    arguments = [values[name] for name in POSITIONAL]
    # The optional positionals come straight after the strategy and before any flag, because that is the order `ws` reads.
    arguments.extend(values[name] for name in POSITIONAL_OPTIONAL if name in values)
    for name in sorted(FLAGS):
        if name not in values or name in POSITIONAL or name in POSITIONAL_OPTIONAL:
            continue
        value = values[name]
        if name in SWITCHES:
            if value.lower() in ("true", "yes", "on", "1"):
                arguments.append(FLAGS[name])
            continue
        arguments.append(f"{FLAGS[name]}={value}")
    return arguments


def self_test():
    sample = """
# a live configuration, with the demo endpoint and a size that costs nothing
engine:
  symbol: UP-USDT-SWAP
  strategy: active_l2
  base_spread_ticks: 3
  post_only: true
  max_orders_per_second: 8
  order_burst: 1
link:
  market_endpoint: 127.0.0.1:8765
  order_latency_us: 10000
"""
    values = parse(sample, "sample")
    assert values[("engine", "symbol")] == "UP-USDT-SWAP", values
    produced = flags(values)
    # The three positionals first, in the order `ws` reads them.
    assert produced[:3] == ["127.0.0.1:8765", "UP-USDT-SWAP", "active_l2"], produced
    assert "--base-spread-ticks=3" in produced, produced
    assert "--post-only" in produced, produced
    assert "--order-latency-us=10000" in produced, produced
    # The burst bucket beside the rate: a key that existed only as a command-line flag until 2026-11-28, so the reader has
    # to emit it or the order-budget card cannot be applied from this file at all.
    assert "--max-orders-per-second=8" in produced, produced
    assert "--order-burst=1" in produced, produced
    # A false switch is absent, not `--post-only false`, which the engine does not accept.
    values[("engine", "post_only")] = "false"
    assert "--post-only" not in flags(values), flags(values)
    # The live order leg is one decision across five keys, and the coupling is checked here rather than on a host: the
    # engine refuses `--venue-endpoint` without `--live-orders`, which on a host is a unit that exits 1 and is restarted.
    values[("link", "venue_endpoint")] = "127.0.0.1:8766"
    try:
        flags(values)
    except ValueError as error:
        assert "engine.live_orders" in str(error), str(error)
    else:
        raise AssertionError("an endpoint without live_orders was accepted")
    values[("engine", "live_orders")] = "true"
    try:
        flags(values)
    except ValueError as error:
        assert "link.venue_max_orders" in str(error), str(error)
    else:
        raise AssertionError("live_orders without the adapter's cap was accepted")
    values[("link", "venue_max_orders")] = "20"
    assert "--live-orders" in flags(values), flags(values)
    assert "--venue-endpoint=127.0.0.1:8766" in flags(values), flags(values)
    # The opening balance is a number or the word `venue`, and `venue` needs the live leg: it reads the account, and a
    # rehearsal has none. A typo is refused by name here rather than by a unit that exits 1 on the host.
    values[("engine", "starting_cash")] = "venue"
    assert "--starting-cash=venue" in flags(values), flags(values)
    values[("engine", "starting_cash")] = "98.5"
    assert "--starting-cash=98.5" in flags(values), flags(values)
    for bad, expect in (("1o00", "starting_cash"), ("0", "starting_cash"), ("-5", "starting_cash")):
        values[("engine", "starting_cash")] = bad
        try:
            flags(values)
        except ValueError as error:
            assert expect in str(error), (bad, str(error))
        else:
            raise AssertionError(f"starting_cash {bad!r} was accepted")
    values[("engine", "starting_cash")] = "venue"
    values[("engine", "live_orders")] = "true"
    del values[("engine", "starting_cash")]
    # `venue` in a rehearsal: no live leg and no venue keys, so the refusal is the cash one and not the endpoint one.
    rehearsal = {("link", "market_endpoint"): "127.0.0.1:8765", ("engine", "symbol"): "UP-USDT-SWAP",
                 ("engine", "strategy"): "active_l2", ("engine", "starting_cash"): "venue"}
    try:
        flags(rehearsal)
    except ValueError as error:
        assert "starting_cash" in str(error), str(error)
    else:
        raise AssertionError("`venue` without the live leg was accepted")
    try:
        values[("link", "snapshot_out")] = "data/runtime/live-snapshot.json"
        flags(values)
    except ValueError as error:
        assert "snapshot_out" in str(error), str(error)
    else:
        raise AssertionError("a snapshot_out was accepted for a live file")
    del values[("link", "snapshot_out")]
    # An unknown key is refused with its name, and a missing required one names itself.
    for bad, expect in (
        ("engine:\n  symboll: UP\n", "engine.symboll"),
        ("engine:\n  symbol: UP\n", "engine.strategy"),
        ("oops\n", "not `key: value`"),
    ):
        try:
            parse(bad, "bad")
        except ValueError as error:
            assert expect in str(error), (expect, str(error))
        else:
            raise AssertionError(f"{bad!r} was accepted")
    # `--set` edits one line of a file it does not own, so its own tests are the allowlist, the refusal by name, and the
    # backup. A temp copy of the sample stands in for the operator's file.
    import tempfile
    with tempfile.TemporaryDirectory() as directory:
        target = f"{directory}/live.yaml"
        with open(target, "w", encoding="utf-8") as handle:
            handle.write(sample)
        assert "--order-latency-us=20000" not in flags(parse(open(target, encoding="utf-8").read())), "sample"
        message = set_key(target, "link.order_latency_us=20000")
        assert "was 10000" in message, message
        assert "--order-latency-us=20000" in flags(parse(open(target, encoding="utf-8").read())), "the edit did not land"
        assert any(name.startswith("live.yaml.") for name in __import__("os").listdir(directory)), "no backup"
        for refused, expect in (("engine.order_size=100000000", "is the operator's"),
                                ("engine.not_a_key=1", "not a live setting")):
            try:
                set_key(target, refused)
            except ValueError as error:
                assert expect in str(error), (refused, str(error))
            else:
                raise AssertionError(f"{refused} was accepted")
        # The order-budget card is one edit or none: the keys the owner delegated land together, in one write and one
        # backup, because half of them is a file that describes a session nobody registered.
        before_backups = [name for name in __import__("os").listdir(directory) if name.startswith("live.yaml.")]
        message = set_key(target, "engine.max_orders_per_second=20,engine.order_burst=4")
        after_backups = [name for name in __import__("os").listdir(directory) if name.startswith("live.yaml.")]
        assert len(after_backups) == len(before_backups) + 1, (before_backups, after_backups)
        assert "max_orders_per_second = 20 (was 8)" in message, message
        applied = flags(parse(open(target, encoding="utf-8").read()))
        for expected in ("--max-orders-per-second=20", "--order-burst=4"):
            assert expected in applied, (expected, applied)
        # ... and one assignment that is refused writes nothing at all.
        with open(target, encoding="utf-8") as handle:
            before_text = handle.read()
        try:
            set_key(target, "engine.order_burst=8,engine.order_size=100000000")
        except ValueError as error:
            assert "is the operator's" in str(error), str(error)
        else:
            raise AssertionError("a mixed assignment was accepted")
        with open(target, encoding="utf-8") as handle:
            assert handle.read() == before_text, "a refused assignment still wrote the file"
        # A key the operator's file does not carry yet is refused unless the caller says `--add`: the order-budget card's
        # `order_burst` line is exactly that case on a host that has never carried it, and "a new key is a decision" is the
        # reason the refusal exists -- so the decision is a flag, not an accident of what the file happened to contain.
        try:
            set_key(target, "engine.ws_seconds=7200")
        except ValueError as error:
            assert "add it by hand" in str(error), str(error)
        else:
            raise AssertionError("an absent key was appended without --add")
        set_key(target, "engine.ws_seconds=7200", add=True)
        added = flags(parse(open(target, encoding="utf-8").read()))
        assert "--ws-seconds=7200" in added, added
        # ... and once it is there, a plain set replaces it, so `--add` is not a permanent mode.
        set_key(target, "engine.ws_seconds=5400")
        assert "--ws-seconds=5400" in flags(parse(open(target, encoding="utf-8").read())), "the replace failed"
    print("self-test OK")
    return 0


def set_key(path, assignment, add=False):
    """Set one key, or several as `a=1,b=2`, in place and atomically -- with a backup, and everything else refused.

    The file is the operator's, so this does not rewrite it as YAML: it replaces the line that carries each key and keeps
    that line's comment and indentation. A key that is not in `SETTABLE` is refused by name (a size, a loss limit or the
    seat is a trading decision), an unknown key is refused by `parse`, and the result is put through the same couplings the
    engine would enforce before anything is written. The old file is kept as `<path>.<UTC stamp>`.

    Several keys at once, because one of them landing and another being refused is a file that describes a session nobody
    registered: the card's four budget values are one edit or none, and the write happens after every check has passed.
    """
    assignments = []
    for part in assignment.split(","):
        name, equal, value = part.partition("=")
        name, value = name.strip(), value.strip()
        if not equal or not name or not value:
            raise ValueError(f"`{part.strip()}` is not KEY=VALUE")
        section, _, key = name.partition(".")
        if (section, key) not in FLAGS and (section, key) not in POSITIONAL and (section, key) not in POSITIONAL_OPTIONAL:
            raise ValueError(f"{name} is not a live setting this engine takes")
        if name not in SETTABLE:
            raise ValueError(f"{name} is the operator's, not the instrument's: this tool sets only "
                             f"{', '.join(sorted(SETTABLE))}")
        assignments.append((section, key, name, value))
    with open(path, encoding="utf-8") as handle:
        text = handle.read()
    values = parse(text, path)
    previous = {name: values.get((section, key), "(absent)") for section, key, name, _ in assignments}
    for section, key, _, value in assignments:
        values[(section, key)] = value
    validate(values, path)

    lines = text.splitlines()
    for section, key, name, value in assignments:
        current = ""
        replaced = False
        for index, raw in enumerate(lines):
            code, has_comment, comment = raw.partition("#")
            stripped = code.strip()
            if not stripped:
                continue
            if not raw.startswith((" ", "\t")):
                current = stripped.split(":", 1)[0].strip()
                continue
            if current == section and stripped.startswith(key + ":"):
                indent = raw[: len(raw) - len(raw.lstrip())]
                lines[index] = f"{indent}{key}: {value}" + (f"    #{comment}" if has_comment else "")
                replaced = True
                break
        if not replaced:
            if not add:
                raise ValueError(f"{path} has no `{key}` under `{section}:` to replace; add it by hand, so a new key is a "
                                 f"decision -- or pass --add to append it under its section")
            current = ""
            last = None
            indent_used = "  "
            for index, raw in enumerate(lines):
                stripped = raw.split("#", 1)[0].strip()
                if not stripped:
                    continue
                if not raw.startswith((" ", "\t")):
                    current = stripped.split(":", 1)[0].strip()
                    continue
                if current == section:
                    last = index
                    indent_used = raw[: len(raw) - len(raw.lstrip())]
            if last is None:
                raise ValueError(f"{path} has no `{section}:` section to add `{key}` under")
            stamp_note = datetime.now(timezone.utc).strftime("%Y-%m-%d")
            lines.insert(last + 1, f"{indent_used}{key}: {value}    # set by --set on {stamp_note}")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    # The stamp has one-second resolution, so two edits inside the same second would share a name and the second would
    # overwrite the first backup -- which is the one thing a backup must not do (measured by the self-test, 2026-11-28).
    backup = f"{path}.{stamp}"
    suffix = 1
    while os.path.exists(backup):
        backup = f"{path}.{stamp}-{suffix}"
        suffix += 1
    shutil.copy2(path, backup)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")
    changed = "; ".join(f"{name} = {value} (was {previous[name]})" for _, _, name, value in assignments)
    return f"{path}: {changed}; the previous file is {backup}"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--set", metavar="KEY=VALUE[,KEY=VALUE]",
                        help="set the keys the instrument owns or the owner delegated (engine.order_burst=4"
                             ",link.venue_max_orders=3000), atomically; refuses every other key by name")
    parser.add_argument("--add", action="store_true",
                        help="append a --set key whose line is absent under its section, with the date in a comment; "
                             "without it an absent key is refused, because a new key is a decision")
    parser.add_argument("--file", metavar="FILE", help="the file --set edits (a copy is kept beside it)")
    parser.add_argument("--flags", metavar="FILE", help="print the engine's arguments for this file, one a line")
    parser.add_argument("--check", metavar="FILE", help="parse the file and say whether it is a live configuration")
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()
    if arguments.set:
        if not arguments.file:
            parser.error("--set needs --file: the file it edits")
        try:
            print(set_key(arguments.file, arguments.set, add=arguments.add))
        except (OSError, ValueError) as error:
            print(f"Error: {error}", file=sys.stderr)
            return 1
        return 0
    if not arguments.flags and not arguments.check:
        parser.error("name --flags FILE or --check FILE")
    target = arguments.flags or arguments.check
    try:
        with open(target, encoding="utf-8") as handle:
            values = parse(handle.read(), target)
    except (OSError, ValueError) as error:
        # A missing file is an error that names it, and an unknown key names itself: neither is a table of zeros.
        print(f"Error: {error}", file=sys.stderr)
        return 1
    if arguments.check:
        # `--check` is the one-command question "is this file a live configuration", so it runs the couplings too: a file
        # that parses and would be refused by the engine on the host is not a live configuration.
        try:
            validate(values, target)
        except ValueError as error:
            print(f"Error: {error}", file=sys.stderr)
            return 1
        print(f"{target}: {len(values)} setting(s), a live configuration")
        return 0
    try:
        printed = flags(values)
    except ValueError as error:
        # The couplings are validated inside `flags`, so this is where a file that parses but would be refused on the host
        # becomes one sentence instead of a unit that exits 1 and is restarted every five seconds.
        print(f"Error: {error}", file=sys.stderr)
        return 1
    print("\n".join(printed))
    return 0

if __name__ == "__main__":
    sys.exit(main())

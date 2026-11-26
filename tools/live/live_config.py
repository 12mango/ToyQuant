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
import sys

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
    ("link", "order_latency_us"): "--order-latency-us",
    ("link", "cancel_latency_us"): "--cancel-latency-us",
    ("link", "venue_endpoint"): "--venue-endpoint",
    ("link", "venue_max_orders"): "--venue-max-orders",
    ("link", "venue_max_notional"): "--venue-max-notional",
    ("link", "venue_orders_per_second"): "--venue-orders-per-second",
    ("link", "snapshot_out"): "--snapshot-out",
}

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


def validate(values, source="<text>"):
    """The couplings the engine enforces, checked where the file is read rather than on the host.

    `--venue-endpoint` and its three neighbours are *part of* `--live-orders`: the engine refuses them without it, and on a
    host that refusal is a unit that exits 1 and is restarted every five seconds. Measured 2026-11-27, the third deploy:
    both relay sockets listening, `ExecMainStatus=1`, `MainPID=0`, and the sentence that explained it sitting in the
    journal unread because nothing in the report quoted the program's own words. One refusal here is cheaper than that.
    """
    live = str(values.get(("engine", "live_orders"), "false")).strip().lower() in ("true", "yes", "on", "1")
    named = [f"{section}.{key}" for section, key in VENUE_KEYS if (section, key) in values]
    if named and not live:
        raise ValueError(f"{source}: {', '.join(named)} is part of --live-orders: add `engine.live_orders: true`, or "
                         f"remove the venue keys (the engine refuses an endpoint that reaches no venue)")
    if live:
        missing = [f"{section}.{key}" for section, key in VENUE_KEYS[:2] if (section, key) not in values]
        if missing:
            raise ValueError(f"{source}: engine.live_orders needs {', '.join(missing)}: the relay's order channel is "
                             f"named, not guessed, and the adapter's own cap is the limit ADR 0005 decision 4 asks for")
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
    print("self-test OK")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--flags", metavar="FILE", help="print the engine's arguments for this file, one a line")
    parser.add_argument("--check", metavar="FILE", help="parse the file and say whether it is a live configuration")
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()
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

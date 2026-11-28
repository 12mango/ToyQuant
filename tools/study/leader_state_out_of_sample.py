#!/usr/bin/env python3
"""Prepare the cuts `fill_markout_by_state.py` needs for a month it has never seen, then run it.

The counterfactual the order-budget card asks for is a fill-level one: on twenty-one days that do not overlap the seven the
gate was found on, drop the fills the leader was moving under and recompute the mean markout. That needs one two-hour cut of
the follower's book per day and one trade slice per day, which is what this prepares; the analysis itself is
`fill_markout_by_state.py`'s and stays in one place.

Why a script and not a loop typed into a shell: the cuts are 21 x ~130 MB and a full `/tmp` is the one failure with no
message of its own (measured 2026-11-26, a batch died with exit 120 and an empty log), so the preparation takes the sweep
tool's own lock, refuses on low free space, and names each day as it lands -- and the command is registered here, in the
tree, rather than in a session's history.

    python3 tools/study/leader_state_out_of_sample.py                       # the twenty-one days of block 4
    python3 tools/study/leader_state_out_of_sample.py --days 2026-11-23,2026-11-24
    python3 tools/study/leader_state_out_of_sample.py --prepare-only        # cut, do not analyse

The days default to 2026-10-30..2026-11-26, which is the block the card's out-of-sample test names: 10-30..11-15 and
11-23..26 are outside the seven days (11-16..11-22) the gate was measured on, and the overlapping seven are skipped rather
than silently counted twice.
"""
import argparse
import subprocess
import sys
from datetime import datetime, timedelta
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "sweep"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import okx_maker_baseline as ob  # noqa: E402  (the lock, the free-space refusal and the two cutters, one implementation)
import book_lead_lag as bl  # noqa: E402  (the window's two bounds, one implementation)

ROOT = Path(__file__).resolve().parents[2]
CUTS = ROOT / "out/cuts"
FOLLOWER = "UP-USDT-SWAP"
SEEN_ON = ("2026-11-16", "2026-11-17", "2026-11-18", "2026-11-19", "2026-11-20", "2026-11-21", "2026-11-22")
DEFAULT_FROM, DEFAULT_TO = "2026-10-30", "2026-11-26"


def day_range(first, last):
    start = datetime.strptime(first, "%Y-%m-%d")
    end = datetime.strptime(last, "%Y-%m-%d")
    days = []
    while start <= end:
        days.append(start.strftime("%Y-%m-%d"))
        start += timedelta(days=1)
    return days


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--days", default="", help=f"comma separated file days (default {DEFAULT_FROM}..{DEFAULT_TO})")
    parser.add_argument("--from", dest="first", default=DEFAULT_FROM)
    parser.add_argument("--to", dest="last", default=DEFAULT_TO)
    parser.add_argument("--tick", type=float, default=0.0001, help="UP's tick, which the cut's level filter uses")
    parser.add_argument("--prepare-only", action="store_true", help="make the cuts and stop")
    args = parser.parse_args(argv)

    days = [part.strip() for part in args.days.split(",") if part.strip()] or day_range(args.first, args.last)
    seen = [day for day in days if day in SEEN_ON]
    days = [day for day in days if day not in SEEN_ON]
    if seen:
        print(f"# skipping {len(seen)} day(s) the gate was measured on: {', '.join(seen)}", flush=True)

    ob.take_lock(CUTS)
    ob.require_free_space(CUTS)
    prepared = []
    for day in days:
        archive = ROOT / "data/v2/crypto" / FOLLOWER / f"{FOLLOWER}-L2orderbook-400lv-{day}.tar.gz"
        if not archive.exists():
            print(f"# {day}: no archive at {archive.relative_to(ROOT)} -- skipped, not counted", flush=True)
            continue
        start_ms, end_ms = bl.window_bounds(day, "13:30", "15:30")
        cut = ob.ensure_cut(archive, CUTS, "13:30", "15:30", args.tick)
        if cut is None:
            print(f"# {day}: no usable cut -- skipped", flush=True)
            continue
        sliced = ob.slice_trades(day, FOLLOWER, start_ms, end_ms, CUTS)
        if sliced is None:
            print(f"# {day}: no trade slice -- skipped", flush=True)
            continue
        prepared.append(day)
        print(f"# {day}: cut {cut.name}, trades {sliced.name}", flush=True)

    if not prepared:
        print("no day is ready: nothing to analyse", file=sys.stderr)
        return 2
    print(f"# {len(prepared)} day(s) ready: {', '.join(prepared)}", flush=True)
    if args.prepare_only:
        return 0
    print("# running the analysis; the moving-state counterfactual is the flat bucket's mean against the pooled mean",
          flush=True)
    return subprocess.run([sys.executable, str(ROOT / "tools/study/fill_markout_by_state.py"),
                           "--days", ",".join(prepared), "--follower", FOLLOWER], cwd=ROOT).returncode


if __name__ == "__main__":
    sys.exit(main())

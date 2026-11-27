#!/usr/bin/env python3
"""The live file's two order-path knobs, A/B on one window: the advance debounce and nothing else.

The measurement it exists for (2026-11-27, the first live window): the session placed **2,547 sell quotes against 44
buy ones** and reached the venue with **201 of 2,591** orders. Neither number reproduces in the engine -- replaying the
same two hours with the live flags reads 31 buy / 40 sell (at `--advance-debounce-ms=40000`, which is what `live.yaml`
carries **and** what the recorded ten-contract cell carries) and 52 buy / 25 sell at `40`. The second arm is a **new
setting rather than a restoration**: 40 s halved the fills and flipped the sign on that tape (3 fills and -0.2317
against 6 and +0.1066), so it is a hypothesis to register and test on more than one day, not a cell to go back to.

The debounce alone still costs half the fills and flips the sign on the same tape, which is why the arm is recorded:

    python3 tools/study/live_knobs_ab.py --days 2026-11-26 --start 13:30 --end 15:30 \\
        --debounce 40000,40 --extra "--base-spread-ticks=3 --post-only --close-out --settle-residual"

The three venue caps belong to `--live-orders` and are refused without it, so a replay cannot model them: that drop is
outside the engine's measured surface and is measured from the live journal instead (`[VENUE] refused_by_adapter`).
"""
import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "sweep"))
import okx_maker_baseline as sweep  # noqa: E402  (the cut and the trade slice, kept in one place)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--days", required=True, help="one or more file days, comma separated (the depth archive's day)")
    parser.add_argument("--symbol", default="UP-USDT-SWAP")
    parser.add_argument("--start", default="13:30", help="window start, UTC")
    parser.add_argument("--end", default="15:30", help="window end, UTC")
    parser.add_argument("--tick", type=float, default=0.0001, help="the instrument's tick, for the cut")
    parser.add_argument("--debounce", default="40000,40",
                        help="comma separated --advance-debounce-ms values; 40000 is the live file, 40 the recorded cells")
    parser.add_argument("--order-size", default="10000000")
    parser.add_argument("--inventory-limit", default="100000000")
    parser.add_argument("--extra", default="--base-spread-ticks=3 --refresh-price-ticks=1 --post-only --close-out "
                                           "--close-out-cross-bps=20 --settle-residual --settle-slippage-ticks=1 "
                                           "--fat-finger-bps=200 --max-orders-per-second=8 --max-exposure-usd=60 "
                                           "--kill-switch-loss-usd=6 --order-latency-us=100000 "
                                           "--cancel-latency-us=100000",
                        help="the run's remaining flags, space separated")
    parser.add_argument("--cuts", default="/tmp/tq/knob_cuts", help="where the depth cut and the trade slice live")
    args = parser.parse_args()

    for day in [part.strip() for part in args.days.split(",") if part.strip()]:
        archive = sweep.depth_archive(sweep.ROOT / "data/v2/crypto", args.symbol, day)
        if archive is None:
            raise SystemExit(f"no {args.symbol}-L2orderbook-400lv-{day}.tar.gz on disk")
        cut = sweep.ensure_cut(archive, args.cuts, args.start, args.end, args.tick)
        if cut is None:
            raise SystemExit(f"{day}: the depth archive has no usable cut for {args.start}-{args.end}")
        trades = sweep.slice_trades(day, args.symbol, sweep.first_timestamp(cut),
                                    sweep.end_of_window(day, args.end), args.cuts)
        if trades is None:
            raise SystemExit(f"{day}: no trades CSV for {args.symbol}")
        for debounce in [part.strip() for part in args.debounce.split(",") if part.strip()]:
            command = [str(sweep.BIN), "l2_replay", str(trades), str(cut), args.symbol, "0", "active_l2", "1",
                       "prorata", "--fast-validation", f"--order-size={args.order_size}",
                       f"--inventory-limit={args.inventory_limit}", f"--advance-debounce-ms={debounce}"] + \
                      [part for part in args.extra.split() if part]
            run = subprocess.run(command, cwd=sweep.ROOT, capture_output=True, text=True)
            print(f"=== {day} --advance-debounce-ms={debounce} exit={run.returncode}", flush=True)
            for line in run.stdout.splitlines():
                if line.startswith(("[STRATEGY_METRICS]", "[PNL]", "[EXECUTION]")):
                    print(line, flush=True)
            if run.returncode != 0:
                detail = run.stderr.strip().splitlines()
                print(detail[0] if detail else run.stdout.strip()[:200], file=sys.stderr)
                return run.returncode
            print(flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())

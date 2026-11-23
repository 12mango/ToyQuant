#!/usr/bin/env python3
"""Both gates on one command: the fee against the spread, and the capture against the position.

Two measurements already exist in this directory and each answers half of the question a strategy review asks.

    screen_spread.py   the spread where trades print, the fee per side in ticks, and the pre-registered rule
                       "half spread at the trades >= three fees"
    venue_ratio.py     R_max = prints x (half spread - cost a unit) / sigma, the largest number of prints per fill
                       that still covers one window's own standard deviation

A table answering both used to be assembled by hand from two runs, and a hand-joined table is how a copied number is
made. This runs both measures over the same files -- imported, not re-implemented, so each ratio has one definition in
the tree -- and prints one row per instrument, with the verdict of each gate and of the two together.

    python3 tools/study/fee_gate_table.py --input data/v2/equities/xnas_mbp10_SPY_2026-09-29_1330_1530.csv --symbol SPY
    python3 tools/study/fee_gate_table.py --input 'data/v2/equities/xnas_mbp10_*.csv*' --clip 1,100

The cost is two passes per file -- about forty seconds on the two-million-row SPY screen -- because the two measures read
the file for different reasons and neither is asked to cache the other's work. A file that is not the schema, or holds
more than one instrument, is refused in a row of its own rather than dropped: a table that silently loses a symbol is
worse than one that names the reason.
"""

import argparse
import glob
from pathlib import Path

import screen_spread
import venue_ratio

SPREAD_FACTOR = 3.0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--input", required=True, help="a file, a glob, or several separated by commas")
    parser.add_argument("--symbol", default="", help="the instrument's name, when the file name does not carry one")
    parser.add_argument("--tick", type=float, default=0.01)
    parser.add_argument("--multiplier", type=float, default=1.0)
    parser.add_argument("--fee-per-share", type=float, default=0.0035)
    parser.add_argument("--min-commission", type=float, default=0.35)
    parser.add_argument("--clip", default="1,100")
    args = parser.parse_args()

    clips = [float(part) for part in args.clip.split(",") if part.strip()]
    paths = []
    for piece in args.input.split(","):
        paths.extend(sorted(glob.glob(piece)) or [piece])

    print("| instrument | half spread at trades | fee a side | fee / half spread | spread gate | R_max | ratio gate |")
    print("|---|---:|---:|---:|---|---:|---|")
    passed = {"spread": 0, "ratio": 0, "both": 0}
    for path in paths:
        grouped = venue_ratio.rows_of(path)
        symbol = args.symbol or (next(iter(grouped)) if len(grouped) == 1 else Path(path).name.split("_")[1]
                                 if len(Path(path).name.split("_")) > 1 else Path(path).name)
        if len(grouped) > 1:
            print(f"| {symbol} | -- | -- | -- | -- | -- | refused: the file carries {len(grouped)} instruments |")
            continue
        if not grouped:
            print(f"| {symbol} | -- | -- | -- | -- | -- | refused: no top of book in this schema |")
            continue
        try:
            screen = screen_spread.measure(path, symbol, args.tick, args.multiplier, args.fee_per_share)
        except SystemExit as error:
            print(f"| {symbol} | -- | -- | -- | -- | -- | refused: {error} |")
            continue
        stats = venue_ratio.measure(next(iter(grouped.values())), args.multiplier)
        if stats is None:
            print(f"| {symbol} | -- | -- | -- | -- | -- | refused: fewer than five spreads |")
            continue
        venue_ratio.fill_ratios(stats, clips, fee_per_unit=args.fee_per_share,
                                min_commission=args.min_commission)
        half_trade = screen["trade_half_spread_ticks"]
        fee_ticks = screen["fee_ticks"]
        spread_ok = fee_ticks > 0.0 and half_trade >= SPREAD_FACTOR * fee_ticks
        ratio_ok = stats["best"] is not None and stats["best"] >= venue_ratio.REFERENCE
        passed["spread"] += bool(spread_ok)
        passed["ratio"] += bool(ratio_ok)
        passed["both"] += bool(spread_ok and ratio_ok)
        print(f"| {symbol} | {half_trade:.3f} ticks | {fee_ticks:.3f} ticks | {fee_ticks / half_trade:.2f} | "
              f"{'pass' if spread_ok else 'fail'} | "
              f"{stats['best']:.0f} | {'pass' if ratio_ok else 'fail'} |", flush=True)

    total = len(paths)
    print()
    print(f"the spread gate (half spread at the trades >= {SPREAD_FACTOR:g} fees) passes {passed['spread']} of {total}")
    print(f"the ratio gate (R_max >= {venue_ratio.REFERENCE:.0f} prints a fill) passes {passed['ratio']} of {total}")
    print(f"both: {passed['both']} of {total}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

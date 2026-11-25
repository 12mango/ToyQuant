#!/usr/bin/env python3
"""Rank the archived screen for the *download* decision: the two homogeneous groups the measured instruments define.

The screen answers "which instruments have a wide book and enough turnover". It does not answer the question the next
step asks, which is **which instrument to spend a manual 400-level depth download on**, and that question has a shape:
an instrument is only worth a download if a quote at our distance still captures the fee, and the distance that matters
depends on what one tick is worth. So the archive is re-ranked into two groups, each named after the instrument that
measured it:

* **UP's shape** -- the tick is worth several basis points, the book is two ticks wide, and three ticks of distance
  captures 10 to 18 bps: enough above the 2 bps fee to survive the adverse selection a wide quote does not face.
  `DNA` is the counter-example at the top of the same ranking: 23 bps of capture and a book that almost never sweeps
  that far, which is why the upper bound is here.
* **CT's shape** -- a narrow tick (0.7 to 2.5 bps) against a book at least four ticks wide, so a 14 bps distance is
  eighteen to forty ticks rather than forty-four, which is where `PIEVERSE` fails: its tick is 0.32 bps and its capture
  at three ticks is 0.96 bps, under the fee.

Both need turnover the strategy can use, which is why the notional floors are 1.5 M and 2 M USD a day rather than the
screen's 500 k. The arithmetic is `okx_spread_screen.analyse`'s own -- this tool filters its rows rather than
re-implementing the medians, so the two cannot drift.

    python3 tools/study/okx_download_rank.py --read data/v2/crypto/okx_tickers_20261125T133101Z.json
    python3 tools/study/okx_download_rank.py --read <archive> --out /tmp/tq/rank.md

The output is a markdown table per group, ranked by capture (group one) or by notional (group two), with a column that
marks the instruments whose depth is already on disk. `--read` is the screen's own flag for an archived sample, so a
window that has closed can still be ranked.
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import okx_spread_screen as screen  # noqa: E402

# The instruments whose depth the owner has downloaded, so a reader can see what the list already covers.
ON_DISK = {"UP-USDT-SWAP", "DNA-USDT-SWAP", "NES-USDT-SWAP", "CT-USDT-SWAP", "PIEVERSE-USDT-SWAP"}


def coarse(row):
    """UP's shape: >= 2 ticks, three-tick capture 10-18 bps, >= 1.5 M USD a day."""
    return ((row["ticks"] or 0.0) >= 2.0 and row["capture"] is not None and
            10.0 <= row["capture"] <= 18.0 and row["notional"] >= 1_500_000)


def fine(row):
    """CT's shape: >= 4 ticks, a tick of 0.7-2.5 bps, >= 2 M USD a day."""
    return ((row["ticks"] or 0.0) >= 4.0 and row["tick_bps"] is not None and
            0.7 <= row["tick_bps"] <= 2.5 and row["notional"] >= 2_000_000)


def markdown(title, selected, order):
    lines = [f"### {title}", "",
             "| instrument | half spread bps | width ticks | tick bps | 3-tick capture bps | notional USD | depth on disk |",
             "|---|---:|---:|---:|---:|---:|---|"]
    for row in sorted(selected, key=order, reverse=True):
        lines.append(f"| {row['symbol']} | {row['median']:.2f} | {row['ticks']:.2f} | {row['tick_bps']:.2f} | "
                     f"{row['capture']:.2f} | {row['notional']:,.0f} | "
                     f"{'yes' if row['symbol'] in ON_DISK else ''} |")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--read", required=True, help="the archived sample set, as okx_spread_screen writes it")
    parser.add_argument("--out", help="where the two tables go; default is stdout")
    args = parser.parse_args()

    stored = json.loads(Path(args.read).read_text(encoding="utf-8"))
    samples = {symbol: {key: list(value) if isinstance(value, list) else value
                        for key, value in entry.items()} for symbol, entry in stored.items()}
    rows = screen.analyse(samples, 0.0, 0.0, 0.0, 3.0, 0.0)
    stamps = [stamp for entry in samples.values() for stamp in entry.get("stamps", [])]
    first, last = (min(stamps), max(stamps)) if stamps else ("?", "?")

    coarse_rows = [row for row in rows if coarse(row)]
    fine_rows = [row for row in rows if fine(row)]
    document = "\n".join([
        f"The archived screen: {len(rows)} instruments with a two-sided book, reads {first} to {last}.",
        "",
        markdown(f"UP's shape -- >= 2 ticks, capture 10-18 bps, >= 1.5 M USD a day ({len(coarse_rows)})",
                 coarse_rows, lambda row: row["capture"]),
        markdown(f"CT's shape -- >= 4 ticks, tick 0.7-2.5 bps, >= 2 M USD a day ({len(fine_rows)})",
                 fine_rows, lambda row: row["notional"]),
    ])
    if args.out:
        Path(args.out).write_text(document, encoding="utf-8")
        print(f"group1={len(coarse_rows)} group2={len(fine_rows)} written to {args.out}")
    else:
        print(document)
    return 0


if __name__ == "__main__":
    sys.exit(main())

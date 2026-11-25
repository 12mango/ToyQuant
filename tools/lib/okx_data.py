#!/usr/bin/env python3
"""Where an OKX archive lives: `data/v2/crypto/<SYMBOL>/` since 2026-11-25, the flat root before that.

The venue's portal hands out `<SYMBOL>-L2orderbook-400lv-<day>.tar.gz` and the public history endpoint a
`okx_trades_<day>_<SYMBOL>.csv`. In one flat folder that is nine instruments and three kinds of file in the same listing,
and a reader cannot see which day belongs to which symbol -- so each instrument gets a folder, and this module is the one
place that knows it. Both layouts are accepted on purpose: the flat one is what every recorded number was measured from,
and a measurement must not start depending on where a file was moved.

    from okx_data import depth_archive, trade_tape

A function returns `None` rather than raising, so the caller reports the missing file by name (the convention every tool
here follows: a missing input is an error that names the file, never a table of zeros).
"""

from pathlib import Path


def _find(root, name):
    """The flat `<root>/<name>` or a per-symbol `<root>/<SYMBOL>/<name>`, else None."""
    root = Path(root)
    flat = root / name
    if flat.is_file():
        return flat
    if not root.is_dir():
        return None
    for folder in sorted(root.iterdir()):
        candidate = folder / name
        if folder.is_dir() and candidate.is_file():
            return candidate
    return None


def depth_archive(root, symbol, day):
    """The 400-level day archive for one instrument, or None when no layout has it."""
    return _find(root, f"{symbol}-L2orderbook-400lv-{day}.tar.gz")


def trade_tape(root, symbol, day):
    """The day's trade tape, as `tools/fetch/fetch_okx_trades.py` writes it, or None."""
    return _find(root, f"okx_trades_{day}_{symbol}.csv")

#!/usr/bin/env python3
"""An OKX day archive's own book: its time-weighted half-spread in a UTC window, and the cut a replay needs.

Why this exists. The screen that put a perpetual on the list read the venue's public ticker, sampled once a second; a
replay reads the venue's 400-level archive. When the two disagree the archive is the one that is right, and the
statistic that matters is not the median of the records -- a book that is one tick wide for ninety percent of a window
is not described by it -- but the median *weighted by how long each spread held*.

The second job is the cut. `src/market/okx_readers.cpp` refuses a depth file whose first record is an update, and a
window has to begin at a snapshot rather than at a line number, so the cut opens on the last snapshot at or before the
window and keeps that snapshot's updates. A cut that started mid-stream would replay a book built from nothing.

A crossed book is counted and skipped rather than measured: `top_of_book()` refuses one, so including it would be
reporting a state no consumer of this engine ever sees.

Usage:
    python3 tools/study/okx_book_window.py data/v2/crypto/UP-USDT-SWAP-L2orderbook-400lv-2026-11-16.tar.gz \
        --start 13:30 --end 15:30 --tick 0.0001 --emit /tmp/tq/upwin

Measured with it (2026-11-25, seven days of `UP-USDT-SWAP` at 13:30-15:30 UTC): the time-weighted median half-spread
is 4.43 to 7.69 bps, two to three ticks, so the fee is 0.26 to 0.45 of the half-spread; no day had a crossed batch.
"""

import argparse
import json
import sys
import tarfile
from datetime import datetime, timezone
from pathlib import Path


def day_of(path):
    """`...-400lv-2026-11-16.tar.gz` -> `2026-11-16`; the archive's naming is the only date the file carries."""
    name = Path(path).name
    if "-400lv-" not in name:
        raise SystemExit(f"{path}: not an L2 order book archive (expected '-400lv-' in the name)")
    return name.split("-400lv-")[-1].split(".tar.gz")[0]


def window_bounds(day_text, start, end):
    day = datetime.strptime(day_text, "%Y-%m-%d").replace(tzinfo=timezone.utc)

    def at(hhmm):
        hour, minute = (int(part) for part in hhmm.split(":"))
        return int(day.replace(hour=hour, minute=minute).timestamp() * 1000)

    return at(start), at(end)


def measure(path, start_ms, end_ms, emit_path=None):
    """Apply the archive in order, keep the window's top of book, and optionally write the window out."""
    book = {"bid": {}, "ask": {}}
    pairs = []
    records = snapshots = emitted = 0
    pending = []
    out = open(emit_path, "wb") if emit_path else None
    try:
        with tarfile.open(path) as archive:
            member = next((item for item in archive.getmembers() if item.name.endswith(".data")), None)
            if member is None:
                raise SystemExit(f"{path}: no .data member")
            for line in archive.extractfile(member):
                records += 1
                if not line.startswith(b"{"):
                    continue
                record = json.loads(line)
                ts = int(record["ts"])
                if ts > end_ms:
                    break
                action = record.get("action")
                if action == "snapshot":
                    snapshots += 1
                    book = {"bid": {}, "ask": {}}
                if ts < start_ms:
                    # Before the window only the state matters. The lines are kept so the cut can open on a snapshot.
                    pending = [line] if action == "snapshot" else pending + [line]
                    for key, side in (("bids", "bid"), ("asks", "ask")):
                        for price, size, *_ in record.get(key, []):
                            book[side][float(price)] = float(size)
                    continue
                if out is not None:
                    for buffered in pending:
                        out.write(buffered)
                    pending = []
                for key, side in (("bids", "bid"), ("asks", "ask")):
                    for price, size, *_ in record.get(key, []):
                        price, size = float(price), float(size)
                        if size == 0.0:
                            book[side].pop(price, None)
                        else:
                            book[side][price] = size
                if out is not None:
                    out.write(line)
                    emitted += 1
                if book["bid"] and book["ask"]:
                    best_bid, best_ask = max(book["bid"]), min(book["ask"])
                    pairs.append((ts, best_bid, best_ask, best_bid >= best_ask))
    finally:
        if out is not None:
            out.close()
    return records, snapshots, pairs, emitted


def weighted_medians(pairs, tick):
    """Duration-weighted medians of the half-spread in bps and of the spread in ticks, over uncrossed intervals."""
    bps, ticks, crossed = [], [], 0
    for index in range(len(pairs) - 1):
        ts, best_bid, best_ask, is_crossed = pairs[index]
        duration = pairs[index + 1][0] - ts
        if duration <= 0:
            continue
        if is_crossed:
            crossed += duration
            continue
        mid = (best_bid + best_ask) / 2.0
        bps.append((duration, (best_ask - best_bid) / 2.0 / mid * 10_000.0))
        ticks.append((duration, (best_ask - best_bid) / tick))

    def weighted(items):
        if not items:
            return None
        items = sorted(items, key=lambda item: item[1])
        total = sum(duration for duration, _ in items)
        running = 0
        for duration, value in items:
            running += duration
            if running * 2 >= total:
                return value
        return items[-1][1]

    return weighted(bps), weighted(ticks), sum(duration for duration, _ in bps), crossed


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("archives", nargs="+")
    parser.add_argument("--start", default="13:30", help="window start, UTC, HH:MM")
    parser.add_argument("--end", default="15:30", help="window end, UTC, HH:MM")
    parser.add_argument("--tick", type=float, default=0.0001, help="the instrument's price step")
    parser.add_argument("--fee-bps", type=float, default=2.0, help="the maker fee the ratio is read against")
    parser.add_argument("--emit", help="write each window here as a depth file, opening on a snapshot")
    args = parser.parse_args()

    emit_dir = Path(args.emit) if args.emit else None
    if emit_dir is not None:
        emit_dir.mkdir(parents=True, exist_ok=True)

    print("| day | window minutes | records | snapshots | median half spread bps | in ticks | fee / spread | crossed share |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|")
    for path in args.archives:
        day_text = day_of(path)
        start_ms, end_ms = window_bounds(day_text, args.start, args.end)
        emit_path = None
        if emit_dir is not None:
            # The name carries the instrument: a cuts directory serves every symbol, and a cut keyed by day alone made a
            # second instrument's run replay the first one's book (measured 2026-11-25 on DNA, whose run read the UP cut).
            symbol = Path(path).name.split("-L2orderbook")[0]
            emit_path = str(emit_dir / f"okx_{symbol}_{day_text}_{args.start.replace(':', '')}.jsonl")
        records, snapshots, pairs, emitted = measure(path, start_ms, end_ms, emit_path)
        median, median_ticks, covered, crossed = weighted_medians(pairs, args.tick)
        if median is None:
            print(f"| {day_text} | 0 | {records} | {snapshots} | n/a | n/a | n/a | n/a |")
            continue
        crossed_share = crossed / (covered + crossed) if (covered + crossed) else 0.0
        print(f"| {day_text} | {covered // 60_000} | {records} | {snapshots} | {median:.2f} | {median_ticks:.2f} | "
              f"{args.fee_bps / median:.2f} | {crossed_share:.4f} |")
        if emit_path:
            print(f"# {emit_path} records={emitted}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

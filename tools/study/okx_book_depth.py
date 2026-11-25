#!/usr/bin/env python3
"""The depth a quote would have stood behind: a cut window's top two levels, time-weighted.

The capacity question is not "how wide is the book" but "how much of it can a quote be", and that needs the sizes at the
touch rather than the spread. `tools/study/okx_book_window.py --emit` writes the window the replay reads; this reads that
same file, keeps the book, and records one mark a second, so the medians are weighted by how long each state held rather
than by how many messages happened to arrive while it did.

    python3 tools/study/okx_book_depth.py /tmp/tq/upwin/okx_2026-11-16_1330.jsonl ...
    # then, per contract: mid x ctVal = USD, and a tenth of the thinner level 1 is the clip the book supports

Measured with it on the seven `UP-USDT-SWAP` windows at 13:30-15:30 UTC (2026-11-25): level 1 is 14 to 161 contracts a
side (median 67 bid / 71 ask) and the two best levels 130 to 555 (median 257 / 329), with the windows' mids 0.166 to
0.228. One contract is 10 UP, so a tenth of the thinner touch is about four contracts and 8 USD -- the arithmetic in
`docs/archive/studies.md` turns that into 27,300 USD of monthly notional and 0.5 percent of the volume tier.
"""

import argparse
import json
from pathlib import Path


def weighted_median(items):
    """The value that splits the duration in half, not the count: a state that held for an hour counts for an hour."""
    if not items:
        return None
    items = sorted(items, key=lambda item: item[1])
    total = sum(duration for duration, _ in items)
    running = 0.0
    for duration, value in items:
        running += duration
        if running * 2 >= total:
            return value
    return items[-1][1]


def snapshot(book, second, marks):
    """One second's last top of book with its own sizes, which is all the time-weighted median needs."""
    if not book["bid"] or not book["ask"]:
        return
    best_bid, best_ask = max(book["bid"]), min(book["ask"])
    if best_bid >= best_ask:
        return
    second_bid = max((price for price in book["bid"] if price < best_bid), default=None)
    second_ask = min((price for price in book["ask"] if price > best_ask), default=None)
    bid_two = book["bid"][best_bid] + (book["bid"][second_bid] if second_bid is not None else 0.0)
    ask_two = book["ask"][best_ask] + (book["ask"][second_ask] if second_ask is not None else 0.0)
    state = (second, best_bid, best_ask, book["bid"][best_bid], book["ask"][best_ask], bid_two, ask_two,
             (best_bid + best_ask) / 2.0)
    if marks and marks[-1][1:5] == state[1:5]:
        return
    marks.append(state)


def one(path):
    """The window's time-weighted top of book: level 1 and the two best levels, per side, plus the mid."""
    book = {"bid": {}, "ask": {}}
    marks = []
    current = None
    with Path(path).open("rb") as handle:
        for line in handle:
            if not line.startswith(b"{"):
                continue
            record = json.loads(line)
            ts = int(record["ts"])
            # OKX's `ts` is milliseconds in this archive (`okx_book_window.py` bounds the window with `* 1000`), so a
            # second is `// 1000` and a duration is already in milliseconds.
            second = ts // 1000
            if current is not None and second != current:
                snapshot(book, current, marks)
            current = second
            for key, side in (("bids", "bid"), ("asks", "ask")):
                for price, size, *_ in record.get(key, []):
                    price, size = float(price), float(size)
                    if size == 0.0:
                        book[side].pop(price, None)
                    else:
                        book[side][price] = size
    if current is not None:
        snapshot(book, current, marks)
    levels = []
    for index in range(len(marks) - 1):
        duration = marks[index + 1][0] - marks[index][0]
        if duration > 0:
            levels.append((duration, marks[index]))
    if not levels:
        return None

    def pick(column):
        return weighted_median([(duration, row[column]) for duration, row in levels])

    return {"bid1": pick(3), "ask1": pick(4), "bid2": pick(5), "ask2": pick(6), "mid": pick(7),
            "covered_s": sum(duration for duration, _ in levels)}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("windows", nargs="+", help="cut windows from okx_book_window.py --emit")
    parser.add_argument("--ct-val", type=float, default=10.0, help="contracts' value, for the USD column")
    args = parser.parse_args()
    print("| day | covered s | bid level 1 | ask level 1 | two levels bid | two levels ask | mid | level 1 USD |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|")
    for path in args.windows:
        result = one(path)
        day = Path(path).name.split("_")[1] if "_" in Path(path).name else Path(path).name
        if result is None:
            print(f"| {day} | 0 | n/a | n/a | n/a | n/a | n/a | n/a |")
            continue
        thinner = min(result["bid1"], result["ask1"])
        print(f"| {day} | {result['covered_s']:.0f} | {result['bid1']:.0f} | {result['ask1']:.0f} | "
              f"{result['bid2']:.0f} | {result['ask2']:.0f} | {result['mid']:.5f} | "
              f"{thinner * args.ct_val * result['mid']:.0f} |")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

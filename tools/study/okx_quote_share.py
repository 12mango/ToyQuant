#!/usr/bin/env python3
"""How much of the book one quote is: the two dimensionless ratios that say whether a size is invisible.

The capacity question -- "is one contract a measurement or a rounding error?" -- is asked with ratios, because dollars per
order mean nothing without the book and the flow beside them:

    order share = one order's USD notional / the time-weighted USD resting at the level we quote
    flow share  = our fills' USD notional / the market's traded USD over the same window

Both come from the day archive (the book) and the trade tape (the flow), so a size ladder can be read against one scale.
A quote is *invisible* at a few per cent of the level it joins; where that stops is what no replay can answer, and these two
ratios are what the live stage watches instead.

    python3 tools/study/okx_quote_share.py --days 2026-11-16,...,2026-11-22 --quote-ticks 3
"""

import argparse
import csv
import json
import statistics
import sys
import tarfile
from datetime import datetime, timedelta, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "lib"))
from okx_data import depth_archive, trade_tape  # noqa: E402

QUANTITY_SCALE = 10_000_000  # engine units of one contract
CONTRACT_TOKENS = 10.0       # ctVal: one contract carries ten tokens


def window_bounds(day_text, start, end):
    day = datetime.strptime(day_text, "%Y-%m-%d").replace(tzinfo=timezone.utc)
    hour, minute = (int(part) for part in start.split(":"))
    start_ms = int(day.replace(hour=hour, minute=minute).timestamp() * 1000)
    hour, minute = (int(part) for part in end.split(":"))
    return start_ms, int(day.replace(hour=hour, minute=minute).timestamp() * 1000)


def tape_name(day_text, start_ms):
    """The tape whose UTC+8 day covers `start_ms`.

    OKX cuts its trade archive on midnight UTC+8 while the depth archive is cut on UTC, so the file named D holds
    [D-1 16:00, D 16:00) and a window starting at or after 16:00 lives in D+1. Reading file D for an evening window gave
    two hours of book against zero trades in the sweep's own history, so the same rule is applied here rather than
    assumed away by a window that happens to be in the morning.
    """
    day = datetime.strptime(day_text, "%Y-%m-%d").replace(tzinfo=timezone.utc)
    sixteen = int(day.replace(hour=16).timestamp() * 1000)
    return day_text if start_ms < sixteen else (day + timedelta(days=1)).strftime("%Y-%m-%d")


def traded_notional(symbol, day_text, start_ms, end_ms, root):
    """The market's USD turnover inside the window, from the day's tape."""
    path = trade_tape(root, symbol, tape_name(day_text, start_ms))
    if path is None:
        return None
    total = 0.0
    with Path(path).open(newline="") as handle:
        for row in csv.DictReader(line for line in handle if not line.startswith("#")):
            try:
                if not start_ms <= int(row["created_time"]) < end_ms:
                    continue
                total += float(row["price"]) * float(row["size"]) * CONTRACT_TOKENS
            except (KeyError, TypeError, ValueError):
                continue
    return total


def depth_at_distance(archive, start_ms, end_ms, quote_ticks, tick):
    """Time-weighted USD resting `quote_ticks` beyond each touch, and the touch and mid for scale.

    The book is rebuilt from the archive's own stream, so the number is the archive's rather than a cut's: a level's size
    is sampled once a record and weighted by the time until the next one, which is the same treatment the half-spread
    profile uses and keeps a quiet hour from counting as much as a busy one.
    """
    book = {"bid": {}, "ask": {}}
    best = {"bid": None, "ask": None}
    sum_distance = {"bid": 0.0, "ask": 0.0}
    sum_touch = {"bid": 0.0, "ask": 0.0}
    covered = 0.0
    last = None
    mid = None
    with tarfile.open(archive) as handle:
        member = next((item for item in handle.getmembers() if item.name.endswith(".data")), None)
        if member is None:
            raise SystemExit(f"{archive}: no .data member")
        for line in handle.extractfile(member):
            if not line.startswith(b"{"):
                continue
            record = json.loads(line)
            ts = int(record["ts"])
            for key, side in (("bids", "bid"), ("asks", "ask")):
                for price, size, *_ in record.get(key, []):
                    price, size = float(price), float(size)
                    if size == 0.0:
                        book[side].pop(price, None)
                    else:
                        book[side][price] = size
            for side, pick in (("bid", max), ("ask", min)):
                if not book[side]:
                    best[side] = None
                elif best[side] is None or best[side] not in book[side]:
                    best[side] = pick(book[side])
            if ts < start_ms or ts >= end_ms:
                last = ts
                continue
            if last is not None and ts > last:
                span = (ts - last) / 1000.0
                covered += span
                for side in ("bid", "ask"):
                    touch = best[side]
                    if touch is None:
                        continue
                    sum_touch[side] += span * book[side].get(touch, 0.0) * CONTRACT_TOKENS * touch
                    level = touch - quote_ticks * tick if side == "bid" else touch + quote_ticks * tick
                    sum_distance[side] += span * book[side].get(round(level, 10), 0.0) * CONTRACT_TOKENS * level
            if best["bid"] is not None and best["ask"] is not None:
                mid = 0.5 * (best["bid"] + best["ask"])
            last = ts
    if covered <= 0.0:
        return None
    return {"distance_usd": 0.5 * (sum_distance["bid"] + sum_distance["ask"]) / covered,
            "touch_usd": 0.5 * (sum_touch["bid"] + sum_touch["ask"]) / covered, "mid": mid}


def fills_notional(fills_path):
    """Our own fills' USD notional, from the trade stream a replay writes."""
    if fills_path is None:
        return None
    total = 0.0
    with Path(fills_path).open(newline="") as handle:
        lines = [line for line in handle if not line.startswith("#")]
    for row in csv.DictReader(lines):
        try:
            total += float(row["price"]) * float(row["quantity"]) / QUANTITY_SCALE * CONTRACT_TOKENS
        except (KeyError, TypeError, ValueError):
            continue
    return total


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--days", required=True, help="comma separated UTC days, as the archive names them")
    parser.add_argument("--symbol", default="UP-USDT-SWAP")
    parser.add_argument("--root", default="data/v2/crypto")
    parser.add_argument("--start", default="13:30")
    parser.add_argument("--end", default="15:30")
    parser.add_argument("--quote-ticks", type=float, default=3.0, help="how far behind the touch the quote rests")
    parser.add_argument("--tick", type=float, default=0.0001)
    parser.add_argument("--order-units", type=int, default=10_000_000,
                        help="one order in engine units, 1e7 being one contract")
    parser.add_argument("--fills", help="a run's own trade stream, for the flow ratio")
    args = parser.parse_args()

    our_fills = fills_notional(args.fills)
    rows = []
    for day_text in [part.strip() for part in args.days.split(",") if part.strip()]:
        archive = depth_archive(ROOT / args.root, args.symbol, day_text)
        if archive is None:
            print(f"# {day_text}: no depth archive")
            continue
        start_ms, end_ms = window_bounds(day_text, args.start, args.end)
        depth = depth_at_distance(archive, start_ms, end_ms, args.quote_ticks, args.tick)
        if depth is None or not depth["mid"]:
            print(f"# {day_text}: no two-sided book inside the window")
            continue
        traded = traded_notional(args.symbol, day_text, start_ms, end_ms, ROOT / args.root)
        order_usd = args.order_units / QUANTITY_SCALE * CONTRACT_TOKENS * depth["mid"]
        rows.append({"day": day_text, "traded": traded, "distance": depth["distance_usd"],
                     "touch": depth["touch_usd"], "order": order_usd})
        print(f"# {day_text}: mid={depth['mid']:.5f} traded={traded or 0:,.0f} USD "
              f"level={depth['distance_usd']:,.2f} USD touch={depth['touch_usd']:,.2f} USD "
              f"order={order_usd:.2f} USD", flush=True)
    if not rows:
        print("no rows: nothing measured")
        return 1

    print()
    print(f"| day | market traded USD | depth {args.quote_ticks:g} ticks out, USD | at the touch, USD |"
          " one order USD | order / level | our fills / traded |")
    print("|---|---:|---:|---:|---:|---:|---:|")
    for row in rows:
        order_share = row["order"] / row["distance"] * 100.0 if row["distance"] else float("nan")
        if our_fills is not None and row["traded"]:
            flow_share = f"{our_fills / row['traded'] * 100.0:.4f}%"
        else:
            flow_share = "n/a"
        print(f"| {row['day']} | {row['traded'] or 0:,.0f} | {row['distance']:,.2f} | {row['touch']:,.2f} | "
              f"{row['order']:.2f} | {order_share:.2f}% | {flow_share} |")
    print()
    print(f"medians: market traded {statistics.median(r['traded'] or 0 for r in rows):,.0f} USD, "
          f"depth at {args.quote_ticks:g} ticks {statistics.median(r['distance'] for r in rows):,.2f} USD, "
          f"one order {statistics.median(r['order'] for r in rows):.2f} USD")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

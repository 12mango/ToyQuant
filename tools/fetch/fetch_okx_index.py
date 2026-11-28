#!/usr/bin/env python3
"""Fetch OKX's index series for one instrument, as 1-minute bars, one day or a window.

**Why the index and not a spot book.** The perp-index basis needs the venue's own index -- the number the swap's mark
price is built from -- and the exchange lists no spot book for this instrument, which is why the roadmap's row read "data
availability unverified" until 2026-11-28. The index exists as a series of its own:

    https://www.okx.com/api/v5/market/index-candles?instId=UP-USDT&bar=1m&limit=100            (recent)
    https://www.okx.com/api/v5/market/history-index-candles?instId=UP-USDT&bar=1m&after=<ms>   (older)

measured 2026-11-28: both answer `code 0`, the live bar read 0.1997 to 0.2001, and a bar thirty days back read 0.1388 --
so the history is served at least a month deep, without a key. A bar is `[ts, open, high, low, close, confirm]` and the
endpoint pages backwards from `after`, at most a hundred bars a call, which is why a day is fifteen calls and a window of
two months about a quarter of an hour at the default pause.

**What a fetched day must be before it is kept.** Every stamp is milliseconds inside the named UTC day and every close is
positive; a day with no bars is named and written as a header only, never as zeros, because an absent bar is an absent
bar (`docs/TOOLS.md`, convention 3).

Usage:
    python3 tools/fetch/fetch_okx_index.py --symbol UP-USDT --date 2026-11-26
    python3 tools/fetch/fetch_okx_index.py --symbol UP-USDT --from 2026-09-28 --to 2026-11-26 --skip-existing
"""
import argparse
import json
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timedelta, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
URLS = ("https://www.okx.com/api/v5/market/history-index-candles",
        "https://www.okx.com/api/v5/market/index-candles")
HEADERS = {"User-Agent": "Mozilla/5.0 (X11; Linux x86_64) toyquant-history/1.0"}
FIELDS = ["ts", "open", "high", "low", "close"]


def day_range(first, last):
    start = datetime.strptime(first, "%Y-%m-%d").replace(tzinfo=timezone.utc)
    end = datetime.strptime(last, "%Y-%m-%d").replace(tzinfo=timezone.utc)
    days = []
    while start <= end:
        days.append(start.strftime("%Y-%m-%d"))
        start += timedelta(days=1)
    return days


def page(inst_id, bar, after_ms, tries=4):
    """One page of bars older than `after_ms`: a list (possibly empty) when an endpoint answered, or None when neither did.

    Empty is an answer -- the history has an end, and a day before the series is a day with no bars rather than a failure.
    A refusal is retried with a backoff, because measured 2026-11-28 a window of sixty days lost eleven days to transient
    refusals while the same stamps answered by hand a minute later; a tool that calls those "FAILED" makes a gap in the
    history and a rate limit look the same.
    """
    for attempt in range(tries):
        for url in URLS:
            query = f"?instId={inst_id}&bar={bar}&limit=100&after={after_ms}"
            request = urllib.request.Request(url + query, headers=HEADERS)
            try:
                with urllib.request.urlopen(request, timeout=30) as answer:
                    payload = json.loads(answer.read().decode())
            except (urllib.error.URLError, json.JSONDecodeError, TimeoutError):
                continue
            if payload.get("code") == "0":
                return [row for row in payload.get("data") or [] if len(row) >= 5]
        time.sleep(1.0 + attempt)
    return None


def fetch_day(inst_id, bar, day, pause):
    start = datetime.strptime(day, "%Y-%m-%d").replace(tzinfo=timezone.utc)
    start_ms = int(start.timestamp() * 1000)
    end_ms = start_ms + 86_400_000
    bars, cursor = {}, end_ms
    while True:
        rows = page(inst_id, bar, cursor)
        if rows is None:
            return None
        if not rows:
            break
        fresh = 0
        for row in rows:
            stamp = int(row[0])
            if start_ms <= stamp < end_ms:
                bars[stamp] = row
                fresh += 1
        oldest = min(int(row[0]) for row in rows)
        if oldest <= start_ms or not fresh:
            break
        cursor = oldest
        time.sleep(pause)
    return [bars[stamp] for stamp in sorted(bars)]


def write_day(path, bars):
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(",".join(FIELDS) + "\n")
        for row in bars:
            handle.write(",".join(row[:5]) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--symbol", required=True, help="the index's instrument, e.g. UP-USDT")
    parser.add_argument("--date", help="one day, YYYY-MM-DD (UTC)")
    parser.add_argument("--from", dest="first", metavar="YYYY-MM-DD", help="first day of a window")
    parser.add_argument("--to", dest="last", metavar="YYYY-MM-DD", help="last day of a window, inclusive")
    parser.add_argument("--bar", default="1m", help="candle size, OKX's own name (default 1m)")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "data/v2/crypto/index")
    parser.add_argument("--skip-existing", action="store_true", help="leave a day that is already on disk alone")
    parser.add_argument("--pause", type=float, default=0.25, help="seconds between calls")
    args = parser.parse_args()

    if args.date:
        days = [args.date]
    elif args.first and args.last:
        days = day_range(args.first, args.last)
    else:
        parser.error("name a --date, or a --from and a --to")

    failures = 0
    for day in days:
        path = args.output_dir / f"{args.symbol}.{args.bar}.{day}.csv"
        if args.skip_existing and path.exists() and path.stat().st_size > 0:
            print(f"{day}: already here, skipped", flush=True)
            continue
        bars = fetch_day(args.symbol, args.bar, day, args.pause)
        if bars is None:
            print(f"{day}: FAILED -- both endpoints refused the page", flush=True)
            failures += 1
            continue
        if not bars:
            print(f"{day}: no bar for that day (written as a header only)", flush=True)
        write_day(path, bars)
        print(f"{day}: bars={len(bars)}", flush=True)
        time.sleep(args.pause)
    print(f"window={args.first}..{args.last} days={len(days)} failed={failures}", flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

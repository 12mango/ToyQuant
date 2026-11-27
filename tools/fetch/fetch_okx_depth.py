#!/usr/bin/env python3
"""Fetch OKX's 400-level order-book archive, one instrument, one day or a window.

**The URL, and why it is written here.** The public history endpoint serves no book module
(`tools/fetch/fetch_okx_trades.py`, and `docs/DATA.md` says the same), and eight CDN paths guessed from the *trade*
URL answered 404 -- so the dataset looked browser-only. It is not: the URL the portal's own page asked for is this
shape, measured 2026-11-27,

    https://static.okx.com/cdn/okx/match/orderbook/pro/L2/400lv/daily/<YYYYMMDD>/<SYM>-L2orderbook-400lv-<YYYY-MM-DD>.tar.gz

and the four days tried against it answered 200 with 4.3-28.7 MB. A day that precedes the instrument's listing answers
404, which is a day to name rather than a file to invent.

**What a fetched file must be before it is kept.** The archive is a `tar.gz` with one `.data` member of JSON lines:
the first is a full `snapshot`, the rest are `update`s, and the day is cut on midnight **UTC** (unlike the trade
tape's UTC+8). This tool refuses a file whose member is not there, whose first record is not a snapshot for the symbol
it asked for, or whose first stamp falls outside the named day -- the three properties
`tools/fetch/okx_depth_align.py` and `tools/study/check_okx_book.py` then read in depth.

Usage:
    python3 tools/fetch/fetch_okx_depth.py --symbol UP-USDT-SWAP --date 2026-11-15
    python3 tools/fetch/fetch_okx_depth.py --symbol UP-USDT-SWAP --from 2026-10-30 --to 2026-11-26 --skip-existing
"""
import argparse
import gzip
import io
import json
import sys
import tarfile
import time
import urllib.error
import urllib.request
from datetime import datetime, timedelta, timezone
from pathlib import Path

URL = ("https://static.okx.com/cdn/okx/match/orderbook/pro/L2/400lv/daily/{stamp}/"
       "{symbol}-L2orderbook-400lv-{day}.tar.gz")
HEADERS = {"User-Agent": "Mozilla/5.0 (X11; Linux x86_64) toyquant-history/1.0"}


def url_for(symbol, day):
    return URL.format(stamp=day.replace("-", ""), symbol=symbol, day=day)


def day_range(first, last):
    start = datetime.strptime(first, "%Y-%m-%d").date()
    end = datetime.strptime(last, "%Y-%m-%d").date()
    if end < start:
        raise ValueError(f"--from {first} is after --to {last}")
    return [(start + timedelta(days=offset)).isoformat() for offset in range((end - start).days + 1)]


def download(url, path):
    """Stream to a `.part` beside the target, so a killed fetch never leaves a file that looks like data.

    The read timeout is short for the reason `fetch_okx_trades.py` records: a CDN read that stalls does not raise, so a
    prefetch can sit behind one chunk for hours unless the timeout is shorter than the patience of the run.
    """
    partial = path.with_suffix(path.suffix + ".part")
    with urllib.request.urlopen(urllib.request.Request(url, headers=HEADERS), timeout=120) as response:
        with partial.open("wb") as target:
            while True:
                chunk = response.read(1 << 20)
                if not chunk:
                    break
                target.write(chunk)
    partial.replace(path)


def check_archive(path, symbol, day):
    """The three properties that make the file usable, each with the reason it failed."""
    try:
        with tarfile.open(path, "r:gz") as archive:
            members = [name for name in archive.getnames() if name.endswith(".data")]
            if len(members) != 1:
                raise RuntimeError(f"expected one .data member, found {len(members)}")
            source = archive.extractfile(members[0])
            if source is None:
                raise RuntimeError(f"member {members[0]} is not readable")
            line = io.TextIOWrapper(source, encoding="utf-8", errors="replace").readline()
    except (tarfile.TarError, OSError, gzip.BadGzipFile) as error:
        raise RuntimeError(f"not a readable tar.gz: {error}") from error
    try:
        record = json.loads(line)
    except json.JSONDecodeError as error:
        raise RuntimeError(f"first record is not JSON: {error}") from error
    if record.get("action") != "snapshot":
        raise RuntimeError(f"first record is a {record.get('action')!r}, not a snapshot")
    if record.get("instId") != symbol:
        raise RuntimeError(f"first record is for {record.get('instId')!r}, not {symbol}")
    start = datetime.strptime(day, "%Y-%m-%d").replace(tzinfo=timezone.utc)
    stamp = int(record.get("ts", 0)) / 1000
    if not start.timestamp() <= stamp < (start + timedelta(days=1)).timestamp():
        raise RuntimeError(f"first record is stamped {datetime.fromtimestamp(stamp, timezone.utc):%Y-%m-%d %H:%M:%S}Z, "
                           f"outside {day} (the book day is cut on midnight UTC)")
    return len(record.get("asks") or []) + len(record.get("bids") or [])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--symbol", required=True, help="for example UP-USDT-SWAP")
    parser.add_argument("--date", help="one day, YYYY-MM-DD (the book day is UTC)")
    parser.add_argument("--from", dest="first", metavar="YYYY-MM-DD", help="first day of a window")
    parser.add_argument("--to", dest="last", metavar="YYYY-MM-DD", help="last day of a window, inclusive")
    parser.add_argument("--output-dir", type=Path, help="defaults to data/v2/crypto/<SYMBOL>")
    parser.add_argument("--skip-existing", action="store_true", help="leave a day that is already on disk alone")
    parser.add_argument("--pause", type=float, default=1.0, help="seconds between days")
    args = parser.parse_args()
    if args.date and (args.first or args.last):
        parser.error("--date and --from/--to are two shapes of one run; pick one")
    if not args.date and not (args.first and args.last):
        parser.error("give --date, or both --from and --to")
    try:
        days = [args.date] if args.date else day_range(args.first, args.last)
    except ValueError as error:
        parser.error(str(error))
    folder = args.output_dir or Path("data/v2/crypto") / args.symbol
    folder.mkdir(parents=True, exist_ok=True)

    fetched, skipped, failed = [], [], []
    for index, day in enumerate(days):
        target = folder / f"{args.symbol}-L2orderbook-400lv-{day}.tar.gz"
        if args.skip_existing and target.exists() and target.stat().st_size > 0:
            print(f"{day}: {target.name} already here, skipped")
            skipped.append(day)
        else:
            url = url_for(args.symbol, day)
            try:
                download(url, target)
                levels = check_archive(target, args.symbol, day)
                print(f"{day}: {target.name} bytes={target.stat().st_size} opening_levels={levels}", flush=True)
                fetched.append((day, target.stat().st_size))
            except urllib.error.HTTPError as error:
                # A day before the listing is a 404, and it is named rather than stored as an empty file.
                print(f"{day}: HTTP {error.code} for {url}", file=sys.stderr)
                target.unlink(missing_ok=True)
                failed.append(day)
            except (RuntimeError, OSError, urllib.error.URLError) as error:
                print(f"{day}: FAILED {error}", file=sys.stderr)
                target.unlink(missing_ok=True)
                failed.append(day)
        if index + 1 < len(days) and args.pause:
            time.sleep(args.pause)

    if len(days) > 1:
        kept = sum(size for _, size in fetched)
        print(f"window={days[0]}..{days[-1]} days={len(days)} fetched={len(fetched)} skipped={len(skipped)} "
              f"failed={len(failed)} kept_bytes={kept}")
        if failed:
            print(f"failed days: {', '.join(failed)}", file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Fetch OKX tick trades from the public history endpoint, one day or a window, and slice one symbol out.

Answers what the public route can and cannot do, for the second venue this repository is heading towards:

* `module=1` is trades, `2` candles, `3` funding rates, `11` margin borrow rates, and the endpoint answers
  without an API key. There is no order book module; that dataset is only offered by the web portal.
* **A request covers at most seven days**: measured 2026-11-27, `begin`/`end` spans of 2 and 7 days answer `code=0`,
  14 and 28 days answer HTTP 400, so a month is four windowed requests or twenty-eight single-day ones.
* **The file is the whole venue, not the instrument**: one swap day is `allswap-trades-<day>.zip`, **333.87 MB** on
  2026-11-26 (it was about 70 MB when this tool was written), so a 28-day window moves ~9.4 GB to keep ~0.3 GB of one
  symbol. This streams the member and writes only the requested symbol, and deletes each zip after slicing it unless
  `--keep-zip` says otherwise.
* **The daily file is cut on the UTC+8 day**, so the file named 2023-03-01 starts at 2023-02-28 16:00 UTC. That is
  the kind of fact that turns into a silently shifted day, so it is written here and printed on every run.
* Columns are `instrument_name,trade_id,side,price,size,created_time`, the timestamp is milliseconds, and `size`
  is in contracts, so a linear swap of contract value 0.01 needs that multiplication before it is a base amount.

Usage:
    python3 tools/fetch/fetch_okx_trades.py --date 2023-03-01 --symbol BTC-USDT-SWAP --output data/v2/crypto/okx_...
    python3 tools/fetch/fetch_okx_trades.py --date 2023-03-01 --symbol BTC-USDT-SWAP --zip /tmp/already-downloaded.zip
    python3 tools/fetch/fetch_okx_trades.py --from 2026-10-30 --to 2026-11-26 --symbol UP-USDT-SWAP --skip-existing
"""

import argparse
import csv
import io
import json
import sys
import time
import urllib.parse
import urllib.request
import zipfile
from datetime import datetime, timedelta, timezone
from pathlib import Path

HISTORY_URL = "https://www.okx.com/api/v5/public/market-data-history"
# The endpoint answers a plain HTTP client but refuses the bare Python user agent, so it is set explicitly.
HEADERS = {"User-Agent": "Mozilla/5.0 (X11; Linux x86_64) toyquant-history/1.0"}
# The file day is UTC+8, which is eight hours ahead of the timestamps inside it.
FILE_DAY_OFFSET = timedelta(hours=8)


def instrument_type(symbol):
    if symbol.endswith("-SWAP"):
        return "SWAP"
    if symbol.count("-") == 1:
        return "SPOT"
    raise ValueError(f"cannot tell the OKX instrument type of {symbol!r}")


def day_bounds(date):
    """Start and end of the file day in milliseconds, which is midnight UTC+8 rather than UTC."""
    start = datetime.strptime(date, "%Y-%m-%d").replace(tzinfo=timezone.utc) - FILE_DAY_OFFSET
    return int(start.timestamp() * 1000), int((start + timedelta(days=1)).timestamp() * 1000)


def find_url(symbol, start_ms, end_ms):
    query = urllib.parse.urlencode(
        {
            "module": 1,
            "instType": instrument_type(symbol),
            "instId": symbol,
            "dateAggrType": "daily",
            "begin": start_ms,
            "end": end_ms,
        }
    )
    with urllib.request.urlopen(urllib.request.Request(f"{HISTORY_URL}?{query}", headers=HEADERS),
                                timeout=60) as response:
        payload = json.load(response)
    if payload.get("code") != "0":
        raise RuntimeError(f"the history endpoint said {payload.get('code')} {payload.get('msg')!r}")
    for entry in payload.get("data", []):
        for detail in entry.get("details", []):
            # The range starts live on the detail, while each file inside it is named by dateTs.
            if int(detail.get("dateRangeStart", 0)) != start_ms:
                continue
            groups = detail.get("groupDetails", [])
            if not groups:
                raise RuntimeError("the archive lists that day but returned no file for it")
            # The groups come back newest first, so the day is found by its stamp rather than by position.
            for group in groups:
                if int(group.get("dateTs", 0)) == start_ms:
                    return group["url"], group.get("filename", "")
            raise RuntimeError(
                f"the archive lists {len(groups)} file(s) for that range but none starts at the requested day")
    raise RuntimeError("no daily file for that date; the archive starts in September 2021 for trades")


def download(url, path):
    with urllib.request.urlopen(urllib.request.Request(url, headers=HEADERS), timeout=600) as response:
        with path.open("wb") as target:
            while True:
                chunk = response.read(1 << 20)
                if not chunk:
                    break
                target.write(chunk)


def is_readable_zip(path):
    """A killed download leaves a partial file that exists and is not a zip, so it is checked before use."""
    try:
        with zipfile.ZipFile(path) as archive:
            return bool(archive.namelist())
    except (OSError, zipfile.BadZipFile):
        return False


def slice_symbol(archive_path, symbol, output, max_rows):
    """Stream the single member and keep one symbol, so a 600 MB CSV is never unpacked to disk."""
    written = 0
    first_ts = None
    last_ts = None
    with zipfile.ZipFile(archive_path) as archive:
        members = [name for name in archive.namelist() if name.endswith(".csv")]
        if len(members) != 1:
            raise RuntimeError(f"expected one CSV member, found {len(members)}")
        with archive.open(members[0]) as source:
            text = io.TextIOWrapper(source, encoding="utf-8", errors="replace", newline="")
            reader = csv.reader(text)
            header = next(reader)[:6]
            if header != ["instrument_name", "trade_id", "side", "price", "size", "created_time"]:
                raise RuntimeError(f"unexpected columns: {header}")
            output.parent.mkdir(parents=True, exist_ok=True)
            with output.open("w", newline="", encoding="utf-8") as target:
                writer = csv.writer(target)
                writer.writerow(header)
                for row in reader:
                    if len(row) < 6 or row[0] != symbol:
                        continue
                    writer.writerow(row[:6])
                    written += 1
                    stamp = int(row[5])
                    first_ts = stamp if first_ts is None else first_ts
                    last_ts = stamp
                    if max_rows is not None and written >= max_rows:
                        break
    return written, first_ts, last_ts


def collect_day(date, symbol, output, max_rows, keep_zip):
    """One file day: find the URL, download the venue-wide zip if it is not here, slice the symbol, drop the zip.

    The zip is the whole venue (333.87 MB for 2026-11-26), so it is deleted after slicing unless `keep_zip`: a 28-day
    window would otherwise leave ~9.4 GB in `/tmp`.
    """
    url, filename = find_url(symbol, *day_bounds(date))
    archive = Path("/tmp") / filename
    if archive.exists() and not is_readable_zip(archive):
        print(f"discarding an incomplete download: {archive}")
        archive.unlink()
    downloaded = not archive.exists()
    if downloaded:
        download(url, archive)
    if not is_readable_zip(archive):
        raise RuntimeError(f"the download is not a readable zip: {archive}")
    zip_bytes = archive.stat().st_size
    try:
        written, first_ts, last_ts = slice_symbol(archive, symbol, output, max_rows)
    finally:
        if not keep_zip:
            archive.unlink(missing_ok=True)
    return {"date": date, "rows": written, "zip_bytes": zip_bytes, "downloaded": downloaded,
            "first_ts": first_ts, "last_ts": last_ts, "output": output}


def day_range(first, last):
    """Every file day from `first` to `last`, inclusive, as ISO dates."""
    start = datetime.strptime(first, "%Y-%m-%d").date()
    end = datetime.strptime(last, "%Y-%m-%d").date()
    if end < start:
        raise ValueError(f"--from {first} is after --to {last}")
    return [(start + timedelta(days=offset)).isoformat() for offset in range((end - start).days + 1)]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--date", help="one file day, for example 2023-03-01, which is midnight UTC+8")
    parser.add_argument("--from", dest="first", metavar="YYYY-MM-DD", help="first file day of a window")
    parser.add_argument("--to", dest="last", metavar="YYYY-MM-DD", help="last file day of a window, inclusive")
    parser.add_argument("--symbol", required=True, help="for example BTC-USDT-SWAP or BTC-USDT")
    parser.add_argument("--output", type=Path, help="defaults to data/v2/crypto/okx_trades_DATE_SYMBOL.csv")
    parser.add_argument("--output-dir", type=Path, default=Path("data/v2/crypto"),
                        help="where a window's files go, one per day (the per-instrument folder layout)")
    parser.add_argument("--zip", type=Path, dest="zip_path", help="reuse a zip already on disk (one day only)")
    parser.add_argument("--max-rows", type=int, help="stop after this many matching rows per day")
    parser.add_argument("--skip-existing", action="store_true",
                        help="leave a day whose output already has rows alone, so a window can be resumed")
    parser.add_argument("--keep-zip", action="store_true", help="do not delete the venue-wide zip after slicing")
    parser.add_argument("--pause", type=float, default=3.0,
                        help="seconds between days: a rapid loop answered HTTP 429 (measured 2026-11-27)")
    args = parser.parse_args()
    if args.max_rows is not None and args.max_rows <= 0:
        parser.error("--max-rows must be positive")
    if args.date and (args.first or args.last):
        parser.error("--date and --from/--to are two shapes of one run; pick one")
    if not args.date and not (args.first and args.last):
        parser.error("give --date, or both --from and --to")
    if args.zip_path and args.first:
        parser.error("--zip reuses one day's archive, so it only goes with --date")
    try:
        days = [args.date] if args.date else day_range(args.first, args.last)
    except ValueError as error:
        parser.error(str(error))

    done, skipped, empty, failed = [], [], [], []
    for index, date in enumerate(days):
        output = (args.output if args.output and len(days) == 1
                  else args.output_dir / f"okx_trades_{date}_{args.symbol}.csv")
        if args.skip_existing and output.exists() and output.stat().st_size > 0:
            print(f"{date}: {output} already has rows, skipped")
            skipped.append(date)
        else:
            try:
                if args.zip_path:
                    archive = args.zip_path
                    if not is_readable_zip(archive):
                        raise RuntimeError(f"the download is not a readable zip: {archive}")
                    written, first_ts, last_ts = slice_symbol(archive, args.symbol, output, args.max_rows)
                    info = {"date": date, "rows": written, "zip_bytes": archive.stat().st_size,
                            "downloaded": False, "first_ts": first_ts, "last_ts": last_ts, "output": output}
                else:
                    info = collect_day(date, args.symbol, output, args.max_rows, args.keep_zip)
                print(f"{date}: rows={info['rows']} zip={info['zip_bytes']}B "
                      f"{'downloaded' if info['downloaded'] else 'reused'} output={info['output']} "
                      f"({info['output'].stat().st_size}B)", flush=True)
                (done if info["rows"] else empty).append(info)
            except (OSError, ValueError, RuntimeError, KeyError) as error:
                # A day that cannot be fetched is named and counted; it never becomes a silent zero.
                print(f"{date}: FAILED {error}", file=sys.stderr)
                failed.append((date, str(error)))
        if len(days) > 1 and index + 1 < len(days) and args.pause:
            time.sleep(args.pause)

    if len(days) == 1 and done:
        info = done[0]
        day_start = datetime.strptime(days[0], "%Y-%m-%d").replace(tzinfo=timezone.utc) - FILE_DAY_OFFSET
        print(f"file_day={days[0]} starts at {day_start:%Y-%m-%d %H:%M} UTC, because the file day is UTC+8")
        if info["first_ts"] is not None:
            first = datetime.fromtimestamp(info["first_ts"] / 1000, tz=timezone.utc)
            last = datetime.fromtimestamp(info["last_ts"] / 1000, tz=timezone.utc)
            print(f"timestamp_unit=milliseconds range={first:%Y-%m-%d %H:%M:%S.%f} .. {last:%Y-%m-%d %H:%M:%S.%f} UTC")
    else:
        rows = sum(info["rows"] for info in done)
        kept = sum(info["output"].stat().st_size for info in done)
        moved = sum(info["zip_bytes"] for info in done if info["downloaded"])
        print(f"window={days[0]}..{days[-1]} days={len(days)} fetched={len(done)} skipped={len(skipped)} "
              f"empty={len(empty)} failed={len(failed)} rows={rows} kept_bytes={kept} "
              f"downloaded_bytes={moved}")
        if empty:
            print(f"empty days (no trade for the symbol in that file day): "
                  f"{', '.join(info['date'] for info in empty)}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())


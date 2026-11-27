#!/usr/bin/env python3
"""Classify what is under `data/`, and say which files are unusable rather than guessing by size.

`data/` is input -- nothing here rewrites a file, re-sorts one or "cleans" one, because a number that was measured from a
file must keep meaning what it meant. What it does is *classify*: every file is a symbol, a kind (400-level depth archive,
trade tape, or something else) and a day, and the classes are reported per symbol with their sizes, so an operator can see
what a 16 GB tree actually holds. `--prune` is the only mode that deletes, it requires an explicit rule, and it prints
every path it removes.

The findings are the ones that cost time when they are discovered by a failed run instead of by this report:

  * **partial** -- a zero-byte or implausibly small file, which is a download that was interrupted and looks like data
  * **orphan-tape / orphan-depth** -- a day with one of the two files and not the other: the replay needs both, and which
    one is missing is the first thing to know
  * **shadow** -- the same symbol, kind and day in two layouts (the flat root and the per-symbol folder), which
    `tools/lib/okx_data.py` accepts on purpose and a reader should still be told about
  * **corrupt** -- an archive whose gzip header does not read (`--check-archives`, off by default because it walks the file)
  * **old** -- a day older than `--keep-days`, listed as a candidate for archival rather than deleted

    python3 tools/fetch/data_inventory.py                        # the report
    python3 tools/fetch/data_inventory.py --prune partial        # remove interrupted downloads only
    python3 tools/fetch/data_inventory.py --prune corrupt --check-archives
"""
import argparse
import gzip
import re
import sys
from collections import defaultdict
from datetime import datetime
from pathlib import Path

KINDS = (("depth", re.compile(r"^(?P<symbol>.+)-L2orderbook-400lv-(?P<day>\d{4}-\d{2}-\d{2})\.tar\.gz$")),
         ("tape", re.compile(r"^okx_trades_(?P<day>\d{4}-\d{2}-\d{2})_(?P<symbol>.+)\.csv$")))
SMALL_BYTES = 1 << 16  # a 64 KB day is not a day; every real file in this tree is megabytes


def classify(path):
    """(kind, symbol, day) for a file this tool knows, else ('other', None, None)."""
    for kind, pattern in KINDS:
        match = pattern.match(path.name)
        if match:
            return kind, match.group("symbol"), match.group("day")
    return "other", None, None


def walk(root):
    """Every file under the root, with its layout (flat or per-symbol) and its classified identity."""
    files = []
    for path in sorted(Path(root).rglob("*")):
        if not path.is_file():
            continue
        kind, symbol, day = classify(path)
        layout = "flat" if path.parent == Path(root) else path.parent.name
        files.append(dict(path=path, size=path.stat().st_size, kind=kind, symbol=symbol, day=day, layout=layout))
    return files


def report(files, keep_days, check_archives, newest_day, small_bytes=SMALL_BYTES):
    """The classification, the per-symbol table, and the findings -- nothing hidden behind a threshold."""
    by_symbol = defaultdict(lambda: defaultdict(int))
    findings = []
    seen = {}
    for entry in files:
        if entry["kind"] == "other":
            findings.append(("other", entry["path"], "not a depth archive or a trade tape"))
            continue
        by_symbol[entry["symbol"]][entry["kind"]] += 1
        if entry["size"] == 0 or entry["size"] < small_bytes:
            findings.append(("partial", entry["path"], f"{entry['size']} bytes"))
        if check_archives and entry["kind"] == "depth":
            try:
                with gzip.open(entry["path"], "rb") as handle:
                    handle.read(512)
            except OSError as error:
                findings.append(("corrupt", entry["path"], str(error)))
        key = (entry["symbol"], entry["kind"], entry["day"])
        if key in seen:
            findings.append(("shadow", entry["path"], f"the same day is also at {seen[key]}"))
        else:
            seen[key] = entry["path"]
    for (symbol, kind, day), path in sorted(seen.items()):
        other = "depth" if kind == "tape" else "tape"
        if (symbol, other, day) not in seen:
            findings.append((f"orphan-{kind}", path, f"no {other} for this day"))
        if newest_day is not None and (newest_day - _day(day)).days > keep_days:
            findings.append(("old", path, f"{(_day(day) - _day(day)).days if False else ''}"
                                            f"older than {keep_days} days"))
    files_by_kind = defaultdict(int)
    bytes_by_kind = defaultdict(int)
    for entry in files:
        files_by_kind[entry["kind"]] += 1
        bytes_by_kind[entry["kind"]] += entry["size"]
    return by_symbol, findings, files_by_kind, bytes_by_kind


def printable(by_symbol, findings, files_by_kind, bytes_by_kind):
    lines = ["symbol                 depth  tapes   (files)"]
    for symbol in sorted(by_symbol):
        counts = by_symbol[symbol]
        lines.append(f"{symbol:22} {counts.get('depth', 0):>5}  {counts.get('tape', 0):>5}")
    lines.append("")
    for kind, count in sorted(files_by_kind.items()):
        lines.append(f"{kind:8} {count:>4} file(s)  {bytes_by_kind[kind] / 2**30:>8.2f} GiB")
    if findings:
        lines.append("")
        lines.append(f"{len(findings)} finding(s):")
        for rule, path, reason in findings:
            lines.append(f"  {rule:14} {path.name}  -- {reason}")
    else:
        lines.append("")
        lines.append("no findings: every day has both files, no partial, no shadow")
    return "\n".join(lines) + "\n"


PRUNABLE = ("partial", "corrupt", "shadow")  # never an orphan (evidence) and never `old` (archival is a decision)


def prune(findings, rule):
    """Delete only what the named rule flags, and print every path: a deletion nobody can read is not one."""
    removed, freed = 0, 0
    for kind, path, reason in findings:
        if kind != rule:
            continue
        size = path.stat().st_size
        path.unlink()
        print(f"pruned {kind:10} {path}  ({size / 2**20:.1f} MiB) -- {reason}")
        removed += 1
        freed += size
    print(f"pruned {removed} file(s), {freed / 2**30:.2f} GiB")
    return removed


def self_test():
    """Four findings and one clean pair, on a temporary tree this tool builds itself."""
    import tempfile
    with tempfile.TemporaryDirectory() as room:
        root = Path(room) / "crypto"
        folder = root / "AAA-USDT-SWAP"
        folder.mkdir(parents=True)
        # A real (tiny) gzip so `--check-archives` has something to read.
        with gzip.open(folder / "AAA-USDT-SWAP-L2orderbook-400lv-2026-01-01.tar.gz", "wb") as handle:
            handle.write(b"{}\n" * 4000)
        (folder / "okx_trades_2026-01-01_AAA-USDT-SWAP.csv").write_text("x" * 100000)
        # A zero-byte tape and an orphan depth for a second day.
        (folder / "okx_trades_2026-01-02_AAA-USDT-SWAP.csv").write_text("")
        with gzip.open(folder / "AAA-USDT-SWAP-L2orderbook-400lv-2026-01-03.tar.gz", "wb") as handle:
            handle.write(b"{}\n" * 4000)
        # The same day in the flat root as well: a shadow.
        with gzip.open(root / "AAA-USDT-SWAP-L2orderbook-400lv-2026-01-01.tar.gz", "wb") as handle:
            handle.write(b"{}\n" * 4000)
        files = walk(root)
        newest = max(_day(f["day"]) for f in files if f["day"])
        by_symbol, findings, _, _ = report(files, keep_days=36500, check_archives=True, newest_day=newest, small_bytes=0)
        rules = {kind for kind, _, _ in findings}
        assert {"partial", "orphan-depth", "shadow"} <= rules, rules
        assert by_symbol["AAA-USDT-SWAP"]["depth"] == 3 and by_symbol["AAA-USDT-SWAP"]["tape"] == 2, by_symbol
        assert printable(by_symbol, findings, {"depth": 3, "tape": 2}, {"depth": 1, "tape": 1})
        # A clean pair on its own reports nothing.
        clean = root / "BBB-USDT-SWAP"
        clean.mkdir()
        with gzip.open(clean / "BBB-USDT-SWAP-L2orderbook-400lv-2026-02-01.tar.gz", "wb") as handle:
            handle.write(b"{}\n" * 4000)
        (clean / "okx_trades_2026-02-01_BBB-USDT-SWAP.csv").write_text("x" * 100000)
        files = [f for f in walk(root) if f["symbol"] == "BBB-USDT-SWAP"]
        _, findings, _, _ = report(files, keep_days=36500, check_archives=True, newest_day=newest, small_bytes=0)
        assert not findings, findings
    print("data_inventory self-test OK (partial, orphan, shadow, corrupt-capable walk; a clean pair reports nothing)")
    return 0


def _day(text):
    """The day a file's name carries, as a date. Defined here rather than beside `classify` because Python resolves a
    module-level name at call time, and this file is read top to bottom by a person."""
    return datetime.strptime(text, "%Y-%m-%d").date()


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("root", nargs="?", default="data/v2/crypto", help="the tree to classify")
    parser.add_argument("--keep-days", type=int, default=120,
                        help="days newer than the newest file are kept; older ones are listed, never deleted")
    parser.add_argument("--check-archives", action="store_true",
                        help="read each archive's first block to catch a corrupt gzip (walks every file)")
    parser.add_argument("--prune", choices=PRUNABLE, help=f"delete the files one rule flags ({', '.join(PRUNABLE)})")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    files = walk(args.root)
    if not files:
        raise SystemExit(f"data_inventory: nothing under {args.root} -- a missing input is an error that names it")
    newest = max((_day(f["day"]) for f in files if f["day"]), default=None)
    by_symbol, findings, files_by_kind, bytes_by_kind = report(files, args.keep_days, args.check_archives, newest)
    print(printable(by_symbol, findings, files_by_kind, bytes_by_kind), end="")
    if args.prune:
        prune(findings, args.prune)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

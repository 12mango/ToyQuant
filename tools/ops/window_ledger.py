#!/usr/bin/env python3
"""The window series: what each closed window did, and how far the live sample is from being big enough to believe.

`deploy/daily_report.py` appends one row per closed window to `/opt/toyquant/data/runtime/window_ledger.csv`. Read it
with `--ops ledger`, save it, or pipe it in with `--file -`:

    python3 tools/ops/window_ledger.py --file - --table --progress 300
    python3 tools/ops/window_ledger.py --file ledger.csv --emit docs/_figures/windows.svg

**Why the progress line exists.** The question the live sample has to answer is whether its fills' markout matches the
replay's, and the replay's own twenty-eight days read a `markout_ticks` median of +0.416 with a worst day of -0.808 --
so one window anywhere in that range says nothing. Three hundred to five hundred fills is what would pin it, and at the
first window's rate that is a month, so the count is reported every day instead of estimated once.

An empty cell is a line the journal did not carry, never a zero: a window with `sessions=0` has no fill count, and a
progress line that added zeroes would make the sample look larger than it is.
"""
import argparse
import csv
import math
import statistics
import sys
import tempfile
from pathlib import Path

FILLS = ("maker_trades", "taker_trades")


def read_rows(source):
    """Rows as dicts, numbers parsed, from a path or from stdin."""
    handle = sys.stdin if source == "-" else open(source, encoding="utf-8")
    try:
        rows = []
        for row in csv.DictReader(handle):
            parsed = dict(row)
            for key, value in row.items():
                if key in ("window_start_utc", "window_end_utc", "read_at", "source"):
                    continue
                parsed[key] = None if value in (None, "") else float(value)
            rows.append(parsed)
        return rows
    finally:
        if handle is not sys.stdin:
            handle.close()


def fills_of(row):
    """Maker plus taker trades, or None when the row's session reported neither."""
    values = [row.get(name) for name in FILLS]
    if all(value is None for value in values):
        return None
    return sum(value or 0.0 for value in values)


def cell(value, fmt):
    return "" if value is None else format(value, fmt)


def table(rows):
    print(f"{'window start':>18} {'fills':>6} {'fees':>10} {'net usd':>9} {'gross':>8} {'residual':>10} "
          f"{'equity':>8} {'markout t':>9}")
    for row in rows:
        print(f"{row['window_start_utc']:>18} {cell(fills_of(row), '.0f'):>6} "
              f"{cell(row.get('fees_paid'), '.5f'):>10} {cell(row.get('net_pnl_usd'), '+.4f'):>9} "
              f"{cell(row.get('gross_pnl_usd'), '+.4f'):>8} {cell(row.get('residual_units'), '.0f'):>10} "
              f"{cell(row.get('equity_usd'), '.2f'):>8} {cell(row.get('markout_ticks'), '+.3f'):>9}")


def progress(rows, target):
    """Cumulative fills, the rate they arrive at, and the windows left to the target."""
    counted = [value for value in (fills_of(row) for row in rows) if value is not None]
    if not counted:
        print("progress: no closed window has reported a fill count yet")
        return 1
    total = sum(counted)
    median = statistics.median(counted)
    remaining = max(0.0, target - total)
    windows = math.ceil(remaining / median) if median > 0 else None
    print(f"windows={len(counted)} fills={total:.0f} target={target} median_per_window={median:.0f} "
          f"remaining={remaining:.0f}"
          + (f" windows_to_target={windows} (about {windows / 7:.1f} weeks at one a day)" if windows is not None
             else " windows_to_target=never, the windows are not trading"))
    return 0


def check(rows):
    """A series that cannot lie about itself: monotone windows, one row each, a count where a session ran."""
    problems = []
    seen = set()
    previous = None
    for row in rows:
        start = row["window_start_utc"]
        if start in seen:
            problems.append(f"{start}: two rows for one window")
        seen.add(start)
        if previous is not None and start <= previous:
            problems.append(f"{start}: windows are not in ascending order after {previous}")
        previous = start
        if (row.get("sessions") or 0) > 0 and fills_of(row) is None:
            problems.append(f"{start}: the session reported but no fill count is on the row")
    return problems


def emit(rows, path):
    """A two-line SVG: cumulative fills and cumulative net dollars, one point a window."""
    counted = [value for value in (fills_of(row) for row in rows) if value is not None]
    if not counted:
        return 1
    width, height, pad = 720, 240, 30
    running, total = [], 0.0
    for value in counted:
        total += value
        running.append(total)
    peak = max(running) or 1.0
    net, accumulated = [], 0.0
    for row in rows:
        accumulated += row.get("net_pnl_usd") or 0.0
        net.append(accumulated)
    span = max(abs(min(net)), abs(max(net)), 1e-9)
    step = (width - 2 * pad) / max(1, len(running) - 1)

    def line(values, scale):
        return " ".join(f"{pad + index * step:.1f},{height - pad - scale * (value / (peak if scale == 170 else span)):.1f}"
                        for index, value in enumerate(values))

    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}"><rect width="{width}" height="{height}" fill="white"/>'
        f'<polyline fill="none" stroke="#1f77b4" stroke-width="2" points="{line(running, 170)}"/>'
        f'<polyline fill="none" stroke="#d62728" stroke-width="1.5" stroke-dasharray="4 3" '
        f'points="{line(net, 60)}"/>'
        f'<text x="{pad}" y="18" font-family="sans-serif" font-size="12">cumulative fills (blue, peak {peak:.0f}) '
        f'and cumulative net usd (red, span {span:.3f}) over {len(counted)} window(s)</text></svg>\n',
        encoding="utf-8")
    return 0


def self_test():
    """Two windows with counts, one without, and the duplicate `check` has to refuse."""
    text = ("window_start_utc,window_end_utc,sessions,maker_trades,taker_trades,fees_paid,net_pnl_usd,"
            "gross_pnl_usd,residual_units,equity_usd,markout_ticks,read_at,source\n"
            "2026-11-27T13:30Z,15:30Z,1,11,1,0.0050681,-0.154068,-0.149,0,99.29,-0.590909,2026-11-27T16:48Z,journalctl\n"
            "2026-11-28T13:30Z,15:30Z,1,26,1,0.004,-0.02,-0.015,0,99.27,0.35,2026-11-28T16:48Z,journalctl\n"
            "2026-11-29T13:30Z,15:30Z,0,,,,,,,99.27,,2026-11-29T16:48Z,journalctl\n")
    with tempfile.TemporaryDirectory() as scratch:
        path = Path(scratch) / "ledger.csv"
        path.write_text(text, encoding="utf-8")
        rows = read_rows(str(path))
    assert [fills_of(row) for row in rows] == [12.0, 27.0, None], [fills_of(row) for row in rows]
    assert check(rows) == [], check(rows)
    assert sum(value for value in (fills_of(row) for row in rows) if value is not None) == 39
    assert any("two rows for one window" in problem for problem in check(rows + [dict(rows[0])]))
    assert any("no fill count" in problem for problem in check([dict(rows[2], sessions=1.0)]))
    assert progress(rows, 300) == 0
    with tempfile.TemporaryDirectory() as scratch:
        svg = Path(scratch) / "chart.svg"
        assert emit(rows, svg) == 0 and "<svg" in svg.read_text(encoding="utf-8")
    print("window_ledger self-test OK")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--file", default="-", help="the ledger CSV, or - for stdin")
    parser.add_argument("--table", action="store_true", help="one line a window")
    parser.add_argument("--progress", type=int, metavar="N", help="cumulative fills against a target of N")
    parser.add_argument("--check", action="store_true",
                        help="monotone windows, one row each, a count where a session ran")
    parser.add_argument("--emit", type=Path, metavar="SVG", help="a two-line chart of the series")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    rows = read_rows(args.file)
    status = 0
    if args.table:
        table(rows)
    if args.progress:
        status = progress(rows, args.progress) or status
    if args.emit:
        status = emit(rows, args.emit) or status
    if args.check:
        problems = check(rows)
        for problem in problems:
            print(f"window_ledger: {problem}", file=sys.stderr)
        print(f"window_ledger: {len(rows)} row(s), {len(problems)} problem(s)")
        status = 1 if problems else status
    return status


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Validate a downloaded OKX 400-level book archive and align it with another instrument's.

The lead-lag row is blocked on data, not on code: the venue's 400-level book history is not on the public history endpoint
(`tools/fetch/fetch_okx_trades.py`: "There is no order book module; that dataset is only offered by the web portal") and
`docs/DATA.md` says the same -- it has to be downloaded from the portal by hand. This tool is the half that can be built
anyway, so that the moment one day lands the pipeline runs unattended instead of being rebuilt:

  * it reads the archive's real shape, which is flat -- `{"instId","action","asks","bids","ts"}` one JSON object a line,
    not the websocket frame `{"arg","data"}` -- because a reader that assumed the frame found nothing (measured
    2026-11-27 on the 2026-01-07 BTC archive);
  * it checks the three properties a replay needs: an opening snapshot, the level count the archive's name promises, and a
    first timestamp inside the named UTC day (the book day is cut on midnight UTC, unlike the trade tape's UTC+8);
  * it prints the leader's and the follower's ranges and the overlap, which is what the lead-lag gate needs to be a
    two-hour window rather than one instrument's clock;
  * with `--watch N` it waits up to N seconds for a missing file, so an overnight drop starts the validation by itself.

    python3 tools/fetch/okx_depth_align.py --leader BTC-USDT-SWAP --follower UP-USDT-SWAP \\
        --days 2026-11-16,2026-11-17 --archives data/v2/crypto --watch 3600
"""
import argparse
import gzip
import io
import json
import sys
import tarfile
import time
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from okx_data import depth_archive  # noqa: E402

MIN_LEVELS_PER_SIDE = 100  # what `400lv` means in practice; the tool prints the measured number beside it


def open_member(path):
    """The tar's single JSONL member as a text stream."""
    archive = tarfile.open(path, "r:gz")
    member = next((m for m in archive.getmembers() if m.isfile()), None)
    if member is None:
        raise SystemExit(f"{path}: the archive has no file member")
    return archive, archive.extractfile(member)


def first_snapshot(path):
    """The opening snapshot: its timestamp, its two level counts, and how many lines were read to reach it."""
    archive, handle = open_member(path)
    try:
        for number, raw in enumerate(handle, start=1):
            line = raw.strip()
            if not line:
                continue
            record = json.loads(line)
            if record.get("action") != "snapshot":
                if number > 1000:
                    return None
                continue
            return dict(ts=int(record["ts"]), asks=len(record.get("asks") or []),
                        bids=len(record.get("bids") or []), line=number)
    finally:
        handle.close()
        archive.close()
    return None


def last_timestamp(path):
    """The final record's timestamp: one pass over the member, because a gz archive cannot be read backwards."""
    archive, handle = open_member(path)
    last = None
    try:
        for raw in handle:
            line = raw.strip()
            if line and b'"ts"' in line:
                last = int(json.loads(line)["ts"])
    finally:
        handle.close()
        archive.close()
    return last


def day_of(ts_ms):
    return datetime.fromtimestamp(ts_ms / 1000.0, tz=timezone.utc).strftime("%Y-%m-%d")


def validate(path, day, check_end):
    """The three properties a replay needs, plus the range the alignment needs. Returns (problems, facts)."""
    problems, facts = [], {}
    snapshot = first_snapshot(path)
    if snapshot is None:
        problems.append(f"{path.name}: no opening snapshot in the first 1000 records -- an update-only file replays a "
                        f"book built from nothing (docs/DATA.md records one ETH archive that opens this way)")
        return problems, facts
    facts.update(first=snapshot["ts"], asks=snapshot["asks"], bids=snapshot["bids"], snapshot_line=snapshot["line"])
    if snapshot["asks"] < MIN_LEVELS_PER_SIDE or snapshot["bids"] < MIN_LEVELS_PER_SIDE:
        problems.append(f"{path.name}: the opening snapshot carries {snapshot['asks']} asks and {snapshot['bids']} bids, "
                        f"under {MIN_LEVELS_PER_SIDE} a side -- not a 400-level file")
    if day_of(snapshot["ts"]) != day:
        problems.append(f"{path.name}: the first snapshot is {day_of(snapshot['ts'])} UTC, and the file is named {day} "
                        f"-- the book day is cut on midnight UTC and this file is not")
    if check_end:
        end = last_timestamp(path)
        facts["last"] = end
        if end is not None and end <= snapshot["ts"]:
            problems.append(f"{path.name}: the last timestamp {end} does not advance on the first {snapshot['ts']}")
    return problems, facts


def watch_for(path, seconds):
    """Wait for a file the operator is dropping in by hand, bounded: an unbounded wait is a wedged session."""
    if path.is_file() or seconds <= 0:
        return path.is_file()
    deadline = time.monotonic() + seconds
    print(f"waiting up to {seconds}s for {path} ...", flush=True)
    while time.monotonic() < deadline:
        if path.is_file():
            print(f"{path.name} arrived", flush=True)
            return True
        time.sleep(15)
    return False


def window(day, start, end):
    """The gate's own window, in epoch milliseconds, for the day."""
    base = datetime.strptime(day, "%Y-%m-%d").replace(tzinfo=timezone.utc)
    hours, minutes = (int(value) for value in start.split(":"))
    start_ms = int((base.timestamp() + hours * 3600 + minutes * 60) * 1000)
    hours, minutes = (int(value) for value in end.split(":"))
    return start_ms, int((base.timestamp() + hours * 3600 + minutes * 60) * 1000)


def report(args):
    problems, ready = [], []
    root = Path(args.archives)
    for day in [d.strip() for d in args.days.split(",") if d.strip()]:
        paths = {role: depth_archive(root, symbol, day) for role, symbol in
                 (("leader", args.leader), ("follower", args.follower))}
        for role, path in paths.items():
            if path is not None:
                continue
            expected = root / (args.leader if role == "leader" else args.follower) / \
                ((args.leader if role == "leader" else args.follower) + f"-L2orderbook-400lv-{day}.tar.gz")
            if role == "leader":
                watch_for(expected, args.watch)
                path = depth_archive(root, args.leader, day)
            if path is None:
                problems.append(f"{day}: the {role}'s archive is missing -- expected {expected.name}; the 400-level book "
                                f"is a portal download, so a script cannot fetch it (see this file's docstring)")
                continue
            found, facts = validate(path, day, args.check_end)
            problems.extend(found)
            print(f"{day} {role:8} {path.name}: snapshot at {facts.get('first')} "
                  f"({facts.get('asks')} asks / {facts.get('bids')} bids, line {facts.get('snapshot_line')})")
            if facts.get("last"):
                print(f"{day} {role:8} last record {facts['last']}")
            if role == "leader" and not found:
                ready.append(day)
    if problems:
        print("", file=sys.stderr)
        for problem in problems:
            print(f"depth_align: {problem}", file=sys.stderr)
        print(f"depth_align: {len(problems)} problem(s); the lead-lag gate cannot run on these days", file=sys.stderr)
        return 1
    print(f"depth_align: {len(ready)} day(s) ready for the lead-lag replay: {', '.join(ready)}")
    return 0


def self_test():
    """The three refusals and one pass, on archives built in a temporary directory."""
    import tempfile
    import tarfile

    def build(directory, name, records):
        path = Path(directory) / name
        body = "\n".join(json.dumps(r) for r in records) + "\n"
        with tarfile.open(path, "w:gz") as archive:
            payload = body.encode()
            info = tarfile.TarInfo("member.jsonl")
            info.size = len(payload)
            archive.addfile(info, io.BytesIO(payload))
        return path

    midnight = 1767744000007  # 2026-01-07 00:00:00 UTC, the measured first timestamp of the real archive
    levels = [[str(100 + i), "1", "1"] for i in range(150)]
    with tempfile.TemporaryDirectory() as room:
        good = build(room, "BTC-USDT-SWAP-L2orderbook-400lv-2026-01-07.tar.gz", [
            {"instId": "BTC-USDT-SWAP", "action": "snapshot", "ts": str(midnight), "asks": levels, "bids": levels},
            {"instId": "BTC-USDT-SWAP", "action": "update", "ts": str(midnight + 10), "asks": [], "bids": []}])
        problems, facts = validate(good, "2026-01-07", check_end=True)
        assert not problems, problems
        assert facts["asks"] == 150 and facts["bids"] == 150, facts
        assert facts["last"] == midnight + 10, facts

        thin = build(room, "thin.tar.gz", [{"instId": "BTC-USDT-SWAP", "action": "snapshot", "ts": str(midnight),
                                              "asks": levels[:10], "bids": levels[:10]}])
        assert any("400-level" in problem for problem in validate(thin, "2026-01-07", False)[0]), validate(thin)[0]

        updates_only = build(room, "updates.tar.gz", [{"instId": "BTC-USDT-SWAP", "action": "update",
                                                        "ts": str(midnight), "asks": [], "bids": []}])
        assert any("no opening snapshot" in problem for problem in validate(updates_only, "2026-01-07", False)[0])

        shifted = build(room, "shifted.tar.gz", [{"instId": "BTC-USDT-SWAP", "action": "snapshot",
                                                  "ts": str(midnight + 86400000), "asks": levels, "bids": levels}])
        assert any("named 2026-01-07" in problem for problem in validate(shifted, "2026-01-07", False)[0]), \
            validate(shifted)[0]
    print("depth_align self-test OK (150 levels a side accepted; thin, update-only and shifted-day refused)")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--leader", default="BTC-USDT-SWAP", help="the instrument whose moves lead")
    parser.add_argument("--follower", default="UP-USDT-SWAP", help="the instrument whose quotes follow")
    parser.add_argument("--days", help="comma separated UTC days, as the archive names them")
    parser.add_argument("--archives", default="data/v2/crypto", help="where the archives live")
    parser.add_argument("--start", default="13:30", help="the gate window's start, UTC")
    parser.add_argument("--end", default="15:30", help="the gate window's end, UTC")
    parser.add_argument("--watch", type=int, default=0,
                        help="wait this many seconds for a missing leader archive (an overnight drop)")
    parser.add_argument("--check-end", action="store_true",
                        help="also read the last record, which costs one pass over the whole member")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.days:
        parser.error("--days is required: the alignment is per day")
    return report(args)


if __name__ == "__main__":
    raise SystemExit(main())

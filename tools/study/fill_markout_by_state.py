#!/usr/bin/env python3
"""Does the leader's *state* predict our fills' markout, even though its return predicts nothing?

The lead-lag row died on returns (`book_lead_lag.py`: peak lag 0 ms, corr 0.013, one standard error). The claim a state
gate rests on is different -- "when the leader falls the small contract's book thins and our fills go bad" -- and that is
fill quality *conditioned on a state*, which a gate can use with zero predictive lead: it filters when we are in the book.

Method: replay the frozen ten-contract cell with `--record-orders`, price each **maker** execution's one-second markout
against the follower's own mid, and bucket the fills by the leader's three-second return at the fill's stamp. The venue's
record carries both legs of every execution (339 maker and 339 taker rows on 2026-11-16, the same quantities), so the taker
rows are skipped -- counting them halves the meaning of every markout.

Measured 2026-11-28 over 2026-11-16..22, 1,375 maker fills, BTC-USDT-SWAP as the leader and UP-USDT-SWAP as the follower:

    leader 3s return   fills   mean markout bps   median   share
    < -10                  0               --        --      --
    -10 .. -2            153            -3.16     +0.00     11%
    -2 .. +2           1,053            -0.78     +2.17     77%
    2 .. 10              164            -3.72     +2.33     12%
    > 10                   5            +4.36     +0.00      0%

Two readings, and the second is the one that changes a design. **Fills do go bad when the leader moves** -- the flat bucket
keeps a +2.17 bps median while the two moving buckets sit at 0.00 and the means fall to -3.2 and -3.7. And **the direction
does not matter**: a leader rise is as bad for a passive fill as a fall, so the state to gate on is `|return|`, a
volatility state, not the crash warning the note proposed. What that would cost is 23 percent of the fills.
"""
import argparse
import csv
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import book_lead_lag as bl  # noqa: E402  (the mid reader and the window bounds, one implementation)

ROOT = bl.ROOT
CUTS = "out/cuts"
OUT = Path("/tmp/tq/fill_state")
BIN = ROOT / "out/build/linux-debug/toy_quant"
DAYS = ["2026-11-16", "2026-11-17", "2026-11-18", "2026-11-19", "2026-11-20", "2026-11-21", "2026-11-22"]
CELL = ["--fast-validation", "--order-size=100000000", "--inventory-limit=1000000000", "--post-only",
        "--extra-layers=1", "--advance-debounce-ms=40000", "--settle-residual", "--settle-slippage-ticks=1"]
BUCKETS = [(-1e9, -10.0, "< -10"), (-10.0, -2.0, "-10..-2"), (-2.0, 2.0, "-2..2"),
           (2.0, 10.0, "2..10"), (10.0, 1e9, "> 10")]


def mids_from_jsonl(path, start_ms, end_ms):
    """The cut is the same record shape as the archive, without the tar."""
    book = {"bid": {}, "ask": {}}
    best = {"bid": None, "ask": None}
    series, last_mid = [], None
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.strip()
        if not line.startswith("{"):
            continue
        record = bl.json.loads(line)
        ts = int(record["ts"])
        if ts >= end_ms:
            break
        touched = {"bid": [], "ask": []}
        for key, side in (("bids", "bid"), ("asks", "ask")):
            for price, size, *_ in record.get(key, []):
                price, size = float(price), float(size)
                if size == 0.0:
                    book[side].pop(price, None)
                else:
                    book[side][price] = size
                touched[side].append((price, size))
        if not book["bid"]:
            best["bid"] = None
        elif best["bid"] is None or best["bid"] not in book["bid"]:
            best["bid"] = max(book["bid"])
        else:
            for price, size in touched["bid"]:
                if size > 0.0 and price > best["bid"]:
                    best["bid"] = price
        if not book["ask"]:
            best["ask"] = None
        elif best["ask"] is None or best["ask"] not in book["ask"]:
            best["ask"] = min(book["ask"])
        else:
            for price, size in touched["ask"]:
                if size > 0.0 and price < best["ask"]:
                    best["ask"] = price
        if best["bid"] is None or best["ask"] is None:
            continue
        mid = 0.5 * (best["bid"] + best["ask"])
        if mid != last_mid:
            series.append((ts, mid))
            last_mid = mid
    return series


def mid_at(series, stamp):
    """The last mid at or before `stamp`, by bisection."""
    if not series or stamp < series[0][0]:
        return None
    low, high, found = 0, len(series) - 1, None
    while low <= high:
        middle = (low + high) // 2
        if series[middle][0] <= stamp:
            found = series[middle][1]
            low = middle + 1
        else:
            high = middle - 1
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--days", default=",".join(DAYS), help="file days both instruments have an archive for")
    parser.add_argument("--leader", default="BTC-USDT-SWAP")
    parser.add_argument("--follower", default="UP-USDT-SWAP")
    args = parser.parse_args()
    days = [part.strip() for part in args.days.split(",") if part.strip()]
    OUT.mkdir(parents=True, exist_ok=True)
    pooled = {name: [] for _, _, name in BUCKETS}
    for day in days:
        trades = Path(CUTS) / f"trades_{args.follower}_{day}.csv"
        cut = Path(CUTS) / f"okx_{args.follower}_{day}_1330.jsonl"
        if not trades.exists() or not cut.exists():
            print(f"# {day}: no cut, skipped", flush=True)
            continue
        record = OUT / f"rec_{day}"
        # The venue's record carries **both legs of every execution**: the maker row is ours and the taker row is the
        # counterparty's mirror (measured 2026-11-16: 339 maker and 339 taker rows, the same 339 quantities, one each
        # side). Counting both halves every markout, so only the maker rows are read, and the replay is skipped when the
        # record is already on disk so a re-analysis does not pay for it again.
        if not Path(f"{record}_orders.csv").exists():
            command = [str(BIN), "l2_replay", str(trades), str(cut), args.follower, "0", "active_l2", "1",
                       "prorata"] + CELL + [f"--record-orders={record}"]
            run = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
            if run.returncode != 0:
                print(f"# {day}: replay failed: {run.stderr.strip()[-200:]}", flush=True)
                continue
        start_ms, end_ms = bl.window_bounds(day, "13:30", "15:30")
        follower = mids_from_jsonl(cut, start_ms, end_ms)
        leader = bl.mids(bl.depth_archive(ROOT / "data/v2/crypto", args.leader, day), start_ms, end_ms)
        fills = 0
        with open(f"{record}_orders.csv", newline="") as handle:
            for row in csv.DictReader(handle):
                if row["kind"] != "report" or row["exec_type"] != "trade":
                    continue
                if row.get("liquidity_role") != "maker":
                    continue
                stamp, price, side = int(row["ts"]), float(row["price"]), row["side"]
                future = mid_at(follower, stamp + 1000)
                leader_now = mid_at(leader, stamp)
                leader_before = mid_at(leader, stamp - 3000)
                if future is None or leader_now is None or leader_before is None or price <= 0:
                    continue
                sign = 1.0 if side == "B" else -1.0
                markout = sign * (future - price) / price * 1e4
                leader_return = (leader_now - leader_before) / leader_before * 1e4
                for low, high, name in BUCKETS:
                    if low <= leader_return < high:
                        pooled[name].append(markout)
                        break
                fills += 1
        print(f"# {day}: fills priced {fills}", flush=True)

    total = sum(len(values) for values in pooled.values()) or 1
    print(f"\n{'leader 3s return':>16} {'fills':>6} {'mean markout':>13} {'median':>8} {'share':>7}")
    for _, _, name in BUCKETS:
        values = sorted(pooled[name])
        if not values:
            print(f"{name:>16} {0:>6} {'':>13} {'':>8} {'':>7}")
            continue
        print(f"{name:>16} {len(values):>6} {sum(values) / len(values):>+13.2f} "
              f"{values[len(values) // 2]:>+8.2f} {len(values) / total:>6.0%}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

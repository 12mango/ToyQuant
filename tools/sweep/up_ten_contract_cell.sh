#!/usr/bin/env bash
# The ten-contract UP cell and its two state arms, as a script rather than a shell history.
#
# It exists because the previous run's driver lived in `/tmp` and was one wipe away from the record being unreproducible:
# the studies entry names the cell by reference ("the frozen ten-contract cell with `--settle-residual`") and those words
# are not enough to rebuild it -- the missing pieces were `--extra-layers=1` and `--settle-slippage-ticks=1`, and finding
# them cost a session. A registered arm carries its command, in the tree.
#
#   bash tools/sweep/up_ten_contract_cell.sh                 # all three arms
#   bash tools/sweep/up_ten_contract_cell.sh --arm f1_tox3   # one of them
#
# The bar, registered on 2026-11-26 before the third arm ran: the worst net day at **-120 bps or better** *and* the closed
# median positive on **5 of 7** at **40 fills or more**. It is in the header of the arm it judges, so the ruler and the
# number cannot be read apart.
#
# Measured 2026-11-27, all three arms over 2026-11-16..22 at 13:30-15:30 UTC (prorata, latency 10 ms):
#
#   arm        fills median  closed median  positive   worst net day  settled median
#   f1_control           50         +10.32       5/7        -202.88          +11.33
#   f1_tox2              42          +2.05       4/7         -76.38           -9.42
#   f1_tox3              44          +0.76       4/7         -76.38          -13.56
#
# **Re-measured on twenty-eight days, 2026-10-30..2026-11-26 (2026-11-28), which is the sampling resolution the row was
# refused on rather than a new threshold.** The month holds six tail windows in both directions (11-14 -1123 bps, 11-22
# +1062, 11-23 -944, 11-26 +848, 11-03 -759, 11-25 -534), so a gate and its window are no longer the same seven days:
#
#   arm             fills median  closed median [range]      positive   net median  USD/window  peak pos
#   f1_control                45   +12.27 [-75.19, +90.37]      22/28        +15.96      0.3630    305M
#   f1_tox3                   35   +15.35 [-70.00, +88.63]      21/28        +13.19      0.2048    240M
#   f3_throttle0              44    +8.10 [-54.35, +90.87]      20/28         +4.54      0.1728    200M
#
# What that changes: the control survives the larger sample (median and worst day both inside the bar), the **jump gate's
# median clause is available after all** -- +15.35 against +12.27, with the worst day 5.5 bps smaller and the leave-one-out
# median 14.52 against 11.18 -- and the **fill floor is what refuses it** (35 against 40), at a 44 percent dollar cost.
# The throttle arm is still worse on every column, so its refusal stands. The frontier row's "the tail and the median are
# not available from the same lever at this size" is therefore contradicted by this sample and is left for the owner to
# move, not for this script to rewrite.
#   bash tools/sweep/up_ten_contract_cell.sh --days 2026-10-30,...,2026-11-26
#
# The control and `f1_tox2` reproduce `rs_up10.csv` and `rs5_up10_tox.csv` to the digit; `f1_tox3` is the registered arm.
# It passes the tail clause by 43.6 bps and fails the median clause, and it is strictly dominated by `f1_tox2` (same worst
# day, lower median, worse settled) because the worst day is toxic under both definitions -- so the line closes with the
# sentence the registration asked for.
set -u
cd "$(dirname "$0")/../.." || exit 1

DAYS=2026-11-16,2026-11-17,2026-11-18,2026-11-19,2026-11-20,2026-11-21,2026-11-22
# The settle arm: the exit a replay cannot fill, priced at the far touch, one tick past it, plus a taker fee.
SETTLE="--settle-residual --settle-taker-bps=5 --settle-slippage-ticks=1"
# Ten contracts a clip into a hundred-contract cap, two layers, post-only, the frozen forty-second suppression.
CELL="--symbol UP-USDT-SWAP --days $DAYS --strategy active_l2 --models prorata
      --base-spread-grid 3 --refresh-grid 1 --latency-us 10000 --tick 0.0001
      --start 13:30 --end 15:30 --order-size 100000000 --inv-limit 1000000000"
BASE="--post-only --extra-layers=1 --advance-debounce-ms=40000 $SETTLE"

OUT=${OUT:-/tmp/tq}
arm=all
while [ $# -gt 0 ]; do
  case "$1" in
    --arm) arm=$2; shift 2 ;;
    --days) DAYS=$2; shift 2 ;;
    --out) OUT=$2; shift 2 ;;
    -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
    *) echo "up_ten_contract_cell: unknown argument $1" >&2; exit 2 ;;
  esac
done
mkdir -p "$OUT"

run() {  # run <name> <switch>
  local name=$1 switch=$2
  echo "== $name start $(date -u +%H:%M:%S)"
  # shellcheck disable=SC2086  # the two flag strings are word lists by construction, not one argument
  python3 tools/sweep/okx_maker_baseline.py $CELL --days "$DAYS" --extra="$BASE $switch" --out "$OUT/$name.csv"
  echo "== $name exit=$?"
}

want() { [ "$arm" = all ] || [ "$arm" = "$1" ]; }
want f1_control && run f1_control ""
want f1_tox2 && run f1_tox2 "--amend-on-toxicity --jump-window-ms=40 --jump-ticks=2"
want f1_tox3 && run f1_tox3 "--amend-on-toxicity --jump-window-ms=40 --jump-ticks=3"
# Frontier 3: the size throttle at the same 15 bps trigger P0.1 was refused at, with the adding side paused and then cut
# to one contract. The closing side keeps its full size in both arms -- that is the whole point of the lever.
want f3_throttle0 && run f3_throttle0 "--throttle-adverse-bps=15 --throttle-units=0"
want f3_throttle1 && run f3_throttle1 "--throttle-adverse-bps=15 --throttle-units=10000000"
echo "== done $(date -u +%H:%M:%S) -- rows in $OUT/f*.csv"

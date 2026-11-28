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
#   bash tools/sweep/up_ten_contract_cell.sh --symbol NES-USDT-SWAP --out /tmp/tq/nes   # the same cell, other seat
#
# `--symbol` and `--tick` exist because the seat rule ranks two instruments on *this* cell. A hard-coded UP here is the
# reason a NES row would have needed a `/tmp` driver, which is the failure `.clinerules` records; the two defaults keep
# every recorded UP row reproducible to the digit.
#
# The debounce ladder (Tier 1 #4) is four runs of one arm, one per value -- `--debounce` is why it is one tracked command:
#   for ms in 40000 5000 1000 250; do bash tools/sweep/up_ten_contract_cell.sh --arm f1_control --debounce $ms \
#       --days <sixty days> --out /tmp/tq/db$ms; done
#
# The control and `f1_tox2` reproduce `rs_up10.csv` and `rs5_up10_tox.csv` to the digit; `f1_tox3` is the registered arm.
# It passes the tail clause by 43.6 bps and fails the median clause, and it is strictly dominated by `f1_tox2` (same worst
# day, lower median, worse settled) because the worst day is toxic under both definitions -- so the line closes with the
# sentence the registration asked for.
set -u
cd "$(dirname "$0")/../.." || exit 1

DAYS=2026-11-16,2026-11-17,2026-11-18,2026-11-19,2026-11-20,2026-11-21,2026-11-22
# The seat the cell runs on and the instrument's own tick. The defaults are UP's, so every recorded row stands; a second
# seat names both, because a tick is a property of the instrument and not of the cell.
SYMBOL=${SYMBOL:-UP-USDT-SWAP}
TICK=${TICK:-0.0001}
# The frozen forty-second suppression, as a value rather than a constant: the debounce ladder varies it and nothing else.
DEBOUNCE=${DEBOUNCE:-40000}
# The settle arm: the exit a replay cannot fill, priced at the far touch, one tick past it, plus a taker fee.
SETTLE="--settle-residual --settle-taker-bps=5 --settle-slippage-ticks=1"

OUT=${OUT:-/tmp/tq}
arm=all
while [ $# -gt 0 ]; do
  case "$1" in
    --arm) arm=$2; shift 2 ;;
    --days) DAYS=$2; shift 2 ;;
    --symbol) SYMBOL=$2; shift 2 ;;
    --tick) TICK=$2; shift 2 ;;
    --debounce) DEBOUNCE=$2; shift 2 ;;
    --out) OUT=$2; shift 2 ;;
    -h|--help) sed -n '2,52p' "$0"; exit 0 ;;
    *) echo "up_ten_contract_cell: unknown argument $1" >&2; exit 2 ;;
  esac
done
# The base command line, built after the arguments for the same reason as CELL below it: expanded beside the defaults, an
# `--advance-debounce-ms` here baked in 40000 and the debounce ladder ran one arm four times and agreed with itself to the
# digit -- measured 2026-11-28, and the four identical CSVs are what gave the mistake away.
BASE="--post-only --extra-layers=1 --advance-debounce-ms=$DEBOUNCE $SETTLE"
# Ten contracts a clip into a hundred-contract cap, two layers, post-only, the frozen forty-second suppression. Built after
# the arguments so the symbol and the tick the caller named reach the engine; the defaults reproduce every recorded UP row
# to the digit.
CELL="--symbol $SYMBOL --days $DAYS --strategy active_l2 --models prorata
      --base-spread-grid 3 --refresh-grid 1 --latency-us 10000 --tick $TICK
      --start 13:30 --end 15:30 --order-size 100000000 --inv-limit 1000000000"
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
# The neighbourhood around that gate, so a threshold that works only at its chosen point is caught as noise rather than
# believed: `--jump-ticks` is an integer, so the interval is its own grid (1, 2, 3, 4) and a "+-20 percent" band is not
# expressible here -- that limitation is worth knowing before the rule is borrowed from a paper.
want f1_tox1 && run f1_tox1 "--amend-on-toxicity --jump-window-ms=40 --jump-ticks=1"
want f1_tox4 && run f1_tox4 "--amend-on-toxicity --jump-window-ms=40 --jump-ticks=4"
# Frontier 3: the size throttle at the same 15 bps trigger P0.1 was refused at, with the adding side paused and then cut
# to one contract. The closing side keeps its full size in both arms -- that is the whole point of the lever.
want f3_throttle0 && run f3_throttle0 "--throttle-adverse-bps=15 --throttle-units=0"
want f3_throttle1 && run f3_throttle1 "--throttle-adverse-bps=15 --throttle-units=10000000"
# Frontier 4, the wind-down: over the last ten minutes the adding side stops and the closing side walks one tick in.
# Measured 2026-11-28 over the same twenty-eight days, beside the control it is compared with:
#
#   arm             fills median  closed median [range]       positive   net median  USD/window  peak pos  settle bps
#   f1_control                45   +12.27 [-75.19, +90.37]       22/28        +15.96      0.3630      305M       2.30
#   f4_winddown10             48   +11.23 [-162.14, +89.94]      21/28         +8.18      0.2587       95M       0.78
#
# It does exactly what it says -- the handover falls from 2.30 to 0.78 bps and the residual from 1.2 contracts to zero,
# and the peak position from 305M to 95M -- and it costs the median, the worst day (-75 to -162) and 29 percent of the
# dollars, because the last ten minutes are the ones that move and the adding side is the only thing averaging into it.
# The pure form the note asked for -- stop adding, leave the price alone -- is not expressible: `--wind-down-skew-ticks`
# is refused below one tick by the engine.
want f4_winddown10 && run f4_winddown10 "--wind-down-skew-minutes=10 --wind-down-skew-ticks=1 --wind-down-end=15:30"
# The pure form, which the engine refused until 2026-11-28 (zero ticks now means "stop the adding side and leave the
# price alone"; `l2_strategy_test` pins it). Measured over the same twenty-eight days it is **byte-identical** to the
# one-tick arm on every column -- 48 fills, +11.23 closed, 8.18 net, 0.2587 USD, 95M peak, LOO 9.15 -- because a closing
# quote three ticks out moved one tick in is still two ticks from the touch and never fills differently. So the arm's whole
# difference from its control is the adding side going quiet, and not the skew.
want f4_winddown10_pure && run f4_winddown10_pure "--wind-down-skew-minutes=10 --wind-down-skew-ticks=0 --wind-down-end=15:30"
# Tier 2 #3's mechanism, in Avellaneda-Stoikov's shape: the reservation price shifts by `-q*gamma*sigma^2*(T - t)`, with the
# greeks folded into one measured number -- `--as-skew-bps` is the shift at full inventory and a full session remaining -- and
# the clock is the session's own (`--session-start=13:30`, `--wind-down-end=15:30`). It is a different lever from the
# wind-down: that one walks the closing side in over the last ten minutes, this one leans the whole session and relaxes to
# nothing at the close. One tick on UP at 0.19 is 5.3 bps, so 25 bps is about five ticks at full inventory, against the
# proportional shift's three.
want f6_as_skew && run f6_as_skew "--as-skew-bps=25 --session-start=13:30 --wind-down-end=15:30"
# The gradient around it: a smaller lean should pay less tail, cost less median, and the curve says whether any value holds
# both -- the same ladder the throttle and the debounce were read on.
want f6_as_skew10 && run f6_as_skew10 "--as-skew-bps=10 --session-start=13:30 --wind-down-end=15:30"
want f6_as_skew16 && run f6_as_skew16 "--as-skew-bps=16 --session-start=13:30 --wind-down-end=15:30"
echo "== done $(date -u +%H:%M:%S) -- rows in $OUT/f*.csv"

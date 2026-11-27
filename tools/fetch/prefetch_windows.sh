#!/usr/bin/env bash
# The days the next questions will need, fetched before they are asked.
#
# Each block below is a question, not a file list, and the fetchers are the two tracked ones (`fetch_okx_depth.py` for the
# 400-level book, `fetch_okx_trades.py` for the tape). Both take `--skip-existing`, so a re-run costs nothing and this
# script is safe to leave on a timer.
#
#   1. **UP, 2026-09-28..2026-10-29** -- the sixty-day sample the "one month is one regime" argument asks for. The 28 days
#      already held (10-30..11-26) read a closed median of +12.27 bps on 22 of 28, but their mean window move is -104 bps
#      at one standard error of 84: a second month is what says whether that is a cell or a fortnight.
#   2. **NES, 2026-10-30..2026-11-26** -- the second seat on the *same* sample as UP, which is what a seat comparison
#      needs (the frontier has NES at three ticks with 46 fills on seven days, and no month).
#   3. **2026-11-27, both instruments** -- the first live window's own day, so the live session can be replayed rather
#      than only read off its journal. **Not published yet when this was written**: the depth URL answered 404 on
#      2026-11-28 07:00 HKT while 11-26 answered 200, so this block is a retry that the lag makes free.
#
# Sizes, measured on the days already held: a depth day is 10-30 MB, a trade day's *sliced* tape is 4-14 MB, and the
# venue-wide zip behind it is 150-615 MB and is deleted after slicing. The kept total for all three blocks is about 1.5 GB.
#
#   bash tools/fetch/prefetch_windows.sh            # all blocks
#   bash tools/fetch/prefetch_windows.sh --block 1  # one of them
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1

status=0
block() { [ -z "${ONLY:-}" ] || [ "$ONLY" = "$1" ]; }

fetch_book() {  # fetch_book <symbol> <from> <to>
  echo "== depth $1 $2..$3"
  python3 tools/fetch/fetch_okx_depth.py --symbol "$1" --from "$2" --to "$3" --skip-existing --pause 1 \
    || { echo "prefetch: depth failed for $1 $2..$3" >&2; status=1; }
}

fetch_tapes() {  # fetch_tapes <symbol> <from> <to>
  echo "== trades $1 $2..$3"
  python3 tools/fetch/fetch_okx_trades.py --symbol "$1" --from "$2" --to "$3" \
    --output-dir "data/v2/crypto/$1" --skip-existing --pause 3 \
    || { echo "prefetch: trades failed for $1 $2..$3" >&2; status=1; }
}

if [ "${1:-}" = "--block" ]; then ONLY=${2:-}; shift 2 || true; fi

block 1 && { fetch_book UP-USDT-SWAP 2026-09-28 2026-10-29; fetch_tapes UP-USDT-SWAP 2026-09-28 2026-10-29; }
block 2 && { fetch_book NES-USDT-SWAP 2026-10-30 2026-11-26; fetch_tapes NES-USDT-SWAP 2026-10-30 2026-11-26; }
block 3 && { fetch_book UP-USDT-SWAP 2026-11-27 2026-11-27; fetch_tapes UP-USDT-SWAP 2026-11-27 2026-11-27
             fetch_book NES-USDT-SWAP 2026-11-27 2026-11-27; fetch_tapes NES-USDT-SWAP 2026-11-27 2026-11-27; }

echo "== prefetch done exit=$status"
exit "$status"

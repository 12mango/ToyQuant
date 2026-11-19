#!/usr/bin/env bash
#
# A session written to a snapshot and resumed against a live venue.
#
# This is the one shape that reaches `[RECON] at=start position` on a *resumed* session: the snapshot flags are
# refused on `ws` ("a session is written and resumed against a replay's own clock"), so the market leg has to be
# a file and the order leg a socket -- `l2_replay --live-orders`, which is what ADR 0005 bought.
#
# Usage:
#   tools/live/resume_live.sh fresh     # a replay over a live order leg, writing the snapshot
#   tools/live/resume_live.sh resume    # resume from it; the venue is asked about every order it listed
#   tools/live/resume_live.sh break     # the same resume from a snapshot that claims a position the venue does
#                                       # not hold, which the position line has to call a break
#
# Environment: BIN, RELAY, TRADES, DEPTH, PORT, OUT (log and snapshot prefix), VENUE (the REST base url),
# INST_CODE, STARTING_CASH, ORDER_SIZE, MAX_ORDERS, POSITION_UNITS (the fabricated one `break` injects).
#
# It is not a gate tier: it needs the demo credentials and a network, so its numbers are quoted through a marked
# block rather than re-derived by `fast`. The credentials are read the way the relay reads them:
#
#     set -a; . secrets/okx-demo.env; set +a
#
# `--order-size` is one contract at the default quantity scale: a `BTC-USDT-SWAP` contract is 0.01 BTC, the
# engine's unit is 1e-6 of the coin, so 10000 units is one contract. A run at the replay's own scale (1 unit = 1
# coin) sends a hundredfold size and the relay refuses it as below a contract, which is the failure this default
# exists to avoid.

set -u
cd "$(dirname "$0")/../.." || exit 1
[ -n "${TOYQUANT_API_KEY:-}" ] || {
  echo "the demo credentials are not in the environment: set -a; . secrets/okx-demo.env; set +a" >&2
  exit 2
}

BIN="${BIN:-out/build/linux-debug/toy_quant}"
RELAY="${RELAY:-out/build/linux-debug/venue_relay}"
TRADES="${TRADES:-data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz}"
SOURCE_DEPTH=data/v2/crypto/deribit_book_snapshot_25_2020-04-01_BTC-PERPETUAL.csv
DEPTH="${DEPTH:-/tmp/tq/resume_depth_200k.csv}"
PORT="${PORT:-8796}"
OUT="${OUT:-/tmp/tq/resume}"
VENUE="${VENUE:-https://www.okx.com}"
INST_CODE="${INST_CODE:-10459}"
STARTING_CASH="${STARTING_CASH:-4917.96}"
ORDER_SIZE="${ORDER_SIZE:-10000}"
MAX_ORDERS="${MAX_ORDERS:-2}"
POSITION_UNITS="${POSITION_UNITS:-10000}"
MODE="${1:-fresh}"

[ -x "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 2; }
[ -x "$RELAY" ] || { echo "relay not found: $RELAY" >&2; exit 2; }
[ -f "$TRADES" ] || { echo "missing trades file: $TRADES" >&2; exit 2; }

mkdir -p "$(dirname "$OUT")"
# The 200k slice the gate cuts for its own runs, cut the same way: the tracked depth file is 3.3 GB and a
# measurement needs a window, not a day.
[ -f "$DEPTH" ] || head -n 200000 "$SOURCE_DEPTH" > "$DEPTH"

# The relay's order leg over REST, which is what a resume needs: `GET /api/v5/trade/orders-pending` is a REST
# query, and the balance and position queries ride on the same surface. Its market leg is off -- the market
# arrives from the file.
"$RELAY" --instrument BTC-USDT-SWAP \
  --order-listen "127.0.0.1:${PORT}" --order-rest "$VENUE" --order-inst-code="$INST_CODE" \
  --max-seconds 900 >> "${OUT}_relay.log" 2>&1 &
relay=$!
trap 'kill "$relay" 2>/dev/null; wait "$relay" 2>/dev/null' EXIT
sleep 2

COMMON=(
  l2_replay "$TRADES" "$DEPTH" BTC-PERPETUAL 0 active_l2 1000000 prorata
  --fast-validation "--starting-cash=${STARTING_CASH}" "--order-size=${ORDER_SIZE}"
  --live-orders "--venue-endpoint=127.0.0.1:${PORT}" "--venue-max-orders=${MAX_ORDERS}"
)

case "$MODE" in
  fresh)
    "$BIN" "${COMMON[@]}" --snapshot-out="${OUT}_snapshot.txt" > "${OUT}_fresh.log" 2>&1
    status=$?
    ;;
  resume)
    "$BIN" "${COMMON[@]}" --snapshot-in="${OUT}_snapshot.txt" > "${OUT}_resume.log" 2>&1
    status=$?
    ;;
  break)
    # The fault is the file, not the venue: the snapshot claims `POSITION_UNITS` the account never had, which is
    # what a resume from a stale or hand-edited file looks like from the run's side. Nothing here touches the
    # venue's position. The fabricated position is priced at the snapshot's own mark, so the equity line stays
    # `ok` -- the quantity is the only disagreement, which is the case the position line exists for.
    [ -f "${OUT}_snapshot.txt" ] || { echo "run 'fresh' first: no ${OUT}_snapshot.txt" >&2; exit 2; }
    mark=$(sed -n 's/^mark=BTC-PERPETUAL://p' "${OUT}_snapshot.txt" | head -1)
    cp "${OUT}_snapshot.txt" "${OUT}_snapshot_broken.txt"
    printf 'position=BTC-PERPETUAL:%s:%s:0:0:0\n' "$POSITION_UNITS" "${mark:-0}" >> "${OUT}_snapshot_broken.txt"
    "$BIN" "${COMMON[@]}" --snapshot-in="${OUT}_snapshot_broken.txt" > "${OUT}_break.log" 2>&1
    status=$?
    ;;
  *)
    echo "usage: $0 fresh|resume|break" >&2
    exit 2
    ;;
esac

# The lines the mode exists to produce, verbatim. A missing line is the failure, so the grep's exit status is not
# swallowed by the trailer.
grep -hE '^\[(RESUME|RECON|SNAPSHOT|VENUE)' "${OUT}_${MODE}.log" || status=1
exit "$status"

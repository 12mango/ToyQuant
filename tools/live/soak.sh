#!/usr/bin/env bash
#
# One live session of a fixed shape, so that two of them differ only in the flag under test.
#
# Why this is a tool and not a paragraph: the same shape has now been typed into a terminal three times (the three
# 2026-11-21 windows, and the 2026-11-23 post-only window that followed them), each time from a scratch script that
# `/tmp` was free to wipe. The repo's own rule for that is `tools/live/resume_live.sh` and `tools/live/okx_probe.py`:
# a harness whose numbers are quoted is tracked.
#
# Usage:
#   set -a; . secrets/okx-demo.env; set +a
#   STARTING_CASH=<the probe's eq> tools/live/soak.sh post-only      # add-liquidity-only quotes
#   STARTING_CASH=<the probe's eq> tools/live/soak.sh baseline       # the same window without the flag
#
# Environment: STARTING_CASH (required -- a live run that opens on a guess prints a `break` in its first equity line),
# DURATION (300), OUT (/tmp/tq/soak), PORT (8841), ORDER_PORT (8842), PROXY (`` dials the venue directly, which is the
# normal way in; `host:port` tunnels both legs), VENUE, INST (BTC-USDT-SWAP), INST_CODE (10459, the venue's own contract
# code), MARKET_DEMO (1: the market leg reads the demo's public stream; 0: the real one, for a symbol the demo does not
# quote, with the order leg still on the demo account), ORDER_SIZE (10000 engine units = one contract),
# INVENTORY_LIMIT (10000), MAX_EXPOSURE_USD (1000), MAX_ORDERS (4000), RATE (2), CLOSE_OUT_CROSS_BPS (20).
#
# Two knobs exist for the scan this tool was written for: STRATEGY (default `passive_l2`; `active_l2` is the mainline,
# with the toxicity, weak-flow, volatility and markout gates) and SPREAD_TICKS (empty = the strategy's own width,
# otherwise `--base-spread-ticks=N`). TAG is appended to every artifact's name, so three widths do not overwrite each
# other's logs and recordings.
#
# The market leg is the venue's public stream through the relay, the order leg is REST through the relay, and both are
# recorded (`<OUT>/<mode>_trades.csv`, `_depth.csv`, `_orders.csv`) so the window can be replayed afterwards. A session
# starts *flat*: it asks the venue for its equity first and refuses to run when the answer is not the cash it was given.
#
# stdin is closed on purpose. A session started by hand from an editor's terminal inherits that terminal, and a child
# that holds it keeps the next command from being delivered at all -- measured 2026-11-23: twenty minutes in which no
# command in the tool's shell executed, with the session itself running fine in the background. `bash .run.sh soak`
# also closes every descriptor; this line makes the script safe to run either way.
exec </dev/null
set -u
cd "$(dirname "$0")/../.." || exit 1

MODE="${1:-}"
case "$MODE" in
    post-only|baseline) ;;
    *)
        echo "usage: $0 <post-only|baseline>" >&2
        exit 2
        ;;
esac
if [ -z "${STARTING_CASH:-}" ]; then
    echo "STARTING_CASH is required: read it with tools/live/okx_probe.py all and pass its eq" >&2
    exit 2
fi
[ -n "${TOYQUANT_API_KEY:-}" ] || {
    echo "the demo credentials are not in the environment: set -a; . secrets/okx-demo.env; set +a" >&2
    exit 2
}

BIN="${BIN:-out/build/linux-debug/toy_quant}"
RELAY="${RELAY:-out/build/linux-debug/venue_relay}"
DURATION="${DURATION:-300}"
OUT="${OUT:-/tmp/tq/soak}"
PORT="${PORT:-8841}"
ORDER_PORT="${ORDER_PORT:-8842}"
PROXY="${PROXY:-}"
VENUE="${VENUE:-https://www.okx.com}"
INST="${INST:-BTC-USDT-SWAP}"
INST_CODE="${INST_CODE:-10459}"
ORDER_SIZE="${ORDER_SIZE:-10000}"
INVENTORY_LIMIT="${INVENTORY_LIMIT:-10000}"
MAX_EXPOSURE_USD="${MAX_EXPOSURE_USD:-1000}"
MAX_ORDERS="${MAX_ORDERS:-4000}"
RATE="${RATE:-2}"
CLOSE_OUT_CROSS_BPS="${CLOSE_OUT_CROSS_BPS:-20}"
STRATEGY="${STRATEGY:-passive_l2}"
SPREAD_TICKS="${SPREAD_TICKS:-}"
TAG="${TAG:-}"
# The market leg is the demo venue's public stream by default, which is the only one that carries BTC and ETH. A symbol
# the demo does not quote has to take its market leg from the real public stream while the order leg stays on the demo
# account, so the two are separate switches: MARKET_DEMO=0 reads the real book.
MARKET_DEMO="${MARKET_DEMO:-1}"
NAME="${MODE}${TAG}"
ARTIFACT="${OUT}/${NAME}"

[ -x "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 2; }
[ -x "$RELAY" ] || { echo "relay not found: $RELAY" >&2; exit 2; }
mkdir -p "$OUT"

# The relays outlive the engine by a margin: the winner is whichever exits first, and a relay that dies early turns a
# working session into `frames_received=0`.
LIFETIME=$((DURATION + 120))
MARKET_FLAGS=()
if [ "$MARKET_DEMO" = "1" ]; then
    MARKET_FLAGS+=(--venue-demo)
fi
"$RELAY" --instrument "$INST" ${MARKET_FLAGS[@]+"${MARKET_FLAGS[@]}"} --proxy="$PROXY" --listen "127.0.0.1:${PORT}" \
    --max-seconds "$LIFETIME" > "${ARTIFACT}_market_relay.log" 2>&1 &
market_relay=$!
"$RELAY" --instrument "$INST" --order-listen "127.0.0.1:${ORDER_PORT}" --order-rest "$VENUE" \
    --order-inst-code="$INST_CODE" --proxy="$PROXY" --max-seconds "$LIFETIME" \
    > "${ARTIFACT}_order_relay.log" 2>&1 &
order_relay=$!
trap 'kill "$market_relay" "$order_relay" 2>/dev/null; wait 2>/dev/null' EXIT
sleep 3

FLAGS=(--fast-validation --ws-seconds="$DURATION" --starting-cash="$STARTING_CASH"
       --order-size="$ORDER_SIZE" --inventory-limit="$INVENTORY_LIMIT"
       --max-exposure-usd="$MAX_EXPOSURE_USD" --live-orders
       --venue-endpoint="127.0.0.1:${ORDER_PORT}" --venue-max-orders="$MAX_ORDERS"
       --venue-orders-per-second="$RATE" --close-out --close-out-cross-bps="$CLOSE_OUT_CROSS_BPS"
       --record="${ARTIFACT}" --record-orders="${ARTIFACT}")
if [ "$MODE" = "post-only" ]; then
    FLAGS+=(--post-only)
fi
if [ -n "$SPREAD_TICKS" ]; then
    FLAGS+=(--base-spread-ticks="$SPREAD_TICKS")
fi

# The six positional arguments come first and the flags after them, in the order this binary reads them. The EXIT trap
# stops the relays, so nothing here repeats that.
"$BIN" ws "127.0.0.1:${PORT}" "$INST" "$STRATEGY" 1000000 prorata "${FLAGS[@]}" > "${ARTIFACT}.log" 2>&1
status=$?
echo "engine=$status mode=$MODE strategy=$STRATEGY spread=${SPREAD_TICKS:-default} out=$OUT"
echo "summary: grep -E '^\\[(PNL|PORTFOLIO|EXECUTION|WIND-DOWN|VENUE|RECON)\\]' ${ARTIFACT}.log"
exit "$status"

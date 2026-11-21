#!/usr/bin/env python3
"""The account's own side of a live measurement: what the venue says it holds.

The engine prints its side as `[RECON] at=start|at=end position venue_units=... ledger_units=...`, and every live run in
the documents quotes one of those lines. This is the other reader of the same account -- the probe a run is set up from
and checked against -- and it asks nothing the engine does not: `GET /api/v5/account/balance`,
`GET /api/v5/account/positions`, `GET /api/v5/trade/orders-pending`, and two placements for the runs that need a position
the ledger does not know about.

    set -a; . secrets/okx-demo.env; set +a
    python3 tools/live/okx_probe.py all            # equity, position, pending orders
    python3 tools/live/okx_probe.py market S 1     # one contract short, at the market
    python3 tools/live/okx_probe.py order B 1      # one contract long, IOC at the venue's own touch

It is a tool rather than part of the engine because it needs the demo key and a network, so it is quoted like
`tools/live/resume_live.sh` rather than re-derived by `fast`. Two things it has to do that are easy to get wrong: the
public endpoints answer a default agent with `403 error code: 1010`, so every request carries a browser `User-Agent`;
and the demo venue is the only environment this tree has a key for, so `x-simulated-trading: 1` is always sent.
"""
import base64
import hashlib
import hmac
import json
import os
import sys
import urllib.parse
import urllib.request
from datetime import datetime, timezone

BASE = os.environ.get("VENUE", "https://www.okx.com")
INST = os.environ.get("INST_ID", "BTC-USDT-SWAP")
# A browser's, not `urllib`'s: the venue's proxy refuses the default agent with `error code: 1010`, which reads like a
# refusal of the request and is a refusal of its name.
UA = ("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
      "Chrome/120.0.0.0 Safari/537.36")


def request(method, path, body=""):
    key = os.environ["TOYQUANT_API_KEY"]
    secret = os.environ["TOYQUANT_API_SECRET"]
    passphrase = os.environ["TOYQUANT_PASSPHRASE"]
    now = datetime.now(timezone.utc)
    stamp = now.strftime("%Y-%m-%dT%H:%M:%S.") + f"{now.microsecond // 1000:03d}Z"
    sign = base64.b64encode(
        hmac.new(secret.encode(), (stamp + method.upper() + path + body).encode(), hashlib.sha256).digest()
    ).decode()
    req = urllib.request.Request(
        BASE + path,
        data=body.encode() if body else None,
        method=method.upper(),
        headers={
            "OK-ACCESS-KEY": key,
            "OK-ACCESS-SIGN": sign,
            "OK-ACCESS-TIMESTAMP": stamp,
            "OK-ACCESS-PASSPHRASE": passphrase,
            "Content-Type": "application/json",
            "x-simulated-trading": "1",
            "User-Agent": UA,
        },
    )
    with urllib.request.urlopen(req, timeout=15) as answer:
        return json.loads(answer.read().decode())


def emit(payload):
    print(json.dumps(payload, sort_keys=True), flush=True)


def balance():
    data = request("GET", "/api/v5/account/balance").get("data") or [{}]
    for entry in data[0].get("details") or []:
        if entry.get("ccy") == "USDT":
            # `eq` is the number `--starting-cash` is supposed to be; `cashBal` is the same number when nothing is
            # borrowed, which is how the demo account runs.
            return entry.get("eq") or entry.get("cashBal") or "0"
    return "0"


def positions():
    query = urllib.parse.urlencode({"instId": INST})
    data = request("GET", "/api/v5/account/positions?" + query).get("data") or []
    return [{"instId": row.get("instId"), "pos": row.get("pos"), "avgPx": row.get("avgPx"),
             "upl": row.get("upl"), "mgnMode": row.get("mgnMode")} for row in data]


def pending():
    query = urllib.parse.urlencode({"instType": "SWAP", "instId": INST})
    data = request("GET", "/api/v5/trade/orders-pending?" + query).get("data") or []
    return [{"ordId": row.get("ordId"), "side": row.get("side"), "sz": row.get("sz"),
             "state": row.get("state"), "px": row.get("px")} for row in data]



def place(side, contracts="1"):
    """One IOC contract at the venue's own touch, which is how a position nobody's ledger knows about gets made.

    An IOC priced at a bid that has moved fills a fraction or nothing, so the answer is read back from the venue rather
    than assumed: `pos` is the contract count (`-1` for one short), and it is the row -- not the size that was asked for
    -- that says how big a break would be.
    """
    ticker_path = "/api/v5/market/ticker?instId=" + INST
    ticker_request = urllib.request.Request(BASE + ticker_path, headers={"User-Agent": UA})
    with urllib.request.urlopen(ticker_request, timeout=15) as answer:
        ticker = json.loads(answer.read().decode())["data"][0]
    price = ticker["bidPx"] if side.upper() == "S" else ticker["askPx"]
    # The venue's own words, not the relay's single letters: `S` is a `sell` here.
    body = json.dumps({"instId": INST, "tdMode": "cross", "side": "sell" if side.upper() == "S" else "buy",
                       "ordType": "ioc", "px": price, "sz": contracts})
    return request("POST", "/api/v5/trade/order", body)


def market(side, contracts="1"):
    """A market order in the venue's own words: it fills or it does not, with no price to go stale on the way."""
    body = json.dumps({"instId": INST, "tdMode": "cross", "side": "sell" if side.upper() == "S" else "buy",
                       "ordType": "market", "sz": contracts})
    return request("POST", "/api/v5/trade/order", body)


def main():
    command = sys.argv[1] if len(sys.argv) > 1 else "all"
    if command in ("equity", "all"):
        emit({"cmd": "equity", "ccy": "USDT", "eq": balance()})
    if command in ("position", "all"):
        emit({"cmd": "position", "instId": INST, "rows": positions()})
    if command in ("pending", "all"):
        emit({"cmd": "pending", "instId": INST, "rows": pending()})
    if command in ("order", "market"):
        side = sys.argv[2] if len(sys.argv) > 2 else "S"
        contracts = sys.argv[3] if len(sys.argv) > 3 else "1"
        answer_json = place(side, contracts) if command == "order" else market(side, contracts)
        data = (answer_json.get("data") or [{}])[0]
        emit({"cmd": command, "side": side.upper(), "sz": contracts, "code": answer_json.get("code"),
              "sCode": data.get("sCode"), "sMsg": data.get("sMsg"), "ordId": data.get("ordId")})
        return 0 if answer_json.get("code") == "0" and data.get("sCode") == "0" else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

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
    python3 tools/live/okx_probe.py rtt UP-USDT-SWAP 12   # place/cancel round trips and the post-only decline rate
    python3 tools/live/okx_probe.py warm BTC-USDT-SWAP 12 # the same two, on one kept-alive tunnel
    python3 tools/live/okx_probe.py amend BTC-USDT-SWAP 12 # amend against cancel+place, one tunnel, alternating order

`amend` is the repricing question rather than the link's: a strategy that moves a resting quote either cancels and
places again, which is two round trips and leaves the level without our quote between them, or amends the order in
place, which is one. Its ruler is registered before the run, because a number without one is a number nobody can act
on: `rtt` and `warm` put a placement at 77.5 ms and a cancel at 78.0 ms p50 on this host's direct route, so
cancel+place is about 155 ms, and **an amend whose p50 lands in one round trip -- the band 75 to 85 ms -- removes
about half of a repricing decision**. A p50 above that band means the venue charges the amend a second round trip
somewhere, and the direction stays unbuilt. Both legs run in one process, on one kept-alive tunnel, alternating which
goes first, so a drift in the link over the run cannot favour either; every order rests `post_only` two percent under
the best bid and neither leg's target can cross, so a fill is not part of this measurement.

`rtt` is the link's own measurement rather than the account's: it places a `post_only` buy two percent under the best
bid, so the order has to rest and the venue's reply is the placement's, times that reply and the cancel that follows it,
and then prices the same order one tick through the ask, where `post_only` must refuse it -- the venue's non-zero `sCode`
is the decline this repository counts as `post_only_cancels`. `warm` does the same on a single CONNECT tunnel, because
`urllib` pays a connection and a TLS handshake per request: the cold number is what a client that reconnects sees, the
warm one is what a relay with a persistent connection sees, and a replay's `--latency-us` is a statement about one of
them rather than about "the network".

It is a tool rather than part of the engine because it needs the demo key and a network, so it is quoted like
`tools/live/resume_live.sh` rather than re-derived by `fast`. Two things it has to do that are easy to get wrong: the
public endpoints answer a default agent with `403 error code: 1010`, so every request carries a browser `User-Agent`;
and the demo venue is the only environment this tree has a key for, so `x-simulated-trading: 1` is always sent.
"""
import base64
import hashlib
import hmac
import http.client
import json
import os
import sys
import time
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


def public_json(path):
    """An unsigned public read, with no demo header: the market data and the instrument table are the same for every
    environment, and `x-simulated-trading` on the instrument list returns only the demo's own subset -- which is how
    `UP-USDT-SWAP` came back as "does not list" while the archive is full of it."""
    req = urllib.request.Request(BASE + path, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=15) as answer:
        return json.loads(answer.read().decode())


def inst_spec(inst):
    """The tick, the lot and the minimum the venue states for this instrument, which a price has to be on."""
    query = urllib.parse.urlencode({"instType": "SWAP", "instId": inst})
    data = public_json("/api/v5/public/instruments?" + query).get("data") or []
    if not data:
        raise SystemExit(f"the venue does not list {inst}")
    row = data[0]
    # The text as the venue writes it, not only the number: a 0.5 tick is a one-decimal grid and a 0.0001 tick is a
    # four-decimal one, and `log10` cannot tell the two apart.
    return {"tickSz": float(row["tickSz"]), "tickText": row["tickSz"], "lotSz": float(row.get("lotSz") or 1),
            "minSz": float(row.get("minSz") or 0), "ctVal": row.get("ctVal")}


def best_quote(inst):
    """The venue's live top of book, not its 24-hour ticker: a ticker read 700 ms before placement is a price the
    market has left, which is how a `post_only` order priced at the stale ask came back accepted instead of refused."""
    rows = public_json("/api/v5/market/books?instId=" + inst + "&sz=1")["data"][0]
    return float(rows["bids"][0][0]), float(rows["asks"][0][0])


def tunnel(host):
    """One CONNECT tunnel to the venue through the proxy, kept for the whole measurement.

    `urllib` opens a fresh TLS connection per request, so its round trip is a *cold* one: DNS through the proxy, TCP,
    TLS, then the request. A relay that places two orders a second does not pay that per order, so the number a replay's
    `--latency-us` should stand for is the warm one, and this is what makes it measurable rather than argued.
    """
    proxy = urllib.parse.urlsplit(os.environ.get("HTTPS_PROXY") or os.environ.get("https_proxy") or "")
    if proxy.hostname is None:
        connection = http.client.HTTPSConnection(host, 443, timeout=15)
    else:
        connection = http.client.HTTPSConnection(proxy.hostname, proxy.port or 7890, timeout=15)
        connection.set_tunnel(host, 443)
    connection.connect()
    return connection


def signed_headers(method, path, body):
    key = os.environ["TOYQUANT_API_KEY"]
    secret = os.environ["TOYQUANT_API_SECRET"]
    passphrase = os.environ["TOYQUANT_PASSPHRASE"]
    now = datetime.now(timezone.utc)
    stamp = now.strftime("%Y-%m-%dT%H:%M:%S.") + f"{now.microsecond // 1000:03d}Z"
    sign = base64.b64encode(
        hmac.new(secret.encode(), (stamp + method.upper() + path + body).encode(), hashlib.sha256).digest()
    ).decode()
    return {"OK-ACCESS-KEY": key, "OK-ACCESS-SIGN": sign, "OK-ACCESS-TIMESTAMP": stamp,
            "OK-ACCESS-PASSPHRASE": passphrase, "Content-Type": "application/json",
            "x-simulated-trading": "1", "User-Agent": UA}


def warm_rtt(connection, inst, rounds, gap_s):
    """The same placement and cancel as `rtt`, on the tunnel that is already open.

    This is the number a relay with a persistent connection actually sees, and it is what a replay's `--latency-us` has
    to stand for; the cold number `rtt` measures answers the different question of what one request costs a client that
    reconnects every time.
    """
    spec = inst_spec(inst)
    lot = one_lot(spec)
    bid, _ask = best_quote(inst)
    resting = on_tick(bid * 0.98, spec)
    place_ms, cancel_ms, codes, leftovers = [], [], {}, []
    for _ in range(rounds):
        body = json.dumps({"instId": inst, "tdMode": "cross", "side": "buy", "ordType": "post_only",
                           "px": resting, "sz": lot})
        headers = signed_headers("POST", "/api/v5/trade/order", body)
        start = time.perf_counter()
        connection.request("POST", "/api/v5/trade/order", body=body, headers=headers)
        payload = json.loads(connection.getresponse().read().decode())
        elapsed = (time.perf_counter() - start) * 1000.0
        row = (payload.get("data") or [{}])[0]
        if payload.get("code") == "0" and row.get("sCode") == "0" and row.get("ordId"):
            place_ms.append(elapsed)
            order_id = row["ordId"]
            cancel_body = json.dumps({"instId": inst, "ordId": order_id})
            headers = signed_headers("POST", "/api/v5/trade/cancel-order", cancel_body)
            start = time.perf_counter()
            connection.request("POST", "/api/v5/trade/cancel-order", body=cancel_body, headers=headers)
            cancel_payload = json.loads(connection.getresponse().read().decode())
            cancel_elapsed = (time.perf_counter() - start) * 1000.0
            cancel_row = (cancel_payload.get("data") or [{}])[0]
            if cancel_payload.get("code") == "0" and cancel_row.get("sCode") == "0":
                cancel_ms.append(cancel_elapsed)
            else:
                leftovers.append(order_id)
        else:
            key = f"{payload.get('code')}/{row.get('sCode')}"
            codes[key] = codes.get(key, 0) + 1
        time.sleep(gap_s)
    swept = 0
    for order_id in leftovers:
        body = json.dumps({"instId": inst, "ordId": order_id})
        headers = signed_headers("POST", "/api/v5/trade/cancel-order", body)
        connection.request("POST", "/api/v5/trade/cancel-order", body=body, headers=headers)
        if json.loads(connection.getresponse().read().decode()).get("code") == "0":
            swept += 1
    emit({"cmd": "warm_rtt", "instId": inst, "rounds": rounds, "lot": lot, "resting_px": resting,
          "place_ms": summary(place_ms), "cancel_ms": summary(cancel_ms), "errors": codes, "swept": swept})
    return 0 if place_ms else 1


def post_on(connection, path, body):
    """One signed request on the kept-alive tunnel, and the wall time of its own round trip."""
    headers = signed_headers("POST", path, body)
    start = time.perf_counter()
    connection.request("POST", path, body=body, headers=headers)
    payload = json.loads(connection.getresponse().read().decode())
    return (time.perf_counter() - start) * 1000.0, payload


def place_post_only(connection, inst, price, lot, resting, codes):
    """A `post_only` buy that has to rest, appended to `resting` when the venue answers with an id."""
    body = json.dumps({"instId": inst, "tdMode": "cross", "side": "buy", "ordType": "post_only",
                       "px": price, "sz": lot})
    _, payload = post_on(connection, "/api/v5/trade/order", body)
    row = (payload.get("data") or [{}])[0]
    if payload.get("code") == "0" and row.get("sCode") == "0" and row.get("ordId"):
        resting.append(row["ordId"])
        return row["ordId"]
    key = f"place {payload.get('code')}/{row.get('sCode')}"
    codes[key] = codes.get(key, 0) + 1
    return None


def cancel_quietly(connection, inst, order_id, resting, codes):
    """A cancel between samples: its own time is not a reading, but a failure is counted."""
    _, payload = post_on(connection, "/api/v5/trade/cancel-order",
                         json.dumps({"instId": inst, "ordId": order_id}))
    row = (payload.get("data") or [{}])[0]
    if payload.get("code") == "0" and row.get("sCode") == "0":
        if order_id in resting:
            resting.remove(order_id)
        return True
    key = f"cancel {payload.get('code')}/{row.get('sCode')}"
    codes[key] = codes.get(key, 0) + 1
    return False


def order_snapshot(inst, order_id):
    """The venue's own answer for one order: its state and its price.

    The price is how an amend is *checked* rather than trusted: an amend the venue acknowledges and does not apply
    would otherwise be read as a fast one.
    """
    query = urllib.parse.urlencode({"instId": inst, "ordId": order_id})
    payload = request("GET", "/api/v5/trade/order?" + query)
    row = (payload.get("data") or [{}])[0]
    return row.get("state") or "?", row.get("px") or ""


def amend_round_trip(connection, inst, rounds, gap_s):
    """Amend against cancel+place, in one process on one kept-alive tunnel.

    The ruler is in this module's docstring and it was written before the run: an amend that lands inside the
    single-round-trip band -- 75 to 85 ms here, where a placement is 77.5 ms and a cancel 78.0 -- halves a repricing
    decision and removes the moment the level has no quote in it. Above the band the venue is charging the amend a
    second round trip somewhere, and the direction stays unbuilt. The two legs alternate which goes first, so drift in
    the link over the run cannot favour either, and every price is two percent under the best bid, where a
    `post_only` order has to rest: a fill is not part of this measurement.
    """
    spec = inst_spec(inst)
    lot = one_lot(spec)
    bid, _ask = best_quote(inst)
    # Both prices from one integer tick count: a half-tick lands on a rounding boundary, where `round` answers to
    # even, and the target then comes out equal to the start -- which times an amend that asks for the price the
    # order already has. Measured once that way: it reads 82.6 ms and means nothing.
    start_tick = round(bid * 0.98 / spec["tickSz"])
    from_px = on_tick(start_tick * spec["tickSz"], spec)
    to_px = on_tick((start_tick + 1) * spec["tickSz"], spec)
    if to_px == from_px:
        raise SystemExit(f"{inst}: the amend target equals the start ({from_px}); a probe that asks for the same "
                         f"price times nothing")
    amend_ms, cancel_ms, place_ms, cancel_place_ms = [], [], [], []
    codes = {}
    states = {}
    moved, not_moved, declined = 0, 0, 0
    resting = []
    try:
        for round_index in range(rounds):
            legs = ("amend", "cancel_place") if round_index % 2 == 0 else ("cancel_place", "amend")
            for leg in legs:
                order_id = place_post_only(connection, inst, from_px, lot, resting, codes)
                if order_id is None:
                    continue
                if leg == "amend":
                    elapsed, payload = post_on(
                        connection, "/api/v5/trade/amend-order",
                        json.dumps({"instId": inst, "ordId": order_id, "newPx": to_px}))
                    row = (payload.get("data") or [{}])[0]
                    if payload.get("code") == "0" and row.get("sCode") == "0":
                        amend_ms.append(elapsed)
                        state, price = order_snapshot(inst, order_id)
                        if price == to_px:
                            moved += 1
                        else:
                            not_moved += 1
                        if state != "live":
                            states[state] = states.get(state, 0) + 1
                    else:
                        declined += 1
                        key = f"amend {payload.get('code')}/{row.get('sCode')}"
                        codes[key] = codes.get(key, 0) + 1
                    cancel_quietly(connection, inst, order_id, resting, codes)
                else:
                    cancel_elapsed, cancel_payload = post_on(
                        connection, "/api/v5/trade/cancel-order",
                        json.dumps({"instId": inst, "ordId": order_id}))
                    cancel_row = (cancel_payload.get("data") or [{}])[0]
                    cancel_ok = cancel_payload.get("code") == "0" and cancel_row.get("sCode") == "0"
                    if cancel_ok:
                        resting.remove(order_id)
                    else:
                        key = f"cancel {cancel_payload.get('code')}/{cancel_row.get('sCode')}"
                        codes[key] = codes.get(key, 0) + 1
                    place_elapsed, place_payload = post_on(
                        connection, "/api/v5/trade/order",
                        json.dumps({"instId": inst, "tdMode": "cross", "side": "buy", "ordType": "post_only",
                                    "px": to_px, "sz": lot}))
                    place_row = (place_payload.get("data") or [{}])[0]
                    place_ok = (place_payload.get("code") == "0" and place_row.get("sCode") == "0" and
                                bool(place_row.get("ordId")))
                    if place_ok:
                        resting.append(place_row["ordId"])
                    else:
                        key = f"place {place_payload.get('code')}/{place_row.get('sCode')}"
                        codes[key] = codes.get(key, 0) + 1
                    if cancel_ok and place_ok:
                        cancel_ms.append(cancel_elapsed)
                        place_ms.append(place_elapsed)
                        cancel_place_ms.append(cancel_elapsed + place_elapsed)
                        cancel_quietly(connection, inst, place_row["ordId"], resting, codes)
            time.sleep(gap_s)
    finally:
        # Nothing the probe placed may outlive it: a stray quote is a position waiting to happen.
        swept = 0
        for order_id in list(resting):
            if cancel_quietly(connection, inst, order_id, resting, codes):
                swept += 1
        swept_failed = len(resting)
    emit({"cmd": "amend", "instId": inst, "rounds": rounds, "lot": lot,
          "from_px": from_px, "to_px": to_px,
          "amend_ms": summary(amend_ms), "cancel_place_ms": summary(cancel_place_ms),
          "cancel_ms": summary(cancel_ms), "place_ms": summary(place_ms),
          "amended_and_moved": moved, "amend_not_moved": not_moved, "amend_declined": declined,
          "amend_states": states, "errors": codes, "swept": swept, "swept_failed": swept_failed})
    return 0 if amend_ms else 1


def timed(method, path, body=""):
    """One signed request and the wall-clock time it took, in milliseconds.

    `perf_counter` rather than the wall clock, because the answer's own timestamp is the venue's clock and this is the
    round trip: the interval from the last byte leaving this process to the first byte of the reply arriving.
    """
    start = time.perf_counter()
    payload = request(method, path, body)
    return (time.perf_counter() - start) * 1000.0, payload


def on_tick(price, spec):
    """The nearest price on the instrument's grid, formatted the way the venue states its tick."""
    decimals = len(spec["tickText"].split(".")[1]) if "." in spec["tickText"] else 0
    return f"{round(price / spec['tickSz']) * spec['tickSz']:.{decimals}f}"


def percentile(values, share):
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(round(share / 100.0 * (len(ordered) - 1)))))
    return round(ordered[index], 1)


def summary(values):
    if not values:
        return {"n": 0}
    return {"n": len(values), "p50": percentile(values, 50), "p95": percentile(values, 95),
            "min": round(min(values), 1), "max": round(max(values), 1)}


def one_lot(spec):
    return f"{max(spec['minSz'], spec['lotSz']):g}"


def order_state(inst, order_id):
    """The venue's word on one order: `live`, `filled`, `canceled`, or the code of a question it would not answer.

    This is where a `post_only` decline actually appears. The venue accepts a crossing post-only order with an `ordId`
    and *then* cancels it, so the placement reply says `code=0` and the decline is a state on the orders channel -- the
    same shape the engine's `post_only_cancels` counts. Counting the placement reply as an acceptance is how a probe
    reports 0 refusals on orders the venue refused.
    """
    query = urllib.parse.urlencode({"instId": inst, "ordId": order_id})
    payload = request("GET", "/api/v5/trade/order?" + query)
    row = (payload.get("data") or [{}])[0]
    return row.get("state") or f"code={payload.get('code')}/sCode={row.get('sCode')}"


def rtt(inst, rounds, gap_s, touch=False):
    """Round-trip time for a post-only placement and its cancel, and the venue's refusal rate for a crossing one.

    The price used for the measured leg is two percent below the best bid, so the order cannot be marketable and the
    venue has to answer with an `ordId` rather than a decline; that makes the interval the placement's, not a refusal's.
    The crossing leg is the same order priced at the ask, which `post_only` forbids: the venue's non-zero `sCode` is the
    decline this repository counts as `post_only_cancels`, so the second number is measured rather than assumed.
    """
    spec = inst_spec(inst)
    lot = one_lot(spec)
    bid, ask = best_quote(inst)
    resting = on_tick(bid * 0.98, spec)
    place_ms, cancel_ms, codes = [], [], {}
    leftovers = []
    for _ in range(rounds):
        body = json.dumps({"instId": inst, "tdMode": "cross", "side": "buy", "ordType": "post_only",
                           "px": resting, "sz": lot})
        elapsed, payload = timed("POST", "/api/v5/trade/order", body)
        row = (payload.get("data") or [{}])[0]
        key = f"{payload.get('code')}/{row.get('sCode')}"
        if payload.get("code") == "0" and row.get("sCode") == "0" and row.get("ordId"):
            place_ms.append(elapsed)
            order_id = row["ordId"]
            cancel_elapsed, cancel_payload = timed(
                "POST", "/api/v5/trade/cancel-order", json.dumps({"instId": inst, "ordId": order_id}))
            cancel_row = (cancel_payload.get("data") or [{}])[0]
            if cancel_payload.get("code") == "0" and cancel_row.get("sCode") == "0":
                cancel_ms.append(cancel_elapsed)
            else:
                codes["cancel " + f"{cancel_payload.get('code')}/{cancel_row.get('sCode')}"] = \
                    codes.get("cancel " + f"{cancel_payload.get('code')}/{cancel_row.get('sCode')}", 0) + 1
                leftovers.append(order_id)
        else:
            codes[key] = codes.get(key, 0) + 1
        time.sleep(gap_s)

    # The crossing leg: the same shape priced one tick *through* the ask, which `post_only` has to refuse.
    #
    # The refusal arrives as an accepted order that the venue then cancels, so the classification is the order's state
    # a second later -- `canceled` is a decline, `filled` is a venue that filled a taker anyway (which is what a demo
    # matching engine does when it ignores the flag), and `live` is a quote that rested because the book had moved.
    crossing = {"declined": 0, "filled": 0, "rested": 0, "unanswered": 0}
    crossing_codes = {}
    refused_codes = {}
    for _ in range(rounds):
        # Two probes, and they answer different questions. `--touch` quotes where the strategy quotes, so the order is
        # marketable only when the book moved inside the round trip: its decline rate is the stale-on-arrival rate under
        # this link, which is the number a `post_only` quote pays for a slow route. The default prices it one tick
        # through the ask, which asks the venue the same question with nothing left to chance -- and at 580 ms it
        # mostly answers "the price is stale", because five of six such orders rested instead of crossing.
        price = on_tick(bid, spec) if touch else on_tick(ask + spec["tickSz"], spec)
        body = json.dumps({"instId": inst, "tdMode": "cross", "side": "buy", "ordType": "post_only",
                           "px": price, "sz": lot})
        _, payload = timed("POST", "/api/v5/trade/order", body)
        row = (payload.get("data") or [{}])[0]
        if payload.get("code") == "0" and row.get("sCode") == "0" and row.get("ordId"):
            time.sleep(1.0)
            state = order_state(inst, row["ordId"])
            if state in ("canceled", "partially_filled_then_canceled"):
                crossing["declined"] += 1
            elif state.startswith("filled"):
                crossing["filled"] += 1
            elif state == "live":
                crossing["rested"] += 1
                leftovers.append(row["ordId"])
            else:
                crossing["unanswered"] += 1
                crossing_codes[state] = crossing_codes.get(state, 0) + 1
            if state not in ("live", "canceled", "filled", "partially_filled"):
                refused_codes[state] = refused_codes.get(state, 0) + 1
        else:
            key = f"{payload.get('code')}/{row.get('sCode')}"
            crossing_codes[key] = crossing_codes.get(key, 0) + 1
            refused_codes[key] = refused_codes.get(key, 0) + 1

    swept = 0
    for order_id in leftovers:
        if not order_id:
            continue
        payload = request("POST", "/api/v5/trade/cancel-order", json.dumps({"instId": inst, "ordId": order_id}))
        if payload.get("code") == "0":
            swept += 1

    emit({"cmd": "rtt", "probe": "touch" if touch else "through_ask", "instId": inst, "rounds": rounds,
          "lot": lot, "resting_px": resting,
          "place_ms": summary(place_ms), "cancel_ms": summary(cancel_ms),
          "crossing": dict(crossing, codes=crossing_codes), "placement_errors": refused_codes,
          "errors": codes, "swept": swept, "swept_failed": len(leftovers) - swept})
    return 0 if place_ms else 1


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
    if command == "rtt":
        instrument = sys.argv[2] if len(sys.argv) > 2 else INST
        rounds = int(sys.argv[3]) if len(sys.argv) > 3 else 12
        gap = float(sys.argv[4]) if len(sys.argv) > 4 else 0.25
        return rtt(instrument, rounds, gap)
    if command in ("touch",):
        instrument = sys.argv[2] if len(sys.argv) > 2 else INST
        rounds = int(sys.argv[3]) if len(sys.argv) > 3 else 12
        gap = float(sys.argv[4]) if len(sys.argv) > 4 else 0.25
        return rtt(instrument, rounds, gap, touch=True)
    if command == "warm":
        instrument = sys.argv[2] if len(sys.argv) > 2 else INST
        rounds = int(sys.argv[3]) if len(sys.argv) > 3 else 12
        gap = float(sys.argv[4]) if len(sys.argv) > 4 else 0.25
        connection = tunnel(urllib.parse.urlsplit(BASE).hostname)
        try:
            return warm_rtt(connection, instrument, rounds, gap)
        finally:
            connection.close()
    if command == "amend":
        instrument = sys.argv[2] if len(sys.argv) > 2 else INST
        rounds = int(sys.argv[3]) if len(sys.argv) > 3 else 12
        gap = float(sys.argv[4]) if len(sys.argv) > 4 else 0.25
        connection = tunnel(urllib.parse.urlsplit(BASE).hostname)
        try:
            return amend_round_trip(connection, instrument, rounds, gap)
        finally:
            connection.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())

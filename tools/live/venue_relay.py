#!/usr/bin/env python3
"""Relay a real venue's public market data to the engine: a client to the venue, a server to the engine.

This is the interchangeable half of the live path. It opens the venue's public stream (a **client**,
over `wss://`, no API key), decodes the venue's own message shapes into the replay CSV rows this
repository already reads, and re-serves those rows as plaintext `ws://` on localhost (a **server**),
where the C++ `WebSocketFeed` reads them exactly as it reads the local sender in tests. So the engine
does not know, and does not need to know, whether the row came from a file, from the local sender or
from a venue -- and swapping this Python relay for a C++ one that speaks `wss://` itself changes
nothing on the engine side.

The first venue is OKX's public channel, because it needs no key and its historical endpoint carries no
order book, so a live socket is its only keyless programmatic book route. Two channels are used:

    trades   one row per print:  exchange,symbol,timestamp,id,side,price,amount
    books    the incremental book: one row per level change,
             exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount

`books` opens with a full snapshot and then sends touched levels, where a size of zero removes one, and
every message carries `seqId` and `prevSeqId`, which is what a gap in the stream is detected against.
Sizes are contract counts, so a quantity is `round(size * contract_size * quantity_scale)`, the same
arithmetic `src/market/okx_readers.cpp` applies to the archive.

Usage:
    # serve the venue to the engine (start this first, then point the engine at it)
    python3 tools/live/venue_relay.py --instrument BTC-USDT-SWAP --listen 127.0.0.1:8765
    ./out/build/linux-debug/toy_quant ws 127.0.0.1:8765 BTC-USDT-SWAP active_l2 1 prorata --fast-validation

    # capture the venue's messages for an offline run, then serve them back with no network
    python3 tools/live/venue_relay.py --capture /tmp/okx.jsonl --max-seconds 60
    python3 tools/live/venue_relay.py --replay /tmp/okx.jsonl --listen 127.0.0.1:8765

    # a long run that exercises the keepalive and the reconnect path against the venue
    python3 tools/live/venue_relay.py --instrument BTC-USDT-SWAP --listen 127.0.0.1:8765 \
        --capture /tmp/okx.jsonl --max-seconds 7200 --ping-seconds 20 --drop-after-seconds 1500

    # the order channel: the engine's order leg, answered by a simulated sandbox venue -- no key, no money.
    # `tests/relay_order_channel_test` drives it with the real adapter and the real transport.
    python3 tools/live/venue_relay.py --simulate-orders --order-listen 127.0.0.1:8766

    # the same channel answered by the venue's own private stream: the three credentials are read from
    # TOYQUANT_API_KEY, TOYQUANT_API_SECRET and TOYQUANT_PASSPHRASE, never from the command line, because a
    # command line reaches a run's `[Mode]` line and every recording it writes.
    python3 tools/live/venue_relay.py --order-listen 127.0.0.1:8766

    python3 tools/live/venue_relay.py --self-test
"""

import argparse
import base64
import hashlib
import hmac
import io
import json
import math
import os
import queue
import socket
import ssl
import struct
import sys
import threading
import time
import urllib.error
import urllib.request
from collections import namedtuple
from datetime import datetime, timezone

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
DEFAULT_VENUE = "wss://ws.okx.com:8443/ws/v5/public"
DEFAULT_INSTRUMENT = "BTC-USDT-SWAP"
DEFAULT_CONTRACT_SIZE = 0.01
DEFAULT_QUANTITY_SCALE = 1000000
# The order leg's grid, for the instrument this relay was written for. A price moves in `tick_size` steps and a
# size in `lot_size` contracts; both are venue facts, so they are flags rather than arithmetic.
DEFAULT_TICK_SIZE = 0.1
DEFAULT_LOT_SIZE = 1
# The four numbers that turn an engine order into a venue order: one contract is `contract_size` of the coin,
# `quantity_scale` engine units are one of the coin, a price sits on `tick_size` and a size on `lot_size`.
OrderMapping = namedtuple("OrderMapping", "contract_size quantity_scale tick_size lot_size")
# OKX closes an idle public connection, and asks for a text ping when nothing has been sent for 30 s.
PING_AFTER_SECONDS = 25
# Rows in flight between the venue thread and the thread serving the engine. Bounded, because an engine
# that falls behind must slow the venue down rather than grow this process without limit; the wait is
# counted and printed, so a stalled consumer is visible in the run's own summary rather than only as RSS.
ROW_QUEUE_CAPACITY = 200000
# How often a session with a fixed-cadence ping looks at the clock. Small, because a ping that is due
# must not wait behind a socket read that has twenty-five seconds to time out.
PING_POLL_SECONDS = 0.25


# ---------------------------------------------------------------- WebSocket framing

class FrameDump:
    """Every frame both ways, one JSON object a line, written from whichever thread moved it.

    A run that heard nothing cannot say whether the venue was silent or this client was looking somewhere else, and the
    first sandbox order proved it: the venue had answered twice and the client had dropped both. The file is appended
    rather than truncated because a reconnect writes several sessions into it, and the order they crossed in is what a
    reader follows. Credentials never reach it: the login is written with its key, passphrase and signature redacted.
    """

    def __init__(self, handle=None):
        self.handle = handle
        self.lock = threading.Lock()
        self.frames = 0

    def record(self, direction, frame):
        if self.handle is None:
            return
        with self.lock:
            self.handle.write(json.dumps({"dir": direction, "frame": frame}) + "\n")
            self.handle.flush()
            self.frames += 1

    def close(self):
        if self.handle is not None:
            self.handle.close()


def encode_frame(payload, opcode=0x1):
    """One unmasked server-to-client frame; a server never masks."""
    header = bytearray()
    header.append(0x80 | opcode)
    length = len(payload)
    if length < 126:
        header.append(length)
    elif length < 65536:
        header.append(126)
        header += struct.pack(">H", length)
    else:
        header.append(127)
        header += struct.pack(">Q", length)
    return bytes(header) + payload


def send_masked(sock, text, opcode=0x1):
    """One masked client-to-server frame; a client always masks."""
    data = text.encode() if isinstance(text, str) else text
    mask = os.urandom(4)
    header = bytearray([0x80 | opcode])
    length = len(data)
    if length < 126:
        header.append(0x80 | length)
    elif length < 65536:
        header.append(0x80 | 126)
        header += struct.pack(">H", length)
    else:
        header.append(0x80 | 127)
        header += struct.pack(">Q", length)
    header += mask
    sock.sendall(bytes(header) + bytes(b ^ mask[i & 3] for i, b in enumerate(data)))


def recv_exact(sock, count):
    buffer = b""
    while len(buffer) < count:
        chunk = sock.recv(count - len(buffer))
        if not chunk:
            return None
        buffer += chunk
    return buffer


def read_frame(sock):
    """One frame off a venue connection, unmasked (a server never masks). None at close."""
    header = recv_exact(sock, 2)
    if header is None:
        return None
    fin = bool(header[0] & 0x80)
    opcode = header[0] & 0x0F
    masked = bool(header[1] & 0x80)
    length = header[1] & 0x7F
    if length == 126:
        length = struct.unpack(">H", recv_exact(sock, 2))[0]
    elif length == 127:
        length = struct.unpack(">Q", recv_exact(sock, 8))[0]
    mask = recv_exact(sock, 4) if masked else None
    payload = recv_exact(sock, length) if length else b""
    if payload is None:
        return None
    if mask:
        payload = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
    return fin, opcode, payload


def websocket_client(url):
    """Opens a `wss://` or `ws://` connection and completes the handshake."""
    secure = url.startswith("wss://")
    rest = url.split("://", 1)[1]
    host_port, _, path = rest.partition("/")
    host, _, port_text = host_port.partition(":")
    port = int(port_text) if port_text else (443 if secure else 80)
    path = "/" + path if path else "/"
    raw = socket.create_connection((host, port), timeout=15)
    sock = ssl.create_default_context().wrap_socket(raw, server_hostname=host) if secure else raw
    key = base64.b64encode(os.urandom(16)).decode()
    request = (
        f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
        f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
        f"User-Agent: Mozilla/5.0 toyquant-relay/1.0\r\n\r\n"
    )
    sock.sendall(request.encode())
    response = b""
    while b"\r\n\r\n" not in response:
        response += sock.recv(1)
    if b" 101 " not in response.split(b"\r\n", 1)[0]:
        raise RuntimeError("venue refused the websocket handshake: " + response.split(b"\r\n")[0].decode())
    return sock



# ---------------------------------------------------------------- decoding a venue message

TRADE_HEADER = "exchange,symbol,timestamp,id,side,price,amount\n"
DEPTH_HEADER = "exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount\n"


class Decoder:
    """One venue connection's message decoder: JSON in, replay CSV rows out.

    It also owns the sequence bookkeeping, because `books` carries the pair `prevSeqId`/`seqId` and a
    break between them is the one thing a live book cannot recover from on its own -- it is a lost
    update, and the count is what "recorded for hours without a drop" is measured against.
    """

    def __init__(self, instrument, contract_size, quantity_scale, max_levels):
        self.exchange = "okx"
        self.instrument = instrument
        self.contract_size = contract_size
        self.quantity_scale = quantity_scale
        self.max_levels = max_levels
        self.last_seq = None
        self.trades = 0
        self.depth_rows = 0
        self.book_messages = 0
        self.gaps = 0

    def amount(self, size_text):
        return int(round(float(size_text) * self.contract_size * self.quantity_scale))

    def decode(self, text):
        """The rows for one venue message; empty for an event, a ping, or a channel we ignore."""
        try:
            message = json.loads(text)
        except ValueError:
            return []
        arg = message.get("arg", {})
        channel = arg.get("channel")
        symbol = arg.get("instId", self.instrument)
        if channel == "trades":
            return self._trades(message, symbol)
        if channel in ("books", "books5", "bbo-tbt"):
            return self._books(message, symbol)
        return []

    def _trades(self, message, symbol):
        rows = []
        for trade in message.get("data", []):
            ts = trade.get("ts")
            if ts is None:
                continue
            rows.append(
                f"{self.exchange},{symbol},{ts},{trade.get('tradeId','')},"
                f"{trade.get('side','')},{trade.get('px','')},{self.amount(trade.get('sz','0'))}\n"
            )
            self.trades += 1
        return rows

    def _books(self, message, symbol):
        action = message.get("action", "snapshot")
        is_snapshot = 1 if action == "snapshot" else 0
        rows = []
        for book in message.get("data", []):
            seq = book.get("seqId")
            prev = book.get("prevSeqId")
            if is_snapshot:
                self.last_seq = seq
            else:
                # A gap is a missing update: the book's `prevSeqId` is not the `seqId` we last saw.
                if self.last_seq is not None and prev is not None and int(prev) != int(self.last_seq):
                    self.gaps += 1
                self.last_seq = seq
            ts = book.get("ts", "0")
            # A snapshot is capped to the levels the engine uses; an update is sparse already and is
            # passed through whole so a level the snapshot kept is never left stale.
            for side, levels in (("bid", book.get("bids", [])), ("ask", book.get("asks", []))):
                if is_snapshot and self.max_levels and len(levels) > self.max_levels:
                    levels = levels[: self.max_levels]
                for level in levels:
                    price, size = level[0], level[1]
                    rows.append(
                        f"{self.exchange},{symbol},{ts},{ts},{is_snapshot},{side},"
                        f"{price},{self.amount(size)}\n"
                    )
                    self.depth_rows += 1
            self.book_messages += 1
        return rows

    def summary(self):
        return (
            f"trades={self.trades} book_messages={self.book_messages} depth_rows={self.depth_rows} "
            f"gaps={self.gaps}"
        )


class Recorder:
    """The direct route: the decoded rows written straight to the replay CSV pair, no relay and no
    engine. It is the counterpart of the relay's server role -- one connection to the venue, the same
    decoder, the same rows -- and it is what makes a live window a file this repository can replay."""

    def __init__(self, prefix):
        self.trades = open(prefix + "_trades.csv", "w", encoding="utf-8")
        self.depth = open(prefix + "_depth.csv", "w", encoding="utf-8")
        self.trades.write(TRADE_HEADER)
        self.depth.write(DEPTH_HEADER)
        self.rows = 0

    def write(self, row):
        # A trade row has seven fields, a depth row eight, which is how the reader tells them apart.
        (self.trades if row.count(",") == 6 else self.depth).write(row)
        self.rows += 1
        # A long recording is flushed as it goes, so a session that is stopped rather than closed loses
        # at most this many rows instead of whatever the file object was holding.
        if self.rows % 4096 == 0:
            self.trades.flush()
            self.depth.flush()

    def close(self):
        self.trades.close()
        self.depth.close()


# ---------------------------------------------------------------- the venue client and the local server

class VenueClient(threading.Thread):
    """A client to the venue: subscribe, then decode every message into the row queue."""

    def __init__(self, url, instrument, books_channel, decoder, rows, capture=None, max_seconds=0,
                 enqueue=True, recorder=None, ping_seconds=0.0, drop_after_seconds=0.0):
        super().__init__(daemon=True)
        self.url = url
        self.instrument = instrument
        self.books_channel = books_channel
        self.decoder = decoder
        self.rows = rows
        self.capture = capture
        self.max_seconds = max_seconds
        self.enqueue = enqueue
        self.recorder = recorder
        self.ping_seconds = ping_seconds
        self.drop_after_seconds = drop_after_seconds
        self.stop = threading.Event()
        self.connected = False
        self.reconnects = 0
        # Backpressure and heartbeat, both of which a six second run never exercises.
        self.queue_waits = 0
        self.queue_high_water = 0
        self.pings_sent = 0
        self.pongs_sent = 0
        # A pong the venue sent back. Non-zero proves the keepalive completed a round trip rather than
        # merely being written to a socket the venue had already stopped reading.
        self.pongs_received = 0

    def subscription(self):
        return json.dumps({
            "op": "subscribe",
            "args": [{"channel": self.books_channel, "instId": self.instrument},
                     {"channel": "trades", "instId": self.instrument}],
        })

    def enqueue_row(self, row):
        """Hands a row to the server thread, waiting for room rather than dropping it.

        A full queue is backpressure: the venue connection stops being read until the engine catches
        up, which is the same rule the engine's own rings follow. The wait and the high-water mark are
        counted so a slow consumer is visible in the summary instead of only as growing memory.
        """
        while not self.stop.is_set():
            try:
                self.rows.put(row, timeout=0.2)
                self.queue_high_water = max(self.queue_high_water, self.rows.qsize())
                return
            except queue.Full:
                self.queue_waits += 1

    def run(self):
        """Connect, and reconnect with a backoff after a drop, until stopped or out of time."""
        deadline = time.time() + self.max_seconds if self.max_seconds else None
        sink = open(self.capture, "a") if self.capture else None
        backoff = 1.0
        try:
            while not self.stop.is_set():
                if deadline is not None and time.time() >= deadline:
                    break
                started = time.monotonic()
                try:
                    self.session(sink, deadline)
                    backoff = 1.0
                except (OSError, RuntimeError) as error:
                    if self.stop.is_set():
                        break
                    self.reconnects += 1
                    # A session that ran for a while is a healthy connection that dropped, not a venue
                    # that refuses to accept one; it starts the backoff over instead of inheriting the
                    # delay of an earlier failure.
                    if time.monotonic() - started >= 30.0:
                        backoff = 1.0
                    print(f"venue connection lost ({error}); reconnecting in {backoff:.0f}s", flush=True)
                if self.stop.is_set():
                    break
                if deadline is not None and time.time() >= deadline:
                    break
                time.sleep(backoff)
                backoff = min(backoff * 2.0, 8.0)
        finally:
            if sink is not None:
                sink.close()
            if self.enqueue:
                self.rows.put(None)  # end of stream, so a waiting server stops cleanly

    def session(self, sink, deadline):
        """One venue connection, handshake to drop. Returns at a clean close or the deadline; a
        transport failure is raised so run() can reconnect. A reconnect is safe for the book because
        the venue re-sends a full snapshot on subscribe, which resets the decoder's sequence."""
        sock = websocket_client(self.url)
        self.connected = True
        session_start = time.monotonic()
        last_ping = session_start
        try:
            send_masked(sock, self.subscription())
            # A fixed-cadence ping has to look at the clock between frames, because a busy stream never
            # lets the read time out; the idle keepalive keeps the long timeout it always had.
            sock.settimeout(PING_POLL_SECONDS if self.ping_seconds > 0 else PING_AFTER_SECONDS)
            while not self.stop.is_set():
                if deadline is not None and time.time() >= deadline:
                    return
                if (self.drop_after_seconds > 0 and
                        time.monotonic() - session_start >= self.drop_after_seconds):
                    # A deliberate drop, so the reconnect path is exercised against the real venue and
                    # not only against a local stand-in. run() counts it and backs off like any other
                    # lost connection.
                    raise RuntimeError(
                        f"dropped after {self.drop_after_seconds:.0f}s for the reconnect test")
                if self.ping_seconds > 0:
                    now = time.monotonic()
                    if now - last_ping >= self.ping_seconds:
                        send_masked(sock, "ping")
                        self.pings_sent += 1
                        last_ping = now
                try:
                    frame = read_frame(sock)
                except socket.timeout:
                    if self.ping_seconds <= 0:
                        send_masked(sock, "ping")  # keep an idle public connection alive
                        self.pings_sent += 1
                        last_ping = time.monotonic()
                    continue
                if frame is None:
                    return
                _, opcode, payload = frame
                if opcode == 0x8:
                    return
                if opcode == 0x9:
                    # A control ping is owed a control pong with the same payload, masked as a client's
                    # frames must be. A session that answers neither this nor the text form is closed
                    # by the venue once its heartbeat times out, which is exactly the failure a six
                    # second run cannot see.
                    send_masked(sock, payload, opcode=0xA)
                    self.pongs_sent += 1
                    continue
                if opcode == 0xA:
                    self.pongs_received += 1
                    continue
                if opcode not in (0x1, 0x0):
                    continue
                text = payload.decode(errors="replace")
                token = text.strip()
                # OKX's heartbeat is a text frame rather than a control frame, and it is answered the
                # same way: a pong for a ping, and neither is a market message to record.
                if token == "ping":
                    send_masked(sock, "pong")
                    self.pongs_sent += 1
                    continue
                if token == "pong":
                    self.pongs_received += 1
                    continue
                # A subscription acknowledgement is protocol, not market data; it does not belong in a
                # capture that is meant to be replayable as a feed.
                if '"event"' in text:
                    continue
                if sink is not None:
                    sink.write(text + "\n")
                    sink.flush()
                for row in self.decoder.decode(text):
                    if self.recorder is not None:
                        self.recorder.write(row)
                    if self.enqueue:
                        self.enqueue_row(row)
        finally:
            sock.close()


def server_handshake(conn):
    """Answer one opening handshake; False if the peer hung up before sending one.

    The other side of `websocket_client`, and the same three lines whether the peer is the engine or the
    self-test's stand-in venue: read the request, hash its key with the RFC 6455 GUID, answer 101.
    """
    request = b""
    while b"\r\n\r\n" not in request:
        chunk = conn.recv(4096)
        if not chunk:
            return False
        request += chunk
    key = ""
    for line in request.decode("latin-1").split("\r\n"):
        if line.lower().startswith("sec-websocket-key:"):
            key = line.split(":", 1)[1].strip()
    accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
    conn.sendall((
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
        f"Connection: Upgrade\r\nSec-WebSocket-Accept: {accept}\r\n\r\n"
    ).encode("latin-1"))
    return True


def accept_client(listen):
    """Blocks until the engine connects, completes the handshake, and returns the socket."""
    host, _, port = listen.partition(":")
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((host, int(port)))
    listener.listen(1)
    print(f"relay listening on ws://{listen}, waiting for the engine ...", flush=True)
    conn, peer = listener.accept()
    if not server_handshake(conn):
        raise RuntimeError("the engine closed before the opening handshake")
    print(f"engine connected from {peer[0]}:{peer[1]}", flush=True)
    return listener, conn


def stream(conn, rows, stop):
    """Forwards rows to the engine until the source ends or the engine goes away."""
    conn.sendall(encode_frame(TRADE_HEADER.encode()))
    conn.sendall(encode_frame(DEPTH_HEADER.encode()))
    sent = 0
    while True:
        try:
            row = rows.get(timeout=0.2)
        except queue.Empty:
            if stop.is_set():
                break
            continue
        if row is None:
            break
        try:
            conn.sendall(encode_frame(row.encode()))
        except OSError:
            break
        sent += 1
    try:
        conn.sendall(encode_frame(b"", opcode=0x8))
    except OSError:
        pass
    return sent



# ---------------------------------------------------------------- the order channel

# The adapter's protocol -- `src/lines/live_venue.h` -- on the engine's side, the venue's JSON on the other.
# The adapter is deliberately ignorant of the venue: it knows an order, a cancel and four replies. This is
# the only place that knows both what the engine sent and what the venue calls it, which is why the
# translation lives here rather than in the engine (ADR 0005, decision 1).


class OrderError(ValueError):
    """A line the channel refuses, with the reason the engine is told."""


def _positive(text, what):
    try:
        value = float(text)
    except ValueError as error:
        raise OrderError(f"{what} '{text}' is not a number") from error
    if not value > 0:
        raise OrderError(f"{what} '{text}' is not positive")
    return value


class OrderTranslator:
    """The engine's lines in, the venue's messages out, and the venue's answers back.

    One translation each way, used by both routes. The simulated venue answers in the venue's own shapes and
    its answers go back through `decode`, so the round trip is exercised with no key, no endpoint and no
    money; a real session replaces only where the messages come from -- the venue's private channel instead
    of the simulator -- and these two translations stay as they are.
    """

    def __init__(self, instrument, inst_code=None, mapping=None):
        self.instrument = instrument
        # The venue's internal code for the instrument. Newer order operations ask for it alongside `instId` and
        # refuse with `50014 "Parameter instIdCode can not be empty"` without it; it is a number the operator reads
        # from the venue's public instruments endpoint, and it is not derived here because a wrong guess would be an
        # order on the wrong contract.
        self.inst_code = inst_code
        # The venue's grid and the engine's scale. Every number is a flag for the same reason `inst_code` is: a
        # wrong guess here is an order of the wrong size at a price the venue refuses.
        self.mapping = mapping or OrderMapping(DEFAULT_CONTRACT_SIZE, DEFAULT_QUANTITY_SCALE,
                                               DEFAULT_TICK_SIZE, DEFAULT_LOT_SIZE)
        # What the channel has been told, keyed by the client order id: a cancel needs the instrument it was
        # placed on, and the `orders` channel repeats a cumulative fill count, which the de-duplication reads.
        self.orders = {}
        # Messages that repeated a fill the engine had already been told about. Printed rather than swallowed: a
        # reconnect re-sends the current state of every live order, so this number being zero on a session with a
        # drop would be the surprising case.
        self.fills_deduplicated = 0

    def units_of(self, contracts):
        """The venue's contracts as engine units -- `Decoder.amount` read the other way."""
        return int(round(contracts * self.mapping.contract_size * self.mapping.quantity_scale))

    def contracts_for(self, units):
        """The engine's units as the venue's contracts, rounded down to the venue's lot.

        One contract is `contract_size` of the coin, so a contract is `contract_size * quantity_scale` engine
        units. Rounded down rather than to nearest, because a size above what the strategy asked for is the
        direction that turns a demonstration into a position; a size that rounds to nothing is refused here,
        where the reason can name both numbers, rather than at the venue as a rejection.
        """
        per_contract = self.mapping.contract_size * self.mapping.quantity_scale
        contracts = int(units // per_contract)
        if self.mapping.lot_size > 1:
            contracts -= contracts % self.mapping.lot_size
        if contracts <= 0:
            raise OrderError(
                f"quantity {units} engine units is {units / self.mapping.quantity_scale:g} of the coin, below one "
                f"contract of {self.mapping.contract_size:g} ({per_contract:g} units), so there is nothing to place")
        return contracts

    def price_on_tick(self, text):
        """The engine's price on the venue's grid, as a decimal the venue accepts.

        The replay's book is priced on the instrument's tick, and the venue's is the same instrument -- but a
        price one step off its grid is refused (`51006`) rather than rounded by the venue, and the engine's
        double can carry digits the grid does not have. Rounding is to the nearest step, which is the smallest
        change the venue can represent rather than a view on the price.
        """
        step = self.mapping.tick_size
        steps = round(float(text) / step)
        decimals = max(0, -int(math.floor(math.log10(step)))) if step < 1 else 0
        return f"{steps * step:.{decimals}f}"

    def encode(self, line):
        fields = line.split()
        if not fields:
            raise OrderError("the line is empty")
        if fields[0] == "ORDER":
            if len(fields) != 7:
                raise OrderError(f"an order line has seven fields and this one has {len(fields)}")
            _, cl_ord_id, symbol, side, price, quantity, tif = fields
            if not cl_ord_id.isdigit():
                raise OrderError(f"the order id '{cl_ord_id}' is not a number")
            if side not in ("B", "S"):
                raise OrderError(f"side '{side}' is not B or S")
            _positive(price, "price")
            if not quantity.isdigit() or int(quantity) == 0:
                raise OrderError(f"quantity '{quantity}' is not a positive integer")
            if tif not in ("Gtc", "Ioc", "Fok"):
                raise OrderError(f"time in force '{tif}' is not Gtc, Ioc or Fok")
            units = int(quantity)
            contracts = self.contracts_for(units)
            self.orders[cl_ord_id] = {"symbol": symbol, "units": units, "contracts": contracts,
                                      "filled": 0, "terminal": False}
            # OKX's shape for a limit order, with both numbers on the venue's grid: `sz` in contracts (one is
            # `contract_size` of the coin, so the engine's `quantity_scale` and the contract's value are the two
            # halves of the conversion) and `px` on the tick. The instrument is the relay's, not the engine's: the
            # replay runs on whatever instrument its book came from, and the venue only knows its own name for the
            # thing, so the operator names it once on `--instrument`.
            order = {"instId": self.instrument, "tdMode": "cross",
                     "side": "buy" if side == "B" else "sell",
                     "ordType": "limit", "px": self.price_on_tick(price), "sz": str(contracts),
                     "clOrdId": cl_ord_id}
            if self.inst_code is not None:
                order["instIdCode"] = self.inst_code
            return {"id": cl_ord_id, "op": "order", "args": [order]}
        if fields[0] == "CANCEL":
            if len(fields) != 2:
                raise OrderError(f"a cancel line has two fields and this one has {len(fields)}")
            cl_ord_id = fields[1]
            if not cl_ord_id.isdigit():
                raise OrderError(f"the order id '{cl_ord_id}' is not a number")
            return {"id": cl_ord_id, "op": "cancel-order", "args": [dict(
                {"instId": self.instrument, "clOrdId": cl_ord_id},
                **({"instIdCode": self.inst_code} if self.inst_code is not None else {}))]}
        raise OrderError(f"'{fields[0]}' is not an order or a cancel")

    def decode(self, message):
        """The venue's message as the adapter's lines: ACK, FILL, CANCELED or REJECT."""
        if not isinstance(message, dict):
            raise OrderError("a venue message is not an object")
        operation = message.get("op")
        # A request the venue refused answers here, at the top level, with an empty `data` list -- so a decoder
        # that only read the entries would turn "parameter id can not be empty" into a silence the engine sees as a
        # venue that ignored the order. The id it echoes back is the one the request carried.
        if operation in ("order", "cancel-order") and str(message.get("code", "0")) != "0":
            order_id = str(message.get("id", "")).strip()
            if not order_id.isdigit():
                raise OrderError(f"the venue refused a request without an id the engine knows: {message}")
            return [f"REJECT {order_id} venue"]
        argument = message.get("arg") if isinstance(message.get("arg"), dict) else {}
        channel = argument.get("channel")
        lines = []
        for entry in message.get("data", []):
            cl_ord_id = str(entry.get("clOrdId", ""))
            if not cl_ord_id.isdigit():
                raise OrderError(f"the venue's message carries no order id the engine knows: {entry}")
            if operation == "order":
                if str(entry.get("sCode", "0")) != "0":
                    lines.append(f"REJECT {cl_ord_id} venue")
                    continue
                known = self.orders.setdefault(cl_ord_id, {"units": 0, "contracts": 0, "filled": 0})
                placed = int(entry.get("sz", known.get("contracts", 0)) or 0)
                known["contracts"] = placed
                # The ACK says what is still working. The cumulative it carries is the venue's count, not one the
                # engine has booked -- an ACK is not a fill -- so it is deliberately not recorded as delivered: the
                # `orders` push that follows reports the same cumulative, and that is where the fill is booked, once.
                already = int(entry.get("accFillSz", 0) or 0)
                lines.append(f"ACK {cl_ord_id} {self.units_of(max(placed - already, 0))}")
            elif operation == "cancel-order":
                # The acknowledgement of a cancel is the venue saying it will act; the state message that
                # follows is the order actually ending, and that is the one that becomes CANCELED.
                if str(entry.get("sCode", "0")) != "0":
                    lines.append(f"REJECT {cl_ord_id} venue")
            elif channel == "orders":
                # A stream of states, not of events: the venue re-sends an order's current state on every change and
                # again after a reconnect, and `accFillSz` is cumulative over the order's life. The rule is one fill
                # per increment of that counter, so a repeated state is not a second fill and a reconnect is not a
                # second session -- without it a fill booked twice is a position the account never held.
                #
                # The increment is priced at this message's `fillPx`, the last price the venue traded at: a push that
                # merges several prints carries no per-print price, and inventing one would be worse than naming the
                # limit here.
                known = self.orders.setdefault(cl_ord_id, {"units": 0, "contracts": 0, "filled": 0})
                previous = int(known.get("filled", 0))
                # A state change the venue sends for an order that did not trade carries no `accFillSz` at all --
                # the cancel path is the common one -- so absent means "no new fill", not "no fill ever".
                reported = entry.get("accFillSz")
                cumulative = previous if reported in (None, "") else int(reported)
                if cumulative < previous:
                    raise OrderError(f"the venue's fill count for order {cl_ord_id} went backwards: "
                                     f"{previous} contracts seen, {cumulative} reported")
                increment = cumulative - previous
                emitted = False
                if increment > 0 and entry.get("fillPx"):
                    lines.append(f"FILL {cl_ord_id} {entry['fillPx']} {self.units_of(increment)}")
                    known["filled"] = cumulative
                    emitted = True
                state = entry.get("state")
                if state == "canceled" and not known.get("terminal"):
                    lines.append(f"CANCELED {cl_ord_id} user")
                    known["terminal"] = True
                    emitted = True
                elif state == "rejected" and not known.get("terminal"):
                    lines.append(f"REJECT {cl_ord_id} venue")
                    known["terminal"] = True
                    emitted = True
                elif state == "filled":
                    known["terminal"] = True
                if not emitted:
                    self.fills_deduplicated += 1
            else:
                raise OrderError(f"the venue's message is not one this channel knows: {sorted(message)}")
        return lines


class SimulatedVenue:
    """The venue's side of the order channel with no key, no endpoint and no money.

    It answers in the venue's own message shapes and the channel decodes those through
    `OrderTranslator.decode`, so what the engine receives is a translation of a venue message rather than a
    branch of the channel that prints frames. The behaviour is deterministic -- half the order fills at its
    own price on arrival, anything above the cap is refused -- because a demonstration whose replies were
    drawn from a distribution could not be reproduced from its command line.
    """

    def __init__(self, max_quantity=100):
        self.max_quantity = max_quantity

    def messages(self, request):
        args = request.get("args") or [{}]
        argument = args[0]
        cl_ord_id = str(argument.get("clOrdId", ""))
        if request.get("op") == "order":
            quantity = int(argument.get("sz", "0"))
            if quantity > self.max_quantity:
                return [{"op": "order", "data": [{"clOrdId": cl_ord_id, "sCode": "51008",
                                                  "sMsg": "order size above the sandbox cap"}]}]
            acknowledgement = {"op": "order",
                               "data": [{"clOrdId": cl_ord_id, "ordId": cl_ord_id, "sCode": "0", "sMsg": "",
                                         "sz": str(quantity), "accFillSz": "0"}]}
            filled = quantity // 2 if quantity >= 2 else quantity
            state = "partially_filled" if filled < quantity else "filled"
            fill = {"arg": {"channel": "orders"},
                    "data": [{"clOrdId": cl_ord_id, "state": state, "fillPx": argument.get("px"),
                              "fillSz": str(filled), "accFillSz": str(filled), "sz": str(quantity)}]}
            return [acknowledgement, fill]
        if request.get("op") == "cancel-order":
            return [{"op": "cancel-order", "data": [{"clOrdId": cl_ord_id, "sCode": "0"}]},
                    {"arg": {"channel": "orders"},
                     "data": [{"clOrdId": cl_ord_id, "state": "canceled"}]}]
        return [{"op": "order", "data": [{"clOrdId": cl_ord_id, "sCode": "50011",
                                          "sMsg": "operation not supported"}]}]


def serve_orders(listen, instrument, max_quantity=100, capture=None, mapping=None):
    """Serves the engine's order leg on a line socket, one frame a line, until the engine goes away.

    The same shape as `accept_client` and `stream`, minus the websocket: `LiveVenue`'s transport speaks lines,
    which is what `TcpVenueTransport` is (ADR 0005, step two). What the venue does with a message is the one
    thing that changes between the simulation and an endpoint; the translation above does not.
    """
    host, _, port = listen.partition(":")
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((host, int(port)))
    listener.listen(1)
    # Printed rather than assumed: a caller can ask for port 0 and read the port back, so it never has to
    # guess which numbers are free on the host.
    print(f"order channel listening on {host}:{listener.getsockname()[1]}", flush=True)
    conn, peer = listener.accept()
    print(f"engine connected from {peer[0]}:{peer[1]}", flush=True)
    translator = OrderTranslator(instrument, mapping=mapping)
    venue = SimulatedVenue(max_quantity)
    sink = open(capture, "a", encoding="utf-8") if capture else None
    orders = cancels = replies = refused = 0
    pending = b""
    try:
        while True:
            chunk = conn.recv(4096)
            if not chunk:
                break
            pending += chunk
            while b"\n" in pending:
                line, _, pending = pending.partition(b"\n")
                text = line.decode(errors="replace").strip()
                if not text:
                    continue
                try:
                    request = translator.encode(text)
                except OrderError as error:
                    # A line the channel cannot translate is answered, not dropped: the adapter counts an
                    # unreadable frame, and silence would look exactly like a venue that ignored the order.
                    token = text.split()[1] if len(text.split()) > 1 and text.split()[1].isdigit() else "0"
                    conn.sendall(f"REJECT {token} venue\n".encode())
                    print(f"refused '{text}': {error}", flush=True)
                    refused += 1
                    continue
                if text.startswith("ORDER"):
                    orders += 1
                else:
                    cancels += 1
                if sink is not None:
                    sink.write(json.dumps(request) + "\n")
                    sink.flush()
                for message in venue.messages(request):
                    for reply in translator.decode(message):
                        conn.sendall(reply.encode() + b"\n")
                        replies += 1
    finally:
        if sink is not None:
            sink.close()
        conn.close()
        listener.close()
    print(f"order channel done: orders={orders} cancels={cancels} replies={replies} refused={refused} "
          f"deduplicated={translator.fills_deduplicated}", flush=True)
    return 0


# ---------------------------------------------------------------- credentials and the private route

def serve_venue_orders(listen, url, credentials, instrument, capture=None, dump=None, inst_type="ANY",
                       subscribe_orders=True, inst_code=None, rest_url=None, mapping=None):
    """Answers the engine's order leg from the venue: placement over REST, state changes over the private channel.

    The engine's side is exactly what the simulation serves -- a line socket, one frame a line -- so the adapter
    and its transport are unchanged and the only thing that moved is where the venue's messages come from. The
    summary at the end carries the session counts, because "it worked" in a live run means the login happened
    again after every drop and the subscription was remade, not that the first one succeeded.

    With `rest_url` the requests are placed by `RestPlacer` and no websocket session is opened at all: the demo
    venue's websocket trade operation answers `50026` while its REST order endpoint places the same order and
    returns an id. The `orders` channel -- fills, state changes -- is therefore not subscribed on this route, which
    is the next piece rather than a hidden one.
    """
    host, _, port = listen.partition(":")
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((host, int(port)))
    listener.listen(1)
    # Printed rather than assumed, for the same reason the simulation prints it: a caller that asked for port 0
    # reads the port back instead of guessing which numbers are free on the host.
    print(f"order channel listening on {host}:{listener.getsockname()[1]}", flush=True)
    conn, peer = listener.accept()
    print(f"engine connected from {peer[0]}:{peer[1]}")
    translator = OrderTranslator(instrument, inst_code, mapping)
    sink = open(capture, "a", encoding="utf-8") if capture else None
    # The raw frames, appended rather than truncated: a run that reconnects writes several sessions into one
    # file, and the order they crossed in is the thing being read.
    dump_sink = FrameDump(open(dump, "a", encoding="utf-8") if dump else None)
    stop = threading.Event()
    orders = cancels = replies = refused = 0

    def deliver(lines):
        nonlocal replies
        for line in lines:
            try:
                conn.sendall(line.encode() + b"\n")
            except OSError:
                # The engine went away mid-report. There is nobody left to answer, so the route stops rather
                # than counting lines nobody received.
                stop.set()
                return
            replies += 1

    # Two routes, one loop: a REST placement answers immediately on this thread, and the websocket route hands each
    # request to the client thread and its answers arrive on the receive thread. With `rest_url` no websocket is
    # opened, so there is no `orders` channel here to receive a fill on -- the next piece, and written down as such.
    placer = RestPlacer(rest_url, credentials, translator, dump=dump_sink) if rest_url else None
    client = None
    if placer is None:
        client = OrderVenueClient(url, credentials, translator, deliver, stop=stop, dump=dump_sink,
                                  inst_type=inst_type, subscribe_orders=subscribe_orders)
        client.start()
    pending = b""
    try:
        while not stop.is_set():
            try:
                chunk = conn.recv(4096)
            except OSError:
                break
            if not chunk:
                break
            pending += chunk
            while b"\n" in pending:
                line, _, pending = pending.partition(b"\n")
                text = line.decode(errors="replace").strip()
                if not text:
                    continue
                try:
                    request = translator.encode(text)
                except OrderError as error:
                    # Answered, not dropped, for the reason the simulation answers: silence would look exactly
                    # like a venue that ignored the order.
                    token = text.split()[1] if len(text.split()) > 1 and text.split()[1].isdigit() else "0"
                    deliver([f"REJECT {token} venue"])
                    print(f"refused '{text}': {error}", flush=True)
                    refused += 1
                    continue
                if text.startswith("ORDER"):
                    orders += 1
                else:
                    cancels += 1
                if sink is not None:
                    sink.write(json.dumps(request) + "\n")
                    sink.flush()
                if placer is not None:
                    deliver(placer.send(request))
                else:
                    client.submit(request)
    finally:
        stop.set()
        if client is not None:
            client.close()
            client.join(timeout=2.0)
        if sink is not None:
            sink.close()
        if dump_sink is not None:
            dump_sink.close()
        conn.close()
        listener.close()
    if placer is not None:
        print(f"order channel done (REST placement): orders={orders} cancels={cancels} replies={replies} "
              f"refused={refused} deduplicated={translator.fills_deduplicated} placed={placer.placed} "
              f"venue_refusals={placer.refused} "
              f"transport_failures={placer.failures} dumped={dump_sink.frames}", flush=True)
        return 0
    print(f"order channel done: orders={orders} cancels={cancels} replies={replies} refused={refused} "
          f"deduplicated={translator.fills_deduplicated} "
          f"logins={client.logins} subscribes={client.subscribes} reconnects={client.reconnects} "
          f"messages={client.messages} other_instruments={client.other_instruments} "
          f"unreadable={client.unreadable} write_failures={client.write_failures} "
          f"dumped={client.dump_frames}", flush=True)
    return 0


# The private channel's login signs `timestamp + "GET" + path`: the signature authenticates the request line of
# the handshake it travels in, and no GET is ever made to that path -- the signature *is* the request. Both
# halves of the preimage are the venue's, which is why they are named here instead of inlined at the call.
VERIFY_PATH = "/users/self/verify"
# The venue's demo endpoint: the same calls and the same order lifecycle as production, with no money (ADR 0005,
# decision 2). Measured on 2026-11-13 against the venue itself: the private socket accepts the login and the `orders`
# subscription on port 443, and the venue sends a `notice` on 8443 saying that port will be retired -- which is why
# this is 443 and not the 8443 a reader may remember. `--order-venue` overrides it.
DEFAULT_PRIVATE_VENUE = "wss://wspap.okx.com:443/ws/v5/private?brokerId=9999"


class VenueCredentials:
    """The three environment variables a private channel needs, and a repr that keeps the secret out.

    A credential that reaches a traceback, a log line or a printed command has to be revoked, and this process
    prints its own summary into a run's log. So the values are read once, from the environment, are never part of
    a message this file sends anywhere but the login, and the repr -- which is what an accidental print gets -- is
    masked. The names are the tool's own: the engine's parser has no credential flag, and ADR 0005 decision 3 is
    the reason a command line cannot carry one.
    """

    VARIABLES = ("TOYQUANT_API_KEY", "TOYQUANT_API_SECRET", "TOYQUANT_PASSPHRASE")

    def __init__(self, api_key, secret, passphrase):
        if not api_key or not secret or not passphrase:
            raise OrderError("a venue credential needs a key, a secret and a passphrase, and one is empty")
        self.api_key = api_key
        self.secret = secret
        self.passphrase = passphrase

    @classmethod
    def from_environment(cls, environ=None):
        """Read the three variables, or refuse with the ones that are missing named.

        Naming the missing variable is the whole point: `--order-listen` without `--simulate-orders` now has a
        backend, and a run that has not been given the variables has to be told which one to set rather than
        told that the route does not exist.
        """
        values = os.environ if environ is None else environ
        missing = [name for name in cls.VARIABLES if not values.get(name)]
        if missing:
            raise OrderError("the private channel needs " + ", ".join(missing) + " in the environment")
        return cls(values[cls.VARIABLES[0]], values[cls.VARIABLES[1]], values[cls.VARIABLES[2]])

    @staticmethod
    def _masked(value):
        # Two characters of a key are enough to tell two credentials apart in a log and useless to anyone else.
        return "***" if len(value) <= 4 else value[:2] + "***"

    def __repr__(self):
        return (f"VenueCredentials(api_key={self._masked(self.api_key)}, "
                f"passphrase={self._masked(self.passphrase)}, secret=***)")


def timestamp_now():
    """The venue's timestamp for a login: Unix epoch **seconds**, as a string.

    Measured against the demo endpoint on 2026-11-13, because the venue is the only judge and the shape is not the
    one a reader would guess from the REST docs: the ISO form `2026-11-13T00:00:00.000Z` is refused with
    `60004 Invalid timestamp`, epoch *milliseconds* with `60006 Timestamp request expired`, and epoch seconds is
    accepted with `{"event":"login","code":"0"}`. The signature is computed over this exact string, so the two have
    to be produced together -- which is why this function and `login_message` are one decision.
    """
    return str(int(time.time()))


def signature(timestamp, secret):
    """base64(HMAC-SHA256(secret, timestamp + "GET" + /users/self/verify)).

    One function, because the signature is the one part of this file that a peer can check and nothing here can:
    the self-test pins one vector of it, and `LocalPrivateVenue` verifies the login that actually crossed a
    socket, so a wrong preimage or a wrong encoding fails a run rather than a sandbox session.
    """
    message = timestamp + "GET" + VERIFY_PATH
    return base64.b64encode(hmac.new(secret.encode(), message.encode(), hashlib.sha256).digest()).decode()


def login_message(credentials, timestamp=None):
    """The `op: login` frame: the four fields the venue checks, and nothing else."""
    timestamp = timestamp_now() if timestamp is None else timestamp
    return {"op": "login", "args": [{"apiKey": credentials.api_key, "passphrase": credentials.passphrase,
                                     "timestamp": timestamp,
                                     "sign": signature(timestamp, credentials.secret)}]}


def subscribe_orders_message(inst_type="ANY"):
    """The `orders` subscription.

    The venue takes an instrument *type* on this channel rather than a symbol, so one subscription covers the
    account and the filter is on this side: `OrderVenueClient.handle` keeps the entries whose `instId` is the
    relay's instrument and counts the rest, because an account with two instruments must not have the second
    one's fills answered to an engine that never traded it. `ANY` is what the private route accepts; the business
    route, which is where an order is placed, refuses it with `60018` and wants a concrete type, which is why the
    token is a parameter rather than a constant.
    """
    return {"op": "subscribe", "args": [{"channel": "orders", "instType": inst_type}]}


def rest_timestamp_now():
    """The venue's timestamp for a REST call: ISO 8601 **milliseconds**.

    The websocket login takes epoch seconds and this takes the ISO form -- measured, not read off one page: a login
    signed this way is refused with `60004`, and a REST call signed the other way with `50112`. Two formats, one venue,
    and a client that shares one timestamp function across both is one error code away from looking like a bad key.
    """
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


def rest_signature(timestamp, secret, method, path, body):
    """base64(HMAC-SHA256(secret, timestamp + method + path + body)); the body is the exact bytes sent."""
    return base64.b64encode(
        hmac.new(secret.encode(), f"{timestamp}{method}{path}{body}".encode(), hashlib.sha256).digest()).decode()


class RestPlacer:
    """Places an order over REST, because the demo venue's websocket trade operation answers `50026`.

    Measured on 2026-11-13, in this order: the websocket `op: order` answered `50026 "System error. Try again later."`
    three times with backoff, a second apart, with the instrument code as a number and as a string; the same order over
    REST, with the account in margin mode, answered `code 0, sCode 0, "Order placed"` and an order id, and the cancel
    that followed answered `code 0` with nothing left pending. So placement lives here and the private websocket stays
    what it is good at: the `orders` channel's state changes.

    The body is the translator's own `args[0]` -- the same shape the websocket op would carry -- so there is one
    translation and two transports, which is the property that makes this a relay rather than a second client.
    """

    PATHS = {"order": ("POST", "/api/v5/trade/order"),
             "cancel-order": ("POST", "/api/v5/trade/cancel-order")}

    def __init__(self, base_url, credentials, translator, dump=None, simulated=True):
        self.base_url = base_url.rstrip("/")
        self.credentials = credentials
        # The same translator the websocket route uses, because the answer needs the same arithmetic: the venue's
        # `sz` is in contracts and the adapter's ACK is in engine units.
        self.translator = translator
        self.dump = dump if dump is not None else FrameDump()
        # The demo host takes the header and the live one does not; a live placement with it is refused, so it is a
        # constructor argument rather than a constant.
        self.simulated = simulated
        self.placed = 0
        self.refused = 0
        self.failures = 0

    def send(self, request):
        """One translated request out, one adapter line back."""
        op = request.get("op")
        if op not in self.PATHS:
            raise OrderError(f"'{op}' is not a request REST places")
        method, path = self.PATHS[op]
        body = json.dumps(request["args"][0], separators=(",", ":"))
        self.dump.record("request", body)
        answer = self._post(method, path, body)
        if answer is None:
            self.failures += 1
            return [f"REJECT {request['id']} venue"]
        return self.lines_for(op, request["id"], answer)

    def lines_for(self, op, order_id, answer):
        """The venue's answer as the adapter's lines, with a refusal's own words printed rather than swallowed."""
        data = (answer.get("data") or [{}])[0]
        code = str(answer.get("code", ""))
        sub = str(data.get("sCode", ""))
        if code == "0" and sub == "0":
            if op == "order":
                self.placed += 1
                placed = int(data.get("sz", "0") or 0)
                return [f"ACK {order_id} {self.translator.units_of(placed)}"]
            return [f"CANCELED {order_id} user"]
        self.refused += 1
        reason = data.get("sMsg") or answer.get("msg", "")
        print(f"the venue refused request {order_id}: code={code} sCode={sub} {reason}".strip(), flush=True)
        return [f"REJECT {order_id} venue"]

    def _post(self, method, path, body):
        timestamp = rest_timestamp_now()
        headers = {"Content-Type": "application/json",
                   # The proxy in front of the venue answers urllib's own user agent with a 403 and `error code: 1010`,
                   # which reads like a venue refusal and is not one.
                   "User-Agent": "curl/8.5.0",
                   "OK-ACCESS-KEY": self.credentials.api_key,
                   "OK-ACCESS-SIGN": rest_signature(timestamp, self.credentials.secret, method, path, body),
                   "OK-ACCESS-TIMESTAMP": timestamp,
                   "OK-ACCESS-PASSPHRASE": self.credentials.passphrase}
        if self.simulated:
            headers["x-simulated-trading"] = "1"
        request = urllib.request.Request(self.base_url + path, data=body.encode(), method=method,
                                         headers=headers)
        try:
            with urllib.request.urlopen(request, timeout=10) as response:
                text = response.read().decode()
        except urllib.error.HTTPError as error:
            text = error.read().decode(errors="replace")
        except OSError as error:
            print(f"the REST placement could not reach the venue ({error})", flush=True)
            return None
        self.dump.record("venue", text)
        try:
            return json.loads(text)
        except json.JSONDecodeError:
            print(f"the venue answered a placement with something that is not JSON: {text[:160]!r}", flush=True)
            return None


class OrderVenueClient(threading.Thread):
    """The private route: login, subscribe `orders`, translate both ways, and reconnect when the venue drops.

    It is a client to the venue the way `VenueClient` is a client to its public stream, and it owns the same one
    thing: the venue's protocol. What crosses to the engine is the adapter's lines, unchanged, so `LiveVenue` and
    `TcpVenueTransport` cannot tell this route from the simulation -- which is the property ADR 0005 decision 1
    asked for and the reason the translation stayed in `OrderTranslator`.

    A reconnect is a *new session*: the venue forgets the old connection, so a client that merely retried the
    socket would subscribe nothing and hear nothing. `session` therefore logs in and subscribes again every time,
    and the three counters say how often each happened. A request whose write failed is not re-queued -- an order
    sent twice is worse than an order nobody answered, and the gateway's own timeout is what notices.
    """

    def __init__(self, url, credentials, translator, deliver, stop=None, backoff_seconds=1.0, dump=None,
                 inst_type="ANY", subscribe_orders=True):
        super().__init__(daemon=True)
        self.url = url
        self.credentials = credentials
        self.translator = translator
        # Where the decoded lines go: the engine's socket in a run, a list in the self-test.
        self.deliver = deliver
        self.stop = stop if stop is not None else threading.Event()
        self.backoff_seconds = backoff_seconds
        # Every frame both ways, written as it crossed. Without it a run that heard nothing cannot say whether
        # the venue was silent or this client was looking somewhere else -- which is the question the first
        # sandbox order left open.
        self.dump = dump
        # The instrument type the `orders` subscription asks for. `ANY` for the private route, a concrete type for
        # the business route, which refuses `ANY` with `60018`.
        self.inst_type = inst_type
        # False for a route that places orders and reads the venue's answers to them but subscribes no channel: the
        # business endpoint serves `op: order` and refuses the `orders` subscription, so a placement route has to be
        # able to log in without one.
        self.subscribe_orders = subscribe_orders
        # Outbound requests wait here so the reader never blocks on a write; `write_loop` owns the socket's
        # send side for the length of one session.
        self.outbox = queue.Queue()
        self.lock = threading.Lock()
        self.sock = None
        self.pending = []
        self.logins = 0
        self.subscribes = 0
        self.reconnects = 0
        self.messages = 0
        self.other_instruments = 0
        self.unreadable = 0
        self.write_failures = 0
        self.error = None

    def record(self, direction, frame):
        """One frame, either way, in the order it crossed. Called from the reader and the writer."""
        if self.dump is not None:
            self.dump.record(direction, frame)

    @property
    def dump_frames(self):
        return self.dump.frames if self.dump is not None else 0

    def submit(self, request):
        """Hands one translated request to the venue. It is sent by the next write, or not at all."""
        self.outbox.put(request)

    def close(self):
        """Ends the thread, including a read it is blocked on: a stop event is not enough with a socket open."""
        self.stop.set()
        with self.lock:
            sock = self.sock
        if sock is not None:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    def run(self):
        backoff = self.backoff_seconds
        while not self.stop.is_set():
            try:
                self.session()
                backoff = self.backoff_seconds
            # `OrderError` is a `ValueError`, and so is a venue message that is not JSON: both end a session and
            # both are worth reconnecting for.
            except (OSError, RuntimeError, ValueError) as error:
                if self.stop.is_set():
                    break
                self.reconnects += 1
                self.error = str(error)
                print(f"private channel lost ({error}); reconnecting in {backoff:.2f}s", flush=True)
            if self.stop.is_set():
                break
            # The wait is interruptible so a stop during the backoff ends the thread now rather than up to eight
            # seconds later, which is what a test that drops a connection deliberately needs.
            if self.stop.wait(backoff):
                break
            backoff = min(backoff * 2.0, 8.0)

    def session(self):
        """One connection to the venue, login to drop."""
        sock = websocket_client(self.url)
        with self.lock:
            self.sock = sock
        writer_stop = threading.Event()
        writer = None
        try:
            # The venue closes an idle private connection, so silence is answered with the text ping rather than
            # waited out; this timeout is what "idle" is measured against.
            sock.settimeout(PING_AFTER_SECONDS)
            self.pending = []
            self.login(sock)
            # A placement route has nothing to subscribe: the venue's answer to each `op: order` is the report.
            if self.subscribe_orders:
                self.subscribe(sock)
            writer = threading.Thread(target=self.write_loop, args=(sock, writer_stop), daemon=True)
            writer.start()
            for message in self.pending:
                self.handle(message)
            self.pending = []
            while not self.stop.is_set():
                try:
                    frame = read_frame(sock)
                except socket.timeout:
                    send_masked(sock, "ping")
                    continue
                if frame is None:
                    raise RuntimeError("the venue closed the private connection")
                _, opcode, payload = frame
                if opcode == 0x8:
                    raise RuntimeError("the venue closed the private connection")
                if opcode == 0x9:
                    send_masked(sock, payload, opcode=0xA)
                    continue
                if opcode not in (0x1, 0x0):
                    continue
                text = payload.decode(errors="replace").strip()
                # The venue's heartbeat is a text frame rather than a control frame, and neither form is an
                # order message.
                if text == "ping":
                    send_masked(sock, "pong")
                    continue
                if text == "pong":
                    continue
                self.record("venue", text)
                self.handle(json.loads(text))
        finally:
            writer_stop.set()
            if writer is not None:
                writer.join(timeout=1.0)
            with self.lock:
                self.sock = None
            sock.close()

    def read_message(self, sock):
        """One venue message, with the heartbeats and the close answered rather than returned."""
        while True:
            frame = read_frame(sock)
            if frame is None:
                raise RuntimeError("the venue closed the private connection")
            _, opcode, payload = frame
            if opcode == 0x8:
                raise RuntimeError("the venue closed the private connection")
            if opcode == 0x9:
                send_masked(sock, payload, opcode=0xA)
                continue
            if opcode == 0xA:
                continue
            text = payload.decode(errors="replace").strip()
            if text == "ping":
                send_masked(sock, "pong")
                continue
            if text == "pong":
                continue
            self.record("venue", text)
            return json.loads(text)

    def login(self, sock):
        """Send the login and wait for its acknowledgement.

        A message that arrives while the acknowledgement is owed is kept, not dropped: the venue can send a state
        change for an order the previous session placed before it answers a login, and that change still has to
        reach the engine.
        """
        send_masked(sock, json.dumps(login_message(self.credentials)))
        # The login frame carries the api key and the signature, so the dump writes the message the venue sees
        # except for those two fields: a file that exists to explain why nothing came back must not be the copy
        # of the key that leaked.
        self.record("request", json.dumps({"op": "login", "args": [{"apiKey": "***", "passphrase": "***",
                                                                   "timestamp": login_message(self.credentials)
                                                                   ["args"][0]["timestamp"],
                                                                   "sign": "***"}]}))
        self.logins += 1
        while True:
            message = self.read_message(sock)
            event = message.get("event")
            if event == "login":
                if str(message.get("code", "0")) != "0":
                    raise OrderError(f"the venue refused the login: code {message.get('code')} "
                                     f"{message.get('msg', '')}".strip())
                return
            if event == "error":
                raise OrderError(f"the venue refused the private channel: {message}")
            self.pending.append(message)

    def subscribe(self, sock):
        """Subscribe `orders` and wait for the acknowledgement, keeping anything else that arrives."""
        send_masked(sock, json.dumps(subscribe_orders_message(self.inst_type)))
        self.record("request", json.dumps(subscribe_orders_message(self.inst_type)))
        self.subscribes += 1
        while True:
            message = self.read_message(sock)
            event = message.get("event")
            if event == "subscribe":
                return
            if event == "error":
                raise OrderError(f"the venue refused the orders subscription: {message}")
            self.pending.append(message)

    def handle(self, message):
        """One venue message: filtered to this instrument, translated, and handed to the engine."""
        if not isinstance(message, dict):
            self.unreadable += 1
            return
        event = message.get("event")
        if event == "error":
            raise RuntimeError(f"the venue reported an error on the private channel: {message}")
        if event:
            return  # a late acknowledgement: protocol, not an order
        entries = message.get("data")
        # Only entries are filtered: a refusal answers with an empty `data` list, and returning on it here would drop
        # the venue's reason before `decode` could turn it into a REJECT -- which is what the sandbox's 50014 was.
        if isinstance(entries, list) and entries:
            kept = [entry for entry in entries
                    if not entry.get("instId") or entry.get("instId") == self.translator.instrument]
            self.other_instruments += len(entries) - len(kept)
            if not kept:
                return
            message = dict(message, data=kept)
        self.messages += 1
        try:
            lines = self.translator.decode(message)
        except OrderError as error:
            # Counted and printed rather than fatal: one message this translation does not know is a bug to see,
            # not a reason to drop a working private channel.
            self.unreadable += 1
            print(f"private channel could not translate a venue message: {error}", flush=True)
            return
        if lines:
            self.deliver(lines)

    def write_loop(self, sock, session_stop):
        """Sends what the engine submitted, for the length of one session."""
        while not self.stop.is_set() and not session_stop.is_set():
            try:
                request = self.outbox.get(timeout=0.05)
            except queue.Empty:
                continue
            try:
                with self.lock:
                    send_masked(sock, json.dumps(request))
            except OSError as error:
                self.write_failures += 1
                print(f"the private channel could not send a request ({error})", flush=True)
                return
            # Recorded after the write succeeded: a frame that never left is not something the venue could
            # have answered, and the dump is read to tell those two apart.
            self.record("request", json.dumps(request))


# ---------------------------------------------------------------- self-test and main


def decode_file(path, decoder):
    """Rows for a captured file: an offline run with no network at all."""
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if line:
                yield from decoder.decode(line)


# The pinned vector: the secret and the timestamp are fixed here and the signature was computed once, outside this
# file, and pasted in. A check that called `signature` for its expectation would be the implementation agreeing
# with itself, and would pass on any wrong preimage; this constant cannot.
GOLDEN_SECRET = "toyquant-relay-self-test-secret"
# 2026-11-13T00:00:00Z as Unix epoch seconds, which is the form the venue accepts (see `timestamp_now`).
GOLDEN_TIMESTAMP = "1794528000"
GOLDEN_SIGN = "WIUsY7IKdr05lNBq2VgPKZu5ME3ZgT1i6w4ixCiJ9W8="


class LocalPrivateVenue(threading.Thread):
    """A stand-in for the venue's private channel: a real socket, a real handshake, and the login checked.

    It is a peer rather than a mock on purpose. The signature is hand-built in this file, so the only thing that
    can catch a wrong preimage or a wrong encoding is a peer that verifies what actually crossed the wire; a test
    that compared the implementation with a second call of itself would agree with any mistake. It answers in the
    venue's own shapes, drops the first connection once -- so the client has to log in and subscribe again -- and
    answers an order the way the simulated venue does, deterministically.
    """

    def __init__(self, secret):
        super().__init__(daemon=True)
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(4)
        self.url = f"ws://127.0.0.1:{self.listener.getsockname()[1]}/ws/v5/private?brokerId=9999"
        self.secret = secret
        self.stop = threading.Event()
        self.connection = None
        self.connections = 0
        self.logins = 0
        self.subscribes = 0
        self.orders = 0
        self.cancels = 0
        self.problems = []

    def halt(self):
        """Ends the stand-in, including a connection it is blocked on."""
        self.stop.set()
        connection = self.connection
        if connection is not None:
            try:
                connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self.listener.close()

    def run(self):
        while not self.stop.is_set():
            try:
                connection, _ = self.listener.accept()
            except OSError:
                return
            self.connection = connection
            try:
                self.serve(connection)
            finally:
                self.connection = None
                connection.close()

    def send(self, connection, message):
        send_masked(connection, json.dumps(message))

    def serve(self, connection):
        if not server_handshake(connection):
            return
        self.connections += 1
        # The drop is made after the first subscription rather than on a timeout, so the test never waits for it.
        drop_after_subscribe = self.connections == 1
        connection.settimeout(5.0)
        while not self.stop.is_set():
            try:
                frame = read_frame(connection)
            except socket.timeout:
                continue
            except OSError:
                return
            if frame is None:
                return
            _, opcode, payload = frame
            if opcode == 0x8:
                return
            if opcode == 0x9:
                send_masked(connection, payload, opcode=0xA)
                continue
            if opcode != 0x1:
                continue
            message = json.loads(payload.decode())
            operation = message.get("op")
            if operation == "login":
                self.check_login(message)
                self.send(connection, {"event": "login", "code": "0", "connId": "self-test"})
            elif operation == "subscribe":
                self.subscribes += 1
                self.send(connection, {"event": "subscribe", "arg": (message.get("args") or [{}])[0]})
                if drop_after_subscribe:
                    return
            elif operation == "order":
                self.orders += 1
                for reply in self.order_replies(message):
                    self.send(connection, reply)
            elif operation == "cancel-order":
                self.cancels += 1
                for reply in self.cancel_replies(message):
                    self.send(connection, reply)
            else:
                self.problems.append(f"the client sent an operation the venue does not know: {message}")

    def check_login(self, message):
        """Every field the venue checks, checked from the message that arrived rather than from the code that
        built it."""
        argument = (message.get("args") or [{}])[0]
        if set(argument) != {"apiKey", "passphrase", "timestamp", "sign"}:
            self.problems.append(f"the login has the wrong fields: {sorted(argument)}")
            return
        timestamp = str(argument.get("timestamp", ""))
        if not timestamp.isdigit() or len(timestamp) != 10:
            self.problems.append(f"the login timestamp is not Unix epoch seconds: {timestamp!r}")
        if argument.get("sign") != signature(timestamp, self.secret):
            self.problems.append("the login signature does not verify: the preimage or the encoding is wrong")
        self.logins += 1

    def order_replies(self, message):
        argument = (message.get("args") or [{}])[0]
        cl_ord_id = str(argument.get("clOrdId", ""))
        instrument = argument.get("instId")
        quantity = int(argument.get("sz", "0"))
        filled = quantity // 2 if quantity >= 2 else quantity
        return [
            {"op": "order", "data": [{"clOrdId": cl_ord_id, "ordId": cl_ord_id, "sCode": "0",
                                      "sz": str(quantity), "accFillSz": "0"}]},
            {"arg": {"channel": "orders", "instId": instrument},
             "data": [{"clOrdId": cl_ord_id, "instId": instrument, "state": "partially_filled",
                       "fillPx": argument.get("px"), "fillSz": str(filled), "accFillSz": str(filled),
                       "sz": str(quantity)}]},
        ]

    def cancel_replies(self, message):
        cl_ord_id = str(((message.get("args") or [{}])[0]).get("clOrdId", ""))
        return [
            {"op": "cancel-order", "data": [{"clOrdId": cl_ord_id, "sCode": "0"}]},
            {"arg": {"channel": "orders"}, "data": [{"clOrdId": cl_ord_id, "state": "canceled"}]},
        ]


def self_test():
    """Decode the shapes a live capture showed, and check the rows and the gap count."""
    decoder = Decoder("BTC-USDT-SWAP", DEFAULT_CONTRACT_SIZE, DEFAULT_QUANTITY_SCALE, 1)

    trade = ('{"arg":{"channel":"trades","instId":"BTC-USDT-SWAP"},"data":['
             '{"instId":"BTC-USDT-SWAP","tradeId":"7","px":"100.5","sz":"2.5",'
             '"side":"buy","ts":"1000","seqId":5}]}')
    rows = decoder.decode(trade)
    assert rows == ["okx,BTC-USDT-SWAP,1000,7,buy,100.5,25000\n"], rows

    # max_levels is 1, so one level a side, both marked as the snapshot's.
    snapshot = ('{"arg":{"channel":"books","instId":"BTC-USDT-SWAP"},"action":"snapshot","data":['
                '{"asks":[["100.6","3","0","1"],["100.7","1","0","1"]],'
                '"bids":[["100.4","5","0","2"],["100.3","2","0","2"]],'
                '"ts":"1000","seqId":10,"prevSeqId":-1}]}')
    assert decoder.decode(snapshot) == [
        "okx,BTC-USDT-SWAP,1000,1000,1,bid,100.4,50000\n",
        "okx,BTC-USDT-SWAP,1000,1000,1,ask,100.6,30000\n",
    ]
    assert decoder.last_seq == 10

    # A removal: size zero, and the sequence continues, so no gap is counted.
    update = ('{"arg":{"channel":"books","instId":"BTC-USDT-SWAP"},"action":"update","data":['
              '{"asks":[["100.6","0","0","0"]],"bids":[],"ts":"1001","seqId":11,"prevSeqId":10}]}')
    assert decoder.decode(update) == ["okx,BTC-USDT-SWAP,1001,1001,0,ask,100.6,0\n"]
    assert decoder.gaps == 0, decoder.gaps

    # A break in the sequence is a lost update, and it is counted rather than hidden.
    gap = ('{"arg":{"channel":"books","instId":"BTC-USDT-SWAP"},"action":"update","data":['
           '{"asks":[],"bids":[],"ts":"1002","seqId":99,"prevSeqId":42}]}')
    decoder.decode(gap)
    assert decoder.gaps == 1, decoder.gaps

    # An event and a non-JSON line are ignored rather than crashing.
    assert decoder.decode('{"event":"subscribe","arg":{"channel":"books"},"connId":"x"}') == []
    assert decoder.decode("pong") == []

    # The order channel: the adapter's lines in, the venue's shapes out, and the venue's answers back. The two
    # processes it joins are tested separately -- `tcp_venue_transport_test` on the engine side, this file on
    # the translation -- so what is pinned here is the shape in the middle, field by field.
    translator = OrderTranslator("BTC-USDT-SWAP")
    # The engine's symbol is the replay's instrument; the venue is told the relay's name for it. A run whose book
    # comes from one exchange and whose orders go to another would otherwise ask for an instrument that venue has
    # never heard of -- which is what the first sandbox order did, with `BTC-PERPETUAL`.
    #
    # The size is 100,000 engine units: the engine's scale is 1e6 and one contract is 0.01 of the coin, so a
    # contract is 10,000 units and the order is ten of them. A relay that passed the engine's number through would
    # ask for a hundred thousand contracts, which is the size error this conversion exists to make impossible.
    request = translator.encode("ORDER 7 BTC-PERPETUAL B 6421.5 100000 Gtc")
    assert request["op"] == "order", request
    argument = request["args"][0]
    assert (argument["instId"], argument["side"], argument["ordType"], argument["px"], argument["sz"],
            argument["clOrdId"]) == ("BTC-USDT-SWAP", "buy", "limit", "6421.5", "10", "7"), argument
    # Under one contract there is nothing to send: refused here, with both numbers, rather than as a venue refusing
    # a size of zero.
    try:
        translator.encode("ORDER 9 BTC-PERPETUAL B 6421.5 1000 Gtc")
    except OrderError as error:
        assert "below one contract" in str(error), str(error)
    else:
        raise AssertionError("a size below one contract was placed")
    # The price goes onto the venue's grid, because the venue refuses an off-grid price rather than rounding it.
    assert translator.encode("ORDER 10 BTC-PERPETUAL B 6421.54 100000 Gtc")["args"][0]["px"] == "6421.5"
    # A venue that trades in lots rather than single contracts rounds down to the lot, not to the nearest one: twelve
    # contracts on a five-contract grid are ten.
    lots = OrderTranslator("BTC-USDT-SWAP", mapping=OrderMapping(0.01, 1000000, 0.1, 5))
    assert lots.encode("ORDER 11 BTC-PERPETUAL B 6421.5 120000 Gtc")["args"][0]["sz"] == "10"
    # The venue's internal instrument code goes on the wire only when the operator names it: its newer order
    # operation refuses without one, and a guessed default would be an order on the wrong contract.
    assert "instIdCode" not in argument, argument
    coded = OrderTranslator("BTC-USDT-SWAP", 10459)
    assert coded.encode("ORDER 7 BTC-PERPETUAL B 6421.5 100000 Gtc")["args"][0]["instIdCode"] == 10459

    venue = SimulatedVenue(max_quantity=100)
    replies = [line for message in venue.messages(request) for line in translator.decode(message)]
    assert replies == ["ACK 7 100000", "FILL 7 6421.5 50000"], replies

    # The `orders` channel is a stream of states and `accFillSz` is cumulative, so one fill per increment of it: a
    # repeated push is not a second fill, and the reconnect that re-sends every live order is not a second session.
    def push(state, filled, order_id="14"):
        return {"arg": {"channel": "orders"}, "data": [
            {"clOrdId": order_id, "state": state, "accFillSz": str(filled), "fillPx": "6421.5"}]}

    # An order the channel has not seen before starts at zero, so its first push reports the whole cumulative size.
    assert translator.decode(push("partially_filled", 6)) == ["FILL 14 6421.5 60000"]
    assert translator.decode(push("partially_filled", 6)) == []
    assert translator.fills_deduplicated == 1, translator.fills_deduplicated
    assert translator.decode(push("partially_filled", 8)) == ["FILL 14 6421.5 20000"]
    assert translator.decode(push("canceled", 8)) == ["CANCELED 14 user"]
    assert translator.decode(push("canceled", 8)) == []
    # An order that fills to completion reports its last fill and then nothing: `filled` is terminal, so a cancel
    # that arrives afterwards is the venue repeating itself rather than a second end to the same order.
    assert translator.decode(push("filled", 3, order_id="13")) == ["FILL 13 6421.5 30000"]
    assert translator.decode(push("canceled", 3, order_id="13")) == []
    # A placement's answer carries the same counter, and the ACK says what is left -- in engine units, which is what
    # the adapter's `ACK <id> <remaining>` means. An ACK is not a fill: the push that follows reports the same
    # cumulative, and that is where the fill is booked.
    ack = translator.decode({"op": "order", "id": "12",
                             "data": [{"clOrdId": "12", "sCode": "0", "sz": "10", "accFillSz": "4"}]})
    assert ack == ["ACK 12 60000"], ack
    filled_on_ack = translator.decode({"arg": {"channel": "orders"}, "data": [
        {"clOrdId": "12", "state": "partially_filled", "accFillSz": "4", "fillPx": "6421.5"}]})
    assert filled_on_ack == ["FILL 12 6421.5 40000"], filled_on_ack
    # A counter that goes backwards is not a duplicate to ignore: it is a venue bug that would otherwise book a
    # position nothing explains.
    try:
        translator.decode(push("partially_filled", 4))
    except OrderError as error:
        assert "went backwards" in str(error), str(error)
    else:
        raise AssertionError("a fill count that went backwards was accepted")

    # A refused request answers at the top level with an empty data list, and it reaches the engine as a REJECT
    # rather than as nothing: the venue's own words ("parameter id can not be empty") were the first sandbox
    # order's whole reply, and a decoder that only read `data` turned it into silence.
    assert translator.decode({"id": "7", "op": "order", "code": "50014",
                              "msg": "Parameter id can not be empty.", "data": []}) == ["REJECT 7 venue"]
    assert translator.decode({"id": "7", "op": "cancel-order", "code": "51000", "msg": "bad", "data": []}) == \
        ["REJECT 7 venue"]
    # A refusal with no id cannot be attributed to an order, so it is refused as unreadable rather than answered to
    # whichever order happens to be first.
    try:
        translator.decode({"id": "", "op": "order", "code": "50014", "data": []})
    except OrderError:
        pass
    else:
        raise AssertionError("a refusal with no id was answered to nobody")

    # Above the sandbox cap the venue refuses, and the refusal reaches the engine as a REJECT rather than as an
    # acknowledgement of an order that was never placed.
    over = translator.encode("ORDER 8 BTC-USDT-SWAP S 6421.5 90000 Gtc")
    small = SimulatedVenue(max_quantity=8)
    replies = [line for message in small.messages(over) for line in translator.decode(message)]
    assert replies == ["REJECT 8 venue"], replies

    # A cancel is answered by the venue's acknowledgement and then by the state message, and only the second
    # one is the order ending.
    cancel = translator.encode("CANCEL 7")
    assert cancel["op"] == "cancel-order" and cancel["args"][0]["instId"] == "BTC-USDT-SWAP", cancel
    replies = [line for message in venue.messages(cancel) for line in translator.decode(message)]
    assert replies == ["CANCELED 7 user"], replies

    # A line the channel does not understand is refused with a reason rather than guessed at.
    for bad in ("FILL 7 1 1", "ORDER 7 BTC-USDT-SWAP X 1 1 Gtc", "CANCEL", "ORDER 7 BTC-USDT-SWAP B 0 100000 Gtc",
                "ORDER 7 BTC-USDT-SWAP B 1 0 Gtc", "ORDER 7 BTC-USDT-SWAP B 1 1.5 Gtc",
                "ORDER 7 BTC-USDT-SWAP B 1 100000 Gtc Now"):
        try:
            translator.encode(bad)
        except OrderError:
            continue
        raise AssertionError(f"{bad!r} was accepted")

    # The private route. The signature is hand-built in this file and nothing else in the tree can check it, so
    # there are two checks and they are the two halves of one claim: a pinned vector, which the implementation
    # cannot agree with by accident, and a peer that verifies the login that actually crossed a socket.
    credentials = VenueCredentials("self-test-key", GOLDEN_SECRET, "self-test-passphrase")
    assert "self-test-key" not in repr(credentials), repr(credentials)
    assert GOLDEN_SECRET not in repr(credentials), repr(credentials)
    assert "self-test-passphrase" not in repr(credentials), repr(credentials)
    assert signature(GOLDEN_TIMESTAMP, GOLDEN_SECRET) == GOLDEN_SIGN, signature(GOLDEN_TIMESTAMP, GOLDEN_SECRET)
    login = login_message(credentials, GOLDEN_TIMESTAMP)
    assert login["op"] == "login" and len(login["args"]) == 1, login
    argument = login["args"][0]
    assert set(argument) == {"apiKey", "passphrase", "timestamp", "sign"}, argument
    assert (argument["apiKey"], argument["passphrase"], argument["timestamp"]) == \
        ("self-test-key", "self-test-passphrase", GOLDEN_TIMESTAMP), argument
    assert argument["sign"] == GOLDEN_SIGN, argument["sign"]
    timestamp = timestamp_now()
    assert timestamp.isdigit() and len(timestamp) == 10, timestamp
    # A credential that is missing is refused with the variable named, which is what a run that has not been given
    # one has to be told -- and it names only what is missing.
    try:
        VenueCredentials.from_environment({"TOYQUANT_API_KEY": "k", "TOYQUANT_API_SECRET": "s"})
    except OrderError as error:
        assert "TOYQUANT_PASSPHRASE" in str(error) and "TOYQUANT_API_KEY" not in str(error), str(error)
    else:
        raise AssertionError("a credential missing its passphrase was accepted")
    assert VenueCredentials.from_environment(
        {"TOYQUANT_API_KEY": "k", "TOYQUANT_API_SECRET": "s", "TOYQUANT_PASSPHRASE": "p"}).api_key == "k"

    # The private route end to end, against a peer: login, the deliberate drop, login and subscribe again, and
    # then an order answered the way the venue answers one. The client prints one "private channel lost" line
    # here, and that line is this test's own drop being reconnected to rather than a failure.
    venue = LocalPrivateVenue(GOLDEN_SECRET)
    venue.start()
    private_translator = OrderTranslator("BTC-USDT-SWAP")
    delivered = []
    # The dump is what a run that heard nothing is read against, so the self-test keeps one and checks it.
    dump = FrameDump(io.StringIO())
    client = OrderVenueClient(venue.url, credentials, private_translator, delivered.extend, backoff_seconds=0.02,
                              dump=dump)
    client.start()
    try:
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline and (venue.logins < 2 or venue.subscribes < 2):
            time.sleep(0.01)
        assert venue.problems == [], venue.problems
        assert (venue.logins, venue.subscribes) == (2, 2), (venue.logins, venue.subscribes, client.error)
        assert client.reconnects == 1, (client.reconnects, client.error)
        client.submit(private_translator.encode("ORDER 7 BTC-USDT-SWAP B 6421.5 100000 Gtc"))
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline and "FILL 7 6421.5 50000" not in delivered:
            time.sleep(0.01)
        assert delivered == ["ACK 7 100000", "FILL 7 6421.5 50000"], delivered
        client.submit(private_translator.encode("CANCEL 7"))
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline and "CANCELED 7 user" not in delivered:
            time.sleep(0.01)
        assert delivered[-1] == "CANCELED 7 user" and venue.cancels == 1, (delivered, venue.cancels)
        assert (client.unreadable, client.write_failures) == (0, 0), (client.unreadable, client.write_failures)
    finally:
        client.close()
        venue.halt()
        client.join(timeout=5.0)
        venue.join(timeout=5.0)
    assert not client.is_alive(), "the private-channel client did not stop"

    # The dump: both directions, one line a frame, and the login's key and signature redacted. A file that exists
    # to explain a silence must not be the copy of the key that leaked.
    entries = [json.loads(line) for line in dump.handle.getvalue().splitlines()]
    assert any(entry["dir"] == "request" for entry in entries), entries
    assert any(entry["dir"] == "venue" for entry in entries), entries
    logins = [entry for entry in entries
              if entry["dir"] == "request" and json.loads(entry["frame"]).get("op") == "login"]
    assert logins, entries
    assert GOLDEN_SECRET not in dump.handle.getvalue(), "the dump wrote the secret"
    assert json.loads(logins[0]["frame"])["args"][0]["apiKey"] == "***", logins[0]
    # The order and the cancel the engine sent are in it, which is what says the frame left.
    assert any("6421.5" in entry["frame"] for entry in entries if entry["dir"] == "request"), entries

    # The REST placement: the demo venue's websocket trade operation answers 50026 and its REST order endpoint places
    # the same order, so this is the route the relay uses for placement. The three answer shapes are pinned, and the
    # two timestamp formats are pinned against each other because mixing them is exactly the failure they caused.
    placer = RestPlacer("https://www.okx.com", credentials, private_translator, simulated=True)
    request = private_translator.encode("ORDER 7 BTC-PERPETUAL B 6421.5 100000 Gtc")
    assert set(RestPlacer.PATHS) == {"order", "cancel-order"}, RestPlacer.PATHS
    assert request["args"][0]["clOrdId"] == "7" and request["id"] == "7", request
    assert placer.lines_for("order", "7", {"code": "0", "msg": "", "data": [{"sCode": "0", "ordId": "1",
                                                                           "sz": "10"}]}) == ["ACK 7 100000"]
    assert placer.lines_for("cancel-order", "7", {"code": "0", "data": [{"sCode": "0",
                                                                        "ordId": "1"}]}) == ["CANCELED 7 user"]
    assert placer.lines_for("order", "7", {"code": "1", "msg": "All operations failed",
                                           "data": [{"sCode": "51010",
                                                     "sMsg": "You can't complete this request under your current "
                                                             "account mode."}]}) == ["REJECT 7 venue"]
    assert (placer.placed, placer.refused) == (1, 1), (placer.placed, placer.refused)
    assert rest_timestamp_now().endswith("Z") and len(rest_timestamp_now()) == 24, rest_timestamp_now()
    assert timestamp_now().isdigit() and len(timestamp_now()) == 10, timestamp_now()

    print("self-test OK")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--venue", default=DEFAULT_VENUE, help="the venue's public websocket url")
    parser.add_argument("--instrument", default=DEFAULT_INSTRUMENT)
    parser.add_argument("--books-channel", default="books", choices=["books", "books5", "bbo-tbt"])
    parser.add_argument("--levels", type=int, default=25,
                        help="levels kept from a snapshot; 0 keeps every level")
    parser.add_argument("--contract-size", type=float, default=DEFAULT_CONTRACT_SIZE)
    parser.add_argument("--quantity-scale", type=int, default=DEFAULT_QUANTITY_SCALE)
    parser.add_argument("--tick-size", type=float, default=DEFAULT_TICK_SIZE,
                        help="the price step the venue accepts on this instrument, which the order leg rounds to; "
                             "the market-data side reads the venue's prices as they arrive")
    parser.add_argument("--lot-size", type=int, default=DEFAULT_LOT_SIZE,
                        help="the size step, in contracts, that the venue accepts; the order leg rounds down to it")
    parser.add_argument("--listen", default="127.0.0.1:8765",
                        help="where to serve the engine; empty to only capture")
    parser.add_argument("--capture", help="write the venue's raw messages here, for a later --replay")
    # The order channel: the other half of the relay, and the one an engine sends to rather than reads from.
    parser.add_argument("--order-listen",
                        help="serve the engine's order leg on this host:port; empty disables it")
    parser.add_argument("--simulate-orders", action="store_true",
                        help="answer the order channel as a sandbox venue would: no key, no endpoint, no money")
    parser.add_argument("--order-cap", type=int, default=100,
                        help="the largest order the simulated venue accepts, in contracts (the size the translator "
                             "sends after the conversion)")
    parser.add_argument("--order-capture",
                        help="write the translated venue requests here, one JSON object a line")
    parser.add_argument("--order-venue", default=DEFAULT_PRIVATE_VENUE,
                        help="the venue's private websocket url for the order leg; the default is its demo "
                             "endpoint, which nothing in this repository has reached yet")
    parser.add_argument("--order-dump",
                        help="write every frame the private channel receives and sends here, one JSON object a "
                             "line, so a run that heard nothing can say what the venue did")
    parser.add_argument("--order-inst-type", default="ANY",
                        choices=["ANY", "SPOT", "MARGIN", "SWAP", "FUTURES", "OPTION"],
                        help="the instrument type the orders subscription asks for: ANY on the private route, a "
                             "concrete type on the business route, which refuses ANY with 60018")
    parser.add_argument("--order-no-subscribe", action="store_true",
                        help="log in without subscribing the orders channel: the venue's answer to each request is "
                             "the only report, which is what a placement route on the business endpoint can have")
    parser.add_argument("--order-inst-code", type=int,
                        help="the venue's internal code for the instrument (instIdCode), which its newer order "
                             "operation requires; read it from the venue's public instruments endpoint")
    parser.add_argument("--order-rest",
                        help="place the order leg over REST at this base url (https://www.okx.com) instead of the "
                             "websocket trade operation, which the demo venue answers with 50026; the orders channel "
                             "is not subscribed on this route, so a fill would not be seen")
    parser.add_argument("--record", help="write the decoded rows straight to PREFIX_trades.csv and "
                                         "PREFIX_depth.csv: the direct route, with no engine")
    parser.add_argument("--replay", help="serve this capture instead of connecting; no network")
    parser.add_argument("--max-seconds", type=float, default=0.0)
    parser.add_argument("--ping-seconds", type=float, default=0.0,
                        help="send a ping every N seconds even while data flows; 0 pings only when idle")
    parser.add_argument("--drop-after-seconds", type=float, default=0.0,
                        help="close and reopen the venue connection every N seconds, for the reconnect test")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    # The four numbers the order leg's two conversions need, in one place: the engine's scale, the contract's
    # value, and the two venue grids a price and a size have to land on.
    mapping = OrderMapping(args.contract_size, args.quantity_scale, args.tick_size, args.lot_size)

    # The order channel has two backends, and a listener with neither says which one is missing rather than
    # accepting a flag and doing nothing: `--simulate-orders` answers as the sandbox venue, and without it the
    # route is the venue's own private channel, which needs the three variables and refuses naming whichever one
    # is absent. ADR 0005 decision 3 is why they come from the environment rather than the command line.
    if args.simulate_orders and not args.order_listen:
        parser.error("--simulate-orders needs --order-listen: there is nothing to answer on")
    # The dump records the venue's frames, and the simulation has none: accepted there it would write an empty
    # file and look like a venue that said nothing.
    if args.order_dump and not args.order_listen:
        parser.error("--order-dump needs --order-listen: there is no channel whose frames it could record")
    if args.order_dump and args.simulate_orders:
        parser.error("--order-dump is the private route's: the simulated venue's frames are this file's own, so "
                     "there is nothing a dump would add")
    if args.order_rest and args.simulate_orders:
        parser.error("--order-rest is the venue's own order endpoint; the simulated venue is what runs without it")
    if args.order_rest and not args.order_listen:
        parser.error("--order-rest needs --order-listen: there is no engine to serve")
    if args.order_listen:
        if args.simulate_orders:
            return serve_orders(args.order_listen, args.instrument, args.order_cap, args.order_capture, mapping)
        try:
            credentials = VenueCredentials.from_environment()
        except OrderError as error:
            parser.error("--order-listen without --simulate-orders serves the venue's private channel, and "
                         f"{error}")
        return serve_venue_orders(args.order_listen, args.order_venue, credentials, args.instrument,
                                  args.order_capture, args.order_dump, args.order_inst_type,
                                  not args.order_no_subscribe, args.order_inst_code, args.order_rest, mapping)

    decoder = Decoder(args.instrument, args.contract_size, args.quantity_scale, args.levels)
    stop = threading.Event()
    # Bounded for a live relay, where a stalled engine must slow the venue down. A replay reads a finite
    # file, so it keeps the unbounded queue and never blocks on a consumer that has already gone away.
    rows = queue.Queue(maxsize=ROW_QUEUE_CAPACITY if args.listen and not args.replay else 0)

    if args.replay:
        recorder = Recorder(args.record) if args.record else None
        for row in decode_file(args.replay, decoder):
            if recorder is not None:
                recorder.write(row)
            rows.put(row)
        if recorder is not None:
            recorder.close()
        rows.put(None)
        if not args.listen:
            print(decoder.summary(), flush=True)
            return 0
        listener, conn = accept_client(args.listen)
        try:
            stream(conn, rows, stop)
        finally:
            conn.close()
            listener.close()
        print(decoder.summary(), flush=True)
        return 0

    if args.listen:
        listener, conn = accept_client(args.listen)
    else:
        listener, conn = None, None

    recorder = Recorder(args.record) if args.record else None
    client = VenueClient(args.venue, args.instrument, args.books_channel, decoder, rows,
                         capture=args.capture, max_seconds=args.max_seconds, enqueue=conn is not None,
                         recorder=recorder, ping_seconds=args.ping_seconds,
                         drop_after_seconds=args.drop_after_seconds)
    client.start()
    try:
        if conn is None:
            client.join()  # capture only: run until the deadline or the venue closes
        else:
            stream(conn, rows, stop)
    except KeyboardInterrupt:
        pass
    finally:
        client.stop.set()
        client.join(timeout=2)
        if recorder is not None:
            recorder.close()
        if conn is not None:
            conn.close()
        if listener is not None:
            listener.close()
    if args.capture:
        print(f"captured the venue's messages to {args.capture}", flush=True)
    if args.record:
        print(f"recorded the decoded rows to {args.record}_trades.csv and {args.record}_depth.csv",
              flush=True)
    print(f"{decoder.summary()} reconnects={client.reconnects} "
          f"queue_high_water={client.queue_high_water} queue_waits={client.queue_waits} "
          f"pings_sent={client.pings_sent} pongs_sent={client.pongs_sent} "
          f"pongs_received={client.pongs_received}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())


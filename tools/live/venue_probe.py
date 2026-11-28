#!/usr/bin/env python3
"""Measure what the venue's path costs, from the host that will trade on it.

Four network levers were proposed on 2026-11-28 (an AWS access point, a websocket order channel, a different region and
Global Accelerator). Three of them are claims about the wire, so the first thing to establish is the wire: DNS, the TCP
connect and the TLS handshake to each name the plan would switch to, and one unauthenticated round trip to the venue's
public clock.

It is read-only by construction -- no credentials, no orders -- so it may be dispatched while a window is running:

    python3 tools/live/venue_probe.py            # three samples a name
    python3 tools/live/venue_probe.py --samples 5

A name that does not resolve is a result rather than an error: `wsaws.okx.com` answers NOERROR with no address at a public
resolver, and printing that is how the plan's premise gets tested instead of assumed.
"""
import argparse
import socket
import ssl
import time

# The two names this tree dials (`src/lines`), the one the proposal would switch to, and the REST edge the order path uses.
TARGETS = (
    ("ws.okx.com", 8443, "/api/v5/public/time"),
    ("ws.okx.com", 443, "/api/v5/public/time"),
    ("wsaws.okx.com", 8443, "/api/v5/public/time"),
    ("www.okx.com", 443, "/api/v5/public/time"),
)


def probe(name, port, path, samples, timeout):
    try:
        infos = socket.getaddrinfo(name, port, proto=socket.IPPROTO_TCP)
    except OSError as exc:
        print(f"{name}:{port}  dns=FAIL ({exc})")
        return
    addresses = sorted({info[4][0] for info in infos})
    print(f"{name}:{port}  dns={','.join(addresses[:3])}")
    context = ssl.create_default_context()
    for _ in range(samples):
        try:
            started = time.perf_counter()
            raw = socket.create_connection((name, port), timeout=timeout)
            tcp = (time.perf_counter() - started) * 1e3
            started = time.perf_counter()
            tls = context.wrap_socket(raw, server_hostname=name)
            handshake = (time.perf_counter() - started) * 1e3
            request = f"GET {path} HTTP/1.1\r\nHost: {name}\r\nConnection: close\r\n\r\n".encode()
            started = time.perf_counter()
            tls.sendall(request)
            first = tls.recv(1)
            ttfb = (time.perf_counter() - started) * 1e3
            tls.close()
            # A websocket port answers an HTTP request with an error status; the byte count is what says the path works.
            print(f"  tcp={tcp:6.1f}ms tls={handshake:6.1f}ms ttfb={ttfb:6.1f}ms "
                  f"total={tcp + handshake + ttfb:6.1f}ms reply={'yes' if first else 'no'}")
        except (OSError, ssl.SSLError) as exc:
            print(f"  FAIL {exc}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--samples", type=int, default=3, help="connections per name")
    parser.add_argument("--timeout", type=float, default=5.0, help="seconds per connection attempt")
    args = parser.parse_args()
    for name, port, path in TARGETS:
        probe(name, port, path, args.samples, args.timeout)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

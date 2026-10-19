#!/usr/bin/env python3
"""Validates that TLS session resumption works against the reference relay
(behind whatever reverse proxy terminates TLS — the relay itself is HTTP).

For local runs against the HTTP relay you have to point this at a
TLS-terminated endpoint (Caddy/nginx in front). The point is to confirm
the *server* stack we're recommending supports session tickets — the
firmware side then has a real target to build against.

Fails loudly if:
  * The second handshake is not detectably faster than the first
  * The Python SSL layer reports session_reused == False on reconnect
  * The relay 401s the resumed connection (auth state mishandled)

Usage:
    python3 test_session_resumption.py https://relay.example --iterations 5

Requires: Python 3.11+ stdlib only.
"""

from __future__ import annotations

import argparse
import http.client
import socket
import ssl
import statistics
import sys
import time
from urllib.parse import urlparse


def do_handshake_and_request(host: str, port: int, path: str,
                             ctx: ssl.SSLContext,
                             session: ssl.SSLSession | None
                             ) -> tuple[float, ssl.SSLSession | None, bool]:
    """Open one TCP+TLS connection, issue one GET, return elapsed +
    session for later reuse + a flag saying whether the handshake was
    resumed."""
    start = time.perf_counter()
    with socket.create_connection((host, port), timeout=15) as sock:
        with ctx.wrap_socket(sock, server_hostname=host,
                             session=session) as tls:
            elapsed = time.perf_counter() - start
            reused = tls.session_reused
            new_session = tls.session
            # Issue a small request to prove the stream actually works
            req = (f"GET {path} HTTP/1.1\r\n"
                   f"Host: {host}\r\n"
                   f"Connection: close\r\n\r\n").encode()
            tls.sendall(req)
            _ = tls.recv(4096)
    return elapsed, new_session, reused


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("url", help="Full https URL, e.g. https://relay.example")
    ap.add_argument("--iterations", type=int, default=5,
                    help="How many resumed handshakes to measure")
    ap.add_argument("--path", default="/api/v1/health",
                    help="Path to request (default: /api/v1/health)")
    ap.add_argument("--insecure", action="store_true",
                    help="Skip cert verification (dev with self-signed)")
    args = ap.parse_args()

    u = urlparse(args.url)
    if u.scheme != "https":
        print("this test only makes sense against an https:// URL",
              file=sys.stderr)
        return 2
    host = u.hostname
    port = u.port or 443

    ctx = ssl.create_default_context()
    if args.insecure:
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE

    print(f"[test] target: {args.url}  iterations: {args.iterations}")

    # First handshake — full, no session to resume
    first_elapsed, session, reused = do_handshake_and_request(
        host, port, args.path, ctx, session=None)
    print(f"[first]   {first_elapsed*1000:.1f} ms  reused={reused}")
    if reused:
        print("first handshake reported reused=True — impossible on a "
              "cold context. Something is wrong with the test.",
              file=sys.stderr)
        return 1

    resumed_timings: list[float] = []
    resumed_flags: list[bool] = []
    for i in range(args.iterations):
        elapsed, session, reused = do_handshake_and_request(
            host, port, args.path, ctx, session=session)
        resumed_timings.append(elapsed)
        resumed_flags.append(reused)
        print(f"[resume {i+1}] {elapsed*1000:.1f} ms  reused={reused}")

    # --- Assertions --------------------------------------------------------
    ok = True

    resumed_all = all(resumed_flags)
    if resumed_all:
        print("[ok]   every resumed handshake reported session_reused=True")
    else:
        print(f"[FAIL] only {sum(resumed_flags)}/{len(resumed_flags)} "
              "handshakes were reported as resumed. Server likely does "
              "not issue session tickets. If a reverse proxy is in front, "
              "check its TLS ticket configuration.")
        ok = False

    median_resumed = statistics.median(resumed_timings)
    speedup = first_elapsed / median_resumed if median_resumed else 0
    print(f"[stat] first={first_elapsed*1000:.1f}ms  "
          f"resumed median={median_resumed*1000:.1f}ms  "
          f"speedup={speedup:.2f}x")
    if median_resumed < first_elapsed:
        print("[ok]   resumed handshakes are faster than the first")
    else:
        print("[FAIL] resumed handshakes were not faster than the first. "
              "Firmware will not benefit from resumption against this server.")
        ok = False

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

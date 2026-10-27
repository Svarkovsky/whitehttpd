#!/usr/bin/env python3
"""Small manual probe against a device.

Needs WH_HOST and WH_PORT, nothing is defaulted: addresses and ports from one
test bench mean nothing on another device or network.

    export WH_HOST=192.0.2.10      # your device
    export WH_PORT=8081            # a free port on it
    python3 tests/probe.py
"""
import os
import socket

H = os.environ.get("WH_HOST")
if not H:
    sys.exit("WH_HOST is not set: point it at the device under test")
P = int(os.environ.get("WH_PORT", "8081"))

CASES = [
    ("200", b"GET /a.txt HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n"),
    ("206", b"GET /a.txt HTTP/1.1\r\nHost: t\r\nRange: bytes=0-3\r\nConnection: close\r\n\r\n"),
    ("301", b"GET /space%20dir HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n"),
    ("404", b"GET /nope HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n"),
    ("403", b"GET /nolist/ HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n"),
    ("400", b"GET /a.txt HTTP/1.1\r\nConnection: close\r\n\r\n"),
    ("501", b"POST / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n"),
]

for name, req in CASES:
    s = socket.create_connection((H, P), timeout=6)
    s.sendall(req)
    buf = b""
    while True:
        d = s.recv(65536)
        if not d:
            break
        buf += d
    s.close()
    print(f"--- {name}\n" + buf.split(b"\r\n\r\n")[0].decode(errors="replace") + "\n")

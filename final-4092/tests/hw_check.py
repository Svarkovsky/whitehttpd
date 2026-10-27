#!/usr/bin/env python3
"""Functional checks of the MIPS BE reference against a real device.

Runs over the network, so it needs a host you control. Nothing here has a
default address: WH_HOST and WH_PORT must both be set, because the addresses
and ports from one test bench mean nothing on another device or network.
There is no default: the script exits rather than guess.

Before the first run create the test root on the device, see setup_root.sh,
and start whitehttpd on the port you are going to use. Use a spare port; do
not disturb a production instance that may be listening on port 80.

    export WH_HOST=192.0.2.10      # your device
    export WH_PORT=8081            # a free port on it
    python3 tests/hw_check.py
"""
import hashlib
import os
import socket
import sys

H = os.environ.get("WH_HOST")
if not H:
    sys.exit("WH_HOST is not set: point it at the device under test")
P = int(os.environ.get("WH_PORT")
        or sys.exit("WH_PORT is not set: point it at a free port on the device"))


def raw(req, timeout=6):
    s = socket.create_connection((H, P), timeout)
    s.sendall(req)
    buf = b""
    try:
        while True:
            d = s.recv(65536)
            if not d:
                break
            buf += d
    except socket.timeout:
        pass
    s.close()
    return buf


def head(req):
    s = socket.create_connection((H, P), timeout=6)
    s.sendall(req)
    buf = b""
    while b"\r\n\r\n" not in buf:
        d = s.recv(65536)
        if not d:
            break
        buf += d
    s.close()
    return buf


def get(path, extra="", ver="1.1"):
    return raw(
        f"GET {path} HTTP/{ver}\r\nHost: t\r\n{extra}Connection: close\r\n\r\n".encode()
    )


def status(r):
    return r.split(b"\r\n")[0]


def ctype(r):
    return [l for l in r.split(b"\r\n") if b"Content-Type" in l]


res = []


def chk(name, cond, info=""):
    res.append((name, bool(cond), info))


# --- basic responses ---------------------------------------------------------
r = get("/")
chk("GET / -> 200 + body", b"200" in status(r) and b"HELLO_ROOT" in r, status(r))
r = get("/a.txt")
chk("GET /a.txt body", b"0123456789abcdef" in r, status(r))
r = head(b"HEAD /a.txt HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n")
chk("HEAD without body", b"200" in status(r) and r.endswith(b"\r\n\r\n"), status(r))
r = get("/nope")
chk("404", b"404" in status(r), status(r))
r = head(b"HEAD /nope HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n")
chk("HEAD 404 without body", b"404" in status(r) and r.endswith(b"\r\n\r\n"), status(r))
r = get("/a.txt", ver="1.0")
chk("HTTP/1.0 echoed", b"HTTP/1.0 200" in status(r), status(r))

# --- directories and redirects ---------------------------------------------------
r = get("/space%20dir")
chk("301 Location from the raw URL",
    b"301" in status(r) and b"Location: /space%20dir/" in r, status(r))
r = get("/sub")
chk("/sub -> 301", b"301" in status(r), status(r))
r = get("/sub/")
chk("/sub/ with index.html -> 200", b"200" in status(r) and b"SUBIDX" in r, status(r))
r = get("/nolist/")
chk("directory without index -> 403", b"403" in status(r), status(r))
r = get("/nolist")
chk("directory without index, no slash -> 301", b"301" in status(r), status(r))

# --- security -----------------------------------------------------------
r = get("/link_file.txt")
chk("symlink to file -> 404", b"404" in status(r), status(r))
r = get("/escdir/secret_outside.txt")
chk("symlink to directory -> 404", b"404" in status(r), status(r))
r = get("/escdir2.txt")
chk("symlink to file, second one -> 404", b"404" in status(r), status(r))
r = get("/escdir2/whsecret.txt")
chk("real directory in root -> 200", b"200" in status(r), status(r))
r = get("/../../etc/passwd")
chk("traversal -> 400", b"400" in status(r), status(r))
r = get("/.")
chk("/. -> 403/404", b"403" in status(r) or b"404" in status(r), status(r))
r = get("/..")
chk("/.. -> 400/403", b"400" in status(r) or b"403" in status(r), status(r))
r = get("/.dotfile")
chk("dotfile -> 403", b"403" in status(r), status(r))
r = raw(b"GET /a.txt HTTP/1.1\r\nHost: t\r\nX: a\r\n b\r\nConnection: close\r\n\r\n")
chk("obs-fold -> 400", b"400" in status(r), status(r))
r = raw(b"GET /a.txt HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n")
chk("Transfer-Encoding rejected", b"400" in status(r) or b"501" in status(r), status(r))
r = raw(b"GET /a.txt HTTP/1.1\r\nHost: t\r\nX: a\x01b\r\nConnection: close\r\n\r\n")
chk("control character in header -> 400", b"400" in status(r), status(r))
r = raw(b"GET /a.txt HTTP/1.1\r\nConnection: close\r\n\r\n")
chk("no Host in HTTP/1.1 -> 400", b"400" in status(r), status(r))
r = raw(b"GET /" + b"a" * 2000 + b" HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n")
chk("URL > 1024 rejected", b"400" in status(r) or b"414" in status(r), status(r))

# --- methods -----------------------------------------------------------------
r = raw(b"POST /a.txt HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n")
chk("POST -> 501", b"501" in status(r), status(r))
r = raw(b"PUT /a.txt HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n")
chk("PUT -> 501", b"501" in status(r), status(r))

# --- MIME -------------------------------------------------------------------
r = get("/index.html")
chk("MIME html", b"text/html" in r, ctype(r))
r = get("/big.bin")
chk("MIME bin", b"application/octet-stream" in r, ctype(r))
r = get("/a.txt")
chk("MIME txt + charset", b"text/plain; charset=utf-8" in r, ctype(r))
r = get("/")
chk("MIME for GET / (index.html) = text/html", b"text/html" in r, ctype(r))
r = get("/sub/")
chk("MIME for GET /sub/ = text/html", b"text/html" in r, ctype(r))

# --- keep-alive and pipelining ------------------------------------------------
s = socket.create_connection((H, P), timeout=6)
s.sendall(b"GET /a.txt HTTP/1.1\r\nHost: t\r\n\r\n"
          b"GET /index.html HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n")
buf = b""
try:
    while True:
        d = s.recv(65536)
        if not d:
            break
        buf += d
except socket.timeout:
    pass
s.close()
chk("keep-alive + pipelining", buf.count(b"HTTP/1.1 200") == 2,
    f"count={buf.count(b'HTTP/1.1 200')}")

# --- large file integrity --------------------------------------------
s = socket.create_connection((H, P), timeout=20)
s.sendall(b"GET /big.bin HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n")
buf = b""
while True:
    d = s.recv(65536)
    if not d:
        break
    buf += d
s.close()
body = buf.split(b"\r\n\r\n", 1)[1] if b"\r\n\r\n" in buf else b""
chk("4 MB transferred in full", len(body) == 4194304,
    f"got={len(body)} md5={hashlib.md5(body).hexdigest()[:8]}")

# --- Cache-Control: cacheable responses only ---
chk("200 Cache-Control", b"Cache-Control: max-age=60" in get("/a.txt"), ctype(get("/a.txt")))
chk("206 Cache-Control", b"Cache-Control: max-age=60" in get("/a.txt", "Range: bytes=0-3\r\n"), "")
chk("404 without Cache-Control", b"Cache-Control" not in get("/nope"), ctype(get("/nope")))
chk("403 without Cache-Control", b"Cache-Control" not in get("/.dotfile"), ctype(get("/.dotfile")))

# --- Range: first-byte-pos overflow -> 416 ---
for spec in ("4294967296-", "9999999999-"):
    r = get("/a.txt", "Range: bytes=" + spec + "\r\n")
    chk("416 overflow " + spec, b"416" in r.split(b"\r\n")[0], r.split(b"\r\n")[0])

ok = sum(1 for _, c, _ in res if c)
for n, c, i in res:
    print(("PASS " if c else "FAIL ") + n + ("" if c else "  << " + str(i)))
print(f"\n{ok}/{len(res)} PASS")
sys.exit(0 if ok == len(res) else 1)

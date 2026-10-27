#!/usr/bin/env python3
"""Functional test-suite for whitehttpd (runs the ELF under qemu-mips-static)."""
import os, sys, time, socket, subprocess, shutil, threading

ROOT = os.path.dirname(os.path.abspath(__file__))
WWW = os.path.join(ROOT, "wwwroot")
PORT = 18951
# Runs the shipped binary. QEMU 7.2.22 accepts an image with no section
# table, so a separate RWX build (wq.bin) is not needed.
BIN = os.path.join(ROOT, "..", "whitehttpd-mips32be")

FILES = {
    "index.html": b"<html>idx</html>",
    "a.txt": bytes(range(256)) * 2,          # 512 bytes
    "s.html": b"0123456789ABCDEF",           # 16 bytes
    "x.css": b"css", "y.js": b"js", "z.json": b"{}",
    "p.png": b"PNG", "q.jpeg": b"JPG", "r.jpg": b"JPG2", "g.gif": b"GIF",
    "v.svg": b"SVG", "i.ico": b"ICO", "d.pdf": b"PDF",
    "w.wasm": b"\0asm", "k.zip": b"PK",
    "m.mjs": b"mjs", "n.md": b"md", "o.htm": b"htm",
    "bin.dat": b"\x00\x01\x02\x03",
    ".hidden": b"secret",
    "sub/deep.txt": b"deep",
}
MIME = {
    ".html": "text/html; charset=utf-8", ".htm": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8", ".js": "application/javascript; charset=utf-8",
    ".mjs": "application/javascript; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".txt": "text/plain; charset=utf-8", ".md": "text/plain; charset=utf-8",
    ".png": "image/png", ".jpeg": "image/jpeg", ".jpg": "image/jpeg",
    ".gif": "image/gif", ".svg": "image/svg+xml", ".ico": "image/x-icon",
    ".pdf": "application/pdf", ".wasm": "application/wasm",
    ".zip": "application/zip", ".dat": "application/octet-stream",
}

failures = []
def check(name, cond, info=""):
    if cond:
        print("  ok   %s" % name)
    else:
        print("  FAIL %s  %s" % (name, info))
        failures.append(name)

def setup():
    shutil.rmtree(WWW, ignore_errors=True)
    for p, data in FILES.items():
        full = os.path.join(WWW, p)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        open(full, "wb").write(data)

def req(raw, timeout=5.0, two=False):
    s = socket.create_connection(("127.0.0.1", PORT), timeout=timeout)
    s.sendall(raw)
    time.sleep(0.30)
    out = b""
    s.settimeout(0.6)
    try:
        while True:
            b = s.recv(65536)
            if not b:
                break
            out += b
    except (socket.timeout, ConnectionResetError):
        pass
    s.close()
    return out

def hdr(resp):
    head = resp.split(b"\r\n\r\n")[0]
    return head.decode("latin1")

def status(resp):
    return hdr(resp).split("\r\n")[0]

def get(path, extra=b"", ver=b"1.1", host=b"h"):
    return (b"GET " + path + b" HTTP/" + ver + b"\r\nHost: " + host +
            b"\r\n" + extra + b"\r\n")

print("preparing wwwroot")
setup()
srv = subprocess.Popen(["qemu-mips-static", BIN, "--port", str(PORT), WWW],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1.2)
try:
    print("\n[1] basic GET / status line / headers")
    r = req(get(b"/index.html"))
    check("200 status", status(r).startswith("HTTP/1.1 200 OK"), status(r))
    check("server hdr", "Server: whitehttpd" in hdr(r), hdr(r))
    check("clen hdr", "Content-Length: %d" % len(FILES["index.html"]) in hdr(r), hdr(r))
    check("ctype html", "Content-Type: text/html; charset=utf-8" in hdr(r), hdr(r))
    check("body", r.split(b"\r\n\r\n", 1)[1] == FILES["index.html"])
    check("keepalive", "Connection: keep-alive" in hdr(r), hdr(r))

    print("\n[2] MIME table (13 types + default)")
    for ext, want in MIME.items():
        name = ("t" + ext).encode()
        open(os.path.join(WWW, name.decode()), "wb").write(b"x")
        r = req(get(b"/" + name))
        check("mime " + ext, "Content-Type: " + want in hdr(r), hdr(r))

    print("\n[3] default index.html for directory")
    r = req(get(b"/"))
    check("dir index 200", status(r).startswith("HTTP/1.1 200 OK"), status(r))
    check("dir index body", r.split(b"\r\n\r\n", 1)[1] == FILES["index.html"])
    r = req(get(b"/sub/"))
    check("sub dir 403 (no index)", status(r).startswith("HTTP/1.1 403"), status(r))

    print("\n[4] 301 redirect")
    r = req(get(b"/sub"))
    check("301", status(r).startswith("HTTP/1.1 301"), status(r))
    check("location", "Location: /sub/" in hdr(r), hdr(r))
    check("301 no body", r.split(b"\r\n\r\n", 1)[1] == b"", r)

    print("\n[5] errors")
    r = req(get(b"/nope.txt"))
    check("404", status(r).startswith("HTTP/1.1 404"), status(r))
    r = req(get(b"/.hidden"))
    check("403 dotfile", status(r).startswith("HTTP/1.1 403"), status(r))
    r = req(b"POST /index.html HTTP/1.1\r\nHost: h\r\n\r\n")
    check("501", status(r).startswith("HTTP/1.1 501"), status(r))
    r = req(b"GET /index.html HTTP/2.0\r\nHost: h\r\n\r\n")
    check("400 bad version", status(r).startswith("HTTP/1.1 400"), status(r))
    r = req(b"GET / HTTP/1.1\r\n\r\n")
    check("400 no host", status(r).startswith("HTTP/1.1 400"), status(r))
    r = req(b"GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n")
    check("400 dup host", status(r).startswith("HTTP/1.1 400"), status(r))
    r = req(b"GET / HTTP/1.1\r\nHost: a\r\nX: y\r\n  z\r\n\r\n")
    check("400 obs-fold", status(r).startswith("HTTP/1.1 400"), status(r))
    r = req(b"GET / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n")
    check("400 TE", status(r).startswith("HTTP/1.1 400"), status(r))
    r = req(b"GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\n\r\n")
    check("400 CL", status(r).startswith("HTTP/1.1 400"), status(r))
    r = req(b"GET //index.html HTTP/1.1\r\nHost: a\r\n\r\n")
    check("400 double slash", status(r).startswith("HTTP/1.1 400"), status(r))
    r = req(b"GET /../etc/passwd HTTP/1.1\r\nHost: a\r\n\r\n")
    check("400 dotdot", status(r).startswith("HTTP/1.1 400"), status(r))
    r = req(b"GET /sub/../index.html HTTP/1.1\r\nHost: a\r\n\r\n")
    check("400 embedded dotdot", status(r).startswith("HTTP/1.1 400"), status(r))
    r = req(b"GET /index.html HTTP/1.1\r\nHost: a\r\nX" + b"y" * 9000 + b"\r\n\r\n")
    check("400 oversize header", status(r).startswith("HTTP/1.1 400"), status(r))
    r = req(b"GET /index.html HTTP/1.1\r\nHost: \r\n\r\n")
    check("400 empty host value", status(r).startswith("HTTP/1.1 400"), status(r))
    r = req(b"GET /%2e%2e/x HTTP/1.1\r\nHost: a\r\n\r\n")
    check("400 %2e%2e", status(r).startswith("HTTP/1.1 400"), status(r))
    r = req(b"GET /%2findex.html HTTP/1.1\r\nHost: a\r\n\r\n")
    check("400 %2F", status(r).startswith("HTTP/1.1 400"), status(r))

    print("\n[6] HEAD + HTTP/1.0 + close")
    r = req(b"HEAD /a.txt HTTP/1.1\r\nHost: h\r\n\r\n")
    check("head 200", status(r).startswith("HTTP/1.1 200 OK"), status(r))
    check("head no body", r.split(b"\r\n\r\n", 1)[1] == b"", repr(r[-40:]))
    check("head clen", "Content-Length: 512" in hdr(r), hdr(r))
    r = req(b"HEAD /nope HTTP/1.1\r\nHost: h\r\n\r\n")
    check("head 404 no body", r.split(b"\r\n\r\n", 1)[1] == b"")
    r = req(b"GET /index.html HTTP/1.0\r\n\r\n")
    check("1.0 no host ok", status(r).startswith("HTTP/1.0 200 OK"), status(r))
    check("1.0 close", "Connection: close" in hdr(r), hdr(r))
    r = req(b"GET /index.html HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n")
    check("conn close", "Connection: close" in hdr(r), hdr(r))

    r = req(b"GET /index.html HTTP/1.1\nHost: h\n\n")
    check("bare LF request", status(r).startswith("HTTP/1.1 200 OK"), status(r))
    r = req(b"GET /index.html HTTP/1.1\r\nHost:h\r\n\r\n")
    check("no space after colon", status(r).startswith("HTTP/1.1 200 OK"), status(r))
    r = req(b"GET /index.html HTTP/1.1\r\nHost:   \th\r\n\r\n")
    check("OWS space+tab", status(r).startswith("HTTP/1.1 200 OK"), status(r))

    print("\n[7] pipelining")
    r = req(get(b"/index.html") + get(b"/s.html") + get(b"/a.txt"), timeout=6)
    check("pipelined 3 answers", r.count(b"HTTP/1.1 200 OK") == 3, r.count(b"HTTP/1.1 200"))

    print("\n[8] urldecode")
    r = req(get(b"/s.html?a=b"))
    check("query ignored", status(r).startswith("HTTP/1.1 200 OK"), status(r))
    r = req(get(b"/%73.html"))
    check("%73 decode", status(r).startswith("HTTP/1.1 200 OK"), status(r))

    print("\n[9] Range (RFC 7233)")
    r = req(get(b"/s.html", b"Range: bytes=0-9\r\n"))
    check("206 0-9", status(r).startswith("HTTP/1.1 206"), status(r))
    check("206 crange", "Content-Range: bytes 0-9/16" in hdr(r), hdr(r))
    check("206 clen", "Content-Length: 10" in hdr(r), hdr(r))
    check("206 body", r.split(b"\r\n\r\n", 1)[1] == b"0123456789", r)

    r = req(get(b"/s.html", b"Range: bytes=10-\r\n"))
    check("206 10-", "Content-Range: bytes 10-15/16" in hdr(r), hdr(r))
    check("206 10- body", r.split(b"\r\n\r\n", 1)[1] == b"ABCDEF", r)

    r = req(get(b"/s.html", b"Range: bytes=-5\r\n"))
    check("206 -5 (suffix)", status(r).startswith("HTTP/1.1 206"), status(r))
    check("206 -5 crange", "Content-Range: bytes 11-15/16" in hdr(r), hdr(r))
    check("206 -5 body", r.split(b"\r\n\r\n", 1)[1] == b"BCDEF", r)

    r = req(get(b"/s.html", b"Range: bytes=-16\r\n"))
    check("-16 == size -> 206 full", status(r).startswith("HTTP/1.1 206"), status(r))
    check("-16 == size crange", "Content-Range: bytes 0-15/16" in hdr(r), hdr(r))
    r = req(get(b"/s.html", b"Range: bytes=-100\r\n"))
    check("-100 > size -> 206 full", status(r).startswith("HTTP/1.1 206"), status(r))
    r = req(get(b"/s.html", b"Range: bytes=-0\r\n"))
    check("-0 -> 416", status(r).startswith("HTTP/1.1 416"), status(r))
    r = req(get(b"/s.html", b"Range: bytes=-\r\n"))
    check("- (no digits) -> 200", status(r).startswith("HTTP/1.1 200 OK"), status(r))

    r = req(get(b"/s.html", b"Range: bytes=0-999\r\n"))
    check("clamp to", "Content-Range: bytes 0-15/16" in hdr(r), hdr(r))

    r = req(get(b"/s.html", b"Range: bytes=20-\r\n"))
    check("416 from>=size", status(r).startswith("HTTP/1.1 416"), status(r))
    check("416 content-range", "Content-Range: bytes */16" in hdr(r), hdr(r))

    r = req(get(b"/s.html", b"Range: bytes=0-1,5-6\r\n"))
    check("multi-range -> 200", status(r).startswith("HTTP/1.1 200 OK"), status(r))
    check("multi-range no CR", "Content-Range" not in hdr(r), hdr(r))

    r = req(get(b"/s.html", b"Range: bytes=abc\r\n"))
    check("garbage -> 200", status(r).startswith("HTTP/1.1 200 OK"), status(r))
    r = req(get(b"/s.html", b"Range: bytes=\r\n"))
    check("empty -> 200", status(r).startswith("HTTP/1.1 200 OK"), status(r))
    r = req(get(b"/s.html", b"Range: bytes=5-3\r\n"))
    check("5-3 -> 416", status(r).startswith("HTTP/1.1 416"), status(r))
    r = req(get(b"/s.html", b"Range: bytes=0-9 \r\n"))
    check("trailing space -> 200", status(r).startswith("HTTP/1.1 200 OK"), status(r))
    r = req(get(b"/s.html", b"Range: items=0-9\r\n"))
    check("other unit -> ignored (no header match) 200",
          status(r).startswith("HTTP/1.1 200 OK"), status(r))
    r = req(b"HEAD /s.html HTTP/1.1\r\nHost: h\r\nRange: bytes=0-3\r\n\r\n")
    check("head+range 206 no body", status(r).startswith("HTTP/1.1 206") and
          r.split(b"\r\n\r\n", 1)[1] == b"", status(r))
    r = req(get(b"/a.txt", b"Range: bytes=500-\r\n"))
    check("tail of 512 file", r.split(b"\r\n\r\n", 1)[1] == FILES["a.txt"][500:], r[-20:])

    # 10+ digit first-byte-pos overflows 32 bits -> beyond any file -> 416
    for spec in (b"4294967296-", b"9999999999-", b"12345678901234-"):
        r = req(get(b"/s.html", b"Range: bytes=" + spec + b"\r\n"))
        check("overflow %s -> 416" % spec.decode(), status(r).startswith("HTTP/1.1 416"), status(r))

    print("\n[10] Cache-Control")
    r = req(get(b"/s.html"))
    check("200 has Cache-Control", b"Cache-Control: max-age=60" in r, hdr(r))
    r = req(get(b"/s.html", b"Range: bytes=0-3\r\n"))
    check("206 has Cache-Control", b"Cache-Control: max-age=60" in r, hdr(r))
    r = req(get(b"/nope"))
    check("404 has no Cache-Control", b"Cache-Control" not in r, hdr(r))
    r = req(get(b"/.hidden"))
    check("403 has no Cache-Control", b"Cache-Control" not in r, hdr(r))
    r = req(get(b"/s.html", b"Range: bytes=999-\r\n"))
    check("416 has no Cache-Control", b"Cache-Control" not in r, hdr(r))
    r = req(b"POST / HTTP/1.1\r\nHost: h\r\n\r\n")
    check("501 has no Cache-Control", b"Cache-Control" not in r, hdr(r))
    r = req(get(b"/sub"))
    check("301 has Cache-Control", b"Cache-Control: max-age=60" in r, hdr(r))
    r = req(b"HEAD /s.html HTTP/1.1\r\nHost: h\r\n\r\n")
    check("HEAD 200 has Cache-Control", b"Cache-Control: max-age=60" in r, hdr(r))

finally:
    srv.terminate()
    srv.wait()

print("\n==== %d failure(s) ====" % len(failures))
for f in failures:
    print("  " + f)
sys.exit(1 if failures else 0)

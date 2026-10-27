#!/usr/bin/env python3
"""Tier-1 feature tests: dotfiles, Cache-Control, ETag, 304, If-Range, logs.

Drives a real process over a socket. Usage:
    python3 test_tier1.py [--binary PATH] [--runner "qemu-mips-static"]
"""
import argparse, os, socket, subprocess, sys, tempfile, time, pathlib

failures = []

def check(name, cond, info=""):
    if cond:
        print("  ok   " + name)
    else:
        failures.append(name)
        print("  FAIL " + name + ("  << " + str(info) if info else ""))

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p

def req(port, path, extra="", method="GET"):
    s = socket.create_connection(("127.0.0.1", port), 5)
    s.sendall(("%s %s HTTP/1.1\r\nHost: t\r\n%sConnection: close\r\n\r\n"
               % (method, path, extra)).encode())
    buf = b""
    while True:
        d = s.recv(65536)
        if not d: break
        buf += d
    s.close(); return buf

def head(r): return r.split(b"\r\n\r\n", 1)[0].decode(errors="replace")
def body(r): return r.split(b"\r\n\r\n", 1)[1] if b"\r\n\r\n" in r else b""
def status(r): return r.split(b"\r\n")[0].decode(errors="replace")
def hval(r, name):
    for line in head(r).split("\r\n"):
        if line.lower().startswith(name.lower() + ":"):
            return line.split(":", 1)[1].strip()
    return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=str(pathlib.Path(__file__).resolve().parents[1] / "build/whitehttpd-nolibc-x86_64"))
    ap.add_argument("--runner", default="")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        www = pathlib.Path(tmp) / "www"; www.mkdir()
        (www / "a.txt").write_bytes(b"0123456789abcdef")
        os.utime(www / "a.txt", (1709649000, 1709649000))  # 2024-03-05 14:30:00 UTC
        (www / ".env").write_bytes(b"SECRET")
        (www / ".git").mkdir(); (www / ".git" / "config").write_bytes(b"git")
        well = www / ".well-known" / "acme-challenge"; well.mkdir(parents=True)
        (well / "tok").write_bytes(b"TOKEN")
        (www / "sub").mkdir(); (www / "sub" / "f.txt").write_bytes(b"s")

        port = free_port()
        log = pathlib.Path(tmp) / "access.log"
        cmd = ([args.runner] if args.runner else []) + [args.binary, str(www),
               "--port", str(port), "--workers", "1", "--log", str(log)]
        srv = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            for _ in range(100):
                try:
                    socket.create_connection(("127.0.0.1", port), 0.2).close(); break
                except OSError: time.sleep(0.05)
            else:
                print("server did not start"); return 1

            print("[dotfiles]")
            check("plain file, 200", status(req(port, "/a.txt")).startswith("HTTP/1.1 200"))
            check(".env -> 403", status(req(port, "/.env")).startswith("HTTP/1.1 403"))
            check(".git/config -> 403", status(req(port, "/.git/config")).startswith("HTTP/1.1 403"))
            check("acme token 200", body(req(port, "/.well-known/acme-challenge/tok")) == b"TOKEN")
            check("traversal via .well-known -> 403",
                  status(req(port, "/.well-known/../.env")).startswith("HTTP/1.1 403"))

            print("[Cache-Control]")
            r = req(port, "/a.txt")
            check("200 carries Cache-Control", "max-age=60" in (hval(r, "Cache-Control") or ""))
            r = req(port, "/a.txt", "Range: bytes=0-3\r\n")
            check("206 carries Cache-Control", "max-age=60" in (hval(r, "Cache-Control") or ""))
            r = req(port, "/nope")
            check("404 without Cache-Control", hval(r, "Cache-Control") is None)

            print("[ETag / Last-Modified]")
            r = req(port, "/a.txt")
            etag, lm = hval(r, "ETag"), hval(r, "Last-Modified")
            check("ETag present", etag is not None and etag.startswith('"'), etag)
            check("Last-Modified present", lm == "Tue, 05 Mar 2024 14:30:00 GMT", lm)
            check("404 without ETag", hval(req(port, "/nope"), "ETag") is None)

            print("[304 Not Modified]")
            r = req(port, "/a.txt", "If-None-Match: %s\r\n" % etag)
            check("If-None-Match match -> 304", status(r).startswith("HTTP/1.1 304"), status(r))
            check("304 without body", body(r) == b"")
            check("304 without Content-Length", hval(r, "Content-Length") is None)
            r = req(port, "/a.txt", 'If-None-Match: "deadbeef-99"\r\n')
            check("If-None-Match mismatch -> 200", status(r).startswith("HTTP/1.1 200"))
            r = req(port, "/a.txt", "If-Modified-Since: %s\r\n" % lm)
            check("If-Modified-Since equal -> 304", status(r).startswith("HTTP/1.1 304"), status(r))
            r = req(port, "/a.txt", "If-Modified-Since: Sun, 06 Nov 1994 08:49:37 GMT\r\n")
            check("If-Modified-Since older -> 200", status(r).startswith("HTTP/1.1 200"))
            r = req(port, "/a.txt", "If-None-Match: *\r\n")
            check("If-None-Match: * -> 304", status(r).startswith("HTTP/1.1 304"))
            # If-None-Match takes precedence over If-Modified-Since (RFC 7232 6).
            r = req(port, "/a.txt", 'If-None-Match: "deadbeef-99"\r\nIf-Modified-Since: %s\r\n' % lm)
            check("If-None-Match wins over If-Modified-Since -> 200",
                  status(r).startswith("HTTP/1.1 200"), status(r))
            r = req(port, "/a.txt", 'If-None-Match: %s\r\nIf-Modified-Since: Sun, 06 Nov 1994 08:49:37 GMT\r\n' % etag)
            check("matching ETag wins over stale date -> 304",
                  status(r).startswith("HTTP/1.1 304"), status(r))

            print("[If-Range]")
            r = req(port, "/a.txt", "Range: bytes=0-3\r\nIf-Range: %s\r\n" % etag)
            check("range match -> 206", status(r).startswith("HTTP/1.1 206"), status(r))
            check("body is 4 bytes", body(r) == b"0123", body(r))
            r = req(port, "/a.txt", 'Range: bytes=0-3\r\nIf-Range: "deadbeef-99"\r\n')
            check("range mismatch -> 200 with full body", status(r).startswith("HTTP/1.1 200") and body(r) == b"0123456789abcdef")
            r = req(port, "/a.txt", "Range: bytes=0-3\r\nIf-Range: %s\r\n" % lm)
            check("date match -> 206", status(r).startswith("HTTP/1.1 206"), status(r))

            print("[HEAD]")
            r = req(port, "/a.txt", method="HEAD")
            check("HEAD 200 without body", status(r).startswith("HTTP/1.1 200") and body(r) == b"")
            r = req(port, "/a.txt", "If-None-Match: %s\r\n" % etag, method="HEAD")
            check("HEAD 304 without body", status(r).startswith("HTTP/1.1 304") and body(r) == b"")

            print("[multi-range: ignored per RFC 7233 s3.1]")
            r = req(port, "/a.txt", "Range: bytes=0-1,4-5\r\n")
            check("multi-range -> 200 full body",
                  status(r).startswith("HTTP/1.1 200") and body(r) == b"0123456789abcdef", status(r))

            print("[logs]")
            time.sleep(0.3)
            text = log.read_text(errors="replace") if log.exists() else ""
            check("log created", bool(text))
            check("200 line in log", '"GET /a.txt HTTP/1.1" 200 16' in text, text[:200])
            check("404 line in log", '" 404 ' in text)
            check("CLF date", "/2026:" in text or "/20" in text, text[:120])
            check("IPv4 in log", any(l.startswith("127.0.0.1 - - [") for l in text.splitlines()),
                  text[:120])

            print("[log overwrite above 256 KiB]")
            log.write_bytes(b"x" * 300000)
            req(port, "/a.txt")
            time.sleep(0.3)
            size = log.stat().st_size
            check("log truncated and rewritten", size < 1000, size)
            check("log holds a fresh line", '"GET /a.txt HTTP/1.1" 200 16' in log.read_text(errors="replace"))
        finally:
            srv.terminate()
            try: srv.wait(timeout=5)
            except subprocess.TimeoutExpired: srv.kill()

    print("\n==== %d failure(s) ====" % len(failures))
    for f in failures: print("  " + f)
    return 1 if failures else 0

if __name__ == "__main__":
    sys.exit(main())

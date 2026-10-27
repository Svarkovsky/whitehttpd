#!/usr/bin/env python3
"""Protocol compliance and CLI tests: Date, Accept-Ranges, OPTIONS, 405,
--version, --help, --index, --no-listing, --pidfile, error log.

Usage: python3 test_tier2.py [--binary PATH] [--runner "qemu-mips-static"]
"""
import argparse, os, socket, subprocess, sys, tempfile, time, pathlib

failures = []

def check(name, cond, info=""):
    if cond: print("  ok   " + name)
    else:
        failures.append(name); print("  FAIL " + name + ("  << " + str(info) if info else ""))

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
def hval(r, n):
    for line in head(r).split("\r\n"):
        if line.lower().startswith(n.lower() + ":"): return line.split(":", 1)[1].strip()
    return None

def run(binary, runner, args):
    cmd = ([runner] if runner else []) + [binary] + args
    return subprocess.run(cmd, capture_output=True, timeout=30)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=str(pathlib.Path(__file__).resolve().parents[1] / "build/whitehttpd-nolibc-x86_64"))
    ap.add_argument("--runner", default="")
    a = ap.parse_args()

    print("[CLI: --version]")
    r = run(a.binary, a.runner, ["--version"])
    out = r.stdout.decode(errors="replace")
    check("exit code 0", r.returncode == 0, r.returncode)
    check("version present", "whitehttpd 1.0.0" in out, out[:80])
    check("author present", "Ivan Svarkovsky" in out)
    check("email present", "ivansvarkovsky@gmail.com" in out)
    check("license present", "AGPL-3.0-or-later" in out)
    check("project URL present", "github.com/Svarkovsky/whitehttpd" in out)

    print("[CLI: --help]")
    r = run(a.binary, a.runner, ["--help"])
    out = r.stdout.decode(errors="replace")
    check("exit code 0", r.returncode == 0)
    for opt in ["--port", "--addr", "--workers", "--index", "--no-listing",
                "--daemon", "--pidfile", "--log", "--version", "--help"]:
        check("description " + opt, opt in out)
    check("author in help", "Ivan Svarkovsky" in out)
    check("license in help", "AGPL-3.0-or-later" in out)
    check("Not implemented section", "Not implemented" in out)
    check("Examples section", "Examples:" in out)
    check("example with --daemon", "--daemon" in out.split("Examples:")[1])
    check("example with --port 80", "--port 80" in out)

    print("[CLI: -v and -h]")
    check("-v works", run(a.binary, a.runner, ["-v"]).stdout.decode().startswith("whitehttpd"))
    check("-h works", "--port" in run(a.binary, a.runner, ["-h"]).stdout.decode())

    with tempfile.TemporaryDirectory() as tmp:
        www = pathlib.Path(tmp) / "www"; www.mkdir()
        (www / "a.txt").write_bytes(b"0123456789abcdef")
        (www / "home.html").write_bytes(b"home")
        (www / "sub").mkdir(); (www / "sub" / "f.txt").write_bytes(b"s")
        port = free_port(); log = pathlib.Path(tmp) / "access.log"; pidf = pathlib.Path(tmp) / "pid"
        cmd = ([a.runner] if a.runner else []) + [a.binary, str(www), "--port", str(port),
               "--workers", "1", "--index", "home.html", "--no-listing",
               "--pidfile", str(pidf), "--log", str(log)]
        srv = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            for _ in range(100):
                try: socket.create_connection(("127.0.0.1", port), 0.2).close(); break
                except OSError: time.sleep(0.05)
            else:
                print("server did not start"); return 1

            print("[Date / Accept-Ranges]")
            r = req(port, "/a.txt")
            d = hval(r, "Date")
            check("Date present", d is not None, d)
            check("Date in IMF-fixdate format",
                  d is not None and d.endswith(" GMT") and "," in d and len(d) == 29, d)
            check("Accept-Ranges: bytes", hval(r, "Accept-Ranges") == "bytes", hval(r, "Accept-Ranges"))
            r = req(port, "/a.txt", "Range: bytes=0-3\r\n")
            check("Accept-Ranges on 206", hval(r, "Accept-Ranges") == "bytes")
            check("Server carries the version", hval(r, "Server") == "whitehttpd/1.0.0", hval(r, "Server"))

            print("[OPTIONS]")
            r = req(port, "/a.txt", method="OPTIONS")
            check("OPTIONS /a.txt -> 200", status(r).startswith("HTTP/1.1 200"), status(r))
            check("Allow: GET, HEAD, OPTIONS", hval(r, "Allow") == "GET, HEAD, OPTIONS", hval(r, "Allow"))
            r = req(port, "*", method="OPTIONS")
            check("OPTIONS * -> 200", status(r).startswith("HTTP/1.1 200"), status(r))
            check("OPTIONS * carries Allow", hval(r, "Allow") == "GET, HEAD, OPTIONS")

            print("[405 / 501]")
            for m in ("POST", "PUT", "DELETE", "PATCH"):
                r = req(port, "/a.txt", method=m)
                check("%s -> 405" % m, status(r).startswith("HTTP/1.1 405"), status(r))
                check("%s carries Allow" % m, hval(r, "Allow") == "GET, HEAD, OPTIONS")
            r = req(port, "/a.txt", method="TRACE")
            check("TRACE -> 501", status(r).startswith("HTTP/1.1 501"), status(r))
            r = req(port, "/a.txt", method="FOOBAR")
            check("unknown method -> 501", status(r).startswith("HTTP/1.1 501"), status(r))

            print("[no state leaks between requests]")
            s = socket.create_connection(("127.0.0.1", port), 5)
            s.sendall(b"OPTIONS /a.txt HTTP/1.1\r\nHost: t\r\n\r\n")
            s.recv(4096)
            s.sendall(b"GET /a.txt HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n")
            buf = b""
            while True:
                d = s.recv(65536)
                if not d: break
                buf += d
            s.close()
            check("GET after OPTIONS on the same connection returns a body",
                  body(buf) == b"0123456789abcdef", body(buf)[:40])

            print("[Allow does not leak]")
            req(port, "/a.txt", method="POST")
            r = req(port, "/a.txt")
            check("GET after POST without Allow", hval(r, "Allow") is None, hval(r, "Allow"))

            print("[--index]")
            r = req(port, "/")
            check("/ serves home.html", body(r) == b"home", body(r))

            print("[--no-listing]")
            r = req(port, "/sub/")
            check("directory without index -> 403", status(r).startswith("HTTP/1.1 403"), status(r))

            print("[--pidfile]")
            check("pidfile created", pidf.exists())
            if pidf.exists():
                txt = pidf.read_text().strip()
                check("pidfile holds a number", txt.isdigit(), txt)
                check("pidfile matches the process", int(txt) == srv.pid, (txt, srv.pid))

            print("[error log]")
            time.sleep(0.3)
            text = log.read_text(errors="replace") if log.exists() else ""
            check("startup line present", "starting whitehttpd 1.0.0" in text, text[:120])
            check("[error] marker present", "[error]" in text)
        finally:
            srv.terminate()
            try: srv.wait(timeout=5)
            except subprocess.TimeoutExpired: srv.kill()

    print("\n==== %d failure(s) ====" % len(failures))
    for f in failures: print("  " + f)
    return 1 if failures else 0

if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Range edges, Host validation, burst and concurrency against a real device.

Runs over the network, so it needs a host you control. Nothing here has a
default address: WH_HOST and WH_PORT must both be set, because the addresses
and ports from one test bench mean nothing on another device or network.
There is no default: the script exits rather than guess.

    export WH_HOST=192.0.2.10      # your device
    export WH_PORT=8081            # a free port on it
    python3 tests/hw_edges.py
"""
import hashlib
import os
import socket
import sys
import time
from concurrent.futures import ThreadPoolExecutor

H = os.environ.get("WH_HOST")
if not H:
    sys.exit("WH_HOST is not set: point it at the device under test")
P = int(os.environ.get("WH_PORT")
        or sys.exit("WH_PORT is not set: point it at a free port on the device"))
def raw(req,t=6):
    s=socket.create_connection((H,P),t); s.sendall(req); b=b""
    try:
        while True:
            d=s.recv(65536)
            if not d: break
            b+=d
    except socket.timeout: pass
    s.close(); return b
def get(p,extra="",ver="1.1"):
    return raw(f"GET {p} HTTP/{ver}\r\nHost: t\r\n{extra}Connection: close\r\n\r\n".encode())
res=[]
def chk(n,c,i=""): res.append((n,bool(c),i))
r=get("/nolist/"); chk("listing no-index -> 403", b"403" in r.split(b"\r\n")[0], r.split(b"\r\n")[0])
r=get("/nolist"); chk("dir no-index no slash -> 301", b"301" in r.split(b"\r\n")[0], r.split(b"\r\n")[0])
r=get("/."); chk("/. -> 403/404", b"403" in r.split(b"\r\n")[0] or b"404" in r.split(b"\r\n")[0], r.split(b"\r\n")[0])
r=get("/.."); chk("/.. -> 400/403", b"400" in r.split(b"\r\n")[0] or b"403" in r.split(b"\r\n")[0], r.split(b"\r\n")[0])
r=get("//a.txt"); _c=r.split(b"\r\n")[0]
chk("//a.txt -> 200 or 400, policy dependent", b"200" in _c or b"400" in _c, _c)
r=get("/a.txt")
hdr=r.split(b"\r\n\r\n")[0]
chk("200: Server + Content-Length",
    b"Server: whitehttpd" in hdr and b"Content-Length: 16" in hdr,
    hdr.decode(errors="replace").replace("\r\n"," | "))
# Range boundaries
for spec,exp,body in [("bytes=0-0",206,b"0"),("bytes=15-15",206,b"f"),("bytes=0-15",206,b"0123456789abcdef"),
                      ("bytes=16-20",416,None),("bytes=-16",206,b"0123456789abcdef"),("bytes=-17",206,b"0123456789abcdef"),
                      ("bytes=-1",206,b"f"),("bytes=0-999",206,b"0123456789abcdef")]:
    r=get("/a.txt",f"Range: {spec}\r\n")
    code=r.split(b"\r\n")[0]
    ok = (str(exp).encode() in code) and (body is None or r.endswith(body))
    chk(f"Range {spec} -> {exp}", ok, code)
# malformed ranges
for spec in ["bytes=abc","bytes=5-2","bytes=","items=0-1","bytes=0-1-2"]:
    r=get("/a.txt",f"Range: {spec}\r\n"); code=r.split(b"\r\n")[0]
    chk(f"bad Range '{spec}' -> 200/416", b"200" in code or b"416" in code, code)
# Host validation
for h in ["t","t:8081","[::1]","bad host","t\r\nX: y"]:
    r=raw(f"GET /a.txt HTTP/1.1\r\nHost: {h}\r\nConnection: close\r\n\r\n".encode())
    code=r.split(b"\r\n")[0]
    chk(f"Host '{h[:12]}' handled", b"200" in code or b"400" in code, code)
# burst 500 sequential
t0=time.time(); bad=0
for i in range(500):
    r=get("/a.txt")
    if b"200" not in r.split(b"\r\n")[0] or not r.endswith(b"0123456789abcdef"): bad+=1
chk("burst 500 sequential", bad==0, f"bad={bad} {time.time()-t0:.2f}s")
# 64 concurrent
def one(i): 
    r=get("/a.txt"); return b"200" in r.split(b"\r\n")[0]
with ThreadPoolExecutor(64) as ex: rs=list(ex.map(one,range(2000)))
chk("2000 req / 64 threads", all(rs), f"ok={sum(rs)}/2000")
# slow client / timeout presence (no crash)
s=socket.create_connection((H,P),6); s.sendall(b"GET /a.txt HTTP/1.1\r\n"); time.sleep(7); s.close()
r=get("/a.txt"); chk("alive after slow client", b"200" in r.split(b"\r\n")[0], r.split(b"\r\n")[0])
ok=sum(1 for _,c,_ in res if c)
for n,c,i in res: print(("PASS " if c else "FAIL ")+n+("" if c else "  << "+str(i)))
print(f"\n{ok}/{len(res)} PASS")
sys.exit(0 if ok == len(res) else 1)

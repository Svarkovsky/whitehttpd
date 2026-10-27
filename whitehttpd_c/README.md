# whitehttpd 1.0.0 - freestanding C

A static file HTTP server written in C with no libc. Every function it needs
- socket setup, epoll, file open, date formatting, string handling - is called
as a raw system call through a thin `nosyscall` layer, so the binary links
against nothing and runs on any Linux that speaks the same ABI.

Ships for three targets from one source:

| Target | Binary | Size |
| --- | --- | ---: |
| x86-64 | `build/whitehttpd-nolibc-x86_64` | 17 008 |
| MIPS32r2 BE | `build/whitehttpd-nolibc-mips32be` | 20 892 |
| MIPS32r2 LE | `build/whitehttpd-nolibc-mips32el` | 20 908 |

Memory is static. No `malloc`, no `realloc`, no `snprintf`, no heap of any kind.
`sizeof(struct connection)` is 128 bytes and every buffer is a fixed-size array
sized at compile time.

## Build

```sh
make build-c            # x86-64
make build-c-mips       # both MIPS targets
make                   # assembler reference and x86-64 together
```

```sh
gcc                -Oz -fno-pic -fno-pie -ffreestanding -nostdlib -static ...
mipsel-linux-gnu-gcc -Oz -march=mips32r2 -mabi=32 -mno-abicalls -G 0 \
                     -ffreestanding -nostdlib -EB ...   # or -EL for little-endian
```

Both endiannesses come from the same `mipsel-linux-gnu-gcc`, the only difference
is `-EB` versus `-EL`. `gcc-mips-linux-gnu` is not needed.

## Run

```sh
./whitehttpd-nolibc-x86_64 [options] [directory]
```

| Flag | Meaning |
| --- | --- |
| `--port N` | listening port, default 8080 |
| `--addr A` | bind address, default all interfaces |
| `--daemon` | detach from the terminal |
| `--workers N` | worker processes, default 4, maximum 16 |
| `--index F` | index file name, default `index.html` |
| `--no-listing` | answer 403 instead of a listing |
| `--log F` | access log in Common Log Format, capped at 262 144 bytes |
| `--pidfile F` | write the pid on start |
| `--version`, `--help` | print and exit |

## What it does

Everything `whitehttpd` (final-4092 as MIPS BE) does, such as `epoll`, dual-stack
IPv6 over one `AF_INET6` socket with `IPV6_V6ONLY=0`, `Range` with `206` and
`Accept-Ranges: bytes`, and the 200, 301, 403, 404 responses. On top of that:

- directory listings with HTML escaping, capped at 64 KiB
- `Date`, `ETag`, `Last-Modified`, `If-None-Match` answered with `304`
- `If-Range` with a date or an entity tag
- multiple ranges are answered with the full body, not `multipart/byteranges`,
  which RFC 7233 §3.1 allows and browsers never ask for
- `OPTIONS` answered `200` with `Allow: GET, HEAD, OPTIONS`
- `405 Method Not Allowed` for other methods
- `--addr` to pin a bind address, IPv4 or IPv6
- access log with rotation at a fixed cap
- worker processes, up to 16; 4 run by default
- `X-Content-Type-Options: nosniff`

Status codes: 200, 206, 301, 304, 400, 403, 404, 405, 414, 416, 431, 500, 501.

Limits, all compile-time constants:

| Constant | Value | Meaning |
| --- | ---: | --- |
| `MAXFD` | 128 | simultaneous descriptors |
| `MAX_WORKERS` | 16 | worker processes |
| `REQ_MAX` | 2 048 | request line |
| `URI_MAX` | 512 | decoded path |
| `HDR_MAX` | 2 048 | header block |
| `LOG_MAX` | 262 144 | access log size |
| `REQUEST_SEC` | 15 | whole-request deadline |
| `IDLE_SEC` | 30 | idle connection |
| `KEEPALIVE_SEC` | 5 | keep-alive lifetime |

## What it does not do

TLS, CGI, compression, HTTP/2, configuration files. Multiple `Range` values are
ignored on purpose.

Supported architectures are x86-64 and MIPS32 O32 only. The reason is in
`third_party/nolibc/arch.h`: it has a branch for x86, a branch for MIPS, and an
`#error` for everything else. ARM fails with `arch-arm.h: No such file`.
`-mabi=n32` fails on `__NR_stat64`, because `ws_stat64` is declared only under
`#if defined(__mips__) && defined(_ABIO32)`.

## Measured

| Platform | CPU | Kernel | Workers | Load | Peak RSS per worker |
| --- | --- | --- | ---: | --- | --- |
| x86-64 | Intel i5-3230M, 4 cores | 6.1.0 | 4 | 800/800, 754 rps | 20 kB |
| ASUS RT-AC57U V3 | QCA5502, 1 core | 3.3.8 | 4 | 120/120 in 0.54 s | 28 kB parent, 20 kB worker |
| MediaTek MT7628 | 24KEc, 1 core | 3.4.113 | 4 | 120/120 in 0.57 s | 72-96 kB |

Four workers is the default on a one-core router as well: the count is a
configured number, not a core count.

Second router, MediaTek MT7628, kernel 3.4.113, little-endian, checked end to
end: `--version` clean, GET and HEAD `200`, `404`, `If-None-Match` answered
`304`, `Range: bytes=0-2` answered `206` with 3 bytes, `OPTIONS` with `Allow`,
`POST` `405`, a 512 kB file transferred with a matching md5, 50 parallel
connections, `VmHWM` 56-120 kB.

## Tests

```sh
python3 tests/test_tier1.py     # 34 checks, process level
python3 tests/test_tier2.py     # 53 checks, protocol level
```

Both pass with 0 failures. The tests talk to a real server process over a
socket; none of them re-implement the state machine and test the copy.

## Size work, measured

Each of these was tried and rejected on evidence:

| Change | Result |
| --- | --- |
| `clang` 14 | +2 012 bytes |
| `lld` | +152 bytes |
| LTO | -20 on x86, `undefined reference to memcpy` on MIPS |
| `-fwhole-program` | -64 on x86, segfault on MIPS, drops `weak memcpy` |
| `gold` | not usable, no `-z noseparate-code` |
| `tcc` | cannot compile the nolibc headers |
| `-Os` instead of `-Oz` | +320 on x86, 0 on MIPS |

Accepted instead: `-Oz`, `strstr(r, "\r\n\r\n")` for the header terminator
(-64 bytes, +500 rps), and the constants in the table above.

## License

AGPL-3.0-or-later. Commercial OEM licensing for vendors who cannot accept the
copyleft: Ivan Svarkovsky <ivansvarkovsky@gmail.com>.

Author: Ivan Svarkovsky <ivansvarkovsky@gmail.com>

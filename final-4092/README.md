# whitehttpd (final-4092 as MIPS BE)

A static file HTTP server written directly in GNU assembler for MIPS32r2,
big-endian only. **4 092 bytes**, which is exactly one memory page: one TLB entry
and no more than one page touched per request.

This is the size reference of the project. For feature parity on three
architectures use the C implementation in `../whitehttpd_c`.

| | |
| --- | --- |
| Size | 4 092 bytes, limit 4 096 |
| Source | `whitehttpd.s`, 1 347 lines |
| Target | MIPS32r2 O32, big-endian |
| Kernel | 3.3.8 and newer |
| Toolchain | `mipsel-linux-gnu-as`, GNU ld 2.40 |
| License | AGPL-3.0-or-later, commercial OEM available |

## Build

```sh
make                # produces whitehttpd-mips32be
make check-size     # fails if the binary grows past 4096 bytes
```

Manual equivalent, when you need to see each step:

```sh
mipsel-linux-gnu-as  -EB -mips32r2 -O2 -32 -G 65536 whitehttpd.s -o whitehttpd.o
mipsel-linux-gnu-objcopy -R .MIPS.abiflags -R .reginfo -R .pdr \
                         -R .comment -R .gnu.attributes whitehttpd.o
mipsel-linux-gnu-ld  -EB -s -N --no-warn-rwx-segments -T whitehttpd.ld \
                     whitehttpd.o -o whitehttpd-mips32be
python3 ../tools/sstrip.py whitehttpd-mips32be
```

Each flag is load bearing:

- `-G 65536` puts every string and buffer in `$gp` range, so a symbol costs one
  4-byte `addiu` instead of `lui`+`ori`.
- `ld -N` drops per-section page alignment, which alone was worth 3.1 kB.
- `sstrip.py` zeroes `e_shoff`, `e_shnum`, `e_shentsize`, `e_shstrndx` and cuts
  the tail. The kernel never reads the section table.
- `whitehttpd.ld` places `_gp` at the start of `.sdata`: every later symbol then
  resolves to a positive 16-bit displacement.

## Run

```sh
./whitehttpd-mips32be [--port N] [--daemon] [directory]
```

Default port is 8080, default directory is the current one. Two flags only:
`--port` and `--daemon`. There is no configuration file.

The binary has no built-in help: every byte of the 4096 available goes to
serving. The manual page is the reference instead.

```sh
man ./whitehttpd.1        # or: groff -man -Tutf8 whitehttpd.1 | less
```

## What it serves

| | |
| --- | --- |
| GET, HEAD | yes |
| HTTP/1.0, HTTP/1.1 | both, Keep-Alive and pipelining |
| `index.html` | served for a directory request |
| Directory redirect | `301` with a trailing slash |
| `Range: bytes=N-M` | `206` with `Content-Range` |
| MIME types | 14 for 17 extensions, `charset=utf-8` for text, `js`, `mjs` and `json` |
| Method other than GET/HEAD | `501` |
| Status codes | 200, 206, 301, 400, 403, 404, 501 |

Security, all enforced by the kernel or by request validation:

- path traversal blocked, `403` on any `..` sequence
- symlinks refused at every component, not only the last one
- `Host` header required on HTTP/1.1, validated against CRLF injection
- obs-fold, `Transfer-Encoding` and bad framing rejected with `400`
- `SIGPIPE` ignored at start, so a client hanging up cannot kill the server
- `HEAD` never sends a body, including on `404` and `403`

## What it does not do

TLS, directory listings (a directory without `index.html` answers `403`),
`--addr`, authentication, chroot or privilege drop, access logs, `ETag`,
`Last-Modified`, `Date`, `OPTIONS`, CGI, compression, HTTP/2, configuration
files.

This is a static-content server with a deliberately narrow contract, not a
general purpose replacement for nginx.

## Architecture

One process, one `epoll` set, one connection state machine. No heap: every
buffer lives in `.sbss` and is sized at assembly time. `io_uring`, threads and
`pthread` are all absent.

The listener is `AF_INET6` with `IPV6_V6ONLY=0`, so the same socket accepts both
IPv4 and IPv6. There is no switch for it: the constants are compiled in, and a
request over `::1` and a request over `127.0.0.1` go through the same path.

Per-request path, all in registers where possible:

1. `epoll_wait` returns a ready descriptor; `EPOLL_CTL_DEL` is skipped because
   `close()` removes it from the set anyway.
2. The request header is read to completion under a cap and a 3 s deadline.
3. The method is compared as one 32-bit word (`0x47455420` is `"GET "`), because
   on big-endian MIPS the constant and the memory bytes agree.
4. The path is percent-decoded in place and opened, or refused.
5. `fstat` on the descriptor gives the size; no second open by name, so there is
   no TOCTOU window.
6. `sendfile` delivers the body; `write` delivers the header.

Deadlines are swept from a 32-bit array on every loop pass, because `epoll` does
not wake up for a connection that has gone silent. `SO_RCVTIMEO` alone is not
enough.

## Tests

```sh
make test          # 96 checks against a real process over a socket
export PATH="../tools/root/usr/bin:$PATH"
make test          # same, under qemu-mips-static
```

96 checks, 0 failures. Verified on an ASUS RT-AC57U V3, kernel 3.3.8, one core:
body integrity over 4 MB, symlink and traversal refusal, 60 parallel
connections, 28 kB peak RSS for the parent and 20 kB per worker.

## Size, honestly

Getting under one page cost features. The reference does not do what the C
implementation does, and the gap is not accidental:

| Method | Result |
| --- | --- |
| `clang` 14 | +2 012 bytes |
| `lld` | +152 bytes |
| LTO | -20 on x86, broken on MIPS |
| `-Os` instead of `-Oz` | +320 bytes on x86 |
| UPX | 4 kB, but peak RSS goes from 4 kB to 60 kB |

## License

AGPL-3.0-or-later. Commercial OEM licensing for router, IoT and camera vendors
who cannot accept the copyleft: Ivan Svarkovsky <ivansvarkovsky@gmail.com>.

Author: Ivan Svarkovsky <ivansvarkovsky@gmail.com>

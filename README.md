# whitehttpd

Two independent HTTP servers in one tree. Both are written from scratch for the
Linux kernel ABI, use no libc, allocate nothing on the heap, and answer requests
from static buffers only. The C server architecture relies on direct Linux
system calls without libc (`nolibc`), making the codebase portable across any
embedded Linux architecture (including x86-64, MIPS BE/LE, ARM/AArch64, and RISC-V).

| Directory | Product | Arch | Size |
| --- | --- | --- | ---: |
| `final-4092/` | whitehttpd (final-4092 as MIPS BE) | MIPS32r2 **BE** only | **4 092** |
| `whitehttpd_c/` | full-feature server | x86-64 | **17 008** |
| `whitehttpd_c/` | same source | MIPS32 **BE** | **20 892** |
| `whitehttpd_c/` | same source | MIPS32 **LE** | **20 908** |

Author: Ivan Svarkovsky &lt;<ivansvarkovsky@gmail.com>&gt;
License: AGPL-3.0-or-later, commercial license available. See [License](#5-license-and-legal-model).

---

## 1. `final-4092/` - whitehttpd (final-4092 as MIPS BE), 4 092 bytes

GNU as source for MIPS32r2 big-endian, frozen. One file, `whitehttpd.s`, 1347
lines. The whole binary fits one 4096-byte memory page and one TLB entry.

**Supported**

- `GET` and `HEAD`, HTTP/1.0 and HTTP/1.1
- Keep-Alive and pipelining
- `index.html` lookup
- 301 redirect for a directory without a trailing slash
- `Range: bytes=N-M`, answered with `206 Partial Content` and `Content-Range`
- 14 MIME types, `charset=utf-8` appended for `text/*`
- symlink and path-traversal protection: `O_NOFOLLOW` on every path component
- IPv6 dual-stack: one `AF_INET6` socket with `IPV6_V6ONLY=0` serves both families
- `epoll` event loop, request timeout 3 s
- codes 200, 206, 301, 400, 403, 404, 501

**Not supported by design**

- TLS
- directory listing (403 by contract)
- `--addr`
- authentication
- `ETag`, `Last-Modified`, `Date`
- `OPTIONS` (answers 501)
- CGI, compression, HTTP/2, config file

Target: ASUS RT-AC57U V3, QCA5502, Linux 3.3.8, OpenWrt and Padavan class
firmware. Verified on real hardware, not only under emulation.

## 2. `whitehttpd_c/` - full-feature server, three targets

Freestanding C on nolibc, one source tree, `src/httpd.c` plus `src/wsock.h`.

| Target | Size |
| --- | ---: |
| `whitehttpd-nolibc-x86_64` | 17 008 |
| `whitehttpd-nolibc-mips32be` | 20 892 |
| `whitehttpd-nolibc-mips32el` | 20 908 |

**Everything the assembler version does** - `epoll`, dual-stack IPv6 over one
`AF_INET6` socket with `IPV6_V6ONLY=0`, `Range` with `206`, codes 200/301/403/404 -
**and on top of that**

- directory listing with HTML escaping, capped at 64 KiB
- `--addr` to bind a specific address, IPv4 or IPv6
- `ETag` and `If-None-Match` answered with `304 Not Modified`
- `Last-Modified` and `Date` headers
- `OPTIONS` answered `200` with `Allow: GET, HEAD, OPTIONS`
- `405 Method Not Allowed` for other methods
- worker processes, `MAX_WORKERS 16`; 4 processes run by default, measured on
  x86-64, on an ASUS RT-AC57U V3 (MIPS BE) and on a MediaTek MT7628 (MIPS LE)
- `X-Content-Type-Options: nosniff`

**Architecture**

`sys_epoll` multiplexer, prefork workers, static `.bss` buffers, direct kernel
syscalls with no libc, own non-blocking state machines. No `malloc`, no
`snprintf`.

**Not supported**

TLS, CGI, compression, HTTP/2, config file, authentication.

Out-of-the-box targets are configured and verified for x86-64 and MIPS32 O32 (BE and LE).
Porting to additional Linux architectures (ARM, RISC-V) requires only adding the
corresponding `arch-*.h` header from upstream Linux nolibc.

---

## 3. Build

```sh
make build          # assembler reference + x86-64 C build
make build-c-mips   # adds both MIPS C targets
./test_all.sh --mips
```

Full instructions, toolchain versions and every rejected optimisation are in
[BUILDING.md](BUILDING.md).

### Measured on hardware

| Platform | CPU | Kernel | Workers | Load test | Peak RSS per worker |
| --- | --- | --- | ---: | --- | --- |
| x86-64 | Intel i5-3230M, 4 cores | 6.1.0 | 4 | 800/800, 754 rps | 20 kB |
| ASUS RT-AC57U V3 | QCA5502, 1 core | 3.3.8 | 4 | 120/120 in 0.54 s, 300 in 0.63 s | 28 kB parent, 20 kB worker |
| MediaTek MT7628 | 24KEc, 1 core | 3.4.113 | 4 | 120/120 in 0.57 s | 72-96 kB |

## 4. Tests

| Suite | Count |
| --- | ---: |
| `tests/regression.py --mips` | 178 checks |
| `whitehttpd_c/tests/test_tier1.py` | 34 checks |
| `whitehttpd_c/tests/test_tier2.py` | 53 checks |
| `final-4092/tests/test_asm_qemu.py` | 96 checks under qemu |

All pass under `qemu-user` for both MIPS byte orders.

## 5. License and Legal Model

The `whitehttpd` project operates under a **Dual-Licensing** model, providing an open-source pathway for the community and commercial licensing for hardware manufacturers:

### 5.1 Open Source License (GNU AGPLv3)
The reference source code is published under the GNU Affero General Public License v3 ([SPDX: `AGPL-3.0-or-later`](https://spdx.org/licenses/AGPL-3.0-or-later.html)).
- Strong network copyleft: modifying, integrating, or running the server within derivative network infrastructure requires distributing the complete corresponding source code under the same AGPLv3 terms.

### 5.2 Commercial OEM License
Designed for original equipment manufacturers (OEMs), router vendors, IoT devices, IP cameras, and proprietary embedded Linux firmware distributions (OpenWrt, Padavan, ASUSWRT):
- Fully waives all AGPLv3 copyleft and source-disclosure obligations.
- Grants perpetual rights to embed `whitehttpd` binaries and source code into proprietary, closed-source commercial firmware and products.
- For commercial licensing inquiries and OEM integration: **Ivan Svarkovsky** &lt;[ivansvarkovsky@gmail.com](mailto:ivansvarkovsky@gmail.com)&gt;.

### 5.3 Contributor License Agreement (CLA)
All exclusive proprietary and commercial rights to the codebase are retained by the author (Ivan Svarkovsky). External contributions and patches are accepted strictly under a Contributor License Agreement to preserve the legal integrity and commercial relicensing authority of the project.

### 5.4 Clean-Room Implementation Declaration (Independence from darkhttpd)
The `whitehttpd` server (both the MIPS32 assembly implementation `whitehttpd.s` / `whitehttpd.s` and the freestanding C implementation `httpd.c` + `wsock.h`) is an independent, clean-room implementation. It is **not** a fork, derivative work, or port of `darkhttpd` (Emil Mikulic, ISC License) and carries zero licensing obligations toward it based on the following:
- **0% Shared Code**: Not a single line of code, data structure, algorithm, or function is copied or derived from `darkhttpd.c`. The entire codebase was designed and written from scratch.
- **Architectural Divergence**:
  - `darkhttpd` relies on libc, single-threaded `poll()`, dynamic heap allocation (`malloc`, `realloc`, `free`), and string formatting via `snprintf`.
  - `whitehttpd` uses zero heap allocations, zero libc dependencies, direct Linux kernel system calls (`syscall`), a non-blocking `epoll` multiplexer, multi-worker preforking, and dedicated static ring/state buffers in `.bss`.
- Third-party components and license notices are tracked in [NOTICE](NOTICE) and [LICENSES/](LICENSES/).
- Security policy: [SECURITY.md](SECURITY.md).

## 6. Scope

Not a drop-in replacement for nginx or Apache. Both servers answer a deliberately
narrow contract: serve static files from one directory, safely, with a small
memory footprint. Suitable for public static content on a modern Linux kernel
from a dedicated directory, running as an unprivileged user.

# Case Study: Porting whitehttpd to ARMv7 Cortex-A8 on Vintage Linux 2.6.32

This document details the engineering challenges and solutions encountered while porting and verifying `whitehttpd_c` on real embedded hardware: a **Seagate GoFlex Satellite** NAS device powered by a **Texas Instruments AM3703 (ARMv7-A Cortex-A8)** SoC running a vintage **Linux 2.6.32** vendor kernel.

---

## 1. Target Hardware & Environment

- **Device**: Seagate GoFlex Satellite (mobile wireless storage)
- **SoC**: Texas Instruments AM3703 / OMAP3730
- **CPU**: ARMv7-A Cortex-A8 (1 core, 800 MHz, dual-issue in-order superscalar, 32 KB L1i, 32 KB L1d, 256 KB L2 cache)
- **Memory**: 128 MB DDR SDRAM
- **Storage**: 500 GB internal mechanical HDD (ext3 on `/opt`)
- **Kernel**: `Linux GoFlexSatellite-NA314GK7 2.6.32 #337 Tue Dec 20 16:06:39 IST 2011 armv7l`
- **Userland ABI Profile**: `armv7sf-k2.6` (ARMv7 EABI with Soft-Float calling convention)

---

## 2. Core Architectural Challenges & Solutions

While modern Linux (5.x/6.x) provides unified system calls like `statx`, `accept4`, and dual-stack IPv6 out-of-the-box, legacy embedded vendor BSPs frequently deviate due to custom kernel builds and code omissions.

Instead of maintaining a separate fork or ad-hoc workarounds, `whitehttpd` addresses these challenges universally within the main codebase, preserving the core principle: **one source tree, zero libc dependencies, running anywhere**.

### 2.1 Missing IPv6 Support in Vendor Kernel (`CONFIG_IPV6=n`)

#### The Problem
Many early consumer NAS and router BSPs stripped out IPv6 to conserve flash memory. On the Seagate 2.6.32 kernel:
```c
listen_fd = ws_socket(WS_AF_INET6, WS_SOCK_STREAM | WS_SOCK_NONBLOCK, 0);
```
failed immediately with `EAFNOSUPPORT` (`errno 97: Address family not supported by protocol`).

#### The Solution: Transparent Dual-Stack Fallback
`whitehttpd` does not assume `AF_INET6` is available. If socket creation fails:
1. The server falls back to opening an `AF_INET` (IPv4) socket.
2. It extracts the IPv4 address from the requested bind target (supporting both pure IPv4 and `::ffff:x.x.x.x` mapped formats).
3. In the accept loop, incoming IPv4 connections are normalized into IPv4-mapped IPv6 addresses (`::ffff:a.b.c.d`).

This allows the rest of the logging, access control, and internal connection structures to remain unified across all kernel configurations.

### 2.2 Unwired `accept4` Syscall in Linux 2.6.32 (`ENOSYS`)

#### The Problem
While `accept4` was upstreamed in Linux 2.6.28, early ARM kernel ports and vendor trees often left `sys_accept4` unwired in `arch/arm/kernel/calls.S`.
Calling syscall 366 (`__NR_accept4`) returned `-ENOSYS` (-38). Because the incoming socket was never drained from the `epoll` read set, this caused an immediate 100% CPU busy loop in `epoll_wait`.

#### The Solution: Atomic Dynamic Syscall Fallback
In `wsock.h`, `ws_accept4` handles missing kernel implementations cleanly:
```c
static inline int ws_accept4(int fd, void *addr, int *len, int flags)
{
    static int no_accept4 = 0;
    if (!no_accept4) {
        long ret = ws_syscall4(__NR_accept4, fd, addr, len, flags);
        if (ret != -38) /* -ENOSYS */
            return __sysret(ret);
        no_accept4 = 1;
    }
    long c = ws_syscall3(__NR_accept, fd, addr, len);
    if (c < 0) return __sysret(c);
    if (flags & WS_SOCK_NONBLOCK) {
        long fl = ws_syscall2(__NR_fcntl, c, 3 /* F_GETFL */);
        if (fl >= 0)
            ws_syscall3(__NR_fcntl, c, 4 /* F_SETFL */, fl | 0x800 /* O_NONBLOCK */);
    }
    return (int)c;
}
```
Modern kernels execute in a single system call without penalty; vintage kernels transparently fall back to standard `accept` (`__NR_accept = 285`) + `fcntl` non-blocking setup.

### 2.3 Legacy `stat64` and `gettimeofday` vs Modern `nolibc`

#### The Problem
Upstream `nolibc` relies on:
- `sys_statx` (added in Linux 4.11)
- `clock_gettime64` (added in Linux 5.1 for 32-bit systems)

Both return `ENOSYS` on Linux 2.6.32, causing file lookup and HTTP date formatting to fail with `500 Internal Server Error`.

#### The Solution
- Mapped `ws_stat` and `ws_fstat` to ARM EABI `sys_fstat64` (`__NR_fstat64 = 197`) and `sys_stat64` (`__NR_stat64 = 195`), using the kernel's 32-bit ARM `struct stat64` layout.
- Bound time polling to `sys_gettimeofday` (`__NR_gettimeofday = 78`).

### 2.4 Soft-Float Toolchain Clashes (`armv7sf` vs `armhf`)

#### The Problem
Modern cross-compilers (`arm-linux-gnueabihf-gcc`) default to the Hard-Float ABI, inserting VFP floating-point register tags into the ELF header (`e_flags`). The `armv7sf` kernel loader strictly expects Soft-Float ABI binaries (`0x5000200, Version5 EABI, soft-float ABI`). Linking standard `libgcc` for division symbols triggered:
```text
ld: error: libgcc.a(_udivmoddi4.o) uses VFP register arguments, whereas output does not
```

#### The Solution
By using `-march=armv7-a -marm -mfloat-abi=soft`, keeping all data paths integer-only, and leveraging freestanding garbage collection (`-Wl,--gc-sections -ffunction-sections -fdata-sections`), the binary links cleanly with zero VFP runtime baggage.

---

## 3. Real-Hardware Verification Results

The binary `whitehttpd-nolibc-armv7` was built on the x86-64 host in 0.1 seconds, transferred over SSH to the Seagate GoFlex Satellite, and tested against live network traffic:

| Metric | Result |
| --- | --- |
| **Binary Size** | **20 604 bytes** (stripped via `sstrip`) |
| **Peak RAM (VmRSS/VmHWM)** | **20–28 kB idle**, **52–208 kB under load** (measured via `/proc/$PID/status` across 4 workers) |
| **Worker Architecture** | **4 preforked workers** by default (configurable via `--workers N`) |
| **Throughput (wrk, 16 B)** | **1 125 req/s** (Keep-Alive), **423 req/s** (Connection: close) measured over network |
| **HTTP Methods** | `GET` (200 OK via `sendfile64`), `HEAD` (no body) |
| **Directory Listing** | Full HTML listing generation with escaping |
| **Byte-Range (RFC 7233)** | `Range: bytes=0-4` -> `206 Partial Content` |
| **Conditional Requests** | `ETag` + `If-None-Match` -> `304 Not Modified` |
| **Security Headers** | `Cache-Control`, `X-Content-Type-Options: nosniff` |

### Sample Live Response from Device:
```http
HTTP/1.1 200 OK
Server: whitehttpd/1.0.0
Connection: keep-alive
Cache-Control: max-age=60
Date: Tue, 03 Nov 2026 08:36:34 GMT
Accept-Ranges: bytes
ETag: "6ae99c66-2b"
Last-Modified: Tue, 03 Nov 2026 08:33:42 GMT
Content-Type: text/plain; charset=utf-8
Content-Length: 43
X-Content-Type-Options: nosniff

Hello from ARMv7 Cortex-A8 (Linux 2.6.32)!
```

---

## 4. How to Build

Cross-compile the ARMv7 target from the project root:
```sh
make build-c-arm
# Output: whitehttpd_c/build/whitehttpd-nolibc-armv7 (20 604 bytes)
```

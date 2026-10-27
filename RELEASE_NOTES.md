# whitehttpd 1.0.0

Autonomous static file web servers written in GNU Assembler and freestanding C without libc, malloc, or external runtime dependencies.

Архитектура Си-сервера построена на прямых системных вызовах без libc (`nolibc`), что делает код переносимым на любые встраиваемые архитектуры Linux (включая x86-64, MIPS BE/LE, ARM/AArch64 и RISC-V).

## Binaries & Exact Sizes
- `whitehttpd-mips32be`: **4,092 bytes** (MIPS32r2 Big-Endian GNU Assembler, Sub-4KB page fit)
- `whitehttpd-nolibc-x86_64`: **17,008 bytes** (x86-64 Freestanding C, non-blocking epoll, prefork)
- `whitehttpd-nolibc-mips32be`: **20,892 bytes** (MIPS32r2 Big-Endian Freestanding C)
- `whitehttpd-nolibc-mips32el`: **20,908 bytes** (MIPS32r2 Little-Endian Freestanding C)

## Security & Architecture
- 0% shared code with darkhttpd (Clean-Room Implementation).
- Non-blocking epoll I/O multiplexing.
- Strict path isolation: component-by-component traversal with `O_NOFOLLOW` / `openat2`.
- Zero dynamic heap allocation (`malloc`/`free`), zero libc overhead.
- Dual-stack IPv6/IPv4 support (in freestanding C profile).

## License
Dual-licensing: GNU AGPLv3 for open source, commercial OEM license available for hardware and firmware vendors.
Author: Ivan Svarkovsky <ivansvarkovsky@gmail.com>

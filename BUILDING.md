# Building whitehttpd

Two independent products in one tree:

| Directory | Product | Toolchain |
| --- | --- | --- |
| `final-4092/` | `whitehttpd` (final-4092 as MIPS BE) | GNU as, MIPS32r2 big-endian |
| `whitehttpd_c/` | freestanding C implementation | gcc, no libc |

Python 3 is required for the tools in `tools/`. The MIPS targets additionally
need a cross compiler and, for the test suite, QEMU user-mode.

## Toolchain versions

The versions below are the ones this tree was built and measured with. They are
a record, not a requirement: newer versions build the same sources, but binary
sizes may shift by tens of bytes and the `4092` / `17008` / `20892` / `20908`
figures in the documentation were produced with exactly these.

| Tool | Version used |
| --- | --- |
| gcc | 12.2.0 |
| mipsel-linux-gnu-gcc | 12.2.0 |
| GNU as, GNU ld | 2.40 |
| Python | 3.11 |
| qemu-user-static | 7.2.22 |

The CI workflow pins `gcc-12` explicitly (see `.github/workflows/ci.yml`) so the
C binaries reproduce the documented sizes; the assembler reference is version
insensitive at the sizes recorded here. `make check-size` in CI is what guards
the 4096 byte ceiling regardless.

Known consequence of a version change: `--no-warn-rwx-segments` exists only in
binutils 2.39 and newer. On older binutils the MIPS link step fails, so the CI
workflow uses a runner image that carries 2.42 or newer.

## Quick start

```sh
make build-asm      # assembler reference, MIPS BE, 4092 bytes
make build-c        # freestanding C, x86-64
make build-c-mips   # freestanding C, MIPS32 BE and LE
make                # everything together
```

Note that `make build` does **not** build the MIPS C targets; `build-c-mips`
does.

## Assembler reference

```sh
cd final-4092
make                # whitehttpd-mips32be
make check-size     # fails if the binary grows past 4096 bytes
```

Manual equivalent, to see each step:

```sh
mipsel-linux-gnu-as -EB -mips32r2 -O2 -32 -G 65536 whitehttpd.s -o whitehttpd.o
mipsel-linux-gnu-objcopy -R .MIPS.abiflags -R .reginfo -R .pdr \
                         -R .comment -R .gnu.attributes whitehttpd.o
mipsel-linux-gnu-ld -EB -s -N --no-warn-rwx-segments -T whitehttpd.ld \
                    whitehttpd.o -o whitehttpd-mips32be
python3 ../tools/sstrip.py whitehttpd-mips32be
```

| Flag | Why it matters |
| --- | --- |
| `-G 65536` | every string and buffer lands in `$gp` range, so a symbol costs one 4-byte `addiu` instead of `lui`+`ori` |
| `-EB` | big-endian, the only target of this source |
| `-mips32r2` | instruction set the router CPUs provide |
| `ld -N` | drops per-section page alignment; worth 3.1 kB on its own |
| `-T whitehttpd.ld` | places `_gp` at the start of `.sdata`, so later symbols resolve to a positive 16-bit displacement |
| `objcopy -R` | strips MIPS-specific sections the kernel never reads |
| `sstrip.py` | zeroes `e_shoff`, `e_shnum`, `e_shentsize`, `e_shstrndx` and cuts the tail |

Rejected after measurement, not by preference:

| Method | Result |
| --- | --- |
| `clang` 14 | +2 012 bytes |
| `lld` | +152 bytes |
| LTO | broken on MIPS |
| `-Os` instead of `-Oz` | +320 bytes on x86 |

## Freestanding C

```sh
cd whitehttpd_c
make                # build/whitehttpd-nolibc-x86_64
make mipsbe         # build/whitehttpd-nolibc-mips32be
make mipsel         # build/whitehttpd-nolibc-mips32el
```

Both byte orders come from one `mipsel-linux-gnu-gcc`; the only difference is
`-EB` versus `-EL`. `gcc-mips-linux-gnu` is not needed.

Flags that matter:

```sh
-Oz -fno-pic -fno-pie -ffreestanding -nostdlib -static
-march=mips32r2 -mabi=32 -mno-abicalls -G 0     # MIPS only
```

`-fno-pic -fno-pie` keeps `.data.rel.ro` out of the writable segment, worth
8.2 kB. `-Oz` is the smallest setting; every alternative measured larger. The
table is in `whitehttpd_c/README.md`.

The cross kernel headers are needed for `asm/types.h`:

```sh
sudo apt-get install gcc-mipsel-linux-gnu binutils-mipsel-linux-gnu \
                     linux-libc-dev-mipsel-cross
```

## Running the tests

```sh
./test_all.sh           # builds and tests x86-64 and the assembler reference
./test_all.sh --mips    # adds MIPS builds and the qemu regression
```

Or target by target:

```sh
make test-qemu                                    # 96 assembler checks under qemu
python3 tests/regression.py --mips                # 178 checks
python3 whitehttpd_c/tests/test_tier1.py          # 34 checks
python3 whitehttpd_c/tests/test_tier2.py          # 53 checks
python3 -m unittest discover -s tests -p 'test_*.py'   # 16 tool tests
```

The `--mips` paths need `qemu-mips-static` and `qemu-mipsel-static` on `PATH`.
A local copy lives in `tools/root/usr/bin` in the development tree and is not
shipped:

```sh
export PATH="$PWD/tools/root/usr/bin:$PATH"
```

## Hardware tests

`final-4092/tests/hw_check.py` and `hw_edges.py` run against a real router over
the network. **Host, port and root directory are yours to supply**: the
addresses and ports in the examples are placeholders from one test bench and
mean nothing on another machine or in another network.

```sh
export WH_HOST=192.0.2.10     # your device, not this one
export WH_PORT=8081           # your free port, not this one
python3 final-4092/tests/hw_check.py
```

Prepare the test root on the device first, see `final-4092/tests/setup_root.sh`.
Run on a spare port and do not disturb a production instance that may already be
listening on port 80.

## Packaging

```sh
python3 tools/package_release.py    # writes dist/
```

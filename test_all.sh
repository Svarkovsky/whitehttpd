#!/usr/bin/env bash
# Builds both products and runs every check.
#
#   ./test_all.sh           build and test, MIPS skipped
#   ./test_all.sh --mips    adds the MIPS builds and the qemu regression
#
# --mips needs mipsel-linux-gnu-gcc and qemu-user-static on PATH.
set -euo pipefail
cd "$(dirname "$0")"

WITH_MIPS=0
[[ "${1:-}" == "--mips" ]] && WITH_MIPS=1

# Local toolchain from tools/root when present; without it neither MIPS nor
# qemu is available, so the script simply carries on without them.
if [[ -d tools/root/usr/bin ]]; then
    export PATH="$PWD/tools/root/usr/bin:$PATH"
fi

echo "== build whitehttpd (final-4092 as MIPS BE)"
make build-asm

echo "== build freestanding C, x86-64"
make build-c

if (( WITH_MIPS )); then
    echo "== build freestanding C, MIPS32 BE and LE"
    make build-c-mips
fi

echo "== ELF check: no RWX segment, NX stack"
python3 tools/check_elf.py \
    final-4092/whitehttpd-mips32be \
    whitehttpd_c/build/whitehttpd-nolibc-x86_64
if (( WITH_MIPS )); then
    python3 tools/check_elf.py \
        whitehttpd_c/build/whitehttpd-nolibc-mips32be \
        whitehttpd_c/build/whitehttpd-nolibc-mips32el
fi

echo "== size ceiling for whitehttpd (final-4092 as MIPS BE), 4096"
make check-size

if (( WITH_MIPS )); then
    echo "== regression, MIPS profile under qemu-user"
    make test
else
    echo "-- regression skipped, rerun with --mips"
fi

if command -v qemu-mips-static >/dev/null 2>&1; then
    echo "== assembler build: 96 checks under qemu, MIPS BE"
    make test-qemu
else
    echo "-- qemu-mips-static not found on PATH, skipping 96 assembler qemu checks"
fi

echo "== unit tests for the ELF and packaging tools"
python3 -m unittest discover -s tests -p 'test_*.py'

echo
echo "ALL CHECKS PASSED"

# nolibc_httpd - minimal epoll HTTP server without libc
#   make            -> native x86_64 (build/whitehttpd-nolibc-x86_64)
#   make mipsbe     -> MIPS32r2 Big-Endian (build/whitehttpd-nolibc-mips32be)
#   make mipsel     -> MIPS32r2 Little-Endian (build/whitehttpd-nolibc-mips32el)
#
# SPDX-License-Identifier: AGPL-3.0-or-later

CC_native ?= $(shell command -v gcc-12 >/dev/null 2>&1 && echo gcc-12 || echo gcc)
CC_mips   ?= mipsel-linux-gnu-gcc

COMMON = -Oz -Wall -Wextra -Werror -fno-pic -fno-pie \
         -ffreestanding -fno-builtin -fno-stack-protector \
         -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident \
         -ffunction-sections -fdata-sections \
         -isystem third_party/nolibc -Isrc

MIPS_BASE = -march=mips32r2 -mabi=32 -mno-abicalls -fno-pic -G 0 \
            -isystem /usr/mipsel-linux-gnu/include \
            -nostdlib -nostartfiles -static -no-pie \
            -Wl,--gc-sections -Wl,-s -Wl,-z,noseparate-code -Wl,-z,noexecstack -Wl,--build-id=none -Wl,-e,__start

.PHONY: all mipsel mipsbe clean

all: build/whitehttpd-nolibc-x86_64

build/whitehttpd-nolibc-x86_64: src/httpd.c src/wsock.h $(wildcard third_party/nolibc/*.h third_party/nolibc/sys/*.h) ../tools/sstrip.py
	mkdir -p build
	$(CC_native) $(COMMON) -nostdlib -nostartfiles -static -no-pie \
	    -Wl,--gc-sections -Wl,-s -Wl,-z,noseparate-code -Wl,-z,noexecstack -Wl,--build-id=none -Wl,-e,_start src/httpd.c -o $@
	../tools/sstrip.py $@
	ln -sf whitehttpd-nolibc-x86_64 build/httpd

mipsbe: build/whitehttpd-nolibc-mips32be

build/whitehttpd-nolibc-mips32be: src/httpd.c src/wsock.h $(wildcard third_party/nolibc/*.h third_party/nolibc/sys/*.h) ../tools/sstrip.py
	mkdir -p build
	$(CC_mips) $(COMMON) $(MIPS_BASE) -EB src/httpd.c -o $@
	../tools/sstrip.py $@
	ln -sf whitehttpd-nolibc-mips32be build/httpd.mipsbe

mipsel: build/whitehttpd-nolibc-mips32el

build/whitehttpd-nolibc-mips32el: src/httpd.c src/wsock.h $(wildcard third_party/nolibc/*.h third_party/nolibc/sys/*.h) ../tools/sstrip.py
	mkdir -p build
	$(CC_mips) $(COMMON) $(MIPS_BASE) -EL src/httpd.c -o $@
	../tools/sstrip.py $@
	ln -sf whitehttpd-nolibc-mips32el build/httpd.mipsel

clean:
	rm -rf build

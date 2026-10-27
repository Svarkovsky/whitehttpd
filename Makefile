# whitehttpd - два продукта с разными задачами.
#
#   final-4092/   ассемблерный эталон на GAS MIPS32r2 BE, 4092 байта.
#                 Доказывает достижимость одной страницы памяти; не развивается.
#   whitehttpd_c/ Freestanding Си на nolibc, x86-64 + MIPS32 BE/LE.
#                 Продукт: листинг, IPv6, prefork, валидаторы, логи.
#
# Сравнивать их по размеру и набору функций некорректно: разные задачи.
# Прошлые редакции, FASM x86-64 и GAS x86-64 в дереве не хранятся:
# история лежит в git, см. backups/whitehttpd-curent-*-precleanup.bundle.

.PHONY: all build check-size test test-qemu clean docs

all: build

# Два продукта с разными задачами: ассемблерный эталон размера и Си-сервер.
build: build-asm build-c

build-asm:
	$(MAKE) -C final-4092

build-c:
	$(MAKE) -C whitehttpd_c

build-c-mips:
	$(MAKE) -C whitehttpd_c mipsbe mipsel

check-size: 
	$(MAKE) -C final-4092 check-size

# Регрессия по MIPS-профилю через qemu-user.
test: build
	PATH="$(CURDIR)/tools/root/usr/bin:$$PATH" python3 tests/regression.py --mips

# Full check suite for the assembler build under QEMU (96 checks).
test-qemu: build
	PATH="$(CURDIR)/tools/root/usr/bin:$$PATH" python3 final-4092/tests/test_asm_qemu.py

clean:
	$(MAKE) -C final-4092 clean
	$(MAKE) -C whitehttpd_c clean

# Doxygen-документация в docs/doxygen (каталог в .gitignore).
docs:
	doxygen Doxyfile

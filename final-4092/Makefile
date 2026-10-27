# whitehttpd (final-4092 as MIPS BE): MIPS32r2, the assembler reference.
# The binary must stay within one 4096 byte page; check-size enforces that.
AS      := mipsel-linux-gnu-as
LD      := mipsel-linux-gnu-ld
OBJCOPY := mipsel-linux-gnu-objcopy
SSTRIP  := ../tools/sstrip.py

BIN := whitehttpd-mips32be
OBJ := whitehttpd.o

all: $(BIN)

$(OBJ): whitehttpd.s
	$(AS) -EB -mips32r2 -O2 -32 -G 65536 $< -o $@
	$(OBJCOPY) -R .MIPS.abiflags -R .reginfo -R .pdr -R .comment \
	    -R .gnu.attributes $@

$(BIN): $(OBJ) whitehttpd.ld
	$(LD) -EB -s -N --no-warn-rwx-segments -T whitehttpd.ld $(OBJ) -o $@
	python3 $(SSTRIP) $@
	@echo "size: $$(wc -c < $@) bytes (limit 4096)"

check-size: $(BIN)
	@sz=$$(wc -c < $(BIN)); \
	if [ $$sz -gt 4096 ]; then echo "FAIL: $$sz > 4096"; exit 1; fi; \
	echo "OK: $$sz bytes, headroom $$((4096 - sz))"

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all clean check-size

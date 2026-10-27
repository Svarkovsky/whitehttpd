#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Remove the ELF section table, retaining every file-backed program segment."""
import pathlib,struct,sys

def sstrip(filename):
    p=pathlib.Path(filename);b=bytearray(p.read_bytes())
    if len(b)<64 or b[:4]!=b'\x7fELF' or b[4]not in(1,2)or b[5]not in(1,2):
        raise ValueError('invalid or unsupported ELF header')
    endian='<'if b[5]==1 else'>';is64=b[4]==2
    phoff=struct.unpack_from(endian+('Q'if is64 else'I'),b,32 if is64 else 28)[0]
    size,count=struct.unpack_from(endian+'HH',b,54 if is64 else 42)
    expected=56 if is64 else 32
    if size!=expected or not count or phoff+count*size>len(b):raise ValueError('invalid program header table')
    keep=max(64 if is64 else 52,phoff+count*size)
    for i in range(count):
        h=struct.unpack_from(endian+('IIQQQQQQ'if is64 else'IIIIIIII'),b,phoff+i*size)
        off,n=(h[2],h[5])if is64 else(h[1],h[4])
        if n:
            if off+n>len(b):raise ValueError('segment exceeds file bounds')
            keep=max(keep,off+n)
    struct.pack_into(endian+('Q'if is64 else'I'),b,40 if is64 else 32,0)
    struct.pack_into(endian+'HHH',b,58 if is64 else 46,0,0,0)
    before=len(b);p.write_bytes(b[:keep]);print(f'sstrip: {p.name}: {keep} bytes (removed {before-keep})')
if __name__=='__main__':
    if len(sys.argv)!=2:raise SystemExit('usage: sstrip.py ELF')
    try:sstrip(sys.argv[1])
    except (OSError,ValueError,struct.error)as e:raise SystemExit(str(e))

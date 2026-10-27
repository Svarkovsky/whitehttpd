#!/usr/bin/env python3
"""Check real ELF program headers without relying on stripped section tables."""
import pathlib,struct,sys,json

def inspect(path):
    b=pathlib.Path(path).read_bytes()
    if b[:4]!=b'\x7fELF' or b[4]not in(1,2)or b[5]not in(1,2):raise ValueError('not ELF')
    e='<'if b[5]==1 else'>';wide=b[4]==2
    entry=struct.unpack_from(e+('Q'if wide else'I'),b,24)[0]
    phoff=struct.unpack_from(e+('Q'if wide else'I'),b,32 if wide else 28)[0]
    size,count=struct.unpack_from(e+'HH',b,54 if wide else 42)
    if size!=(56 if wide else 32):raise ValueError('invalid program header width')
    stack=False;executable=False;loads=[]
    for i in range(count):
        h=struct.unpack_from(e+('IIQQQQQQ'if wide else'IIIIIIII'),b,phoff+i*size)
        kind,flags,offset,va,filesz,memsz=(h[0],h[1],h[2],h[3],h[5],h[6])if wide else(h[0],h[6],h[1],h[2],h[4],h[5])
        if kind==3:raise ValueError('unexpected dynamic interpreter')
        if kind==1:
            if flags&3==3:raise ValueError('writable executable LOAD segment')
            if filesz>memsz or (filesz and offset+filesz>len(b)):raise ValueError('invalid LOAD bounds')
            if flags&1 and va<=entry<va+memsz:executable=True
            loads.append({'flags':flags,'file_bytes':filesz,'memory_bytes':memsz})
        if kind==0x6474e551:
            stack=True
            if flags&1:raise ValueError('executable stack')
    if not stack:raise ValueError('no explicit NX stack header')
    if not executable:raise ValueError('entry outside executable segment')
    return {'file':str(path),'bytes':len(b),'bits':64 if wide else 32,'byte_order':'little'if e=='<'else'big','loads':loads,'NX_stack':True}
if __name__=='__main__':
    try:print(json.dumps([inspect(p)for p in sys.argv[1:]],indent=2))
    except (OSError,ValueError,struct.error)as e:raise SystemExit(str(e))

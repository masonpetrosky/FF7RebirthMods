"""Find rel32 calls/jmps to an RVA and absolute qword pointers to it: callers.py <exe> <rva hex> [<rva hex>...]"""
import re
import struct
import sys

exe = sys.argv[1]
targets = {int(a, 16) for a in sys.argv[2:]}
d = open(exe, "rb").read()
pe = struct.unpack_from("<I", d, 0x3C)[0]
nsec = struct.unpack_from("<H", d, pe + 6)[0]
optsz = struct.unpack_from("<H", d, pe + 20)[0]
base = struct.unpack_from("<Q", d, pe + 24 + 24)[0]
secs = []
for i in range(nsec):
    name, vsz, va, rsz, rptr = struct.unpack_from("<8sIIII", d, pe + 24 + optsz + 40 * i)
    secs.append((name.rstrip(b"\0").decode(), va, rsz, rptr))
for name, va, rsz, rptr in secs:
    if name != ".text":
        continue
    t = d[rptr:rptr + rsz]
    for m in re.finditer(rb"[\xe8\xe9]", t):
        i = m.start()
        if i + 5 > len(t):
            break
        tgt = va + i + 5 + struct.unpack_from("<i", t, i + 1)[0]
        if tgt in targets:
            print("%s at %x -> %x" % ("call" if t[i] == 0xE8 else "jmp", va + i, tgt))
for tgt in targets:
    q = struct.pack("<Q", base + tgt)
    for m in re.finditer(re.escape(q), d):
        for name, va, rsz, rptr in secs:
            if rptr <= m.start() < rptr + rsz:
                print("pointer in %s at %x -> %x" % (name, va + m.start() - rptr, tgt))

"""Disassemble ff7rebirth_.exe around an RVA: disasm.py <exe> <rva hex> [bytes before] [bytes after]
Picks a start in the window whose instruction stream lands exactly on the RVA. Resolves RIP-relative targets to
RVAs and shows the UTF-16/ASCII strings they point at."""
import re
import struct
import sys

import capstone

exe, rva = sys.argv[1], int(sys.argv[2], 16)
before = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x60
after = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x40
data = open(exe, "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
optsz = struct.unpack_from("<H", data, pe + 20)[0]
base = struct.unpack_from("<Q", data, pe + 24 + 24)[0]
secs = []
for i in range(nsec):
    name, vsz, va, rsz, rptr = struct.unpack_from("<8sIIII", data, pe + 24 + optsz + 40 * i)
    secs.append((va, max(vsz, rsz), rptr))


def off(r):
    for va, sz, rptr in secs:
        if va <= r < va + sz:
            return rptr + r - va
    return None


def string_at(r):
    o = off(r)
    if o is None:
        return ""
    raw = data[o:o + 160]
    m = re.match(rb"((?:[\x20-\x7e]\x00){4,})", raw)
    if m:
        return 'u"%s"' % m.group(1).decode("utf-16le")
    m = re.match(rb"([\x20-\x7e]{4,})", raw)
    if m:
        return '"%s"' % m.group(1).decode()
    return ""


md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)


def listing(start):
    o = off(start)
    return list(md.disasm(data[o:o + (rva - start) + after], base + start))


best = None
for delta in range(0, 16):
    start = rva - before + delta
    ins = listing(start)
    if any(i.address - base == rva for i in ins):
        best = ins
        break
if best is None:
    best = listing(rva)
for ins in best:
    r = ins.address - base
    note = ""
    m = re.search(r"\[rip ([+-]) (0x[0-9a-f]+)\]", ins.op_str)
    if m:
        tgt = r + ins.size + (int(m.group(2), 16) * (1 if m.group(1) == "+" else -1))
        note = "  ; -> %x %s" % (tgt, string_at(tgt))
    elif ins.mnemonic.startswith(("call", "j")) and ins.op_str.startswith("0x"):
        note = "  ; -> %x" % (int(ins.op_str, 16) - base)
    print("%s%08x  %-8s %s%s" % ("=>" if r == rva else "  ", r, ins.mnemonic, ins.op_str, note))

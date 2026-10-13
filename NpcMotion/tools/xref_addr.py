"""Find RIP-relative references in .text to given RVAs: xref_addr.py <exe> <rva hex> [<rva hex> ...]
Matches any instruction whose 4-byte displacement (at the end of a 6-11 byte window) resolves to the RVA."""
import struct
import sys

import capstone

exe = sys.argv[1]
targets = {int(a, 16) for a in sys.argv[2:]}
data = open(exe, "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
optsz = struct.unpack_from("<H", data, pe + 20)[0]
for i in range(nsec):
    name, vsz, va, rsz, rptr = struct.unpack_from("<8sIIII", data, pe + 24 + optsz + 40 * i)
    if name.startswith(b".text"):
        break
text = data[rptr:rptr + rsz]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
hits = set()
# a rip-relative disp32 at position p means: instruction end e (p+4 <= e <= p+4+4 imm), target = va+e+disp
for p in range(0, len(text) - 4):
    disp = int.from_bytes(text[p:p + 4], "little", signed=True)
    for imm in (0, 1, 2, 4):
        e = p + 4 + imm
        if va + e + disp in targets:
            hits.add((p, imm, va + e + disp))
for p, imm, t in sorted(hits):
    # verify by disassembling from a few candidate starts
    for back in range(2, 8):
        s = p - back
        ins = next(md.disasm(text[s:s + 16], va + s), None)
        if ins and ins.size == back + 4 + imm and "rip" in ins.op_str:
            print("%08x  %s %s  -> %x" % (va + s, ins.mnemonic, ins.op_str, t))
            break

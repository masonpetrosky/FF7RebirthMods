"""List console variables matching a prefix with the address of the variable each FAutoConsoleVariableRef points at
(lea r8, [rip+X] just before lea rdx, [rip+name]) and its initial bytes in the exe.
Usage: cvar_refs.py <exe> <name prefix>"""
import re
import struct
import sys

exe, prefix = sys.argv[1], sys.argv[2]
d = open(exe, "rb").read()
pe = struct.unpack_from("<I", d, 0x3C)[0]
nsec = struct.unpack_from("<H", d, pe + 6)[0]
optsz = struct.unpack_from("<H", d, pe + 20)[0]
secs = []
for i in range(nsec):
    name, vsz, va, rsz, rptr = struct.unpack_from("<8sIIII", d, pe + 24 + optsz + 40 * i)
    secs.append((name.rstrip(b"\0").decode(), va, vsz, rsz, rptr))
text = next(s for s in secs if s[0] == ".text")


def file_off(rva):
    for n, va, vsz, rsz, rptr in secs:
        if va <= rva < va + rsz:
            return rptr + rva - va
    return None


names = {}
for m in re.finditer(re.escape(prefix.encode("utf-16le")) + rb"(?:[\x20-\x7e]\x00)*", d):
    s = m.group().decode("utf-16le")
    for n, va, vsz, rsz, rptr in secs:
        if rptr <= m.start() < rptr + rsz:
            names[va + m.start() - rptr] = s
t = d[text[4]:text[4] + text[3]]
tva = text[1]
for m in re.finditer(rb"\x48\x8d\x15", t):  # lea rdx, [rip+disp32]
    i = m.start()
    tgt = tva + i + 7 + struct.unpack_from("<i", t, i + 3)[0]
    if tgt not in names:
        continue
    var = None
    for back in range(7, 48):  # lea r8, [rip+disp32] = 4c 8d 05
        j = i - back
        if t[j:j + 3] == b"\x4c\x8d\x05":
            var = tva + j + 7 + struct.unpack_from("<i", t, j + 3)[0]
            break
    init = ""
    if var is not None:
        o = file_off(var)
        init = d[o:o + 4].hex() if o is not None else "bss"
    print("%-60s var %s init %s" % (names[tgt], ("%x" % var) if var else "?", init))

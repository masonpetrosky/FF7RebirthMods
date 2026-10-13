"""Find code that references a string: xref.py <exe> <text> [utf16|ascii]. Prints the string RVAs and every
RIP-relative lea/mov in .text that points at them."""
import re
import struct
import sys

exe, text = sys.argv[1], sys.argv[2]
enc = sys.argv[3] if len(sys.argv) > 3 else "utf16"
data = open(exe, "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
optsz = struct.unpack_from("<H", data, pe + 20)[0]
secs = {}
for i in range(nsec):
    name, vsz, va, rsz, rptr = struct.unpack_from("<8sIIII", data, pe + 24 + optsz + 40 * i)
    secs[name.rstrip(b"\0").decode()] = (va, rsz, rptr)
needle = text.encode("utf-16le") if enc == "utf16" else text.encode()
targets = set()
for m in re.finditer(re.escape(needle), data):
    o = m.start()
    for name, (va, rsz, rptr) in secs.items():
        if rptr <= o < rptr + rsz:
            targets.add(va + o - rptr)
            print("string at rva %x (%s)" % (va + o - rptr, name))
va, rsz, rptr = secs[".text"]
text_bytes = data[rptr:rptr + rsz]
for m in re.finditer(rb"[\x48\x4c][\x8d\x8b][\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", text_bytes):
    i = m.start()
    disp = struct.unpack_from("<i", text_bytes, i + 3)[0]
    tgt = va + i + 7 + disp
    if tgt in targets:
        print("ref at rva %x -> %x" % (va + i, tgt))

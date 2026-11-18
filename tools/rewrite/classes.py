"""A program's C++ classes from its MSVC RTTI (from one port's tools/classes.py): each
class's vtables (through the complete object locators that name its type descriptor) and the
methods in them, as a first map of the program into modules for a rewrite (modules.py).

    classes.py PROGRAM.EXE > classes.txt
Lines: class, vtable address, its methods (code addresses in order). Only programs built with
RTTI have the type descriptors this reads.
"""
import os, re, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'translator'))
from pe import PE

m = PE(sys.argv[1])
img, base = m.image, m.base
lo, hi = m.text_range()
u32 = lambda va: struct.unpack_from('<I', img, va - base)[0]
inimg = lambda va: base <= va < base + len(img) - 4
# type descriptors: vftable pointer, spare, ".?AV...@@"
tds = {}
for mt in re.finditer(rb'\.\?A[VU]([A-Za-z0-9_@$?]+?)@@\x00', img):
    td = base + mt.start() - 8
    tds[td] = mt.group(1).decode().replace('@', '::')
# complete object locators: signature 0, offset, constructor displacement offset, type
# descriptor, class hierarchy
cols = {}
for off in range(0, len(img) - 20, 4):
    sig, o, cdo, td, chd = struct.unpack_from('<IIIII', img, off)
    if sig == 0 and td in tds and inimg(chd):
        cols[base + off] = (tds[td], o)
# a vtable follows a pointer to its locator; it runs while the entries point into code and
# until the next locator pointer
out = []
for off in range(4, len(img) - 4, 4):
    col = struct.unpack_from('<I', img, off - 4)[0]
    if col not in cols:
        continue
    vt = base + off
    meths = []
    a = vt
    while inimg(a) and lo <= u32(a) < hi:
        meths.append(u32(a))
        a += 4
        if u32(a) in cols:
            break
    if meths:
        name, o = cols[col]
        out.append((name, vt, o, meths))
for name, vt, o, meths in sorted(out, key=lambda x: min(x[3])):
    print('%-40s vtable %08x (offset %d): %s' % (name, vt, o, ' '.join('%08x' % x for x in meths)))
print('%d classes with vtables' % len(set(x[0] for x in out)), file=sys.stderr)

#!/usr/bin/env python3
"""The user folders' pipeline lists (vk_pipelines.bin, our renderer's) merged into one text list (a port's
data/vk_pipelines.txt), the list built into the program (tools/vk9/plist2h.py): "keysize N", then one line per pipeline, its key in hex and its vertex
declaration's elements in hex. Keys only: the shaders appear as hashes of their bytecode, nothing of the game's code.
Usage: tools/vk9/vk_plist.py OUT.txt FILE.bin... (pipelines kept in first-seen order, each once)"""
import struct, sys

out, ins = sys.argv[1], sys.argv[2:]
keysize, seen, lines = None, set(), []
for p in ins:
    b = open(p, 'rb').read()
    if b[:8] != b'ETVKPL01':
        continue
    ks, n = struct.unpack_from('<II', b, 8)
    if keysize is None:
        keysize = ks
    if ks != keysize:
        print('%s: key of %d bytes, not %d: left out' % (p, ks, keysize))
        continue
    o = 16
    for _ in range(n):
        k = b[o:o + ks]
        nel, = struct.unpack_from('<I', b, o + ks)
        el = b[o + ks + 4:o + ks + 4 + 8 * nel]
        o += ks + 4 + 8 * nel
        if k not in seen:
            seen.add(k)
            lines.append(k.hex() + ' ' + el.hex())
with open(out, 'w') as f:
    f.write('# Pipelines our Vulkan renderer makes on the test routes (user folders merged by tools/vk9/vk_plist.py): built into\n')
    f.write('# the program (tools/vk9/plist2h.py) and made by worker threads as the game makes its shaders. Keys only.\n')
    f.write('keysize %d\n' % (keysize or 0))
    for l in lines:
        f.write(l + '\n')
print('%s: %d pipelines from %d lists' % (out, len(lines), len(ins)))

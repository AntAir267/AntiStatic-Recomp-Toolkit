"""A map of a program into modules for a rewrite (from one port's tools/modules.py):
every translated function gets the class whose vtable holds it (classes.py's output) or, between
classes, the nearest class method before it (the linker keeps a source file's functions
together), and each run of one class becomes a module. With a coverage file (a coverage build's
<PORT_ENV>_COVERAGE output, runtime/rewrite/coverage.c) the functions a route reaches are counted:
those are the ones an A/B check can verify, so batches are made of them.

    modules.py GEN CLASSES [COVERAGE] [--below ADDR] [--skip ADDR]... > modules.txt
GEN: the generated code's folder (its extents.txt); CLASSES: classes.py's output; --below: only
functions under ADDR (hex), e.g. the exe without its DLLs; --skip: a method many classes share
(the pure-call stub every abstract class points at), which owns nothing.
Lines: first-last address, functions (reached), instructions (reached), class.
"""
import os, re, sys

args, below, skip = [], None, set()
it = iter(sys.argv[1:])
for a in it:
    if a == '--below':
        below = int(next(it), 16)
    elif a == '--skip':
        skip.add(int(next(it), 16))
    else:
        args.append(a)
if len(args) < 2:
    sys.exit(__doc__)
ext = []
for l in open(os.path.join(args[0], 'extents.txt')):
    e, lo, hi, n = l.split()[:4]
    e = int(e, 16)
    if below is None or e < below:
        ext.append((e, int(n)))
ext.sort()
owner = {}
for l in open(args[1]):
    if ':' not in l:
        continue
    name = l.split()[0]
    meths = re.findall(r'\b([0-9a-f]{8})\b', l.split(':', 1)[1])
    for a in meths:
        a = int(a, 16)
        if a not in skip:
            owner.setdefault(a, name)
cov = set()
if len(args) > 2:
    cov = set(int(x, 16) for x in open(args[2]) if x.strip())
mods, cur = [], None
for a, n in ext:
    name = owner.get(a, cur[0] if cur else '(start)')
    if not cur or name != cur[0]:
        cur = [name, a, a, 0, 0, 0, 0]
        mods.append(cur)
    cur[2] = a
    cur[3] += 1
    cur[4] += n
    if a in cov:
        cur[5] += 1
        cur[6] += n
for name, lo, hi, nf, ni, cf, ci in mods:
    print('%08x-%08x %4d (%4d) %6d (%6d) %s' % (lo, hi, nf, cf, ni, ci, name))

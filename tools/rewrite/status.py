"""Where a port's rewrite as hand-written C stands (docs/REWRITE.md; from one port's
tools/decomp_status.py): each file registering rewrites (DECOMP) with its count, then the totals
against the REWRITES list; a commented-out registration counts as off.

    status.py LIST DIR [GEN] [--todo] [--nm BINARY] [--skip FILE] [--range LO HI]
LIST: the build config's REWRITES file, or - for every function of GEN's extents.txt (the game's
own function list: with --range LO HI, hex, only those in one module, e.g. the exe's image);
DIR: the folder of rewrite files (searched recursively
for *.c); GEN: the generated code's folder, for sizes (its extents.txt). --todo: only the
functions not rewritten yet, with their sizes in instructions.
--nm BINARY: count what the built program registers (its decomp_entry_<address> symbols, which
every DECOMP / DECOMP_SF and the port's own macros over them make) instead of searching the sources
for "DECOMP(", which misses registrations made through macros. --skip FILE: addresses (one a
line, # comments) left out of the count, e.g. exception funclets that are no functions of their own.
"""
import glob, os, re, subprocess, sys

def opt(name):
    if name in sys.argv:
        i = sys.argv.index(name)
        if i + 1 >= len(sys.argv):
            sys.exit(__doc__)
        return sys.argv[i + 1]
    return None

def addr_list(path):
    return [int(l.split('#')[0].strip(), 16) for l in open(path) if l.split('#')[0].strip()]

nm_bin, skip_file = opt('--nm'), opt('--skip')
rng = None
if '--range' in sys.argv:
    k = sys.argv.index('--range')
    if k + 2 >= len(sys.argv):
        sys.exit(__doc__)
    rng = (int(sys.argv[k + 1], 16), int(sys.argv[k + 2], 16))
args, i = [], 1
while i < len(sys.argv):
    a = sys.argv[i]
    if a in ('--nm', '--skip'):
        i += 2
        continue
    if a == '--range':
        i += 3
        continue
    if not a.startswith('--'):
        args.append(a)
    i += 1
if len(args) < 2:
    sys.exit(__doc__)
lst, folder = args[0], args[1]
gen = args[2] if len(args) > 2 else None
if lst == '-':
    if not gen:
        sys.exit('LIST - needs GEN (its extents.txt is the list)')
    funcs = [int(l.split()[0], 16) for l in open(os.path.join(gen, 'extents.txt')) if l.strip()]
else:
    funcs = addr_list(lst)
if rng:
    funcs = [a for a in funcs if rng[0] <= a < rng[1]]
skip = set(addr_list(skip_file)) if skip_file else set()
funcs = [a for a in funcs if a not in skip]
# the functions with a hook of their own (HOOKS: a stack fix's hook, a port's), which run a rewrite
# registered on them although they are not in a REWRITES list (gen/funcs.h: "hook_x ... at ADDR")
hooked = set()
if gen and os.path.exists(os.path.join(gen, 'funcs.h')):
    hooked = {int(m.group(1), 16) for m in re.finditer(r'^extern int \(\*hook_\w+\)\(CPU \*c\);\s+/\* at ([0-9a-f]{8})',
                                                       open(os.path.join(gen, 'funcs.h')).read(), re.M)}
done, off = {}, {}
for p in sorted(glob.glob(os.path.join(folder, '**', '*.c'), recursive=True)):
    s = open(p, errors='replace').read()
    for m in re.finditer(r'^(\s*(?:/\*|//)\s*)?DECOMP(?:_SF)?\(([0-9a-fA-F]+),', s, re.M):
        (off if m.group(1) else done)[int(m.group(2), 16)] = os.path.relpath(p, folder)
if nm_bin:
    out = subprocess.run(['nm', nm_bin], capture_output=True, text=True)
    if out.returncode:
        sys.exit('nm %s: %s' % (nm_bin, out.stderr.strip()))
    built = {int(m.group(1), 16) for m in re.finditer(r' decomp_entry_([0-9a-f]+)$', out.stdout, re.M)}
    done = {a: done.get(a, '(registered in %s)' % os.path.basename(nm_bin)) for a in built}
    off = {a: f for a, f in off.items() if a not in built}
sizes = {}
if gen and os.path.exists(os.path.join(gen, 'extents.txt')):
    for l in open(os.path.join(gen, 'extents.txt')):
        e, lo, hi, n = l.split()[:4]
        sizes[int(e, 16)] = int(n)
size = lambda a: sizes.get(a, 0)
todo = [a for a in funcs if a not in done]
extra = sorted(set(done) - set(funcs) - skip)
skipped_done = len(skip & set(done))
if '--todo' in sys.argv:
    for a in todo:
        print('%08x %5d%s' % (a, size(a), '  (off: %s)' % off[a] if a in off else ''))
else:
    files = {}
    for a in funcs:
        if a in done:
            files.setdefault(done[a], []).append(a)
    for f in sorted(files):
        print('%-30s %4d' % (f, len(files[f])))
where = lambda a: done[a] if done[a].startswith('(') else 'in ' + done[a]
for a in extra:
    if a in hooked:
        print('%08x registered %s, not in the list (runs on its own hook)' % (a, where(a)))
    else:
        print('%08x registered %s but not in the list (no hook: it never runs)' % (a, where(a)))
print('%d of %d rewritten%s, %d to go%s' % (
    len(funcs) - len(todo), len(funcs),
    ' (%d of %d instructions)' % (sum(size(a) for a in funcs if a in done), sum(size(a) for a in funcs)) if sizes else '',
    len(todo), ', %d off' % len([a for a in todo if a in off]) if off else '') +
    (', %d of the --skip addresses rewritten besides' % skipped_done if skipped_done else ''))

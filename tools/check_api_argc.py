"""Checks how many bytes each Windows function the runtime provides pops off the stack, against
mingw-w64's 32-bit import definitions (lib32/*.def: `Name@N` is stdcall popping N bytes; a name
without @ is cdecl, popping nothing). A wrong count leaves the program's stack pointer off after
the call, which crashes it far from the cause. (pcrecomp checks its own the same way:
tools/pe/stdcall_argc.py; this script is ours.)

    python3 tools/check_api_argc.py MINGW_LIB32_DIR [RUNTIME_DIR...]

MINGW_LIB32_DIR: mingw-w64's lib32 definitions (zig ships them: <zig>/lib/libc/mingw/lib32).
The runtime folders default to runtime/. An entry imported by ordinal is checked under the name
its comment gives. Prints every mismatch and the functions mingw doesn't
list; exit status 1 if a count differs.
"""
import glob, os, re, sys

if len(sys.argv) < 2:
    sys.exit(__doc__)
defs_dir = sys.argv[1]
roots = sys.argv[2:] or [os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'runtime')]

def ordinal_name(lines, k, num):
    """the name an ordinal entry's comment gives: `/* #2 SysAllocString */` above it (up to 8
    lines), `/* WSAStartup */` after it, or `ordinal 17: InitCommonControls` above it"""
    m = re.search(r'\)\s*(?:\{.*\})?\s*/\*\s*([A-Za-z_]\w+)', lines[k])
    if m:
        return m.group(1)
    for j in range(k - 1, max(k - 9, -1), -1):
        m = re.search(r'(?:#|ordinal )%s:? ([A-Za-z_]\w+)' % num, lines[j])
        if m:
            return m.group(1)
    return None


ours = []       # (dll, name, nargs or None for cdecl, file:line)
pat = re.compile(r'^\s*(API|API_WEAK|API_CDECL)\(([A-Z0-9_]+), ([A-Za-z0-9_]+)(?:, (\d+))?\)')
for root in roots:
    for f in sorted(glob.glob(os.path.join(root, '**', '*.c'), recursive=True)):
        lines = open(f, errors='replace').read().split('\n')
        for n, line in enumerate(lines, 1):
            m = pat.match(line)
            if m:
                kind, dll, name, nargs = m.groups()
                if name.isdigit():          # an ordinal: the name its comment gives
                    name = ordinal_name(lines, n - 1, name) or name
                ours.append((dll, name, None if kind == 'API_CDECL' else int(nargs),
                             '%s:%d' % (os.path.relpath(f, os.path.dirname(root)), n)))
# (macro-made entries: SETJMP_DLL(MSVCRT) and the like are cdecl and not checked here)

cache = {}


def lookup(dll, name):
    """'stdcall', bytes  /  'cdecl', 0  /  None (not listed)"""
    if dll not in cache:
        tab = {}
        p = os.path.join(defs_dir, dll.lower() + '.def')
        if os.path.exists(p):
            for line in open(p, errors='replace'):
                line = line.split(';')[0].strip()
                if not line or line.upper().startswith(('LIBRARY', 'EXPORTS')) or line.startswith('#'):
                    continue
                tok = line.split()[0]
                tok = tok.split('==')[0]
                m = re.match(r'^([A-Za-z0-9_?@$]+?)@(\d+)$', tok)
                if m:
                    tab.setdefault(m.group(1), ('stdcall', int(m.group(2))))
                else:
                    tab.setdefault(tok, ('cdecl', 0))
        cache[dll] = tab if os.path.exists(p) else None
    tab = cache[dll]
    if tab is None:
        return 'no-def'
    return tab.get(name)


bad, missing, nodef, ok = [], [], set(), 0
for dll, name, nargs, where in ours:
    r = lookup(dll, name)
    if r == 'no-def':
        nodef.add(dll)
        continue
    if r is None:
        missing.append((dll, name, where))
        continue
    kind, nbytes = r
    want = None if kind == 'cdecl' else nbytes // 4
    if want == nargs or (want is None and nargs == 0) or (want == 0 and nargs is None):
        ok += 1
    else:
        bad.append((dll, name, nargs, want, where))
for dll, name, nargs, want, where in bad:
    print('MISMATCH %s!%s: ours pops %s, mingw-w64 says %s  (%s)' % (
        dll, name, 'nothing (cdecl)' if nargs is None else '%d args' % nargs,
        'cdecl' if want is None else '%d args' % want, where))
for dll, name, where in missing:
    print('not in mingw-w64 lib32: %s!%s  (%s)' % (dll, name, where))
print('%d match, %d differ, %d not listed%s' % (
    ok, len(bad), len(missing), '; no definitions for %s' % ', '.join(sorted(nodef)) if nodef else ''))
sys.exit(1 if bad else 0)

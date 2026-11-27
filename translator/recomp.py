"""AntiStatic's static recompiler: a 32-bit Windows program (MSVC-built, x86) and the DLLs
translated with it -> C, all in one program image.

    recomp.py <game folder> <build config .py> <output dir>

The build config (see docs/BUILD-CONFIG.md) names the modules with their SHA-256, so only the
exact files it describes are translated. Writes into the output dir: funcs.h (prototypes),
code_NNN.c (one C function per program function, named by its address), table.c (address ->
function table, the modules, the imports the runtime provides and the ones another module
provides), stubs.c (imports the runtime does not provide yet), x87_special.c, image.h
(constants), warnings.txt. Files are rewritten only when their contents change, so make rebuilds
only what changed.

Nothing of the game's code or data is copied into the toolkit or a game's repository: the
generated C comes from the player's own files and is never shared.
"""
import sys, os, time, hashlib, struct, importlib.util
sys.path.insert(0, os.path.dirname(__file__))
from pe import PE
from image import Image
import eh
from x86 import Decoder
from discover import Discovery
from codegen import FuncGen, Program, Unsupported, import_symbol

LINES_PER_FILE = 30000


class out_file:
    """A text file written only when its contents change, so make rebuilds only what changed."""

    def __init__(self, path):
        self.path, self.parts = path, []

    def write(self, t):
        self.parts.append(t)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        if exc[0]:
            return False
        text = ''.join(self.parts)
        try:
            with open(self.path, newline='\n') as fh:
                if fh.read() == text:
                    return False
        except OSError:
            pass
        with open(self.path, 'w', newline='\n') as fh:
            fh.write(text)
        return False


def load_config(path):
    spec = importlib.util.spec_from_file_location('buildcfg', path)
    cfg = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(cfg)
    return cfg


def find_file(folder, name):
    """The file in folder whose name matches without regard to case (Windows names). name may be
    a path relative to folder ("Binaries/Game.exe", either slash): each part is matched so."""
    path = folder
    for part in [p for p in name.replace('\\', '/').split('/') if p]:
        for f in os.listdir(path):
            if f.lower() == part.lower():
                path = os.path.join(path, f)
                break
        else:
            sys.exit('%s is missing from %s' % (name, folder))
    return path


def code_pointers(img, m):
    """Code addresses the module holds in data: its relocated words outside code that point
    into code; for a module without relocations, the aligned words of its data sections that
    do (checked before they become functions)."""
    out = set()
    for a in m.fixups:
        v = m.u32(a)
        if img.in_text(v) and not img.in_text(a):
            out.add(v)
    if m.fixups:
        return out                  # relocated: the relocations mark every real address
    for s in m.sections:
        if s['flags'] & (0x20 | 0x20000000) or s['name'] in ('.rsrc', '.reloc'):
            continue
        n = min(s['vsize'], s['rsize']) if s['vsize'] else s['rsize']
        for off in range(0, n - 3, 4):
            v = struct.unpack_from('<I', m.image, s['va'] + off)[0]
            if img.in_text(v):
                out.add(v)
    return out


def reloc_code_pointers(img, m):
    """Every relocated word that points into code, in code too (an immediate pushed as a
    callback): exact addresses, accepted as functions unless inside another one's code."""
    out = set()
    for a in m.fixups:
        v = m.u32(a)
        if img.in_text(v):
            out.add(v)
    return out


def module_list(cfg):
    """MODULES as dicts {file, sha256, base, load}. The older tuple form
    (name, file, sha256[, base]) is read too; base None = the module's preferred base."""
    out = []
    for d in cfg.MODULES:
        if not isinstance(d, dict):
            d = dict(name=d[0], file=d[1], sha256=d[2], base=d[3] if len(d) > 3 else None)
        d = dict(d)
        d.setdefault('base', None)
        d.setdefault('load', 'start')
        out.append(d)
    return out


SETJMP_NAMES = {'_setjmp', '_setjmp3', 'setjmp', '_setjmpex'}
LONGJMP_NAMES = {'longjmp'}


def frame_switch_jumps(funcs):
    """`jmp reg` within six instructions after both `mov esp, X` (X not ebp: an epilogue's
    `mov esp, ebp; pop ebp` restores the caller's own frame) and `mov ebp, Y` (not a pop)."""
    out = set()
    for f in funcs.values():
        addrs = sorted(f.insns)
        for n, a in enumerate(addrs):
            i = f.insns[a]
            if i.mn != 'jmp' or not i.ops or i.ops[0][0] != 'r':
                continue
            sp = bp = False
            for b in addrs[max(0, n - 6):n]:
                j = f.insns[b]
                if j.mn != 'mov' or len(j.ops) != 2 or j.ops[0][0] != 'r':
                    continue
                src = j.ops[1]
                if j.ops[0][1] == 'esp' and not (src[0] == 'r' and src[1] == 'ebp'):
                    sp = True
                elif j.ops[0][1] == 'ebp':
                    bp = True
            if sp and bp:
                out.add(a)
    return out


def setjmp_places(cfg, mods, funcs, host, links):
    """({function: return addresses of its setjmp calls}, {longjmp's indirect jumps}).
    setjmp is an import by name (the runtime's, or a translated C runtime's export), a function
    in the build config's SETJMP list (a C runtime linked into the program), or a one-jump stub
    to either; longjmp likewise (LONGJMP), its jumps through a register or memory being where it
    goes back into the function that called setjmp."""
    sj = set(getattr(cfg, 'SETJMP', []))
    lj = set(getattr(cfg, 'LONGJMP', []))
    for m in mods:
        for name, va in m.exports.items():
            if name in SETJMP_NAMES:
                sj.add(va)
            elif name in LONGJMP_NAMES:
                lj.add(va)
    sj_iat = {iat for iat, (dll, name) in host.items() if name in SETJMP_NAMES}
    sj_iat |= {iat for iat, (va, dll, name) in links.items() if name in SETJMP_NAMES}
    is_mem_abs = lambda op: op[0] == 'm' and op[3] is None and op[4] is None
    for e, f in funcs.items():          # import stubs: jmp [iat]
        if len(f.insns) == 1:
            i = next(iter(f.insns.values()))
            if i.mn == 'jmp' and i.ops and is_mem_abs(i.ops[0]) and i.ops[0][6] in sj_iat:
                sj.add(e)
            elif i.mn == 'jmp' and i.ops and i.ops[0][0] == 'i' and i.ops[0][1] in sj:
                sj.add(e)
    places = {}
    for e, f in funcs.items():
        for a, i in f.insns.items():
            if i.mn != 'call' or not i.ops:
                continue
            op = i.ops[0]
            if (op[0] == 'i' and op[1] in sj) or (is_mem_abs(op) and op[6] in sj_iat):
                ret = a + i.size
                if ret in f.insns:
                    places.setdefault(e, set()).add(ret)
    jumps = set()
    for e in lj:
        f = funcs.get(e)
        for a, i in (f.insns.items() if f else ()):
            if i.mn == 'jmp' and i.ops and i.ops[0][0] in ('m', 'r'):
                jumps.add(a)
    return places, jumps


def rewrite_list(cfg, cfgpath):
    """the build config's REWRITES file: addresses, one a line (hex, # comments)"""
    name = getattr(cfg, 'REWRITES', None)
    if not name:
        return []
    path = os.path.join(os.path.dirname(os.path.abspath(cfgpath)), name)
    out = []
    for n, l in enumerate(open(path), 1):
        l = l.split('#')[0].strip()
        if l:
            a = int(l, 16)
            if a in out:
                sys.exit('REWRITES: %08x is listed twice (%s line %d)' % (a, name, n))
            out.append(a)
    return out


def main():
    gamedir, cfgpath, out = sys.argv[1:4]
    os.makedirs(out, exist_ok=True)
    cfg = load_config(cfgpath)
    if not hasattr(cfg, 'MODULES'):
        # a single-program config (SHA256 and no MODULES): the first argument is the program
        cfg.MODULES = [dict(file=os.path.basename(gamedir), sha256=cfg.SHA256)]
        gamedir = os.path.dirname(gamedir) or '.'
    cfg.MODULES = module_list(cfg)
    mods = []
    for d in cfg.MODULES:
        path = find_file(gamedir, d['file'])
        sha = hashlib.sha256(open(path, 'rb').read()).hexdigest()
        if sha != d['sha256']:
            sys.exit('%s is not the file this build describes (%s; sha256 %s)' % (path, cfg.NAME, sha))
        m = PE(path, d['base'])
        m.name = os.path.basename(d['file'].replace('\\', '/'))   # the name imports use
        m.file = os.path.relpath(path, gamedir).replace(os.sep, '/')   # as on disk, for the loader
        m.load = d['load']
        mods.append(m)
    # byte changes to the code before translation (CODE_PATCHES: va -> (original hex, new hex));
    # the original bytes are checked so a patch never lands on other code
    for va, (orig, new) in sorted(getattr(cfg, 'CODE_PATCHES', {}).items()):
        orig, new = bytes.fromhex(orig.replace(' ', '')), bytes.fromhex(new.replace(' ', ''))
        m = next(m for m in mods if m.base <= va < m.base + m.size_of_image)
        o = va - m.base
        if len(orig) != len(new) or bytes(m.image[o:o + len(orig)]) != orig:
            sys.exit('code patch at %08x: the original bytes differ' % va)
        m.image[o:o + len(new)] = new
    img = Image(mods)

    # imports: the ones another translated module exports, and the ones the runtime provides
    redirect, links, host = {}, {}, {}
    # IMPORT_ALIASES (build config): {(importing module, dll): other dll}, or {dll: other dll} for
    # every module: a module's imports from one DLL linked to another translated module's exports
    # (Nitronic Rush: d3dx9_43.dll's msvcrt.dll imports to the translated msvcr90.dll; the port
    # checks that the data imports' layouts agree: _iob, __pioinfo...)
    aliases = {}
    for k, v in getattr(cfg, 'IMPORT_ALIASES', {}).items():   # (any case: Windows names)
        aliases[tuple(x.lower() for x in k) if isinstance(k, tuple) else k.lower()] = v.lower()
    names = set(m.name.lower() for m in mods)
    for k in aliases:
        mod, dll = k if isinstance(k, tuple) else (None, k)
        if mod is not None and mod not in names:
            sys.exit('IMPORT_ALIASES: no module %s (the build has %s)' % (mod, ', '.join(sorted(names))))
        if not any(d.lower() == dll for m in mods for d, _n, _a in m.imports
                   if mod is None or m.name.lower() == mod):
            sys.exit('IMPORT_ALIASES: %s imports nothing from %s' % (mod or 'no module', dll))
    for m in mods:
        for dll, name, iat in m.imports:
            alias = aliases.get((m.name.lower(), dll.lower())) or aliases.get(dll.lower())
            r = img.resolve_import(alias, name) if alias else img.resolve_import(dll, name)
            if alias and r is None:
                sys.exit('%s: %s!%s has no export in %s (IMPORT_ALIASES)' % (m.name, dll, name, alias))
            if r is None:
                host[iat] = (dll, name)
                continue
            tm, va = r
            links[iat] = (va, dll, name)
            if img.in_text(va):
                redirect[iat] = va
    dec = Decoder(img, redirect)
    # int3s that carry on (a debugger's "continue"): decoded as a nop (an assert whose hook logs)
    cont = list(getattr(cfg, 'INT3_CONTINUE', []))
    for a in cont:
        i = dec.at(a)
        assert i is not None and i.mn == 'int3', '%08x is not an int3' % a
        i.mn, i.ops, i.text = 'nop', [], 'nop   ; int3 carried on from (INT3_CONTINUE)'
    seeds = set()
    img.reloc_sites = set(a for m in mods for a in m.fixups)   # (discover.data_at reads them)
    from discover import data_at
    for m in mods:
        if m.entry:
            seeds.add(m.entry)
        # exported data a C++ runtime keeps in its code section (MSVCP90's vtables `??_7...@@6B@`
        # and static members `?x@...@@2...`): a decorated name that doesn't end in Z (a function
        # type's end) is data, not a function [verified: Nitronic Rush's msvcp90 and msvcr90, 138
        # addresses; their plain-named exports are all code]
        data = set(va for name, va in m.exports.items() if name.startswith('?') and not name.endswith('Z'))
        # ... and exported data an ordinal or a plain name can't tell apart (MFC71's CRuntimeClass and
        # message-map structures, by ordinal): what discover.data_at sees as data
        data |= set(va for va in list(m.exports.values()) + list(m.ordinals.values())
                    if img.in_text(va) and data_at(img, dec, va))
        for name, va in m.exports.items():
            if img.in_text(va) and va not in data:
                seeds.add(va)
        for o, va in m.ordinals.items():
            if img.in_text(va) and va not in data:
                seeds.add(va)
        if data:
            print('%s: %d exports are data (decorated, not ending in Z, or data by their bytes): not functions' % (m.name, len(data)))
        # TLS callbacks: run by the loader before the entry point and on every thread start
        for va in (m.tls or {}).get('callbacks', []):
            if img.in_text(va):
                seeds.add(va)
    forced = set()
    for f, n in getattr(cfg, 'NORETURN_EXPORTS', []):
        mm = img.by_name.get(f.lower())
        va = mm.export(n) if mm else None
        if va:
            forced.add(va)
    forced |= set(getattr(cfg, 'NORETURN_ADDRS', []))     # (statically linked runtimes: by address)
    # import stubs (jmp [IAT]) to a function that never returns never return either (the exe's
    # own _CxxThrowException stub: without this, the code after each throw ran on into the next
    # function's body)
    by_iat = {iat: va for iat, (va, d, n) in links.items()}
    for m in mods:
        lo, hi = m.text_range()
        a = lo
        while a + 6 <= hi:
            if img.u8(a) == 0xff and img.u8(a + 1) == 0x25 and by_iat.get(img.u32(a + 2)) in forced:
                forced.add(a)
                a += 6
            else:
                a += 1
    # code pointers held in data, checked before they become functions: relocated words (exact
    # addresses: static constructors, vtables, exception handlers' funclets) are accepted unless
    # they land inside another function's code (a relocated word can also be a label inside a
    # function, or an address in a data table); the aligned words of a module without
    # relocations must also look like a function start
    cands = set()
    img.reloc_pointers = set()
    img.reloc_sites = set(a for m in mods for a in m.fixups)
    for m in mods:
        cands |= code_pointers(img, m)
        img.reloc_pointers |= reloc_code_pointers(img, m)

    # exception handling: handler funclets are functions; places a catch / __except goes on at
    # a statically linked C runtime's handlers aren't imports: the build config names them
    # (CXX_FRAME_HANDLERS, SEH_HANDLERS, SEH_PROLOGS; from penguins-native)
    eh_entries, eh_places, n_fi, n_st = eh.scan(
        img, eh.handler_addresses(img, '__CxxFrameHandler') | eh.handler_addresses(img, '__CxxFrameHandler3') |
        set(getattr(cfg, 'CXX_FRAME_HANDLERS', [])),
        eh.handler_addresses(img, '_except_handler3') | set(getattr(cfg, 'SEH_HANDLERS', [])),
        set(getattr(cfg, 'SEH_PROLOGS', [])))
    seeds |= eh_entries
    print('exception tables: %d C++ (FuncInfo), %d SEH scope tables; %d handler funclets, %d places '
          'to go on at' % (n_fi, n_st, len(eh_entries), len(set().union(*eh_places.values()))))

    t0 = time.time()
    disc = Discovery(img, dec, {va: n for va, (d, n) in host.items()}, cfg.NORETURN_IMPORTS,
                     cfg.EXTRA_ENTRIES, cfg.JUMP_TABLES, cfg.NOT_ENTRIES, forced)
    funcs = disc.run(sorted(seeds), cands)
    # CODE_SECTION_POINTERS (build config, from nitronic-native): code pointers a code section
    # holds in its own data. Microsoft's runtime DLLs keep read-only data in .text (vtables, the C
    # runtime's initializer tables), so a function may be named only by a relocated word there
    # (MSVCR90's first initializer, 0x7854ee76). Which relocated words of a code section are data
    # is known only once the code is: those no instruction holds and no switch table lists. Their
    # targets are candidates like those from data sections (a target that is itself a relocated
    # word is a table, not code), and discovery runs again until nothing new turns up.
    if getattr(cfg, 'CODE_SECTION_POINTERS', False):
        all_fixups = set(a for m in mods for a in m.fixups)
        tried = set()
        for _ in range(8):
            at, tables = {}, set()
            for f in funcs.values():
                at.update(f.insns)
                for table, count, _how in f.jt_info.values():   # (switch tables: labels, not functions)
                    tables.update(range(table, table + 4 * count, 4))

            def in_insn(a):
                for back in range(12):
                    i = at.get(a - back)
                    if i is not None and i.next >= a + 4:
                        return True
                return False
            more = set()
            for m in mods:
                for a in m.fixups:
                    if not img.in_text(a) or a in tables or in_insn(a):
                        continue
                    v = m.u32(a)
                    if img.in_text(v) and v not in funcs and v not in tried and v not in all_fixups \
                            and v not in cfg.NOT_ENTRIES:
                        more.add(v)
            if not more:
                break
            tried |= more
            cands |= more
            print("code pointers in code sections' data: %d more candidates" % len(more))
            disc = Discovery(img, dec, {va: n for va, (d, n) in host.items()}, cfg.NORETURN_IMPORTS,
                             cfg.EXTRA_ENTRIES, cfg.JUMP_TABLES, cfg.NOT_ENTRIES, forced)
            funcs = disc.run(sorted(seeds), cands)
    errs = [(e, a, msg) for e, f in funcs.items() for a, msg in f.errors]
    print('discovery: %d functions, %d instructions, %d errors (%.1fs)' % (
        len(funcs), sum(len(f.insns) for f in funcs.values()), len(errs), time.time() - t0))
    for m in mods:
        lo, hi = m.text_range()
        n = sum(1 for e in funcs if lo <= e < hi)
        print('  %-18s %08x %5d functions' % (m.name, m.base, n))
    for e in sorted(e for e, f in funcs.items() if e not in f.insns):
        print('  %08x: no code at this entry (%s): a stub that stops with a message' % (e, funcs[e].errors[:1]))
    for e in errs[:20]:
        print('  error in %08x at %08x: %s' % e)
    for l in disc.log[:40]:
        print('  ' + l)

    # every function's extent, for finding who holds an address (work tools)
    with out_file(os.path.join(out, 'extents.txt')) as fh:
        for e in sorted(funcs):
            f = funcs[e]
            lo = min(f.insns) if f.insns else e
            hi = max(a + i.size for a, i in f.insns.items()) if f.insns else e
            fh.write('%08x %08x %08x %d\n' % (e, lo, hi, len(f.insns)))
    # every instruction's address and its function (tools/ghidra_check.py)
    with out_file(os.path.join(out, 'insns.txt')) as fh:
        for e in sorted(funcs):
            fh.write('%08x %s\n' % (e, ' '.join('%x' % a for a in sorted(funcs[e].insns))))
    with out_file(os.path.join(out, 'rejected.txt')) as fh:
        for a, why in sorted(disc.rejected.items()):
            fh.write('%08x %s\n' % (a, why[0] if why else '?'))

    prog = Program(img, funcs, disc.noreturn, host, cfg.NORETURN_IMPORTS,
                   getattr(cfg, 'NONLOCAL_JUMPS', []))
    prog.set_tables(getattr(cfg, 'TABLES', []))
    prog.hooks = dict(getattr(cfg, 'HOOKS', {}))
    prog.eh_places = eh_places
    # setjmp / longjmp: the place after each call of a setjmp is one a longjmp goes on at, kept
    # like a catch's (runtime/win32/eh.c); a translated longjmp's last jump is a nonlocal jump
    prog.setjmp_places, lj_jumps = setjmp_places(cfg, mods, funcs, host, links)
    prog.nonlocal_jumps |= lj_jumps
    # a C runtime's own continuation jumps (MSVCR90's _JumpToContinuation, its catch-block frame
    # switch): a jump through a register just after esp and ebp are both loaded from somewhere
    # new goes on in another function's frame, so the runtime resumes the function that holds the
    # place (build config NONLOCAL_JUMPS, found here) instead of calling it as a function
    fj = frame_switch_jumps(funcs)
    prog.nonlocal_jumps |= fj
    if fj:
        print('frame-switching jumps (continuations): %s' % ' '.join('%08x' % a for a in sorted(fj)))
    if prog.setjmp_places:
        print('setjmp: %d calls in %d functions; %d translated longjmp jumps' % (
            sum(len(v) for v in prog.setjmp_places.values()), len(prog.setjmp_places), len(lj_jumps)))
    prog.inline = dict(getattr(cfg, 'INLINE', {}))
    prog.set_consts(getattr(cfg, 'CONSTS', {}))
    prog.icall_fpu = dict(getattr(cfg, 'ICALL_FPU', {}))
    # (one instruction, or a list of them sharing one runtime-set operand address)
    prog.mem_overrides = {a: n for n, (at, orig) in getattr(cfg, 'MEM_OVERRIDES', {}).items()
                          for a in (at if isinstance(at, (list, tuple)) else [at])}
    prog.frame_steps = dict(getattr(cfg, 'FRAME_STEPS', {}))
    prog.frame_counts = dict(getattr(cfg, 'FRAME_COUNTS', {}))
    # functions run at the game's own fixed tick (runtime framerate_tick_call): hooked by a
    # wrapper of their own
    for a in getattr(cfg, 'TICKS', []):
        prog.hooks[a] = 'tick_%x' % a
    # the functions being rewritten as hand-written C (runtime/rewrite/rewrite.h, docs/REWRITE.md):
    # REWRITES names a list file (one hex address a line, # comments; relative to the build
    # config), and each listed function gets the hook hook_dc_<address> its DECOMP fills
    # INNER_HOOKS (off unless set): a rewritten function the port also hooks
    # (HOOKS) gets its hook_dc_ inside F_x_orig, so the port's hook (widescreen, controller menus)
    # wraps the rewrite as it wrapped the translation. True: every REWRITES address with a HOOKS
    # entry; a dict {addr: 'name'}: those, as hook_<name> (each must have a HOOKS entry).
    inner_cfg = getattr(cfg, 'INNER_HOOKS', None)
    prog.inner_hooks = dict(inner_cfg) if isinstance(inner_cfg, dict) else {}
    for a in rewrite_list(cfg, cfgpath):
        if a not in funcs:
            sys.exit('REWRITES: %08x is not the start of a function the translation has (padding? an '
                     'address inside one? gen/extents.txt lists them; NOT_ENTRIES / EXTRA_ENTRIES change them)' % a)
        if a in prog.inner_hooks:
            continue
        if a in prog.hooks and prog.hooks[a] != 'dc_%x' % a:
            if inner_cfg is True:
                prog.inner_hooks[a] = 'dc_%x' % a
                continue
            sys.exit('REWRITES: %08x already has the hook %s (INNER_HOOKS = True puts the rewrite inside it)'
                     % (a, prog.hooks[a]))
        prog.hooks[a] = 'dc_%x' % a
    for a, h in prog.inner_hooks.items():
        if a not in prog.hooks:
            sys.exit('INNER_HOOKS: %08x has no hook of its own (HOOKS) to be inside' % a)
    prog.flag_interface(log=lambda s: None)
    prog.fpu_interface()
    os.makedirs(out, exist_ok=True)

    # ---------------------------------------------------------------- functions
    warns = []
    chunks, cur, curlines = [], [], 0
    nlines = 0
    for e in sorted(funcs):
        g = FuncGen(prog, funcs[e])
        try:
            text = g.gen()
        except Unsupported as ex:
            warns.append('%08x: %s' % (e, ex))
            text = ('/* %08x: not translated: %s */\nvoid %s(CPU *c)\n{\n    unsupported(c, 0x%xu, "%s");\n}\n'
                    % (e, ex, prog.fname(e), e, str(ex).replace('"', "'")))
        warns.extend('%08x: %s' % (e, w) for w in g.warn)
        n = text.count('\n')
        nlines += n
        cur.append(text)
        curlines += n
        if curlines > LINES_PER_FILE:
            chunks.append(cur)
            cur, curlines = [], 0
    if cur:
        chunks.append(cur)
    for k, ch in enumerate(chunks):
        with out_file(os.path.join(out, 'code_%03d.c' % k)) as fh:
            fh.write('/* Generated from the user\'s game files by tools/recomp.py. Do not edit or share. */\n')
            fh.write('#include "cpu.h"\n#include "funcs.h"\n#include "funcs_weak.h"\n\n')
            fh.write('\n'.join(ch))
    for fn in os.listdir(out):
        if fn.startswith('code_') and fn.endswith('.c') and int(fn[5:8]) >= len(chunks):
            os.remove(os.path.join(out, fn))
    host_syms = sorted(set(import_symbol(d, n) for d, n in host.values()))
    with out_file(os.path.join(out, 'funcs.h')) as fh:
        fh.write('#pragma once\n#include "cpu.h"\n')
        for e in sorted(funcs):
            fh.write('void %s(CPU *c);\n' % prog.fname(e))
        # imports: plain prototypes here, so a port file that defines one (and includes this
        # header) makes a strong definition. The generated files' references are weak through
        # funcs_weak.h (ELF), or fall back to missing_<name> through stubs.c's /alternatename
        # (Windows): the runtime's or the port's definition if there is one, else missing_<name>.
        for s in host_syms:
            fh.write('void %s(CPU *c);\nvoid missing_%s(CPU *c);\n' % (s, s))
        for t in prog.tables:
            fh.write('extern uint32_t reloc_%s, limit_%s;\n' % (t['name'], t['name']))
        for name in sorted(getattr(cfg, 'CONSTS', {})):
            fh.write('extern uint32_t rc_%s;\n' % name)
        for name in sorted(getattr(cfg, 'MEM_OVERRIDES', {})):
            fh.write('extern uint32_t rm_%s;   /* the operand\'s address; the runtime may change it */\n' % name)
        for a, h in sorted(prog.hooks.items()):
            fh.write('extern int (*hook_%s)(CPU *c);   /* at %08x; returns 1 if it did the call itself */\n' % (h, a))
            fh.write('void %s_orig(CPU *c);\n' % prog.fname(a))
        for a, h in sorted(prog.inner_hooks.items()):
            fh.write('extern int (*hook_%s)(CPU *c);   /* at %08x, inside %s_orig (after hook_%s) */\n'
                     % (h, a, prog.fname(a), prog.hooks[a]))

    with out_file(os.path.join(out, 'funcs_weak.h')) as fh:
        fh.write('/* Generated by tools/recomp.py: the imports as weak references, for the generated files and\n'
                 '   rewrites (runtime/rewrite/rewrite.h) only. A file that defines an import must not include\n'
                 '   this: its definition would be weak too. On Windows (COFF) a weak reference against a weak\n'
                 '   definition goes to whichever object links first, so there stubs.c names missing_<name> as\n'
                 '   each import\'s /alternatename instead and the references stay plain. */\n'
                 '#pragma once\n#ifndef _WIN32\n')
        for s in host_syms:
            fh.write('#pragma weak %s\n' % s)
        fh.write('#endif\n')

    # ---------------------------------------------------------------- tables
    with out_file(os.path.join(out, 'table.c')) as fh:
        fh.write('/* Generated by tools/recomp.py */\n#include "cpu.h"\n#include "funcs.h"\n#include "funcs_weak.h"\n#include "runtime.h"\n\n')
        fh.write('const FuncEntry func_table[] = {\n')
        for e in sorted(funcs):
            fh.write('    {0x%08xu, %s},\n' % (e, prog.fname(e)))
        fh.write('};\nconst int func_count = %d;\n\n' % len(funcs))
        # the functions whose callers read flags after them (Program.ret_flags; CF 1, PF 2, AF 4, ZF 8,
        # SF 16, OF 32): the register audit compares those flags (runtime/rewrite/audit.c)
        fh.write('const uint32_t flags_out_table[] = {\n')
        for e, m in sorted(prog.ret_flags.items()):
            if m:
                fh.write('    0x%08xu, %du,\n' % (e, m))
        fh.write('    0, 0\n};\n\n')
        fh.write('/* imports the runtime provides */\nconst ImportEntry import_table[] = {\n')
        for va, (dll, name) in sorted(host.items()):
            sym = import_symbol(dll, name)
            fh.write('    {0x%08xu, "%s", "%s", %s, missing_%s},\n' % (va, dll, name.replace('\\', '\\\\'), sym, sym))
        fh.write('};\nconst int import_count = %d;\n\n' % len(host))
        fh.write('/* imports another translated module provides: slot -> address */\nconst LinkEntry link_table[] = {\n')
        for va, (t, dll, name) in sorted(links.items()):
            fh.write('    {0x%08xu, 0x%08xu},   /* %s!%s */\n' % (va, t, dll, name.replace('*/', '* /')))
        fh.write('    {0, 0}\n};\nconst int link_count = %d;\n\n' % len(links))
        fh.write('const ModuleEntry module_table[] = {\n')
        for d in cfg.MODULES:
            m = img.by_name[os.path.basename(d['file'].replace('\\', '/')).lower()]
            texts = m.text_ranges()
            if len(texts) > 8:
                sys.exit('%s has more than 8 code sections' % m.name)
            fh.write('    {"%s", "%s", "%s", 0x%08xu, 0x%08xu, 0x%08xu, %d, {%s}, 0x%08xu, %d, %d},\n' % (
                d.get('name', m.name), m.file, d['sha256'], m.base, m.size_of_image, m.entry or 0, len(texts),
                ', '.join('{0x%08xu, 0x%08xu}' % r for r in texts), m.preferred_base, int(m.is_dll),
                int(d['load'] == 'start')))
        fh.write('};\nconst int module_count = %d;\n\n' % len(cfg.MODULES))
        for t in prog.tables:
            fh.write('uint32_t reloc_%s = 0x%08xu, limit_%s = %du;\n' % (t['name'], t['base'], t['name'], t['count']))
        for a, h in sorted(prog.hooks.items()):
            if h.startswith('tick_'):
                fh.write('static int tick_wrap_%x(CPU *c) { return framerate_tick_call(c, %s_orig, 0x%xu); }\n'
                         % (a, prog.fname(a), a))
                fh.write('int (*hook_%s)(CPU *c) = tick_wrap_%x;\n' % (h, a))
            else:
                fh.write('int (*hook_%s)(CPU *c);\n' % h)
        for a, h in sorted(prog.inner_hooks.items()):
            fh.write('int (*hook_%s)(CPU *c);\n' % h)
        for name, (a, orig) in sorted(getattr(cfg, 'MEM_OVERRIDES', {}).items()):
            fh.write('uint32_t rm_%s = 0x%08xu;\n' % (name, orig))
        # the hooked functions (their wrappers): the reference build runs these as the
        # translated build does, so the runtime's hooks act in both
        fh.write('const FuncEntry hooked_table[] = {\n')
        for a, h in sorted(prog.hooks.items()):
            fh.write('    {0x%08xu, %s},\n' % (a, prog.fname(a)))
        fh.write('    {0, 0}\n};\nconst int hooked_count = %d;\n' % len(prog.hooks))
        for name, d in sorted(getattr(cfg, 'CONSTS', {}).items()):
            fh.write('uint32_t rc_%s = 0x%xu;\n' % (name, d['value']))
        fh.write('const TableEntry table_list[] = {\n')
        for t in prog.tables:
            fh.write('    {"%s", &reloc_%s, &limit_%s, 0x%08xu, %du, %du},\n' % (
                t['name'], t['name'], t['name'], t['base'], t['entry'], t['count']))
        fh.write('    {0}\n};\n')
    # each import's stand-in for when nothing defines it (they no longer compete with the runtime's
    # weak defaults: two weak definitions went to whichever linked first, the stub)
    with out_file(os.path.join(out, 'stubs.c')) as fh:
        fh.write('/* Generated by tools/recomp.py */\n#include "cpu.h"\n#include "funcs.h"\n#include "funcs_weak.h"\n#include "runtime.h"\n\n')
        done = set()
        for va, (dll, name) in sorted(host.items()):
            sym = import_symbol(dll, name)
            if sym in done:
                continue
            done.add(sym)
            fh.write('void missing_%s(CPU *c) { missing_import(c, "%s", "%s"); }\n'
                     % (sym, dll, name.replace('\\', '\\\\')))
        fh.write('\n#ifdef _WIN32\n/* each import falls back to its missing_<name> when nothing defines it (any definition,\n'
                 '   weak or strong, wins over it whatever the link order) */\n'
                 '#define AS_STR2(x) #x\n#define AS_STR(x) AS_STR2(x)\n#define ALT(s) __asm__(".section .drectve,\\"yn\\"\\n.ascii \\" -alternatename:" '
                 'AS_STR(__USER_LABEL_PREFIX__) #s "=" AS_STR(__USER_LABEL_PREFIX__) "missing_" #s "\\"\\n.text");\n')
        for s in sorted(done):
            fh.write('ALT(%s)\n' % s)
        fh.write('#endif\n')
    trans = {}
    for f in funcs.values():
        for a, i in f.insns.items():
            # reference runs (Unicorn) compute these on the host CPU instead; fprem and fprem1 too
            # (the CRT's sin and cos reduce huge arguments with them)
            if i.mn in ('fsin', 'fcos', 'fsincos', 'fptan', 'fpatan', 'fyl2x', 'fyl2xp1', 'f2xm1',
                        'fprem', 'fprem1'):
                trans[a] = i.raw[-1]
    with out_file(os.path.join(out, 'x87_special.c')) as fh:
        fh.write('/* Generated by tools/recomp.py: x87 transcendental instructions (address, opcode byte) */\n')
        fh.write('#include <stdint.h>\nconst uint32_t x87_special_addr[] = {\n')
        fh.write(''.join('    0x%08xu,\n' % a for a in sorted(trans)) or '    0,\n')
        fh.write('};\nconst uint8_t x87_special_op[] = {\n')
        fh.write(''.join('    0x%02x,\n' % trans[a] for a in sorted(trans)) or '    0,\n')
        fh.write('};\nconst int x87_special_count = %d;\n' % len(trans))
        # rdtsc and cpuid: the reference build answers them as the translated code does
        cpu = {a: i.raw[-1] for f in funcs.values() for a, i in f.insns.items() if i.mn in ('rdtsc', 'cpuid')}
        fh.write('const uint32_t cpu_special_addr[] = {\n')
        fh.write(''.join('    0x%08xu,\n' % a for a in sorted(cpu)) or '    0,\n')
        fh.write('};\nconst uint8_t cpu_special_op[] = {\n')
        fh.write(''.join('    0x%02x,\n' % cpu[a] for a in sorted(cpu)) or '    0,\n')
        fh.write('};\nconst int cpu_special_count = %d;\n' % len(cpu))
        # SSE reciprocal approximations (rcpss/rcpps/rsqrtss/rsqrtps): hardware and emulators
        # round them differently; the reference runs do them on the host CPU too
        approx = {a: len(i.raw) for f in funcs.values() for a, i in f.insns.items()
                  if i.mn in ('rcpss', 'rcpps', 'rsqrtss', 'rsqrtps')}
        fh.write('const uint32_t sse_special_addr[] = {\n')
        fh.write(''.join('    0x%08xu,\n' % a for a in sorted(approx)) or '    0,\n')
        fh.write('};\nconst uint8_t sse_special_len[] = {\n')
        fh.write(''.join('    %d,\n' % approx[a] for a in sorted(approx)) or '    0,\n')
        fh.write('};\nconst int sse_special_count = %d;\n' % len(approx))
    exe = next(m for m in mods if m.name.lower().endswith('.exe'))
    with out_file(os.path.join(out, 'image.h')) as fh:
        fh.write('#pragma once\n')
        fh.write('#define IMAGE_BASE 0x%08xu\n' % exe.base)
        fh.write('#define IMAGE_SIZE 0x%08xu\n' % exe.size_of_image)
        fh.write('#define IMAGE_ENTRY 0x%08xu\n' % exe.entry)
        fh.write('#define EXE_NAME "%s"\n' % exe.name)
        fh.write('#define STACK_RESERVE 0x%08xu\n' % exe.stack_reserve)
        fh.write('#define BUILD_NAME "%s"\n' % cfg.NAME)
        fh.write('#define MODULE_COUNT %d\n' % len(mods))
        for name, a in sorted(getattr(cfg, 'DATA', {}).items()):
            fh.write('#define GAME_%s 0x%08xu\n' % (name.upper(), a))
    with out_file(os.path.join(out, 'warnings.txt')) as fh:
        fh.write('\n'.join(warns) + '\n')
    print('generated %d functions, %d lines in %d files, %d warnings (%.1fs)' % (
        len(funcs), nlines, len(chunks), len(warns), time.time() - t0))
    print('imports: %d from the runtime (%d distinct), %d between modules (%d calls made direct)' % (
        len(host), len(host_syms), len(links), len(redirect)))
    for w in warns[:30]:
        print('  ' + w)
    # out_file leaves an unchanged output's time alone (so only what changed recompiles): a
    # Makefile's "translated" target depends on this stamp, touched by every finished run
    with open(os.path.join(out, '.translated'), 'w') as fh:
        fh.write('%d functions\n' % len(funcs))


if __name__ == '__main__':
    main()

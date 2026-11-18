"""AntiStatic self-test: the translator checked against the Unicorn CPU emulator.

    python3 tests/selftest/run.py [--seeds N] [--only NAME] [--no-pcrecomp] [--forms FILE]... [--only-forms]

Assembles tests/selftest/cases.s (GNU as), wraps it in a small PE32 program, translates that with
translator/recomp.py, builds the translated C with the CPU core (runtime/cpu) and harness.c, then
runs every case on N random inputs both ways and compares registers, the flags the cases store,
scratch memory and the stack. Needs gcc, binutils and Python with capstone and unicorn (set
PYTHON to use another interpreter for the translator). Exit status 0 = every case matched.

pcrecomp_cases.s / .json are pcrecomp's lifter cases (MIT; made by import_pcrecomp.py): each runs
once, from its own fixed registers and memory, compared the same way.

--forms FILE adds the cases forms.py made from a game's own instructions (build/selftest/forms_*.s,
never committed); --only-forms runs just those.
"""
import argparse, json, os, random, re, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
BUILD = os.path.join(ROOT, 'build', 'selftest')
BASE, TEXT_RVA = 0x400000, 0x1000
SCRATCH, SCRATCH_SIZE = 0x10000000, 0x1000
STACK, STACK_SIZE = 0x20000000, 0x10000
STACK_TOP = STACK + 0xf000
SENTINEL = 0x7efe0000
RESULTS = 0xf00
ALL_FLAGS = 0x8d5
REGS = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']


def sh(*cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit('%s failed:\n%s%s' % (' '.join(cmd), r.stdout[-3000:], r.stderr[-3000:]))
    return r.stdout


def cases(files=None):
    """[(name, flag mask)] from cases.s (and the forms files), in order"""
    out, mask = [], ALL_FLAGS
    lines = [l for f in [os.path.join(HERE, 'cases.s')] + list(files or []) for l in open(f)]
    for line in lines:
        m = re.match(r'#\s*mask\s+(0x[0-9a-f]+)', line)
        if m:
            mask = int(m.group(1), 16)
            continue
        m = re.match(r'(case_\w+):', line)
        if m:
            out.append((m.group(1), mask))
            mask = ALL_FLAGS
    return out


def assemble(forms=()):
    os.makedirs(BUILD, exist_ok=True)
    o, elf, raw = (os.path.join(BUILD, n) for n in ('cases.o', 'cases.elf', 'text.bin'))
    sh('as', '--32', '-o', o, os.path.join(HERE, 'cases.s'), os.path.join(HERE, 'pcrecomp_cases.s'), *forms)
    sh('ld', '-m', 'elf_i386', '-Ttext=0x%x' % (BASE + TEXT_RVA), '-e', 'case_add', '-o', elf, o)
    sh('objcopy', '-O', 'binary', '-j', '.text', elf, raw)
    syms = {}
    for line in sh('nm', elf).splitlines():
        parts = line.split()
        if len(parts) != 3:
            continue
        a, t, name = parts
        if t in 'tT':
            syms[name] = int(a, 16)
    return open(raw, 'rb').read(), syms


def make_pe(text, entry):
    """A minimal PE32 program: headers and one code section, no imports, no relocations."""
    falign, salign = 0x200, 0x1000
    raw_size = (len(text) + falign - 1) & ~(falign - 1)
    size_image = TEXT_RVA + ((len(text) + salign - 1) & ~(salign - 1))
    hdr = bytearray(0x400)
    hdr[0:2] = b'MZ'
    struct.pack_into('<I', hdr, 0x3c, 0x80)
    pe = 0x80
    hdr[pe:pe + 4] = b'PE\0\0'
    struct.pack_into('<HHIIIHH', hdr, pe + 4, 0x14c, 1, 0, 0, 0, 0xe0, 0x0102)
    o = pe + 24
    struct.pack_into('<HBBIIIIII', hdr, o, 0x10b, 6, 0, raw_size, 0, 0, entry - BASE, TEXT_RVA, 0)
    struct.pack_into('<IIIHHHHHHIIIIHHIIIIII', hdr, o + 28, BASE, salign, falign, 4, 0, 0, 0, 4, 0, 0,
                     size_image, 0x400, 0, 3, 0, 0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
    so = o + 0xe0
    struct.pack_into('<8sIIIIIIHHI', hdr, so, b'.text', len(text), TEXT_RVA, raw_size, 0x400, 0, 0, 0, 0,
                     0x60000020)
    return bytes(hdr) + text + bytes(raw_size - len(text))


def translate(exe_path, syms, case_names):
    import hashlib
    sha = hashlib.sha256(open(exe_path, 'rb').read()).hexdigest()
    entries = sorted(a for n, a in syms.items() if n.startswith('case_') or n.startswith('helper_'))
    cfg = os.path.join(BUILD, 'selftest_cfg.py')
    with open(cfg, 'w') as fh:
        fh.write('NAME = "AntiStatic self-test"\n')
        fh.write('MODULES = [dict(file="selftest.exe", sha256="%s")]\n' % sha)
        fh.write('EXTRA_ENTRIES = [%s]\n' % ', '.join('0x%x' % a for a in entries))
        fh.write('NOT_ENTRIES = []\nJUMP_TABLES = {}\nNORETURN_IMPORTS = []\n')
    gen = os.path.join(BUILD, 'gen')
    os.makedirs(gen, exist_ok=True)
    py = os.environ.get('PYTHON', sys.executable)
    log = sh(py, os.path.join(ROOT, 'translator', 'recomp.py'), BUILD, cfg, gen)
    open(os.path.join(BUILD, 'translate.log'), 'w').write(log)
    warns = open(os.path.join(gen, 'warnings.txt')).read().strip()
    if warns:
        print('translator warnings:\n' + warns)
    return gen


def build(gen):
    srcs = sorted(os.path.join(gen, f) for f in os.listdir(gen) if f.endswith('.c'))
    exe = os.path.join(BUILD, 'harness')
    sh('gcc', '-std=gnu11', '-O2', '-g', '-w', '-fno-strict-aliasing', '-frounding-math',
       '-DCPU_FAST_MEM', '-I' + gen, '-I' + os.path.join(ROOT, 'runtime', 'cpu'), '-I' + HERE,
       '-o', exe, os.path.join(HERE, 'harness.c'), os.path.join(ROOT, 'runtime', 'cpu', 'fpu.c'),
       *srcs, '-lm')
    return exe


def inputs(rng):
    regs = [rng.getrandbits(32) for _ in range(8)]
    regs[4] = 0                                     # esp: set by each side
    regs[5] = rng.choice([regs[5], STACK + 0x8000])  # ebp: sometimes a usable frame
    s = bytearray(rng.getrandbits(8) for _ in range(SCRATCH_SIZE))
    for k in range(32):
        struct.pack_into('<d', s, 0x100 + 8 * k, rng.uniform(-1000, 1000))
        struct.pack_into('<f', s, 0x200 + 4 * k, rng.uniform(-1000, 1000))
    for k in range(RESULTS, SCRATCH_SIZE):
        s[k] = 0
    return regs, bytes(s)


def pcrecomp_cases():
    """{name: (fixed registers, scratch, flag mask)} from pcrecomp_cases.json"""
    meta = json.load(open(os.path.join(HERE, 'pcrecomp_cases.json')))
    out = {}
    for name, m in meta['cases'].items():
        regs = [m['regs'].get(r, 0) for r in REGS]
        s = bytearray(SCRATCH_SIZE)
        for off, data in m['mem'].items():
            b = bytes.fromhex(data)
            s[int(off, 16):int(off, 16) + len(b)] = b
        out[name] = (regs, bytes(s), m['mask'], m.get('emulator_gap'))
    return out


def run_unicorn(image, fn, regs, scratch):
    from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UcError
    from unicorn import x86_const as X
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map(BASE, (len(image) + 0xfff) & ~0xfff)
    uc.mem_write(BASE, image)
    uc.mem_map(SCRATCH, SCRATCH_SIZE)
    uc.mem_write(SCRATCH, scratch)
    uc.mem_map(STACK, STACK_SIZE)
    uc.reg_write(X.UC_X86_REG_CR0, (uc.reg_read(X.UC_X86_REG_CR0) & ~0x4) | 0x2)   # FPU, no EM
    uc.reg_write(X.UC_X86_REG_CR4, uc.reg_read(X.UC_X86_REG_CR4) | 0x600)           # SSE on
    uc.reg_write(X.UC_X86_REG_EFLAGS, 0x202)
    uc.reg_write(X.UC_X86_REG_FPCW, 0x37f)                                         # as FNINIT
    uc.reg_write(X.UC_X86_REG_MXCSR, 0x1f80)
    for name, v in zip(REGS, regs):
        uc.reg_write(getattr(X, 'UC_X86_REG_' + name.upper()), v)
    esp = STACK_TOP - 4
    uc.mem_write(esp, struct.pack('<I', SENTINEL))
    uc.reg_write(X.UC_X86_REG_ESP, esp)
    status = 0
    try:
        uc.emu_start(fn, SENTINEL, count=1000000)
    except UcError as e:
        if uc.reg_read(X.UC_X86_REG_EIP) != SENTINEL:
            status = 1
    out = [uc.reg_read(getattr(X, 'UC_X86_REG_' + n.upper())) for n in REGS]
    return out, bytes(uc.mem_read(SCRATCH, SCRATCH_SIZE)), bytes(uc.mem_read(STACK_TOP - 256, 256)), status


def compare(name, mask, a, b):
    """differences between the emulator's result a and the translation's b"""
    ra, sa, ka, sta = a
    rb, sb, kb, stb = b
    out = []
    if sta or stb:
        return ['status: emulator %d, translation %d' % (sta, stb)] if sta != stb or sta else []
    for n, x, y in zip(REGS, ra, rb):
        if x != y:
            out.append('%s: %08x, translation %08x' % (n, x, y))
    if ka != kb:
        k = next(i for i in range(256) if ka[i] != kb[i])
        out.append('stack byte -%d: %02x, translation %02x' % (256 - k, ka[k], kb[k]))
    for k in range(0, SCRATCH_SIZE, 4):
        x, y = sa[k:k + 4], sb[k:k + 4]
        if x == y:
            continue
        if k >= RESULTS and (k - RESULTS) % 4 == 0 and name_stores_flags(name, k - RESULTS):
            fx, fy = struct.unpack('<I', x)[0], struct.unpack('<I', y)[0]
            if (fx ^ fy) & mask == 0:
                continue
            out.append('flags at R+%d: %03x, translation %03x (compared %03x)' % (k - RESULTS, fx & 0xfff, fy & 0xfff, mask))
            continue
        out.append('scratch +%03x: %s, translation %s' % (k, x.hex(), y.hex()))
    return out


FLAG_SLOTS = {}


def name_stores_flags(name, slot):
    return slot in FLAG_SLOTS.get(name, ())


def flag_slots():
    """case name -> the result slots its FL macro writes"""
    cur = None
    for line in open(os.path.join(HERE, 'cases.s')):
        m = re.match(r'(case_\w+):', line)
        if m:
            cur = m.group(1)
            FLAG_SLOTS[cur] = set()
        m = re.match(r'\s+FL\s+(\d+)', line)
        if m and cur:
            FLAG_SLOTS[cur].add(int(m.group(1)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--seeds', type=int, default=200)
    ap.add_argument('--only')
    ap.add_argument('--no-pcrecomp', action='store_true')
    ap.add_argument('--forms', action='append', default=[])
    ap.add_argument('--only-forms', action='store_true')
    args = ap.parse_args()
    text, syms = assemble(args.forms)
    exe_path = os.path.join(BUILD, 'selftest.exe')
    pe = make_pe(text, syms['case_add'])
    open(exe_path, 'wb').write(pe)
    image = bytes(pe[:0x400]) + bytes(TEXT_RVA - 0x400) + text      # as mapped
    image_path = os.path.join(BUILD, 'image.bin')
    open(image_path, 'wb').write(image)
    clist = cases(args.forms)
    pcases = {} if args.no_pcrecomp or args.only_forms else pcrecomp_cases()
    known = {n for n, m in clist} | set(pcases) | {n for n in syms if n.startswith('case_pc_')}
    # cases made by assembler macros: found by their symbols, all flags compared
    clist += [(n, ALL_FLAGS) for n in sorted((n for n in syms if n.startswith('case_') and n not in known),
                                              key=lambda n: syms[n])]
    clist += [(n, pcases[n][2]) for n in sorted(pcases, key=lambda n: syms[n])]
    if args.only_forms:
        clist = [(n, m) for n, m in clist if n.startswith('case_form_')]
    clist = [(n, m) for n, m in clist if not args.only or n == 'case_' + args.only or n == args.only]
    gen = translate(exe_path, syms, [n for n, m in clist])
    harness = build(gen)
    flag_slots()
    for n in list(pcases) + [n for n, m in clist if n.startswith('case_form_')]:
        FLAG_SLOTS[n] = {0}
    rng = random.Random(1)
    jobs = []
    for name, mask in clist:
        if name in pcases:
            jobs.append((name, mask, list(pcases[name][0]), pcases[name][1]))
            continue
        for _ in range(args.seeds):
            regs, scratch = inputs(rng)
            jobs.append((name, mask, regs, scratch))
    blob = b''.join(struct.pack('<9I', syms[n], *r) + s for n, m, r, s in jobs)
    p = subprocess.run([harness, image_path], input=blob, capture_output=True)
    rec = 8 * 4 + SCRATCH_SIZE + 256 + 4
    res = p.stdout
    if len(res) != rec * len(jobs):
        print('the harness stopped after %d of %d runs:\n%s' % (len(res) // rec, len(jobs), p.stderr.decode()[-2000:]))
    failed = {}
    for k, (name, mask, regs, scratch) in enumerate(jobs):
        if (k + 1) * rec > len(res):
            failed.setdefault(name, ['no result (harness crashed)'])
            continue
        r = res[k * rec:(k + 1) * rec]
        native = (list(struct.unpack_from('<8I', r)), r[32:32 + SCRATCH_SIZE],
                  r[32 + SCRATCH_SIZE:32 + SCRATCH_SIZE + 256], struct.unpack_from('<I', r, rec - 4)[0])
        emu = run_unicorn(image, syms[name], regs, scratch)
        d = compare(name, mask, emu, native)
        if d and name not in failed:
            failed[name] = ['inputs ' + ' '.join('%s=%08x' % (n, v) for n, v in zip(REGS, regs))] + d[:6]
    gaps = {n for n in failed if n in pcases and pcases[n][3]}
    for n in sorted(gaps):
        print('%-22s differs where the emulator is wrong: %s' % (n, pcases[n][3]))
        print('    ' + failed.pop(n)[-1])
    for n in sorted(n for n in pcases if pcases[n][3] and n not in gaps and any(n == x for x, m in clist)):
        print('%-22s now matches though marked as an emulator gap: check the note' % n)
    for name, mask in clist:
        if (name in pcases or name.startswith('case_form_')) and name not in failed:
            continue
        print('%-22s %s' % (name, 'FAIL' if name in failed else 'ok'))
        for l in failed.get(name, []):
            print('    ' + l)
    own = [n for n, m in clist if n not in pcases and not n.startswith('case_form_')]
    forms = [n for n, m in clist if n.startswith('case_form_')]
    if own:
        print('%d of %d cases matched on %d inputs each' % (len([n for n in own if n not in failed]), len(own), args.seeds))
    if forms:
        print('%d of %d game instruction forms matched on %d inputs each' % (
            len([n for n in forms if n not in failed]), len(forms), args.seeds))
    if pcases:
        pc = [n for n, m in clist if n in pcases]
        print('%d of %d pcrecomp cases matched (their fixed inputs)' % (len([n for n in pc if n not in failed]), len(pc)))
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()

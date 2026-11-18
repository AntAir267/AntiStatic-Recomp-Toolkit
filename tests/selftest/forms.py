"""Self-test cases from a game's own instructions (SwARc's idea: its lifter tests take every
distinct instruction form in the EXE). Reads a translation's instruction list (GEN/insns.txt,
GEN/table.c's modules) and the player's own module files, keeps one instruction of each distinct
form (mnemonic, prefixes, operand kinds, sizes and registers), and writes each as a self-test
case: memory operands pointed at the scratch page, x87 forms given a stack of three values first,
SSE and MMX forms their registers loaded from scratch and stored back after. run.py --forms FILE
runs them on random inputs in Unicorn and translated, like the hand-written cases.

    python3 tests/selftest/forms.py GAME_DIR GEN_DIR [--out FILE] [--max N]

The output (default build/selftest/forms_<module>.s) holds the game's own instruction bytes
re-assembled: it stays in build/ and is never committed or published.
Left out: 3DNow! and other instructions the games never run, the approximate reciprocals
(rcp*/rsqrt*: each CPU maker's answer differs, so the emulator is no reference), control flow, bt* on memory by a register (a random offset reaches anywhere), the stack pointer as a destination, string instructions with rep,
divisions (a random divisor faults), segment-relative memory, privileged and system
instructions, and the forms whose result is undefined on a real CPU (bsf/bsr on zero, 16-bit
double shifts by more than 16).
"""
import argparse, os, re, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'translator'))
from pe import PE       # noqa: E402
import x86              # noqa: E402

MEM = '[0x10000040]'    # scratch + 0x40: every rewritten memory operand
SKIP = {'jmp', 'call', 'ret', 'retn', 'retf', 'iret', 'iretd', 'int', 'int3', 'into', 'loop',
        'loope', 'loopne', 'jecxz', 'jcxz', 'leave', 'enter', 'hlt', 'cpuid', 'rdtsc', 'in', 'out',
        'div', 'idiv', 'popfd', 'popf', 'popal', 'popa', 'pushal', 'pusha', 'bound', 'lock',
        'les', 'lds', 'lss', 'lfs', 'lgs', 'cli', 'sti', 'insb', 'insw', 'insd', 'outsb', 'outsw', 'outsd', 'ud2', 'xlatb', 'fninit', 'finit', 'fldenv', 'fnstenv', 'frstor', 'fnsave',
        'fxsave', 'fxrstor', 'ldmxcsr', 'fldcw', 'aaa', 'aas', 'daa', 'das', 'aam', 'aad',
        'femms', 'wait', 'fwait', 'nop', 'emms', 'pause', 'sfence', 'lfence', 'mfence',
        'prefetchnta', 'prefetcht0', 'prefetcht1', 'prefetcht2', 'clflush', 'movntq',
        'maskmovq', 'maskmovdqu', 'fdecstp', 'fincstp', 'ffree', 'fnclex', 'fclex',
        'fsin', 'fcos', 'fsincos', 'fptan', 'fpatan', 'f2xm1', 'fyl2x', 'fyl2xp1', 'fprem',
        'fprem1', 'fscale', 'fxtract'}
# never run by the games (data read as code in unreached corners; the CPU the program sees has
# no 3DNow!), or answered differently by each CPU maker (the approximate reciprocals: the host
# CPU's answer is the translation's, the emulator computes another)
NEVER_RUN = {'arpl', 'int1', 'icebp', 'xbegin', 'xend', 'xabort', 'xtest', 'pi2fd', 'pi2fw',
             'pf2id', 'pf2iw', 'pswapd', 'pmulhrw', 'pavgusb'}
APPROXIMATE = {'rcpss', 'rcpps', 'rsqrtss', 'rsqrtps'}
STRING = re.compile(r'^(rep[a-z]* )?(movs|stos|lods|scas|cmps)[bwd]$')
# flags compared after each mnemonic (CF 1, PF 4, AF 0x10, ZF 0x40, SF 0x80, OF 0x800): the
# others are undefined after it on a real CPU
ALL = 0x8d5
MASKS = {**{m: ALL & ~0x10 for m in ('and', 'or', 'xor', 'test')},
         **{m: 0x1 | 0x4 | 0x40 | 0x80 for m in ('shl', 'sal', 'shr', 'sar', 'shld', 'shrd')},
         **{m: 0x1 | 0x4 | 0x40 | 0x80 | 0x10 for m in ('rol', 'ror', 'rcl', 'rcr')},
         **{m: 0x1 | 0x800 for m in ('mul', 'imul')},
         **{m: 0x1 | 0x40 for m in ('bt', 'bts', 'btr', 'btc')},
         'bsf': 0x40, 'bsr': 0x40}
X87_POPS = {'faddp': 1, 'fsubp': 1, 'fsubrp': 1, 'fmulp': 1, 'fdivp': 1, 'fdivrp': 1, 'fstp': 1,
            'fistp': 1, 'fisttp': 1, 'fbstp': 1, 'fcomp': 1, 'fucomp': 1, 'ficomp': 1,
            'fcomip': 1, 'fucomip': 1, 'fcompp': 2, 'fucompp': 2}
X87_PUSH = {'fld', 'fild', 'fbld', 'fld1', 'fldz', 'fldpi', 'fldl2e', 'fldl2t', 'fldlg2', 'fldln2'}


def modules(gen):
    src = open(os.path.join(gen, 'table.c')).read()
    body = src[src.index('module_table[] = {'):]
    body = body[:body.index('};')]
    out = [(m.group(2), int(m.group(3), 16)) for m in
           re.finditer(r'\{"([^"]*)", "([^"]*)", "[0-9a-f]{64}", 0x([0-9a-f]+)u', body)]
    # (the older layout of the ports from lixs-native: file, sha256, base)
    return out or [(m.group(1), int(m.group(2), 16)) for m in
                   re.finditer(r'\{"([^"]*)", "[0-9a-f]{64}", 0x([0-9a-f]+)u', body)]


def form_key(i):
    ops = []
    for o in i.ops:
        if o[0] == 'r':
            ops.append(('r', o[1]))
        elif o[0] == 'i':
            ops.append(('i', o[2]))
        else:
            ops.append(('m', o[1], o[2]))
    return (i.mn, i.prefixes, tuple(ops))


def why_not(i):
    pre, _, m = i.mn.rpartition(' ')
    if m in SKIP or STRING.match(i.mn) or (m.startswith('j') and m != 'jmp'):
        return 'kind'
    if m in NEVER_RUN or m.startswith('pf'):
        return 'never run'
    if m in APPROXIMATE:
        return 'approximate'
    if pre:
        return 'prefix'
    for o in i.ops:
        if o[0] == 'm' and o[2] not in (None, 'ds', 'ss'):
            return 'segment'
        if o[0] == 'r' and o[1] in ('cs', 'ds', 'es', 'ss', 'fs', 'gs'):
            return 'segment register'
    if i.ops and i.ops[0][0] == 'r' and i.ops[0][1] in ('esp', 'sp') and m not in ('cmp', 'test', 'push'):
        return 'writes esp'
    if m == 'xchg' and any(o[0] == 'r' and o[1] == 'esp' for o in i.ops):
        return 'writes esp'
    if m == 'pop' and i.ops and i.ops[0][0] == 'r' and i.ops[0][1] == 'esp':
        return 'writes esp'
    if m in ('shld', 'shrd') and ((i.ops[0][0] == 'm' and i.ops[0][1] == 2) or
                                  (i.ops[0][0] == 'r' and i.ops[0][1] in x86_reg16())):
        return 'undefined'      # (16-bit, by more than 16: the result is undefined)
    if m in ('bt', 'bts', 'btr', 'btc') and i.ops[0][0] == 'm' and i.ops[1][0] == 'r':
        return 'bit string'     # (a register bit offset reaches anywhere in memory)
    if m in ('bsf', 'bsr'):
        return 'undefined'      # (the destination when the source is 0: Intel and AMD differ)
    return None


def x86_reg16():
    return {'ax', 'bx', 'cx', 'dx', 'si', 'di', 'bp', 'sp'}


def asm_text(i):
    m = i.mn
    t = i.text[len(m):].strip() if i.text.startswith(m) else i.text.split(' ', 1)[1] if ' ' in i.text else ''
    if m != 'lea':                          # (lea reads no memory: its address is just arithmetic)
        t = re.sub(r'\[[^\]]*\]', MEM, t)
    t = t.replace('xword ptr', 'tbyte ptr')
    # (capstone names a scalar's memory operand xmmword; the assembler wants its real size)
    if 'xmmword ptr' in t:
        if re.search(r'sd$|^(u?comisd|cvtsd2s[is]|cvttsd2si)$', m) and m not in ('movsd',):
            t = t.replace('xmmword ptr', 'qword ptr')
        elif re.search(r'ss$|^(u?comiss|cvtss2s[di]|cvttss2si)$', m):
            t = t.replace('xmmword ptr', 'dword ptr')
    return ('%s %s' % (m, t)).strip()


def wrap(name, i, mask):
    m = i.mn
    xmm = any(o[0] == 'r' and o[1].startswith('xmm') for o in i.ops)
    mmx = any(o[0] == 'r' and re.match(r'^mm\d$', o[1]) for o in i.ops)
    x87 = m.startswith('f')
    out = ['# mask 0x%x' % mask, '%s:' % name,
           '        mov dword ptr [0x10000ffc], esp']    # (put back before ret: push/pop forms)
    if xmm:
        out += ['        movups xmm%d, [0x%x]' % (k, 0x10000400 + 16 * k) for k in range(8)]
    if mmx:
        out += ['        movq mm%d, [0x%x]' % (k, 0x10000400 + 8 * k) for k in range(8)]
    if x87:
        out += ['        fld qword ptr [0x%x]' % (0x10000100 + 8 * k) for k in range(3)]
    out.append('        ' + asm_text(i))
    out.append('        mov esp, dword ptr [0x10000ffc]')   # (mov: the flags kept for FL)
    out.append('        FL 0')
    if xmm:
        out += ['        movups [0x%x], xmm%d' % (0x10000f10 + 16 * k, k) for k in range(8)]
    if mmx:
        out += ['        movq [0x%x], mm%d' % (0x10000f90 + 8 * k, k) for k in range(8)]
        out.append('        emms')
    if x87:
        depth = 3 - X87_POPS.get(m, 0) + (1 if m in X87_PUSH else 0)
        out += ['        fstp qword ptr [0x%x]' % (0x10000fd0 + 8 * k) for k in range(max(depth, 0))]
    out.append('        ret')
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('game')
    ap.add_argument('gen')
    ap.add_argument('--out')
    ap.add_argument('--max', type=int, default=4000)
    args = ap.parse_args()
    mods = []
    for f, base in modules(args.gen):
        try:
            mods.append(PE(os.path.join(args.game, f), base))
        except (OSError, ValueError) as e:
            print('%s: %s' % (f, e))
    seen, skipped, cases = {}, {}, []
    for line in open(os.path.join(args.gen, 'insns.txt')):
        for a in (int(x, 16) for x in line.split()[1:]):
            m = next((m for m in mods if m.base <= a < m.base + m.size_of_image - 16), None)
            if not m:
                continue
            i = x86.decode(bytes(m.image[a - m.base:a - m.base + 16]), a)
            if i is None:
                continue
            k = form_key(i)
            if k in seen:
                continue
            seen[k] = a
            why = why_not(i)
            if why:
                skipped[why] = skipped.get(why, 0) + 1
                continue
            cases.append(i)
    cases = cases[:args.max]
    name0 = os.path.splitext(os.path.basename(mods[0].path))[0] if mods else 'game'
    slug = re.sub(r'[^a-z0-9]+', '_', name0.lower()).strip('_')
    out = args.out or os.path.join(ROOT, 'build', 'selftest', 'forms_%s.s' % slug)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    lines = ['# Generated by tests/selftest/forms.py from the player\'s own %s: not for publishing.' % name0,
             '# (assembled after cases.s, whose FL macro it uses)',
             '        .intel_syntax noprefix',
             '        .text']
    for n, i in enumerate(cases):
        base = i.mn.split()[-1]
        lines += wrap('case_form_%04d_%s' % (n, re.sub(r'[^a-z0-9]', '_', base)), i, MASKS.get(base, ALL))
        lines.append('        # %08x %s' % (i.addr, i.text))
    open(out, 'w').write('\n'.join(lines) + '\n')
    print('%d forms written to %s (%d distinct, %d left out: %s)' % (
        len(cases), out, len(seen), sum(skipped.values()),
        ', '.join('%s %d' % kv for kv in sorted(skipped.items()))))


if __name__ == '__main__':
    main()

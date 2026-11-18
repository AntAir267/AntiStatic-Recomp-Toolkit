"""Code discovery for a 32-bit MSVC Windows program.

Finds every function (entry point, call targets, code pointers held in data or pushed as
immediates, per-build extras), the instructions of each one, its switch jump tables and the
places where control passes to another function without a call (tail calls, fall-through into
the next function).
"""
import struct
from collections import defaultdict
import simd

TERMINATORS = {'ret', 'retf', 'iret', 'iretd', 'hlt', 'int3', 'ud2'}
COND_JUMPS = {'jo', 'jno', 'jb', 'jae', 'je', 'jne', 'jbe', 'ja', 'js', 'jns', 'jp', 'jnp',
              'jl', 'jge', 'jle', 'jg', 'jcxz', 'jecxz', 'loop', 'loope', 'loopne'}
# Never seen in real code of this kind of program: their presence means we decoded data.
WEIRD = {'in', 'out', 'insb', 'insw', 'insd', 'outsb', 'outsw', 'outsd', 'hlt', 'cli', 'sti',
         'iret', 'iretd', 'into', 'bound', 'arpl', 'les', 'lds', 'lss', 'lfs', 'lgs', 'lgdt',
         'lidt', 'sgdt', 'sidt', 'sldt', 'lldt', 'str', 'ltr', 'lar', 'lsl', 'clts', 'invd',
         'wbinvd', 'rdmsr', 'wrmsr', 'sysenter', 'sysexit', 'syscall', 'sysret', 'aaa', 'aas',
         'daa', 'das', 'aam', 'aad', 'salc', 'femms', 'retf', 'lcall', 'ljmp', 'int1', 'icebp',
         'ud2', 'verr', 'verw', 'smsw', 'lmsw', 'invlpg', 'rsm', 'cmpxchg8b', 'pi2fd', 'pi2fw',
         'ud0', 'ud1', 'fsetpm', 'fneni', 'fndisi'}

# The MMX the translator does (codegen.FuncGen.mmx): a small integer set (a colour blend)
MMX = {'movd', 'punpcklbw', 'pmullw', 'paddusw', 'psraw', 'packsswb'}


def _string_at(pe, a):
    """a NUL-terminated run of 4+ printable characters at a, ASCII or UTF-16"""
    b = pe.bytes(a, 64)
    pr = lambda x: 0x20 <= x < 0x7f or x in (9, 10, 13)
    n = 0
    while n < len(b) and pr(b[n]):
        n += 1
    if n >= 4 and n < len(b) and b[n] == 0:
        return True
    n = 0
    while 2 * n + 1 < len(b) and pr(b[2 * n]) and b[2 * n + 1] == 0:
        n += 1
    return n >= 4 and 2 * n + 1 < len(b) and b[2 * n] == 0 and b[2 * n + 1] == 0


def data_at(pe, dec, a, n=3):
    """Bytes at a that are data, not a function's start (code pointers found in data, exports):
    - one of the first n instructions reads across a relocated word: a relocation inside a real
      instruction lies wholly inside it, after its opcode (C++ FuncInfo records that `mov eax,
      offset FuncInfo` names, vtables, CRuntimeClass and message-map structures in MFC71's .text:
      ~520 of its "functions" [verified: work/upstream/REVIEW-pcrecomp-discovery.md, LA Rush]);
    - a NUL-terminated string (pcrecomp 6d1af42: `push offset "CLSID"`), unless it opens with two
      register pushes (62d538b: push ebx; push esi; push edi; push 0 reads "SVWj") or its first
      instructions hold a relocated address.
    Only in a module with relocations: without them neither test can tell."""
    sites = getattr(pe, 'reloc_sites', None)
    if not sites:
        return False
    m = pe.module_at(a)
    if not (m is not None and getattr(m, 'fixups', None)):
        # (a module without relocations has no way to tell: its absolute addresses end in a zero
        # byte, so `push ecx; push offset x` reads "Qh" + 3 printable bytes + NUL [verified:
        # Nitronic Rush's exe, 120 real functions lost to the string rule before this])
        return False
    b0, b1 = pe.u8(a), pe.u8(a + 1)
    if not (0x50 <= b0 <= 0x57 and 0x50 <= b1 <= 0x57) and _string_at(pe, a):
        # ... in a relocated module, code that uses an address has the relocation inside its
        # instruction: a string-looking start whose first instructions hold one is code (LA Rush's
        # `push offset dtor; call _atexit` static initializers)
        x, code = a, False
        for _ in range(2):
            i = dec.at(x)
            if i is None:
                break
            code = code or any(i.addr < r and r + 4 <= i.next for r in range(i.addr + 1, i.next) if r in sites)
            x = i.next
        if not code:
            return True
    x = a
    for _ in range(n):
        i = dec.at(x)
        if i is None:
            break
        for r in range(i.addr - 3, i.next):
            if r in sites and not (i.addr < r and r + 4 <= i.next):
                return True
        if i.mn in ('ret', 'jmp'):
            break
        x = i.next
    return False


def is_weird(i):
    m = i.mn
    if m == 'nop':
        return False                     # (multi-byte padding NOPs, `nop word ptr cs:[eax+eax]`: GCC/LLVM/ICC)
    if m in WEIRD:
        return True
    if m == 'int':
        return True
    if m.startswith('p') and m not in ('push', 'pop', 'pushal', 'popal', 'pushaw', 'popaw', 'pushfd',
                                       'popfd', 'pause') and m not in simd.MNEMONICS and m not in MMX:
        return True                      # other MMX / 3DNow!: never run (the CPU the program sees has none)
    for op in i.ops:
        if op[0] == 'r' and op[1] in ('cs', 'ds', 'es', 'ss', 'fs', 'gs', 'cr0', 'dr0'):
            if m == 'mov' and op[1] in ('ds', 'es'):
                continue                 # flat segments: reading ds, or loading es with it
            return True
        if op[0] == 'm' and op[2] not in (None, 'fs', 'ds', 'ss', 'es'):
            return True
    return False


class Func:
    def __init__(self, entry):
        self.entry = entry
        self.insns = {}           # addr -> Insn
        self.jtables = {}         # jmp addr -> [targets]
        self.jt_info = {}         # jmp addr -> (table, count, how)
        self.tails = {}           # insn addr -> function entry reached without a call
        self.calls = set()        # direct call targets
        self.code_imms = set()    # immediates pointing into .text
        self.errors = []          # (addr, message)
        self.has_ret = False
        self.indirect_jumps = set()   # jmp reg/mem that is not a jump table: a tail call
        self.import_tails = {}    # insn addr -> iat address (jmp [__imp_X])
        self.fall_tails = {}      # insn addr -> function entry its fall-through runs into
        self.jt_tails = {}        # jump-table jmp addr -> cases that are other functions' entries


class _Interior:
    """The addresses inside (not at the start of) the instructions of a set of functions, asked
    one at a time: a byte is inside one if an instruction starts up to 14 bytes before it and
    reaches past it (x86 instructions are at most 15 bytes); building the set of every such
    byte cost Stranglehold's discovery a fifth of its time. .at: every instruction by address."""
    __slots__ = ('at',)

    def __init__(self, funcs):
        self.at = {}
        for f in funcs.values():
            self.at.update(f.insns)

    def __contains__(self, q):
        at = self.at
        if q in at:
            return False
        for b in range(1, 15):
            i = at.get(q - b)
            if i is not None and i.size > b:
                return True
        return False


class _Asked:
    """The known-entry set as traverse sees it, remembering every address it asked about."""
    __slots__ = ('s', 'deps')

    def __init__(self, s):
        self.s, self.deps = s, {}

    def __contains__(self, a):
        r = self.deps[a] = a in self.s
        return r


class Discovery:
    def __init__(self, pe, dec, imports_by_iat, noreturn_imports=(), extra_entries=(),
                 jt_override=None, not_entries=(), forced_noreturn=()):
        self.pe, self.dec = pe, dec
        self.iat = imports_by_iat
        self.noreturn_imports = set(noreturn_imports)
        self.extra = set(extra_entries)
        self.jt_override = jt_override or {}
        self.not_entries = set(not_entries)
        self.forced_noreturn = set(forced_noreturn)   # functions known never to return
        self.noreturn = set(self.forced_noreturn)
        self.table_ends = set()          # first byte after each jump table / index table in code
        self.log = []
        self.table_ends = set()             # addresses right after a switch's jump table
        self._tcache = {}                   # entry -> (Func, known answers, noreturn answers)
        self._weird = {}                    # instruction address -> is_weird

    # ------------------------------------------------------------------ jump tables
    def _prev(self, body_ends, a):
        return body_ends.get(a)

    def jump_table(self, f, i, body_ends):
        """jmp dword ptr [idx*4 + table]: returns (table, count, targets, how) or None."""
        if i.addr in self.jt_override:
            table, count = self.jt_override[i.addr]
            return table, count, self._read_table(table, count), 'config'
        op = i.ops[0]
        if op[0] != 'm' or op[3] is not None or op[4] is None or op[5] != 4:
            return None
        idx, table = op[4], op[6]
        if not self.dec.in_text(table) and self.pe.section_at(table) is None:
            return None
        # walk back through straight-line code for the bound check
        a, steps, byte_tab = i.addr, 0, None
        want = idx
        negated = False                  # `neg reg` before the jump: entries before the table
        via = None                       # the conditional jump that led here, if the walk took one
        while steps < 12:
            p = self._prev(body_ends, a)
            if p is None or p.mn in ('jmp', 'ret', 'int3'):
                # this block is entered by a branch: continue from the conditional jump to it
                srcs = [x for x in f.insns.values() if x.mn in COND_JUMPS and x.ops and x.ops[0][0] == 'i'
                        and x.ops[0][1] == a]
                if len(srcs) != 1 or via is not None:
                    break
                via = srcs[0].mn
                p = srcs[0]
                steps += 1
                a = p.addr
                continue
            steps += 1
            a = p.addr
            m = p.mn
            if isinstance(want, tuple):
                # the index came from a variable in memory (unoptimised code: `cmp [v], n; ja;
                # mov reg, [v]`): the bound is the compare on that variable
                if m == 'cmp' and p.ops[0][0] == 'm' and _mem_key(p.ops[0]) == want and p.ops[1][0] == 'i':
                    n = p.ops[1][1] + 1
                    if byte_tab is not None:
                        count = max(self.pe.bytes(byte_tab, n)) + 1
                        self.table_ends.add(byte_tab + n)
                        self.table_ends.add(table + 4 * count)
                        return table, count, self._read_table(table, count), 'byte table %x[%d] (mem)' % (byte_tab, n)
                    self.table_ends.add(table + 4 * n)
                    return table, n, self._read_table(table, n), 'cmp mem'
                # a store to it (unoptimised code keeps the switch value in its frame): the value
                # came from the stored register; any other write ends the search
                if p.ops and p.ops[0][0] == 'm' and _mem_key(p.ops[0]) == want and m not in ('cmp', 'test'):
                    if m == 'mov' and p.ops[1][0] == 'r':
                        want = p.ops[1][1]
                        continue
                    break
                continue
            if byte_tab is None and m in ('mov', 'movzx') and len(p.ops) == 2 and p.ops[1][0] == 'm' \
                    and p.ops[1][1] == 1 and p.ops[0][0] == 'r' and _same_reg(p.ops[0][1], want):
                bop = p.ops[1]
                if bop[3] is not None and bop[4] is None:
                    byte_tab = bop[6]
                    want = bop[3]
                    continue
                if bop[3] is None and bop[4] is not None and bop[5] == 1:
                    byte_tab = bop[6]
                    want = bop[4]
                    continue
            if m == 'sub' and via == 'jb' and p.ops[0][0] == 'r' and _same_reg(p.ops[0][1], want) and \
                    p.ops[1][0] == 'i' and 0 < p.ops[1][1] <= 16:
                # `sub reg, k / jb` to the jump: index -k..-1, the entries just before the table
                k = p.ops[1][1]
                start = table - 4 * k
                return start, k, self._read_table(start, k), 'sub, below zero'
            if m == 'neg' and p.ops[0][0] == 'r' and _same_reg(p.ops[0][1], want):
                negated = not negated
                continue
            if m == 'cmp' and p.ops[1][0] == 'i' and (
                    (p.ops[0][0] == 'r' and _same_reg(p.ops[0][1], want)) or
                    (p.ops[0][0] == 'm' and _mem_key(p.ops[0]) == want)):
                # reached through `jb`/`jl` (below the value: one fewer case) or the usual `ja` to
                # the default (up to and including it)
                n = p.ops[1][1] + (0 if via in ('jb', 'jl') else 1)
                if negated:
                    # index -(n-1)..0: the entries end at the table address
                    start = table - 4 * (n - 1)
                    targets = [t for t in self._read_table(start, n) if self.dec.in_text(t)]
                    return start, n, targets, 'cmp, negated'
                if byte_tab is not None:
                    idxs = self.pe.bytes(byte_tab, n)
                    count = max(idxs) + 1
                    self.table_ends.add(byte_tab + n)
                    self.table_ends.add(table + 4 * count)
                    return table, count, self._read_table(table, count), 'byte table %x[%d]' % (byte_tab, n)
                self.table_ends.add(table + 4 * n)
                return table, n, self._read_table(table, n), 'cmp'
            # anything else writing the register we track ends the search
            if p.ops and p.ops[0][0] == 'r' and _same_reg(p.ops[0][1], want) and m not in ('cmp', 'test'):
                if m in ('mov', 'movzx', 'movsx') and p.ops[1][0] == 'r':
                    want = p.ops[1][1]
                    continue
                if m in ('mov', 'movzx', 'movsx') and p.ops[1][0] == 'm':
                    # loaded from memory (a local, or a global in unoptimised code): follow it to
                    # the compare on it, or to the register stored there
                    want = _mem_key(p.ops[1])
                    continue
                if m in ('add', 'sub', 'dec', 'inc', 'and', 'xor'):
                    if m in ('and',) and p.ops[1][0] == 'i':
                        n = p.ops[1][1] + 1
                        return table, n, self._read_table(table, n), 'and mask'
                    if m == 'xor' and byte_tab is not None:
                        continue
                    continue
                break
        # no bound found: read plausible entries
        targets, n = [], 0
        while n < 1024:
            v = self.pe.u32(table + 4 * n)
            if not self.dec.in_text(v):
                break
            n += 1
        if n == 0:
            return None
        self.log.append('jump table at %08x: no bound found, %d entries by scan' % (i.addr, n))
        return table, n, self._read_table(table, n), 'scan'

    def _read_table(self, table, n):
        return [self.pe.u32(table + 4 * k) for k in range(n)]

    # ------------------------------------------------------------------ one function
    def traverse(self, entry, known, first_error=False):
        """The function at entry, given the known entries. A body depends only on which addresses
        are known entries and which callees never return (and on the files): a cached body is
        used again while every one of those answers it asked for is still the same, so the result
        is the same as a fresh traversal (discovery redoes every body each round; Stranglehold's
        73,683 functions were traversed 636,238 times)."""
        c = self._tcache.get(entry)
        if c is not None:
            # (the answers kept as sets of yes and no, checked with set operations: a body that
            # asked about thousands of addresses was checked one address at a time in Python, a
            # billion checks for Nitronic Rush's D3DCompiler_43)
            f, kyes, kno, nyes, nno = c
            nr = self.noreturn
            if kyes <= known and kno.isdisjoint(known) and nyes <= nr and nno.isdisjoint(nr):
                return f
        rec = _Asked(known)
        self._ndeps = {}
        f = self._traverse(entry, rec, first_error)
        if first_error and f.errors:
            return f                    # (cut short at its first error: not kept)
        kd, nd = rec.deps, self._ndeps
        self._tcache[entry] = (f, frozenset(a for a, v in kd.items() if v), frozenset(a for a, v in kd.items() if not v),
                               frozenset(t for t, v in nd.items() if v), frozenset(t for t, v in nd.items() if not v))
        return f

    def _traverse(self, entry, known, first_error=False):
        dec = self.dec
        weird = self._weird
        f = Func(entry)
        body_ends = {}
        todo = [entry]
        while todo:
            if first_error and f.errors:
                break                   # (a candidate's check: one error decides, and only the first is kept)
            a = todo.pop()
            if a in f.insns:
                continue
            i = dec.at(a)
            if i is None:
                f.errors.append((a, 'cannot decode'))
                continue
            f.insns[a] = i
            body_ends[i.next] = i
            m = i.mn
            nxt = i.next
            succ = []
            w = weird.get(a)
            if w is None:
                w = weird[a] = is_weird(i)
            if w:
                f.errors.append((a, 'unexpected instruction ' + i.text))
            if m in ('ret', 'retn'):
                f.has_ret = True
            elif m in TERMINATORS:
                pass
            elif m == 'jmp':
                op = i.ops[0]
                if op[0] == 'i':
                    t = op[1]
                    if t != entry and t in known:
                        f.tails[a] = t
                    elif not dec.in_text(t):
                        f.errors.append((a, 'jump outside code %08x' % t))
                    else:
                        succ.append(t)
                elif op[0] == 'm' and op[3] is None and op[4] is None and op[6] in self.iat:
                    f.import_tails[a] = op[6]
                else:
                    jt = self.jump_table(f, i, body_ends) if op[0] == 'm' else None
                    if jt and all(t in known and t != entry for t in jt[2]):
                        # a table of functions (also called through elsewhere): a tail call
                        f.indirect_jumps.add(a)
                        jt = None
                    elif jt:
                        table, count, targets, how = jt
                        f.jtables[a] = targets
                        f.jt_info[a] = (table, count, how)
                        self.table_ends.add(table + 4 * count)
                        if how == 'scan' or how == 'and mask':
                            # entries an index can never select may be neighbouring data
                            targets = [t for t in targets if dec.in_text(t) and dec.at(t)]
                            f.jtables[a] = targets
                        for t in targets:
                            if not dec.in_text(t):
                                f.errors.append((a, 'jump table target outside code %08x' % t))
                            elif t in known and t != entry:
                                f.jt_tails.setdefault(a, set()).add(t)   # a case that is another function
                            else:
                                succ.append(t)
                    else:
                        f.indirect_jumps.add(a)
            elif m in COND_JUMPS:
                t = i.ops[0][1]
                if t != entry and t in known:
                    f.tails[a] = t
                elif not dec.in_text(t):
                    f.errors.append((a, 'jump outside code %08x' % t))
                else:
                    succ.append(t)
                succ.append(nxt)
            elif m == 'call':
                op = i.ops[0]
                returns = True
                if op[0] == 'i':
                    t = op[1]
                    if dec.in_text(t):
                        f.calls.add(t)
                        nr = self._ndeps[t] = t in self.noreturn
                        if nr:
                            returns = False
                    else:
                        f.errors.append((a, 'call outside code %08x' % t))
                elif op[0] == 'm' and op[3] is None and op[4] is None and op[6] in self.iat:
                    if self.iat[op[6]] in self.noreturn_imports:
                        returns = False
                if returns:
                    succ.append(nxt)
            else:
                succ.append(nxt)
            # code addresses used as immediates (callbacks, handlers; also MSVC's branch-free
            # selects like `neg; sbb; not; and reg, offset func`)
            if m not in ('call', 'jmp') and m not in COND_JUMPS:
                sites = getattr(self.pe, 'reloc_sites', None)
                if sites and hasattr(self.pe, 'module_at') and not self.pe.module_at(a).fixups:
                    sites = None            # (this module has no relocations: every immediate counts)
                for op in i.ops:
                    if op[0] == 'i' and op[2] == 4 and dec.in_text(op[1]):
                        # a relocated module: only an immediate the relocations mark is an address
                        if sites and not any(k in sites and self.pe.u32(k) == op[1]
                                             for k in range(a + 1, i.next - 3)):
                            continue
                        f.code_imms.add(op[1])
            for s in succ:
                if s != entry and s in known:
                    if s == nxt:
                        f.fall_tails[a] = s     # runs on into the next function
                    else:
                        f.tails[a] = s
                else:
                    todo.append(s)
        return f

    def acceptable_candidate(self, f):
        if f.errors or not f.insns:
            return False
        # no function starts on alignment padding: a stray data word pointing at the nops before
        # a function made the padding one and the real method behind it (a vtable's, 0x425550 in
        # LIXS) was then refused as inside it
        if self.pe.u8(f.entry) in (0x90, 0xcc):
            return False
        if data_at(self.pe, self.dec, f.entry):
            return False                    # (a relocated word or a string is data, relocated or not)
        if f.entry in getattr(self.pe, 'reloc_pointers', ()):
            return True                     # a relocated address: the DLL's own pointer to it
        return self.looks_like_start(f.entry)

    def looks_like_start(self, a):
        """A code pointer found in data must sit where a function can begin: 16-byte aligned
        (MSVC's function alignment) or right after padding or an instruction that does not
        fall through, or right after a jump table in the code. ASCII text in data often forms
        .text addresses; this rejects those."""
        if a % 16 == 0 or a in self.table_ends:
            return True
        # a frame-setting prologue (mov edi, edi / push ebp / mov ebp, esp) is a start whatever
        # precedes it: MSVC puts switch tables and literal data between functions
        head = self.pe.bytes(a, 5)
        if head == b'\x8b\xff\x55\x8b\xec' or head[:3] == b'\x55\x8b\xec':
            return True
        if a in getattr(self, 'table_ends', ()):
            return True
        b = self.pe.u8(a - 1)
        if b in (0xcc, 0x90):
            return True
        for back in range(1, 8):
            i = self.dec.at(a - back)
            if i is not None and i.next == a and i.mn in ('ret', 'jmp'):
                return True
        return False

    # ------------------------------------------------------------------ whole program
    def run(self, seeds, ptr_candidates):
        """seeds: trusted entries. ptr_candidates: code addresses found in data (validated)."""
        known = set(seeds) | self.extra
        known -= self.not_entries
        rejected = {}
        for rnd in range(20):
            rejected = {}                # looked at again each round: bodies change as entries settle
            funcs = {}
            work = sorted(known)
            pending_cands = set(ptr_candidates)
            while work:
                new = []
                for e in work:
                    if e in funcs:
                        continue
                    f = self.traverse(e, known)
                    funcs[e] = f
                    # an incremental-linking stub (a function that is one jmp): its target is a
                    # function of its own, so it keeps its own address (as in Ghidra) and the stub
                    # becomes a tail jump
                    i0 = self.dec.at(e)
                    if i0 is not None and i0.mn == 'jmp' and i0.ops[0][0] == 'i':
                        t = i0.ops[0][1]
                        if t not in known and t not in self.not_entries and self.dec.in_text(t):
                            known.add(t)
                            new.append(t)
                    for t in f.calls:
                        if t not in known and t not in self.not_entries:
                            known.add(t)
                            new.append(t)
                    pending_cands |= f.code_imms
                # validate pointer candidates once the trusted set settles
                if not new:
                    interior = self._interior(funcs)
                    occupied = set(interior.at)
                    for cand in sorted(pending_cands - known - set(rejected) - self.not_entries):
                        if cand in occupied and not self.after_terminator(cand):
                            # code pointers in data point at function starts, never into the
                            # middle of another function's code (decided before walking it: the
                            # walk was thrown away; Nitronic Rush's D3DCompiler_43 walked 59,000
                            # instructions for each of thousands)
                            rejected[cand] = ['inside the code of another function']
                            continue
                        g = self.traverse(cand, known, first_error=True)
                        if self.acceptable_candidate(g) and cand not in interior:
                            known.add(cand)
                            new.append(cand)
                            occupied.update(g.insns)
                        else:
                            rejected[cand] = g.errors[:1] or ['inside an instruction']
                    pending_cands = set()
                work = new
            # bodies depend on the full entry set: redo them all
            funcs = {e: self.traverse(e, known) for e in sorted(known)}
            # a candidate turned away as lying inside other code may have met code that the final
            # set no longer has (bytes decoded on the way to it, a body that changed): look again
            interior = self._interior(funcs)
            occupied = set(interior.at)
            again = []
            for cand, why in sorted(rejected.items()):
                if why[0] in ('inside an instruction', 'inside the code of another function') and \
                        cand not in occupied and cand not in interior:
                    g = self.traverse(cand, known, first_error=True)
                    if self.acceptable_candidate(g) and not (set(g.insns) & occupied):
                        again.append(cand)
            for cand in again:
                del rejected[cand]
                known.add(cand)
                self.log.append('code pointer %08x accepted on a second look' % cand)
            nr = self._noreturn(funcs)
            if nr == self.noreturn and not again:
                break
            self.noreturn = nr
        self.rejected = rejected
        return funcs

    def after_terminator(self, a):
        """True if the code before a cannot fall into it (a function another one jumps to)."""
        if self.pe.u8(a - 1) == 0xcc or a in self.table_ends:
            return True
        for back in range(1, 8):
            i = self.dec.at(a - back)
            if i is not None and i.next == a and i.mn in ('ret', 'jmp', 'int3'):
                return True
        return False

    def _interior(self, funcs):
        """Addresses inside (not at the start of) instructions of the known functions."""
        return _Interior(funcs)

    def _noreturn(self, funcs):
        returns = set()
        changed = True
        while changed:
            changed = False
            for e, f in funcs.items():
                if e in returns:
                    continue
                if f.has_ret or f.indirect_jumps or f.import_tails or f.errors or \
                        any(t in returns for t in f.tails.values()) or \
                        any(t in returns for t in f.fall_tails.values()):
                    returns.add(e)
                    changed = True
        return (set(funcs) - returns) | (self.forced_noreturn & set(funcs))


def _mem_key(op):
    """a memory operand as a comparable key (base, index, scale, displacement)"""
    return ('m', op[3], op[4], op[5], op[6])


def _same_reg(a, b):
    fam = {'eax': 'a', 'ax': 'a', 'al': 'a', 'ah': 'a', 'ebx': 'b', 'bx': 'b', 'bl': 'b', 'bh': 'b',
           'ecx': 'c', 'cx': 'c', 'cl': 'c', 'ch': 'c', 'edx': 'd', 'dx': 'd', 'dl': 'd', 'dh': 'd',
           'esi': 'si', 'si': 'si', 'edi': 'di', 'di': 'di', 'ebp': 'bp', 'bp': 'bp', 'esp': 'sp'}
    if isinstance(a, tuple) or isinstance(b, tuple):
        return False
    return fam.get(a, a) == fam.get(b, b)


def data_code_pointers(pe, dec):
    """Aligned dwords in the initialised data sections that point into .text."""
    out = set()
    for s in pe.sections:
        if s['name'] in ('.rdata', '.data'):
            lo = pe.base + s['va']
            n = min(s['vsize'], s['rsize'])
            for off in range(0, n - 3, 4):
                v = struct.unpack_from('<I', pe.image, s['va'] + off)[0]
                if dec.in_text(v):
                    out.add(v)
    return out

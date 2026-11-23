"""Exception handling tables of MSVC 6 code (C++ try/catch and SEH __try/__except), found in
the program image: where execution resumes after a handler, and the handler pieces the
runtime calls as functions.

C++ (FuncInfo, magic 0x19930520): each function with try blocks registers an __ehhandler stub
`mov eax, FuncInfo; jmp __CxxFrameHandler`. FuncInfo {magic, maxState, pUnwindMap, nTryBlocks,
pTryBlockMap, ...}; UnwindMap entries {toState, action} (action: a destructor funclet, called);
TryBlockMap entries {tryLow, tryHigh, catchHigh, nCatches, pHandlerArray}; HandlerType
{adjectives, pType, dispCatchObj, addressOfHandler} (the catch funclet, called; it returns the
address to resume at in `mov eax, imm32 ... ret`).

SEH (_except_handler3): the function's prologue pushes -1, its scope table and the handler;
scope table entries {enclosingLevel, filter, handler}: with a filter (called) the handler is the
__except block, entered by a call that never returns (a place to resume at); without one it is
a __finally block (called).

SEH4 (Visual C++ 2005/2008, _except_handler4): the prologue pushes -2, its scope table and the
handler (6A FE 68 <table> 68 <handler>), or calls __SEH_prolog4 after pushing the table (the helper
is known by its own first bytes: push <handler>; push fs:[0]). The table starts with four cookie
offsets (16 bytes); its entries are as SEH3's with -2 as the outermost level. The scope table is
what the prologue pushes (the handler stores it XORed with the security cookie, which the
translation never reads), so places are keyed by it as SEH3's are.

scan() returns (entries, places, ...): addresses to translate as functions, and per
registration (the __ehhandler stub or the scope table, which the owning function's prologue
pushes) the addresses execution may go on at after its handlers (codegen gives the pushing
functions a resume point, runtime/eh.c jumps there)."""
import struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

FUNCINFO_MAGIC = 0x19930520
# Visual C++ 7 (0x19930521) and 2005/2008 (0x19930522) add fields at the end of FuncInfo; the
# part read here is the same (Nitronic Rush: VC 2008, __CxxFrameHandler3)
FUNCINFO_MAGICS = (0x19930520, 0x19930521, 0x19930522)


def _u32(img, va):
    try:
        return img.u32(va)
    except (IndexError, AttributeError):
        return None


def _continuations(img, md, start, limit=0x400):
    """The `mov eax, imm32` values a catch funclet returns (each `ret` after one), up to the
    padding after a `ret` (int3 or nop, as compilers put between functions): reading on into the
    next function took its `mov eax, imm; ... ret` for a continuation."""
    out = set()
    data = img.bytes(start, limit)
    last_imm = None
    for i in md.disasm(data, start):
        if i.mnemonic == 'mov' and i.op_str.startswith('eax, 0x') and i.size == 5:
            last_imm = int(i.op_str[5:], 16)
        elif i.mnemonic == 'ret':
            if last_imm is not None and img.in_text(last_imm):
                out.add(last_imm)
                last_imm = None
            else:
                break
            nb = img.bytes(i.address + i.size, 1)
            if not nb or nb[0] in (0xcc, 0x90):
                break
        elif i.mnemonic in ('int3',):
            break
    return out


def handler_addresses(img, name):
    """Every address that means MSVCRT's `name`: the export, the import slots that hold it and
    `jmp [slot]` thunks to it."""
    out = set()
    for dll in ('msvcrt.dll', 'msvcr70.dll', 'msvcr71.dll', 'msvcr80.dll', 'msvcr90.dll'):
        crt = img.by_name.get(dll)              # (a C runtime translated with the program)
        if crt and crt.export(name):
            out.add(crt.export(name))
    for m in img.modules:
        for dll, n, iat in m.imports:
            if n != name:
                continue
            out.add(iat)
            lo, hi = m.text_range()
            code = img.bytes(lo, hi - lo)
            pat = b'\xff\x25' + struct.pack('<I', iat)
            k = code.find(pat)
            while k >= 0:
                out.add(lo + k)
                k = code.find(pat, k + 1)
    return out


SEH4_TOP = 0xfffffffe


def _seh4_prolog_helper(img, a):
    """Is a an __SEH_prolog4 (/_GS): push <handler>; push dword fs:[0]"""
    b = img.bytes(a, 12) if img.in_text(a) else b''
    return len(b) == 12 and b[0] == 0x68 and b[5:12] == b'\x64\xff\x35\x00\x00\x00\x00'


def _seh4_table_ok(img, st):
    """A plausible SEH4 scope table: its outermost entry's level is -2 and its handler is code."""
    enc, hnd = _u32(img, st + 16), _u32(img, st + 24)
    return enc == SEH4_TOP and hnd is not None and img.in_text(hnd)


def scan(img, cxx_handlers=None, seh_handlers=None, seh_prologs=()):
    """cxx_handlers / seh_handlers: addresses that mean __CxxFrameHandler / _except_handler3
    (the functions, the import slots holding them and jump thunks to them); seh_prologs: MSVC 7's
    __SEH_prolog helpers (SEH3 frames set up by a call)."""
    if cxx_handlers is None:
        cxx_handlers = handler_addresses(img, '__CxxFrameHandler')
    if seh_handlers is None:
        seh_handlers = handler_addresses(img, '_except_handler3')
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    entries, places = set(), {}
    funcinfos, scopetables, scopetables4 = {}, set(), set()     # FuncInfo -> its stubs
    prolog4 = {}                                # helper address -> is __SEH_prolog4
    for m in img.modules:
        lo, hi = m.text_range()
        code = img.bytes(lo, hi - lo)
        # __ehhandler: B8 <FuncInfo> then E9 rel32 / FF 25 [slot]
        k = code.find(b'\xb8')
        while k >= 0 and k + 10 <= len(code):
            fi = struct.unpack_from('<I', code, k + 1)[0]
            tgt = None
            if code[k + 5] == 0xe9:
                tgt = (lo + k + 10 + struct.unpack_from('<i', code, k + 6)[0]) & 0xffffffff
            elif code[k + 5] == 0xff and code[k + 6] == 0x25:
                tgt = struct.unpack_from('<I', code, k + 7)[0]
            if tgt in cxx_handlers and _u32(img, fi) in FUNCINFO_MAGICS:
                funcinfos.setdefault(fi, set()).add(lo + k)
            k = code.find(b'\xb8', k + 1)
        # SEH3 prologue: 6A FF 68 <scopetable> 68 <handler> 64 A1 00 00 00 00
        k = code.find(b'\x6a\xff\x68')
        while k >= 0 and k + 18 <= len(code):
            if code[k + 7] == 0x68 and code[k + 12:k + 18] == b'\x64\xa1\x00\x00\x00\x00':
                st, h = struct.unpack_from('<I', code, k + 3)[0], struct.unpack_from('<I', code, k + 8)[0]
                if h in seh_handlers:
                    scopetables.add(st)
            k = code.find(b'\x6a\xff\x68', k + 1)
        # SEH4: 6A FE 68 <scopetable> 68 <handler> 64 A1 00 00 00 00 (or 64 8B 0D / 64 FF 35: other
        # registers), the table checked for SEH4's shape
        k = code.find(b'\x6a\xfe\x68')
        while k >= 0 and k + 15 <= len(code):
            if code[k + 7] == 0x68 and code[k + 12] == 0x64:
                st = struct.unpack_from('<I', code, k + 3)[0]
                if _seh4_table_ok(img, st):
                    scopetables4.add(st)
            k = code.find(b'\x6a\xfe\x68', k + 1)
        # SEH4 through __SEH_prolog4: push <locals size>; push <scopetable>; call <helper>
        k = code.find(b'\x68')
        while k >= 0 and k + 15 <= len(code):
            if code[k + 5] == 0xe8:
                tgt = (lo + k + 10 + struct.unpack_from('<i', code, k + 6)[0]) & 0xffffffff
                if tgt not in prolog4:
                    prolog4[tgt] = _seh4_prolog_helper(img, tgt)
                st = struct.unpack_from('<I', code, k + 1)[0]
                if prolog4[tgt] and _seh4_table_ok(img, st):
                    scopetables4.add(st)
            k = code.find(b'\x68', k + 1)
        # SEH3 through a helper (MSVC 7's __SEH_prolog, build config SEH_PROLOGS): push <locals size>;
        # push <scopetable>; call <helper> (from penguins-native: 78 functions in Penguins!, the
        # CRT's _threadstartex among them)
        if seh_prologs:
            k = code.find(b'\x68')
            while k >= 0 and k + 10 <= len(code):
                if code[k + 5] == 0xe8:
                    tgt = (lo + k + 10 + struct.unpack_from('<i', code, k + 6)[0]) & 0xffffffff
                    if tgt in seh_prologs:
                        scopetables.add(struct.unpack_from('<I', code, k + 1)[0])
                k = code.find(b'\x68', k + 1)
    for fi, stubs in sorted(funcinfos.items()):
        conts = set()
        maxstate, punwind, ntry, ptry = (_u32(img, fi + 4), _u32(img, fi + 8), _u32(img, fi + 12),
                                         _u32(img, fi + 16))
        for s in range(min(maxstate or 0, 1000)):
            act = _u32(img, punwind + 8 * s + 4)
            if act and img.in_text(act):
                entries.add(act)
        for t in range(min(ntry or 0, 1000)):
            tb = ptry + 20 * t
            ncatch, parr = _u32(img, tb + 12), _u32(img, tb + 16)
            for h in range(min(ncatch or 0, 100)):
                fn = _u32(img, parr + 16 * h + 12)
                if fn and img.in_text(fn):
                    entries.add(fn)
                    conts |= _continuations(img, md, fn)
        if conts:
            for st in stubs:
                places.setdefault(st, set()).update(conts)
    for st in sorted(scopetables):
        for e in range(64):
            enc, flt, hnd = _u32(img, st + 12 * e), _u32(img, st + 12 * e + 4), _u32(img, st + 12 * e + 8)
            if enc is None or not (enc == 0xffffffff or enc < e) or not hnd or not img.in_text(hnd):
                break
            if flt:
                if not img.in_text(flt):
                    break
                entries.add(flt)
                entries.add(hnd)        # (the __except block: see SEH4's below)
                places.setdefault(st, set()).add(hnd)
            else:
                entries.add(hnd)
    for st in sorted(scopetables4):
        for e in range(64):
            base = st + 16 + 12 * e
            enc, flt, hnd = _u32(img, base), _u32(img, base + 4), _u32(img, base + 8)
            if enc is None or not (enc == SEH4_TOP or enc < e) or not hnd or not img.in_text(hnd):
                break
            if flt:
                if not img.in_text(flt):
                    break
                entries.add(flt)
                # the __except block: a place, and code of its own to translate (normal flow jumps
                # over it; a program without relocations gave no other way to find it: Nitronic
                # Rush's 5 "not translated")
                entries.add(hnd)
                places.setdefault(st, set()).add(hnd)
            else:
                entries.add(hnd)
    return entries, places, len(funcinfos), len(scopetables) + len(scopetables4)

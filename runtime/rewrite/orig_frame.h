/* THE ORIGINAL'S FRAME, for rewrites that keep it (from one port's rewrite, the form its tidy of
   2026-11-13 left after eight per-batch copies were merged). E is the entry
   stack pointer (a function using these keeps `uint32_t E = c->esp;`, at its return address); every
   call is made with esp where the original's is at that call and with the original's return
   address, the callee-saved registers the original pushes are written to their slots, and a
   function with an exception record builds it and points fs:[0] at it while its body runs. The
   stack below the entry then holds what the original's did when later code reads it as leftovers
   or a check compares it (docs/REWRITE.md "Stack leftovers", "Calls at the original's depth").
   rewrite.h's game_call_at is the seed of this; rewrite.h also has EARG, Saved / save_regs /
   restore_regs, x87() and fcom_ax.

   Two parts need the port's names, and are left out without them: the MSVC 6 scalar deleting
   destructors need OP_DELETE (the game's operator delete, cdecl), the inlined MSVCP60 std::string
   pieces IAT_STRING_TIDY, IAT_STRING_GROW, IAT_STRING_EOS, IAT_STRING_ASSIGN_CSTR,
   IAT_STRING_COPYN, IAT_STRING_ERASE, IAT_STD_XLEN (import slots), IAT_STRING_NPOS and
   IAT_NULL_CSTR (data imports) and OP_DELETE: #define them before including this header. */
#pragma once
#include "rewrite.h"

/* CALLS. Three ways to say where the original's esp is, kept because the files were written from
   listings annotated one way or another; all end in game_call_at with the original's return address.
   The method forms set ecx; their arguments are evaluated when the call is made, after esp and ecx
   are set, so an argument that itself calls something is computed first (REWRITE.md "Argument evaluation order").

   1. From the entry, at the call instruction: OCALL(at, ...) calls with esp = E - at once the n
      arguments are pushed (as an annotated listing gives it: "E-<at>"). OCALLM sets ecx, then esp. */
#define OCALL(at, ret, fn, n, ...) \
    (c->esp = E - (at) + 4u * (uint32_t)(n), game_call_at(c, ret, fn, n, ##__VA_ARGS__))
#define OCALLM(at, ret, fn, obj, n, ...) (c->ecx = (obj), OCALL(at, ret, fn, n, ##__VA_ARGS__))
/* the same for a float result on the x87 stack (popped) */
#define OCALL_ST0(...) ({ OCALL(__VA_ARGS__); f80 _v = ST(0); FPOP(); _v; })
#define OCALLM_ST0(at, ret, fn, obj, n, ...) ({ OCALLM(at, ret, fn, obj, n, ##__VA_ARGS__); f80 _v = ST(0); FPOP(); _v; })
/* a call taking a std::string by value (the callee destroys it) whose four dwords the original
   built at E - at (its esp at the call) */
#define OCALLM_STR(at, ret, fn, obj, S) \
    OCALLM(at, ret, fn, obj, 4, R32((S)), R32((S) + 4), R32((S) + 8), R32((S) + 12))

/* 2. As a stack pointer, before the pushes: AT(sp, ...) calls with the arguments pushed below sp.
      ATM sets esp, then ecx. */
#define AT(sp, ret, fn, n, ...) (c->esp = (sp), game_call_at(c, (ret), (fn), (n), ##__VA_ARGS__))
#define ATM(sp, ret, fn, obj, n, ...) (c->esp = (sp), c->ecx = (obj), game_call_at(c, (ret), (fn), (n), ##__VA_ARGS__))
#define AT_ST0(...) ({ AT(__VA_ARGS__); f80 _v = ST(0); FPOP(); _v; })
#define ATM_ST0(...) ({ ATM(__VA_ARGS__); f80 _v = ST(0); FPOP(); _v; })
/* a virtual call through obj's vtable slot */
#define ATV(sp, ret, obj, slot, nargs, ...) ({ uint32_t v_ = (obj); ATM(sp, ret, R32(R32(v_) + (slot)), v_, nargs, ##__VA_ARGS__); })

/* 3. As a stack pointer, at the call instruction: ATP(at, ...) calls with esp = at once the n
      arguments are pushed (ATP(at, ...) is AT(at + 4n, ...)). */
#define ATP(at, ret, fn, n, ...) (c->esp = (at) + 4u * (n), game_call_at(c, ret, fn, n, ##__VA_ARGS__))
#define ATPM(at, ret, fn, obj, n, ...) \
    (c->esp = (at) + 4u * (n), c->ecx = (obj), game_call_at(c, ret, fn, n, ##__VA_ARGS__))
/* an import called through its table slot */
#define IMP(iat) R32(iat)
/* a COM / virtual method's address: the object's vtable entry at off */
static inline uint32_t vmethod(uint32_t obj, uint32_t off) { return R32(R32(obj) + off); }
/* a tail jump to fn with the caller's return address still at E (`jmp [reg + X]'): the callee
   returns straight to the caller; npop as the target's ret */
static inline int tail_call(CPU *c, uint32_t E, uint32_t fn, int npop)
{
    c->esp = E + 4;
    game_call_at(c, R32(E), fn, 0);
    return decomp_ret(c, E, c->eax, npop);
}

/* THE EXCEPTION RECORD. The EH prologue's (push -1; push handler; push fs:[0]; mov fs:[0], esp) at
   E-0xc */
static inline void eh_open(CPU *c, uint32_t E, uint32_t handler)
{
    W32(E - 4, 0xffffffffu);
    W32(E - 8, handler);
    W32(E - 0xc, R32(c->fs_base));
    W32(c->fs_base, E - 0xc);
}
/* the same where the prologue reads the link through eax (`push -1; push handler; mov eax, fs:[0];
   push eax; mov fs:[0], esp'): eax keeps the previous link. A distinct helper because the destructors
   it is used in return or pass on that eax. */
static inline void eh_enter(CPU *c, uint32_t E, uint32_t handler)
{
    c->eax = R32(c->fs_base);
    W32(E - 4, 0xffffffffu);
    W32(E - 8, handler);
    W32(E - 0xc, c->eax);
    W32(c->fs_base, E - 0xc);
}
/* the epilogue: fs:[0] back; ecx holds the previous record as the original leaves it (`mov ecx,
   [esp+N]; mov fs:[0], ecx') */
static inline void eh_leave(CPU *c, uint32_t E)
{
    c->ecx = R32(E - 0xc);
    W32(c->fs_base, c->ecx);
}
/* the same with the last state stored first */
static inline void eh_close(CPU *c, uint32_t E, uint32_t state)
{
    W32(E - 4, state);
    eh_leave(c, E);
}
/* an unwind state stored while the body runs (the record's E-4) */
#define EH_STATE(n) W32(E - 4, (uint32_t)(n))
#define EH_STATE8(n) W8(E - 4, (uint8_t)(n))    /* where the original stores a byte */

/* THE CALLEE-SAVED REGISTERS. The caller's are kept and put back with rewrite.h's Saved / save_regs /
   restore_regs; regs_set gives them the values the original holds at a call (they show in the
   callee's pushes) */
static inline void regs_set(CPU *c, uint32_t ebx, uint32_t ebp, uint32_t esi, uint32_t edi)
{
    c->ebx = ebx; c->ebp = ebp; c->esi = esi; c->edi = edi;
}

#if defined(OP_DELETE)
/* MSVC 6'S SCALAR DELETING DESTRUCTORS (__thiscall, ret 4). The plain form every class has: `push
   esi; mov esi, ecx; call dtor; test [esp+8], 1; je; push esi; call delete; add esp, 4; mov eax,
   esi; pop esi; ret 4', at address `at' (the calls return to at+8 and at+0x15); the flag read at
   entry */
static inline int scalar_delete(CPU *c, uint32_t at, uint32_t dtor)
{
    uint32_t E = c->esp, self = c->ecx, flags = ARG(0), esi = c->esi;
    W32(E - 4, esi);
    c->esi = self;
    ATM(E - 4, at + 8, dtor, self, 0);
    if (flags & 1)
        AT(E - 4, at + 0x15, OP_DELETE, 1, self);
    c->esi = esi;
    return decomp_ret(c, E, self, 1);
}
/* the general form: `push esi; mov esi, ecx; call dtor; test [esp+8], 1; [lea eax, [esi-adj];] je;
   push esi(-adj); call operator delete' with the destructor's and the delete's return addresses;
   returns the object (this - adj). Kept beside the plain form because it reads the flag after the
   destructor ran, from the argument slot, where the plain one reads it at entry. */
static inline int scalar_delete_adj(CPU *c, uint32_t dtor, uint32_t ret_dtor, uint32_t ret_delete, uint32_t adj)
{
    uint32_t E = c->esp, self = c->ecx, esi = c->esi;
    W32(E - 4, esi);
    c->esi = self;
    OCALLM(4, ret_dtor, dtor, self, 0);
    uint32_t obj = self - adj;
    if (adj)
        c->eax = obj;
    if (EARG(0) & 1) {
        OCALL(8, ret_delete, OP_DELETE, 1, obj);
        if (adj)
            c->eax = obj;
    }
    c->esi = esi;
    return decomp_ret(c, E, obj, 1);
}
#endif

/* X87 PIECES. A compare's status word (fcom a, b; fnstsw ax): C0 0x100 below, C3 0x4000 equal, all
   of C0, C2, C3 unordered; eax keeps its upper half */
static inline uint32_t fsw_cmp(CPU *c, f80 a, f80 b)
{
    fpu_com(c, a, b);
    return fpu_sw(c);
}
static inline f80 f32at(uint32_t a) { return (f80)RF32(a); }
/* fcom's condition bits for a against b as `test ah, mask' sees them: C0 (0x01) below, C3 (0x40)
   equal, all three with C2 (0x45) unordered */
static inline unsigned of_fcmp(f80 a, f80 b)
{
    if (a != a || b != b) return 0x45;
    return a < b ? 0x01 : a == b ? 0x40 : 0;
}
/* fcom / fcomp then fnstsw ax: the condition bits (as of_fcmp's) and ax the status word, as the
   original leaves eax (rewrite.h's fcom_ax also takes the x87 stack's depth at the fnstsw) */
static inline unsigned of_fcom(CPU *c, f80 a, f80 b)
{
    fpu_com(c, a, b);
    c->eax = (c->eax & 0xffff0000u) | fpu_sw(c);
    return (c->fsw >> 8) & 0x45u;
}
static inline uint32_t fbits(f80 v)
{
    float f = (float)x87(v);              /* rounded on the x87, then stored */
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* dwords copied as the original's rep movsd / mov pairs do (floats as integers: a signalling NaN
   stays signalling) */
static inline void dwords_copy(uint32_t d, uint32_t s, uint32_t ndw)
{
    for (uint32_t i = 0; i < 4 * ndw; i += 4)
        W32(d + i, R32(s + i));
}
/* MSVC 6's vector size: (last - first) >> shift, 0 when there is no buffer */
static inline uint32_t vector_count(uint32_t first, uint32_t last, int shift)
{
    return first ? (uint32_t)((int32_t)(last - first) >> shift) : 0;
}

/* MSVCP60'S std::string as it lays it out: the allocator (a byte), char *ptr (the byte at ptr - 1 a
   reference count: 0 one owner, 0xfe / 0xff frozen), the length, the capacity */
#ifndef STRF_ALLOC
#define STRF_ALLOC 0x0
#define STRF_PTR 0x4
#define STRF_LEN 0x8
#define STRF_RES 0xc
#endif
#if defined(IAT_STRING_TIDY) && defined(OP_DELETE)
/* INLINED std::string PIECES. */
static inline uint32_t ostr_npos(void) { return R32(R32(IAT_STRING_NPOS)); }
static inline uint32_t ostr_cstr(uint32_t p) { return p ? p : R32(IAT_NULL_CSTR); }
static inline uint32_t cstr_len(uint32_t s)
{
    uint32_t n = 0;
    while (R8(s + n)) n++;
    return n;
}

/* the inlined std::string destructor's release of buffer p (its reference count the byte before:
   0 or 0xff frees the block, else the count goes down), with operator delete called at sp (esp
   before its push). eax's low byte and ecx as the original's `lea ecx, [p-1]; mov al, [ecx]' form
   leaves them; the pointer is read through eax (`mov eax, [s+4]; test eax, eax'), so a null one
   leaves eax 0 [verified: one port's listings and the register audit].
   The fields are left as they are: the callers clear them (str_clear). One body; the forms after
   it only say the delete's esp another way or take the string instead of its buffer. */
static inline void ostr_release(CPU *c, uint32_t sp, uint32_t ret, uint32_t p)
{
    if (!p) {
        c->eax = 0;
        return;
    }
    uint8_t n = R8(p - 1);
    c->eax = (p & 0xffffff00u) | n;
    c->ecx = p - 1;
    if (n == 0 || n == 0xff)
        AT(sp, ret, OP_DELETE, 1, p - 1);
    else {
        W8(p - 1, (uint8_t)(n - 1));
        c->eax = (c->eax & 0xffffff00u) | (uint8_t)(n - 1);
    }
}
/* the delete's esp given from the entry as OCALL's (E - at at the call instruction) */
static inline void ostr_release_e(CPU *c, uint32_t E, uint32_t at, uint32_t ret, uint32_t p)
{
    ostr_release(c, E - at + 4, ret, p);
}
/* the same for the string at s (+4 its buffer), its pointer, length and capacity cleared after */
static inline void ostr_release_clear_e(CPU *c, uint32_t E, uint32_t at, uint32_t ret, uint32_t s)
{
    ostr_release_e(c, E, at, ret, R32(s + 4));
    W32(s + 4, 0);
    W32(s + 8, 0);
    W32(s + 0xc, 0);
}
/* the string at s (buffer +4), the delete (`push ecx; call delete') at sp */
static inline void str_release_at(CPU *c, uint32_t s, uint32_t sp, uint32_t ret)
{
    ostr_release(c, sp, ret, R32(s + 4));
}
/* the string at s, the delete's esp given at the call instruction (as ATP's) */
static inline void str_release_atp(CPU *c, uint32_t s, uint32_t at, uint32_t ret)
{
    ostr_release(c, at + 4, ret, R32(s + 4));
}
/* the release from the entry with no register set (one port's menus: those functions' eax and ecx
   were settled without it, so it is not ostr_release_e). at is the esp at the delete call, ret its
   return address */
#define OSTR_RELEASE(p, at, ret) do {                                       \
        uint32_t _q = (p) - 1;                                             \
        uint8_t _n = R8(_q);                                               \
        if (_n == 0 || _n == 0xff) OCALL(at, ret, OP_DELETE, 1, _q);    \
        else W8(_q, (uint8_t)(_n - 1));                                    \
    } while (0)
/* the string's fields after the release: buffer, length, capacity 0 */
static inline void str_clear(uint32_t s)
{
    W32(s + 4, 0);
    W32(s + 8, 0);
    W32(s + 0xc, 0);
}
/* a std::vector's three pointers (begin, end, capacity at v+4..) cleared after its buffer is freed */
static inline void vec_clear(uint32_t v)
{
    W32(v + 4, 0);
    W32(v + 8, 0);
    W32(v + 0xc, 0);
}

/* THE INLINED string(const char *) at T, its calls made from the original's depth: base is the
   esp offset before their pushes (E - base), ret the return addresses of the calls made, in order.
   Two forms (MSVC 6 has five; REWRITE.md):
     ostr_grow: the allocator byte, then _Tidy(false) (OS_TIDY) or the fields cleared, slot (if
       any) given the length, _Grow(n, true), the copy, then _Eos(n) (OS_EOS) or the length and NUL
       written inline;
     ostr_assign: the allocator byte, _Tidy(false), assign(cstr, n).
   edi as the inlined strlen (repnz scasb) leaves it */
enum { OS_TIDY = 1, OS_EOS = 2 };
static inline void ostr_grow(CPU *c, uint32_t E, uint32_t base, uint32_t T, uint8_t alloc, uint32_t cstr,
                             uint32_t slot, int flags, const uint32_t *ret)
{
    uint32_t n = (uint32_t)strlen(gptr(cstr));
    W8(T + STRF_ALLOC, alloc);
    if (flags & OS_TIDY)
        OCALLM(base + 4, *ret++, R32(IAT_STRING_TIDY), T, 1, 0);
    else {
        W32(T + STRF_PTR, 0);
        W32(T + STRF_LEN, 0);
        W32(T + STRF_RES, 0);
    }
    c->edi = cstr + n + 1;
    if (slot)
        W32(slot, n);
    if (OCALLM(base + 8, *ret++, R32(IAT_STRING_GROW), T, 2, n, 1) & 0xff) {
        memmove(gptr(R32(T + STRF_PTR)), gptr(cstr), n);
        c->edi = R32(T + STRF_PTR) + n;
        c->esi = cstr + n;
        if (flags & OS_EOS)
            OCALLM(base + 4, *ret, R32(IAT_STRING_EOS), T, 1, n);
        else {
            W32(T + STRF_LEN, n);
            W8(R32(T + STRF_PTR) + n, 0);
        }
    }
}
static inline void ostr_assign(CPU *c, uint32_t E, uint32_t base, uint32_t T, uint8_t alloc, uint32_t cstr,
                               uint32_t r_tidy, uint32_t r_assign)
{
    W8(T + STRF_ALLOC, alloc);
    OCALLM(base + 4, r_tidy, R32(IAT_STRING_TIDY), T, 1, 0);
    uint32_t n = (uint32_t)strlen(gptr(cstr));
    c->edi = cstr + n + 1;
    OCALLM(base + 8, r_assign, R32(IAT_STRING_ASSIGN_CSTR), T, 2, cstr, n);
}
/* the third form, "fields cleared, then the inlined assign(cstr, strlen(cstr))": the allocator byte
   and fields, _Xlen past 0xfffffffd, then by the buffer's state (a shared buffer released and
   _Tidy(false) or _Copy(n); an empty name _Tidy(true); too small or too big a reserve _Tidy(true)
   and _Copy(n)), the copy, the length and NUL. ret: the return addresses of _Xlen, the shared
   _Tidy(false), its next call, the empty _Tidy(true), its next call, _Tidy(true), _Copy(n) and the
   next call after the copy; returns the next call's return address for the path taken. ebx gets n,
   ebp the string [verified: one port's menu methods, their listings] */
static inline uint32_t ostr_cleared(CPU *c, uint32_t E, uint32_t base, uint32_t T, uint8_t alloc,
                                    uint32_t cstr, const uint32_t *ret)
{
    uint32_t n = (uint32_t)strlen(gptr(cstr));
    c->ebp = T;
    W8(T + STRF_ALLOC, alloc);
    W32(T + STRF_PTR, 0);
    W32(T + STRF_LEN, 0);
    W32(T + STRF_RES, 0);
    c->edi = cstr + n + 1;
    c->ebx = n;
    if (n > 0xfffffffdu)
        OCALL(base, ret[0], R32(IAT_STD_XLEN), 0);
    uint32_t p = R32(T + STRF_PTR);
    uint8_t cnt = p ? R8(p - 1) : 0;
    if (p && cnt != 0 && cnt != 0xff) {
        if (n == 0) {
            W8(p - 1, (uint8_t)(cnt - 1));
            OCALLM(base + 4, ret[1], R32(IAT_STRING_TIDY), T, 1, 0);
            return ret[2];
        }
        OCALLM(base + 4, ret[6], R32(IAT_STRING_COPYN), T, 1, n);
    } else if (n == 0) {
        OCALLM(base + 4, ret[3], R32(IAT_STRING_TIDY), T, 1, 1);
        return ret[4];
    } else {
        uint32_t res = R32(T + STRF_RES);
        if (res > 0x1f || res < n) {
            OCALLM(base + 4, ret[5], R32(IAT_STRING_TIDY), T, 1, 1);
            OCALLM(base + 4, ret[6], R32(IAT_STRING_COPYN), T, 1, n);
        }
    }
    memmove(gptr(R32(T + STRF_PTR)), gptr(cstr), n);
    c->edi = R32(T + STRF_PTR) + n;
    c->esi = cstr + n;
    W32(T + STRF_LEN, n);
    W8(R32(T + STRF_PTR) + n, 0);
    return ret[7];
}
/* the inlined string::assign(const string &s, 0, npos) (one port's record copies):
   itself: erase(n, npos) then erase(0, 0); s's whole buffer when it may be shared (not frozen):
   _Tidy(true) and the buffer taken with its count raised; else _Grow(n, true), the copy and _Eos.
   Calls with their arguments pushed below base; ret[] erase, erase, _Tidy, _Grow, _Eos
   [verified: that port's listing] */
static inline void ostr_assign_string(CPU *c, uint32_t base, uint32_t d, uint32_t s, const uint32_t ret[5])
{
    uint32_t np = ostr_npos(), len = R32(s + STRF_LEN), n = np < len ? np : len;
    if (d == s) {
        ATPM(base - 8, ret[0], R32(IAT_STRING_ERASE), d, 2, n, np);
        ATPM(base - 8, ret[1], R32(IAT_STRING_ERASE), d, 2, 0, 0);
        return;
    }
    if (n && n == len && R8(ostr_cstr(R32(s + STRF_PTR)) - 1) < 0xfe) {
        ATPM(base - 4, ret[2], R32(IAT_STRING_TIDY), d, 1, 1);
        uint32_t p = ostr_cstr(R32(s + STRF_PTR));
        W32(d + STRF_PTR, p);
        W32(d + STRF_LEN, R32(s + STRF_LEN));
        W32(d + STRF_RES, R32(s + STRF_RES));
        W8(p - 1, (uint8_t)(R8(p - 1) + 1));
        return;
    }
    if (ATPM(base - 8, ret[3], R32(IAT_STRING_GROW), d, 2, n, 1) & 0xff) {
        memmove(gptr(R32(d + STRF_PTR)), gptr(ostr_cstr(R32(s + STRF_PTR))), n);
        ATPM(base - 4, ret[4], R32(IAT_STRING_EOS), d, 1, n);
    }
}
#endif

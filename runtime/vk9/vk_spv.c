/* Our own Vulkan renderer: the game's shaders compiled at run time. A game hands the device its
   shaders as bytecode (made from its own files or kept in its program), so nothing is compiled ahead of time and nothing of the game's is kept in
   the repository: the bytecode the device is handed (Direct3D 8's, as vk_d3d8.c gives it to our
   Direct3D 9: DXVK's TranslateVertexShader8) becomes SPIR-V here, once per bytecode hash.
   This is etm-native's tools/d3d2spv.py (its compiler: shader model 1-3 to SPIR-V as DXVK 3.1.1's
   dxbc-spirv converts them, so the pictures match DXVK's) written in C for what Direct3D 8 has: vs_1_0
   and vs_1_1, ps_1_0 to ps_1_3. The meaning of each instruction follows DXVK, as d3d2spv.py's notes
   record: "legacy" multiplies (0 x anything = 0) in mul, mad, the dot products and matrices, lrp; rsq
   and log of |x|; ps_1_x constants clamped to [-1, 1]; vs_1_x colour outputs clamped to [0, 1]; ps_1_x
   co-issued pairs read their sources before either writes; DXVK's multiply-add fusion (an add of a
   multiply's single-use result becomes an Fma); dot products as a chain of Fmas kept precise; the
   fog and the alpha test at the end of a pixel shader; float controls as DXVK asks (vk_d3d9.c
   module_get keeps what the device has). The interface is runtime/vk_abi.h's.
   Not here (logged; draws with such a shader are skipped): ps_1_4, texbeml, texreg2ar/gb, texm3x3 (the
   matrix alone; texm3x3pad, tex, spec and vspec are done), texm3x2depth, dst, relative addressing in
   pixel shaders, the shadow filter (a depth compare takes one sample). */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "vk_abi.h"
#include "runtime.h"

static double vk_spv_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* ------------------------------------------------------------------ SPIR-V words */
typedef struct { uint32_t *w; uint32_t n, cap; } W;
static void wpush(W *w, uint32_t v)
{
    if (w->n == w->cap) {
        w->cap = w->cap ? w->cap * 2 : 64;
        w->w = realloc(w->w, w->cap * 4);
    }
    w->w[w->n++] = v;
}
static void wcat(W *d, const W *s) { for (uint32_t i = 0; i < s->n; i++) wpush(d, s->w[i]); }
static void ins_a(W *out, uint32_t op, const uint32_t *a, uint32_t n)
{
    wpush(out, ((n + 1) << 16) | op);
    for (uint32_t i = 0; i < n; i++) wpush(out, a[i]);
}
#define INS(out, op, ...) do { const uint32_t _a[] = {__VA_ARGS__}; ins_a((out), (op), _a, sizeof _a / 4); } while (0)
#define INS0(out, op) ins_a((out), (op), NULL, 0)

enum {
    OpName = 5, OpExtension = 10, OpExtInstImport = 11, OpExtInst = 12, OpMemoryModel = 14, OpEntryPoint = 15,
    OpExecutionMode = 16, OpCapability = 17, OpTypeVoid = 19, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22,
    OpTypeVector = 23, OpTypeImage = 25, OpTypeSampler = 26, OpTypeSampledImage = 27, OpTypeArray = 28,
    OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypePointer = 32, OpTypeFunction = 33, OpConstantTrue = 41,
    OpConstantFalse = 42, OpConstant = 43, OpConstantComposite = 44, OpSpecConstant = 50, OpFunction = 54,
    OpFunctionEnd = 56, OpVariable = 59, OpLoad = 61, OpStore = 62, OpAccessChain = 65, OpDecorate = 71,
    OpMemberDecorate = 72, OpVectorShuffle = 79, OpCompositeConstruct = 80, OpCompositeExtract = 81,
    OpSampledImage = 86, OpImageSampleImplicitLod = 87, OpImageSampleDrefImplicitLod = 89, OpConvertFToS = 110,
    OpFNegate = 127, OpIAdd = 128, OpFAdd = 129, OpFSub = 131, OpFMul = 133, OpFDiv = 136, OpShiftRightLogical = 194,
    OpBitwiseAnd = 199, OpSelect = 169, OpIEqual = 170, OpINotEqual = 171, OpULessThan = 176, OpFOrdEqual = 180,
    OpFOrdNotEqual = 182, OpFOrdLessThan = 184, OpFOrdGreaterThan = 186, OpFOrdLessThanEqual = 188,
    OpFOrdGreaterThanEqual = 190, OpLogicalAnd = 167, OpPhi = 245, OpSelectionMerge = 247, OpLabel = 248, OpBranch = 249,
    OpBranchConditional = 250, OpReturn = 253, OpDemoteToHelperInvocation = 5380
};
enum { GLSL_RoundEven = 2, GLSL_FAbs = 4, GLSL_Floor = 8, GLSL_Fract = 10, GLSL_Exp2 = 29, GLSL_Log2 = 30,
       GLSL_InverseSqrt = 32, GLSL_FMin = 37, GLSL_FMax = 40, GLSL_FClamp = 43, GLSL_SClamp = 45, GLSL_Fma = 50 };
enum { DEC_SpecId = 1, DEC_Block = 2, DEC_ArrayStride = 6, DEC_BuiltIn = 11, DEC_NonWritable = 24, DEC_Location = 30,
       DEC_Binding = 33, DEC_DescriptorSet = 34, DEC_Offset = 35, DEC_NoContraction = 42, DEC_Centroid = 16 };
enum { SC_UniformConstant = 0, SC_Input = 1, SC_Uniform = 2, SC_Output = 3, SC_Function = 7, SC_PushConstant = 9 };
#define FC2_DEFAULT (0x8 | 0x10000 | 0x20000 | 0x40000 | 0x4)

/* register types */
enum { R_TEMP, R_INPUT, R_CONST, R_ADDR_TEX, R_RASTOUT, R_ATTROUT, R_TEXOUT, R_CONSTINT, R_COLOROUT, R_DEPTHOUT,
       R_SAMPLER, R_CONST2, R_CONST3, R_CONST4, R_CONSTBOOL, R_LOOP, R_TEMPF16, R_MISC, R_LABEL, R_PRED };
#define VARY_COLOR 0
#define VARY_TEX 2
#define VARY_FOG 10
#define NVARY 11
#define TGT_POS 100
#define TGT_PSIZE 101

/* ------------------------------------------------------------------ the module (d3d2spv.py Module) */
typedef struct { uint32_t tag, a, b, c, d, id; } Key;
typedef struct { uint32_t id, ty; int n; } Val;

typedef struct {
    uint32_t bound;
    W caps, exts, annot, glob, vars, code, iface;
    Key *keys;
    int nkeys, capkeys;
    uint32_t glsl;
    char err[160];
} Mod;

static uint32_t mnew(Mod *m) { return m->bound++; }
static uint32_t *key_find(Mod *m, uint32_t tag, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    for (int i = 0; i < m->nkeys; i++) {
        Key *k = &m->keys[i];
        if (k->tag == tag && k->a == a && k->b == b && k->c == c && k->d == d) return &k->id;
    }
    return NULL;
}
static void key_add(Mod *m, uint32_t tag, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t id)
{
    if (m->nkeys == m->capkeys) {
        m->capkeys = m->capkeys ? m->capkeys * 2 : 64;
        m->keys = realloc(m->keys, (size_t)m->capkeys * sizeof *m->keys);
    }
    m->keys[m->nkeys++] = (Key){tag, a, b, c, d, id};
}
/* a type or constant, made once: tag and up to four words of key; the instruction's operands */
static uint32_t once(Mod *m, uint32_t tag, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t op, int result_first,
                     uint32_t rtype, const uint32_t *ops, uint32_t nops)
{
    uint32_t *f = key_find(m, tag, a, b, c, d);
    if (f) return *f;
    uint32_t id = mnew(m);
    uint32_t buf[16];
    uint32_t n = 0;
    if (result_first) buf[n++] = id;
    else buf[n++] = rtype, buf[n++] = id;
    for (uint32_t i = 0; i < nops; i++) buf[n++] = ops[i];
    ins_a(&m->glob, op, buf, n);
    key_add(m, tag, a, b, c, d, id);
    return id;
}
enum { K_VOID = 1, K_BOOL, K_F32, K_I32, K_U32, K_VEC, K_PTR, K_FN, K_IMG, K_SMP, K_SIMG, K_RTA, K_ARR, K_CF, K_CU, K_CI,
       K_CB, K_CV };
static uint32_t t_void(Mod *m) { return once(m, K_VOID, 0, 0, 0, 0, OpTypeVoid, 1, 0, NULL, 0); }
static uint32_t t_bool(Mod *m) { return once(m, K_BOOL, 0, 0, 0, 0, OpTypeBool, 1, 0, NULL, 0); }
static uint32_t t_f32(Mod *m) { uint32_t o[] = {32}; return once(m, K_F32, 0, 0, 0, 0, OpTypeFloat, 1, 0, o, 1); }
static uint32_t t_i32(Mod *m) { uint32_t o[] = {32, 1}; return once(m, K_I32, 0, 0, 0, 0, OpTypeInt, 1, 0, o, 2); }
static uint32_t t_u32(Mod *m) { uint32_t o[] = {32, 0}; return once(m, K_U32, 0, 0, 0, 0, OpTypeInt, 1, 0, o, 2); }
static uint32_t t_vec(Mod *m, uint32_t base, int n)
{
    if (n == 1) return base;
    uint32_t o[] = {base, (uint32_t)n};
    return once(m, K_VEC, base, (uint32_t)n, 0, 0, OpTypeVector, 1, 0, o, 2);
}
static uint32_t t_fv(Mod *m, int n) { return t_vec(m, t_f32(m), n); }
static uint32_t t_bv(Mod *m, int n) { return t_vec(m, t_bool(m), n); }
static uint32_t t_ptr(Mod *m, uint32_t sc, uint32_t t) { uint32_t o[] = {sc, t}; return once(m, K_PTR, sc, t, 0, 0, OpTypePointer, 1, 0, o, 2); }
static uint32_t t_func(Mod *m, uint32_t r) { uint32_t o[] = {r}; return once(m, K_FN, r, 0, 0, 0, OpTypeFunction, 1, 0, o, 1); }
static uint32_t t_image(Mod *m, uint32_t dim, uint32_t depth)
{
    uint32_t f = t_f32(m);
    uint32_t o[] = {f, dim, depth, 0, 0, 1, 0};
    return once(m, K_IMG, dim, depth, 0, 0, OpTypeImage, 1, 0, o, 7);
}
static uint32_t t_sampler(Mod *m) { return once(m, K_SMP, 0, 0, 0, 0, OpTypeSampler, 1, 0, NULL, 0); }
static uint32_t t_sampled(Mod *m, uint32_t img) { uint32_t o[] = {img}; return once(m, K_SIMG, img, 0, 0, 0, OpTypeSampledImage, 1, 0, o, 1); }
static uint32_t t_rtarray(Mod *m, uint32_t t) { uint32_t o[] = {t}; return once(m, K_RTA, t, 0, 0, 0, OpTypeRuntimeArray, 1, 0, o, 1); }
static Val c_u32(Mod *m, uint32_t u);
static uint32_t t_array(Mod *m, uint32_t t, uint32_t n, uint32_t stride)
{
    uint32_t *f = key_find(m, K_ARR, t, n, 0, 0);
    if (f) return *f;
    uint32_t len = c_u32(m, n).id;
    uint32_t o[] = {t, len};
    uint32_t id = once(m, K_ARR, t, n, 0, 0, OpTypeArray, 1, 0, o, 2);
    INS(&m->annot, OpDecorate, id, DEC_ArrayStride, stride);
    return id;
}
static Val c_fbits(Mod *m, uint32_t b)
{
    uint32_t t = t_f32(m), o[] = {b};
    return (Val){once(m, K_CF, b, 0, 0, 0, OpConstant, 0, t, o, 1), t, 1};
}
static Val c_f32(Mod *m, float f) { uint32_t b; memcpy(&b, &f, 4); return c_fbits(m, b); }
static Val c_u32(Mod *m, uint32_t u) { uint32_t t = t_u32(m), o[] = {u}; return (Val){once(m, K_CU, u, 0, 0, 0, OpConstant, 0, t, o, 1), t, 1}; }
static Val c_i32(Mod *m, int32_t i) { uint32_t t = t_i32(m), o[] = {(uint32_t)i}; return (Val){once(m, K_CI, (uint32_t)i, 0, 0, 0, OpConstant, 0, t, o, 1), t, 1}; }
static Val c_bool(Mod *m, int b)
{
    uint32_t t = t_bool(m);
    return (Val){once(m, K_CB, (uint32_t)b, 0, 0, 0, b ? OpConstantTrue : OpConstantFalse, 0, t, NULL, 0), t, 1};
}
/* a float vector constant from bit patterns */
static Val c_vecb(Mod *m, const uint32_t *bits, int n)
{
    uint32_t ids[4];
    for (int i = 0; i < n; i++) ids[i] = c_fbits(m, bits[i]).id;
    uint32_t t = t_fv(m, n);
    return (Val){once(m, K_CV, ids[0], ids[1], n > 2 ? ids[2] : 0, n > 3 ? ids[3] : 0, OpConstantComposite, 0, t, ids, (uint32_t)n), t, n};
}
static Val c_vec4f(Mod *m, float a, float b, float c, float d)
{
    float f[4] = {a, b, c, d};
    uint32_t u[4];
    memcpy(u, f, 16);
    return c_vecb(m, u, 4);
}
static Val spec_u32(Mod *m, uint32_t sid, uint32_t dflt)
{
    uint32_t i = mnew(m), t = t_u32(m);
    INS(&m->glob, OpSpecConstant, t, i, dflt);
    INS(&m->annot, OpDecorate, i, DEC_SpecId, sid);
    return (Val){i, t, 1};
}
static uint32_t gvar(Mod *m, uint32_t ptr_t, uint32_t sc)
{
    uint32_t i = mnew(m);
    INS(&m->glob, OpVariable, ptr_t, i, sc);
    wpush(&m->iface, i);
    return i;
}
static uint32_t fvar(Mod *m, uint32_t t)
{
    uint32_t i = mnew(m);
    INS(&m->vars, OpVariable, t_ptr(m, SC_Function, t), i, SC_Function);
    return i;
}
static void deco(Mod *m, uint32_t id, uint32_t d) { INS(&m->annot, OpDecorate, id, d); }
static void deco1(Mod *m, uint32_t id, uint32_t d, uint32_t a) { INS(&m->annot, OpDecorate, id, d, a); }

/* ------------------------------------------------------------------ the bytecode */
typedef struct { int typ, num, swz[4], mod, rel; } Src;
typedef struct { int typ, num, mask, sat, shift, centroid; } Dst;
typedef struct { int op, co, idx; int has_dst; Dst dst; Src src[4]; int nsrc; uint32_t dcl; const uint32_t *raw; int nraw; } Ins;

static void tok_reg(uint32_t t, int *typ, int *num) { *typ = (int)(((t >> 28) & 7) | ((t >> 8) & 0x18)); *num = (int)(t & 0x7ff); }
static Dst dst_of(uint32_t t)
{
    Dst x;
    tok_reg(t, &x.typ, &x.num);
    x.mask = (t >> 16) & 15;
    int rm = (t >> 20) & 15;
    x.sat = rm & 1;
    x.centroid = (rm & 4) != 0;
    int sh = (t >> 24) & 15;
    x.shift = sh >= 8 ? sh - 16 : sh;
    return x;
}
static Src src_of(uint32_t t)
{
    Src x;
    tok_reg(t, &x.typ, &x.num);
    int s = (t >> 16) & 0xff;
    for (int c = 0; c < 4; c++) x.swz[c] = (s >> (2 * c)) & 3;
    x.mod = (t >> 24) & 15;
    x.rel = (t & (1 << 13)) != 0;          /* (shader model 1: a0.x) */
    return x;
}
/* (opcode: has a destination, sources) */
static int op_info(int op, int *has_dst, int *nsrc)
{
    static const struct { int op, d, s; } t[] = {
        {0, 0, 0}, {1, 1, 1}, {2, 1, 2}, {3, 1, 2}, {4, 1, 3}, {5, 1, 2}, {6, 1, 1}, {7, 1, 1}, {8, 1, 2}, {9, 1, 2},
        {10, 1, 2}, {11, 1, 2}, {12, 1, 2}, {13, 1, 2}, {14, 1, 1}, {15, 1, 1}, {16, 1, 1}, {17, 1, 2}, {18, 1, 3},
        {19, 1, 1}, {20, 1, 2}, {21, 1, 2}, {22, 1, 2}, {23, 1, 2}, {24, 1, 2}, {46, 1, 1}, {64, 1, 0}, {65, 1, 0},
        {66, 1, 0}, {67, 1, 1}, {68, 1, 1}, {69, 1, 1}, {70, 1, 1}, {71, 1, 1}, {72, 1, 1}, {73, 1, 1}, {74, 1, 1},
        {76, 1, 2}, {77, 1, 1}, {78, 1, 1}, {79, 1, 1}, {80, 1, 3}, {88, 1, 3}, {90, 1, 3}};
    for (size_t i = 0; i < sizeof t / sizeof *t; i++)
        if (t[i].op == op) { *has_dst = t[i].d; *nsrc = t[i].s; return 1; }
    return 0;
}
enum { O_NOP = 0, O_MOV, O_ADD, O_SUB, O_MAD, O_MUL, O_RCP, O_RSQ, O_DP3, O_DP4, O_MIN, O_MAX, O_SLT, O_SGE, O_EXP, O_LOG,
       O_LIT, O_DST, O_LRP, O_FRC, O_M4X4, O_M4X3, O_M3X4, O_M3X3, O_M3X2, O_DCL = 31, O_MOVA = 46, O_TEXCOORD = 64,
       O_TEXKILL, O_TEX, O_TEXBEM, O_TEXBEML, O_TEXM3X2PAD = 71, O_TEXM3X2TEX, O_EXPP = 78, O_LOGP, O_CND, O_DEF,
       O_CMP = 88, O_DP2ADD = 90 };

/* ------------------------------------------------------------------ the compiler (d3d2spv.py Shader) */
typedef struct { int typ, num; uint32_t var; } Reg;
typedef struct { int typ, num; Val v; } Def;
typedef struct { int kind, typ, num; uint32_t var; } InVar;
typedef struct { int typ, num; uint32_t var; int tgt; } OutReg;
typedef struct { int typ, num, usage, index; } Dcl;
typedef struct { int j, si, k, mj, mk; } Fuse;          /* (add j, source si, component k) <- (mul mj, component mk) */
typedef struct { int j, k; Val a, b; } Cap;
typedef struct { Dst d; Val v; } Deferred;

typedef struct {
    Mod m;
    int ps, major, minor;
    Ins *prog;
    int nprog;
    Reg regs[64];
    int nregs;
    Def defs[96];
    int ndefs;
    VkShaderDef defl[96];
    int ndefl;
    uint32_t cmax;
    int rel, kills;
    uint32_t inputs;
    InVar invars[32];
    int ninvars;
    OutReg outs[16];
    int nouts;
    Dcl dcls[32];
    int ndcls;
    uint32_t samplers, proj_used;
    int tex_made;
    uint32_t texvar[4], teximg[4], smp, pc;
    Val spec_kinds, spec_proj;
    uint32_t cbuf, a0, fc, vfog;
    uint32_t fn, entry_label, cur_label;
    Fuse *fuse;
    int nfuse;
    Cap caps[64];
    int ncaps;
    Deferred *deferred;
    int ndeferred, defer;
} Sh;

#define FAIL(s, ...) do { if (!(s)->m.err[0]) snprintf((s)->m.err, sizeof (s)->m.err, __VA_ARGS__); } while (0)

static uint32_t fv(Sh *s, int n) { return t_fv(&s->m, n); }
static uint32_t op1(Sh *s, uint32_t op, uint32_t rtype, const uint32_t *a, uint32_t n)
{
    uint32_t r = mnew(&s->m);
    uint32_t buf[16] = {rtype, r};
    for (uint32_t i = 0; i < n; i++) buf[2 + i] = a[i];
    ins_a(&s->m.code, op, buf, n + 2);
    return r;
}
#define OP(s, op, rt, ...) ({ const uint32_t _a[] = {__VA_ARGS__}; op1((s), (op), (rt), _a, sizeof _a / 4); })
static void lab(Sh *s, uint32_t i) { INS(&s->m.code, OpLabel, i); s->cur_label = i; }

static Val extract(Sh *s, Val v, int c)
{
    if (v.n == 1) return v;
    uint32_t f = t_f32(&s->m);
    return (Val){OP(s, OpCompositeExtract, f, v.id, (uint32_t)c), f, 1};
}
static Val splat(Sh *s, Val v, int n)
{
    if (v.n == n) return v;
    if (v.n != 1) v = extract(s, v, 0);
    uint32_t a[4] = {v.id, v.id, v.id, v.id};
    return (Val){op1(s, OpCompositeConstruct, fv(s, n), a, (uint32_t)n), fv(s, n), n};
}
static Val shuffle(Sh *s, Val v, const int *comps, int n)
{
    if (v.n == 1) return n > 1 ? splat(s, v, n) : v;
    if (n == 1) return extract(s, v, comps[0]);
    int same = n == v.n;
    for (int i = 0; i < n && same; i++) same = comps[i] == i;
    if (same) return v;
    uint32_t a[6] = {v.id, v.id};
    for (int i = 0; i < n; i++) a[2 + i] = (uint32_t)comps[i];
    return (Val){op1(s, OpVectorShuffle, fv(s, n), a, (uint32_t)n + 2), fv(s, n), n};
}
static Val construct(Sh *s, const Val *c, int n)
{
    if (n == 1) return c[0];
    uint32_t a[4];
    for (int i = 0; i < n; i++) a[i] = c[i].id;
    return (Val){op1(s, OpCompositeConstruct, fv(s, n), a, (uint32_t)n), fv(s, n), n};
}
static Val cf(Sh *s, float f) { return c_f32(&s->m, f); }
static Val c4(Sh *s, float a, float b, float c, float d) { return c_vec4f(&s->m, a, b, c, d); }
static Val binop(Sh *s, uint32_t op, Val a, Val b, int precise)
{
    int n = a.n > b.n ? a.n : b.n;
    a = splat(s, a, n), b = splat(s, b, n);
    uint32_t r = OP(s, op, fv(s, n), a.id, b.id);
    if (precise) deco(&s->m, r, DEC_NoContraction);
    return (Val){r, fv(s, n), n};
}
static Val add(Sh *s, Val a, Val b) { return binop(s, OpFAdd, a, b, 0); }
static Val sub(Sh *s, Val a, Val b) { return binop(s, OpFSub, a, b, 0); }
/* (sv_: since AntiStatic v0.2.46 runtime.h brings in <math.h>, whose C23 fmul / fdiv these names met) */
static Val sv_fmul(Sh *s, Val a, Val b) { return binop(s, OpFMul, a, b, 0); }
static Val sv_fdiv(Sh *s, Val a, Val b) { return binop(s, OpFDiv, a, b, 0); }
static Val neg(Sh *s, Val a) { return (Val){OP(s, OpFNegate, a.ty, a.id), a.ty, a.n}; }
static Val ext(Sh *s, uint32_t inst, int nargs, Val a0, Val a1, Val a2)
{
    Val args[3] = {a0, a1, a2};
    int n = 0;
    for (int i = 0; i < nargs; i++) if (args[i].n > n) n = args[i].n;
    uint32_t a[5] = {s->m.glsl, inst};
    for (int i = 0; i < nargs; i++) a[2 + i] = splat(s, args[i], n).id;
    return (Val){op1(s, OpExtInst, fv(s, n), a, (uint32_t)nargs + 2), fv(s, n), n};
}
static Val ext1(Sh *s, uint32_t inst, Val a) { return ext(s, inst, 1, a, a, a); }
static Val ext2(Sh *s, uint32_t inst, Val a, Val b) { return ext(s, inst, 2, a, b, b); }
static Val ext3(Sh *s, uint32_t inst, Val a, Val b, Val c) { return ext(s, inst, 3, a, b, c); }
static Val cmpv(Sh *s, uint32_t op, Val a, Val b)
{
    int n = a.n > b.n ? a.n : b.n;
    a = splat(s, a, n), b = splat(s, b, n);
    uint32_t t = t_bv(&s->m, n);
    return (Val){OP(s, op, t, a.id, b.id), t, n};
}
static Val sel(Sh *s, Val c, Val a, Val b)
{
    int n = a.n > b.n ? a.n : b.n;
    if (c.n > n) n = c.n;
    a = splat(s, a, n), b = splat(s, b, n);
    return (Val){OP(s, OpSelect, fv(s, n), c.id, a.id, b.id), fv(s, n), n};
}
/* (DXVK's strict float emulation: (b == 0 ? 0 : a) x (a == 0 ? 0 : b)) */
static void legacy(Sh *s, Val a, Val b, Val *a2, Val *b2)
{
    int n = a.n > b.n ? a.n : b.n;
    a = splat(s, a, n), b = splat(s, b, n);
    Val z = n > 1 ? (n == 4 ? c4(s, 0, 0, 0, 0) : c_vecb(&s->m, (const uint32_t[4]){0, 0, 0, 0}, n)) : cf(s, 0);
    *a2 = sel(s, cmpv(s, OpFOrdEqual, b, z), z, a);
    *b2 = sel(s, cmpv(s, OpFOrdEqual, a, z), z, b);
}
static Val mul(Sh *s, Val a, Val b, int precise)
{
    Val a2, b2;
    legacy(s, a, b, &a2, &b2);
    return binop(s, OpFMul, a2, b2, precise);
}
static Val mad(Sh *s, Val a, Val b, Val c, int precise)
{
    Val a2, b2;
    legacy(s, a, b, &a2, &b2);
    Val r = ext3(s, GLSL_Fma, a2, b2, splat(s, c, a2.n));
    if (precise) deco(&s->m, r.id, DEC_NoContraction);
    return r;
}
static Val dot(Sh *s, Val a, Val b, int n, const Val *acc)
{
    Val r = {0, 0, 0};
    int have = acc != NULL;
    if (acc) r = *acc;
    for (int i = 0; i < n; i++) {
        Val x = extract(s, a, i), y = extract(s, b, i);
        r = !have ? mul(s, x, y, 1) : mad(s, x, y, r, 1);
        have = 1;
    }
    return r;
}
static Val saturate(Sh *s, Val v)
{
    int n = v.n;
    return ext3(s, GLSL_FClamp, v, n > 1 ? splat(s, cf(s, 0), n) : cf(s, 0), n > 1 ? splat(s, cf(s, 1), n) : cf(s, 1));
}
static Val clampv(Sh *s, Val v, float lo, float hi)
{
    int n = v.n;
    return ext3(s, GLSL_FClamp, v, splat(s, cf(s, lo), n), splat(s, cf(s, hi), n));
}

/* ---- registers */
static uint32_t reg(Sh *s, int typ, int num)
{
    for (int i = 0; i < s->nregs; i++) if (s->regs[i].typ == typ && s->regs[i].num == num) return s->regs[i].var;
    if (s->nregs == 64) { FAIL(s, "too many registers"); return s->regs[0].var; }
    uint32_t v = fvar(&s->m, fv(s, 4));
    s->regs[s->nregs++] = (Reg){typ, num, v};
    return v;
}
static Val load4(Sh *s, uint32_t var) { return (Val){OP(s, OpLoad, fv(s, 4), var), fv(s, 4), 4}; }
static Val load_reg(Sh *s, int typ, int num) { return load4(s, reg(s, typ, num)); }
static void store_reg(Sh *s, uint32_t var, Val v, int mask)
{
    if (mask == 15) { INS(&s->m.code, OpStore, var, v.id); return; }
    uint32_t old = OP(s, OpLoad, fv(s, 4), var);
    uint32_t a[6] = {old, v.id};
    for (int c = 0; c < 4; c++) a[2 + c] = (mask >> c & 1) ? 4 + (uint32_t)c : (uint32_t)c;
    uint32_t nw = op1(s, OpVectorShuffle, fv(s, 4), a, 6);
    INS(&s->m.code, OpStore, var, nw);
}
static uint32_t addr_var(Sh *s)
{
    if (!s->a0) s->a0 = fvar(&s->m, t_vec(&s->m, t_i32(&s->m), 4));
    return s->a0;
}
static Val const_load(Sh *s, int num, int rel)
{
    Mod *m = &s->m;
    if (!rel)
        for (int i = 0; i < s->ndefs; i++) if (s->defs[i].typ == R_CONST && s->defs[i].num == num) return s->defs[i].v;
    Val idx = c_i32(m, num + (s->ps ? VK_PS_EXTRA : 0)), chunk = c_i32(m, 0), index;
    if (!rel) {
        if ((uint32_t)num + 1 > s->cmax) s->cmax = (uint32_t)num + 1;
        index = idx;
        if (!s->ps) chunk = c_i32(m, num / VK_VS_CHUNK), index = c_i32(m, num % VK_VS_CHUNK);
    } else {
        if (s->ps) { FAIL(s, "relative addressing in a pixel shader"); return cf(s, 0); }
        s->rel = 1;
        s->cmax = VK_VS_CONSTS;
        uint32_t it = t_i32(m);
        uint32_t p = OP(s, OpAccessChain, t_ptr(m, SC_Function, it), addr_var(s), c_i32(m, 0).id);
        uint32_t a = OP(s, OpLoad, it, p);
        uint32_t sum = OP(s, OpIAdd, it, a, idx.id);
        index = (Val){OP(s, OpExtInst, it, m->glsl, GLSL_SClamp, sum, c_i32(m, 0).id, c_i32(m, VK_VS_CONSTS - 1).id), it, 1};
    }
    uint32_t ptr;
    if (s->ps) ptr = OP(s, OpAccessChain, t_ptr(m, SC_Uniform, fv(s, 4)), s->cbuf, c_i32(m, 0).id, index.id);
    else ptr = OP(s, OpAccessChain, t_ptr(m, SC_Uniform, fv(s, 4)), s->cbuf, chunk.id, c_i32(m, 0).id, index.id);
    Val v = load4(s, ptr);
    if (s->ps && s->major == 1) v = clampv(s, v, -1.0f, 1.0f);
    return v;
}

static int dcl_find(Sh *s, int typ, int num, int *usage, int *index)
{
    for (int i = 0; i < s->ndcls; i++)
        if (s->dcls[i].typ == typ && s->dcls[i].num == num) { *usage = s->dcls[i].usage; *index = s->dcls[i].index; return 1; }
    return 0;
}
static uint32_t invar_find(Sh *s, int kind, int typ, int num)
{
    for (int i = 0; i < s->ninvars; i++)
        if (s->invars[i].kind == kind && s->invars[i].typ == typ && s->invars[i].num == num) return s->invars[i].var;
    return 0;
}
static Val vs_input(Sh *s, int num)
{
    uint32_t var = invar_find(s, 0, R_INPUT, num);
    if (!var) {
        int usage, index;
        if (!dcl_find(s, R_INPUT, num, &usage, &index)) { FAIL(s, "vertex input v%d not declared", num); return c4(s, 0, 0, 0, 0); }
        int loc = vk_input_loc((unsigned)usage, (unsigned)index);
        if (loc < 0) { FAIL(s, "vertex input usage %d index %d", usage, index); return c4(s, 0, 0, 0, 0); }
        var = gvar(&s->m, t_ptr(&s->m, SC_Input, fv(s, 4)), SC_Input);
        deco1(&s->m, var, DEC_Location, (uint32_t)loc);
        s->inputs |= 1u << loc;
        s->invars[s->ninvars++] = (InVar){0, R_INPUT, num, var};
    }
    return load4(s, var);
}
static Val ps_input(Sh *s, int typ, int num)
{
    uint32_t var = invar_find(s, 1, typ, num);
    if (!var) {
        int loc = (typ == R_INPUT ? VARY_COLOR : VARY_TEX) + num;
        var = gvar(&s->m, t_ptr(&s->m, SC_Input, fv(s, 4)), SC_Input);
        deco1(&s->m, var, DEC_Location, (uint32_t)loc);
        /* (colours at the centroid of the covered samples, the rest at the pixel's centre: DXVK's io map) */
        if (loc == VARY_COLOR || loc == VARY_COLOR + 1) deco(&s->m, var, DEC_Centroid);
        if (s->ninvars < 32) s->invars[s->ninvars++] = (InVar){1, typ, num, var};
    }
    return load4(s, var);
}
static Val texcoord_in(Sh *s, int num) { return ps_input(s, R_ADDR_TEX, num); }
static Val frag_coord(Sh *s)
{
    if (!s->fc) {
        s->fc = gvar(&s->m, t_ptr(&s->m, SC_Input, fv(s, 4)), SC_Input);
        deco1(&s->m, s->fc, DEC_BuiltIn, 15);
    }
    return load4(s, s->fc);
}
static uint32_t vs_out(Sh *s, int typ, int num)
{
    for (int i = 0; i < s->nouts; i++) if (s->outs[i].typ == typ && s->outs[i].num == num) return s->outs[i].var;
    int tgt;
    if (typ == R_RASTOUT) tgt = num == 0 ? TGT_POS : num == 1 ? VARY_FOG : TGT_PSIZE;
    else if (typ == R_ATTROUT) tgt = VARY_COLOR + num;
    else tgt = VARY_TEX + num;
    if (s->nouts == 16 || tgt < 0 || (tgt >= NVARY && tgt < TGT_POS)) { FAIL(s, "output %d.%d", typ, num); return 0; }
    uint32_t var = fvar(&s->m, fv(s, 4));
    s->outs[s->nouts++] = (OutReg){typ, num, var, tgt};
    return var;
}

static Val src_raw(Sh *s, const Src *x)
{
    switch (x->typ) {
    case R_CONST: return const_load(s, x->num, x->rel);
    case R_INPUT: return s->ps ? ps_input(s, R_INPUT, x->num) : vs_input(s, x->num);
    case R_ADDR_TEX:
        if (s->ps) return load_reg(s, R_ADDR_TEX, x->num);  /* (ps_1_x's texture register: a temp) */
        FAIL(s, "a0 as a source");
        return c4(s, 0, 0, 0, 0);
    case R_TEMP: return load_reg(s, R_TEMP, x->num);
    }
    FAIL(s, "source register type %d", x->typ);
    return c4(s, 0, 0, 0, 0);
}
static Val src(Sh *s, const Src *x, int n)
{
    Val v = src_raw(s, x);
    int ident = x->swz[0] == 0 && x->swz[1] == 1 && x->swz[2] == 2 && x->swz[3] == 3;
    if (n < 4 || !ident) v = shuffle(s, v, x->swz, n);
    switch (x->mod) {
    case 0: break;
    case 1: v = neg(s, v); break;
    case 2: v = sub(s, v, cf(s, 0.5f)); break;
    case 3: v = sub(s, cf(s, 0.5f), v); break;
    case 4: v = add(s, sv_fmul(s, v, cf(s, 2.0f)), cf(s, -1.0f)); break;
    case 5: v = add(s, sv_fmul(s, v, cf(s, -2.0f)), cf(s, 1.0f)); break;
    case 6: v = sub(s, cf(s, 1.0f), v); break;
    case 7: v = sv_fmul(s, v, cf(s, 2.0f)); break;
    case 8: v = sv_fmul(s, v, cf(s, -2.0f)); break;
    case 11: v = ext1(s, GLSL_FAbs, v); break;
    case 12: v = neg(s, ext1(s, GLSL_FAbs, v)); break;
    default: FAIL(s, "source modifier %d", x->mod); break;
    }
    return v;
}
static void write_dst(Sh *s, const Dst *d, Val v)
{
    switch (d->typ) {
    case R_TEMP: store_reg(s, reg(s, R_TEMP, d->num), v, d->mask); return;
    case R_ADDR_TEX:
        if (s->ps) { store_reg(s, reg(s, R_ADDR_TEX, d->num), v, d->mask); return; }
        break;
    case R_RASTOUT: case R_ATTROUT: case R_TEXOUT:
        if (!s->ps) { uint32_t o = vs_out(s, d->typ, d->num); if (o) store_reg(s, o, v, d->mask); return; }
        break;
    }
    FAIL(s, "destination register type %d", d->typ);
}
static void dst_store(Sh *s, const Dst *d, Val v)
{
    v = splat(s, v, 4);
    if (d->shift) v = sv_fmul(s, v, cf(s, d->shift > 0 ? (float)(1 << d->shift) : 1.0f / (float)(1 << -d->shift)));
    if (d->sat) v = saturate(s, v);
    write_dst(s, d, v);
}
static void result(Sh *s, const Ins *i, Val v)
{
    if (s->defer) {
        s->deferred = realloc(s->deferred, (size_t)(s->ndeferred + 1) * sizeof *s->deferred);
        s->deferred[s->ndeferred++] = (Deferred){i->dst, v};
    } else dst_store(s, &i->dst, v);
}

/* ---- textures */
static void tex_bindings(Sh *s)
{
    if (s->tex_made) return;
    s->tex_made = 1;
    Mod *m = &s->m;
    static const uint32_t dims[4][2] = {{1, 0}, {3, 0}, {2, 0}, {1, 1}};
    for (int b = 0; b < 4; b++) {
        uint32_t img = t_image(m, dims[b][0], dims[b][1]);
        uint32_t var = gvar(m, t_ptr(m, SC_UniformConstant, t_rtarray(m, img)), SC_UniformConstant);
        deco1(m, var, DEC_DescriptorSet, 0);
        deco1(m, var, DEC_Binding, (uint32_t)b);
        s->texvar[b] = var, s->teximg[b] = img;
    }
    s->smp = gvar(m, t_ptr(m, SC_UniformConstant, t_rtarray(m, t_sampler(m))), SC_UniformConstant);
    deco1(m, s->smp, DEC_DescriptorSet, 0);
    deco1(m, s->smp, DEC_Binding, 4);
    uint32_t arr = t_array(m, t_u32(m), VK_MAX_SAMPLER_SLOTS, 4);
    uint32_t st = mnew(m);
    INS(&m->glob, OpTypeStruct, st, arr);
    deco(m, st, DEC_Block);
    INS(&m->annot, OpMemberDecorate, st, 0, DEC_Offset, 0);
    s->pc = gvar(m, t_ptr(m, SC_PushConstant, st), SC_PushConstant);
    s->spec_kinds = spec_u32(m, VK_SPEC_KINDS, 0);
    s->spec_proj = spec_u32(m, VK_SPEC_PROJ, 0);
}
static Val spec_field(Sh *s, Val spec, int shift, int bits)
{
    Mod *m = &s->m;
    uint32_t v = OP(s, OpShiftRightLogical, t_u32(m), spec.id, c_u32(m, (uint32_t)shift).id);
    return (Val){OP(s, OpBitwiseAnd, t_u32(m), v, c_u32(m, (1u << bits) - 1).id), t_u32(m), 1};
}
/* a structured if/else chain over cases (cond NULL: the last), each making a vec4; the chosen one */
typedef Val (*CaseFn)(Sh *s, void *ctx, int arg);
typedef struct { Val cond; int has_cond; CaseFn fn; int arg; } Case;
static Val branch(Sh *s, void *ctx, const Case *cs, int n)
{
    Mod *m = &s->m;
    if (!cs[0].has_cond || n == 1) return cs[0].fn(s, ctx, cs[0].arg);
    uint32_t lt = mnew(m), lf = mnew(m), merge = mnew(m);
    INS(&m->code, OpSelectionMerge, merge, 0);
    INS(&m->code, OpBranchConditional, cs[0].cond.id, lt, lf);
    lab(s, lt);
    Val v1 = cs[0].fn(s, ctx, cs[0].arg);
    uint32_t end1 = s->cur_label;
    INS(&m->code, OpBranch, merge);
    lab(s, lf);
    Val v2 = branch(s, ctx, cs + 1, n - 1);
    uint32_t end2 = s->cur_label;
    INS(&m->code, OpBranch, merge);
    lab(s, merge);
    return (Val){OP(s, OpPhi, fv(s, 4), v1.id, end1, v2.id, end2), fv(s, 4), 4};
}
typedef struct { Val coord, tidx, sidx; } SampleCtx;
static Val sample_color(Sh *s, void *cp, int b)
{
    SampleCtx *c = cp;
    Mod *m = &s->m;
    uint32_t img = s->teximg[b];
    uint32_t p = OP(s, OpAccessChain, t_ptr(m, SC_UniformConstant, img), s->texvar[b], c->tidx.id);
    uint32_t im = OP(s, OpLoad, img, p);
    uint32_t sp = OP(s, OpAccessChain, t_ptr(m, SC_UniformConstant, t_sampler(m)), s->smp, c->sidx.id);
    uint32_t sm = OP(s, OpLoad, t_sampler(m), sp);
    uint32_t si = OP(s, OpSampledImage, t_sampled(m, img), im, sm);
    static const int xyz[3] = {0, 1, 2};
    Val cc = shuffle(s, c->coord, xyz, b == 0 ? 2 : 3);
    return (Val){OP(s, OpImageSampleImplicitLod, fv(s, 4), si, cc.id), fv(s, 4), 4};
}
static Val sample_shadow(Sh *s, void *cp, int arg)
{
    (void)arg;
    SampleCtx *c = cp;
    Mod *m = &s->m;
    uint32_t img = s->teximg[3];
    static const int xy[2] = {0, 1};
    Val cc = shuffle(s, c->coord, xy, 2);
    Val dref = extract(s, c->coord, 2);
    uint32_t p = OP(s, OpAccessChain, t_ptr(m, SC_UniformConstant, img), s->texvar[3], c->tidx.id);
    uint32_t im = OP(s, OpLoad, img, p);
    uint32_t sp = OP(s, OpAccessChain, t_ptr(m, SC_UniformConstant, t_sampler(m)), s->smp, c->sidx.id);
    uint32_t sm = OP(s, OpLoad, t_sampler(m), sp);
    uint32_t si = OP(s, OpSampledImage, t_sampled(m, img), im, sm);
    uint32_t f = t_f32(m);
    return splat(s, (Val){OP(s, OpImageSampleDrefImplicitLod, f, si, cc.id, dref.id), f, 1}, 4);
}
/* sampler slot sl at coord (vec4), the kind of texture by the spec constant (ps_1_x declares none) */
static Val sample(Sh *s, int sl, Val coord)
{
    Mod *m = &s->m;
    if (sl >= VK_MAX_SAMPLER_SLOTS) { FAIL(s, "sampler s%d", sl); return c4(s, 0, 0, 0, 0); }
    tex_bindings(s);
    s->samplers |= 1u << sl;
    uint32_t p = OP(s, OpAccessChain, t_ptr(m, SC_PushConstant, t_u32(m)), s->pc, c_i32(m, 0).id, c_i32(m, sl).id);
    Val word = {OP(s, OpLoad, t_u32(m), p), t_u32(m), 1};
    SampleCtx c = {coord, {OP(s, OpBitwiseAnd, t_u32(m), word.id, c_u32(m, 0xffff).id), t_u32(m), 1},
                   {OP(s, OpShiftRightLogical, t_u32(m), word.id, c_u32(m, 16).id), t_u32(m), 1}};
    Val kind = spec_field(s, s->spec_kinds, 2 * sl, 2);
    Case cs[4];
    static const int order[4] = {1, 2, 3, 0};
    for (int i = 0; i < 4; i++) {
        int k = order[i];
        cs[i].has_cond = i < 3;
        if (i < 3) cs[i].cond = (Val){OP(s, OpIEqual, t_bool(m), kind.id, c_u32(m, (uint32_t)k).id), t_bool(m), 1};
        cs[i].fn = k == 3 ? sample_shadow : sample_color;
        cs[i].arg = k == 1 ? 1 : k == 2 ? 2 : 0;
    }
    return branch(s, &c, cs, 4);
}
/* ps_1_x tex: coord / coord.w when the slot's D3DTTFF_PROJECTED is set (spec constant), per component */
static Val project(Sh *s, int sl, Val coord)
{
    Mod *m = &s->m;
    tex_bindings(s);
    s->proj_used |= 1u << sl;
    Val bit = spec_field(s, s->spec_proj, sl, 1);
    Val on = {OP(s, OpINotEqual, t_bool(m), bit.id, c_u32(m, 0).id), t_bool(m), 1};
    Val w = extract(s, coord, 3), xs[4], r[4];
    for (int c = 0; c < 4; c++) xs[c] = extract(s, coord, c);
    for (int c = 0; c < 4; c++) r[c] = sel(s, on, sv_fdiv(s, xs[c], w), xs[c]);
    return construct(s, r, 4);
}
static Val extra(Sh *s, int i)
{
    Mod *m = &s->m;
    uint32_t ptr = OP(s, OpAccessChain, t_ptr(m, SC_Uniform, fv(s, 4)), s->cbuf, c_i32(m, 0).id, c_i32(m, i).id);
    return load4(s, ptr);
}

/* ---- DXVK's multiply-add fusion (d3d2spv.py fusions) */
static int is_fuse_cw(int op)
{
    return op == O_MOV || op == O_ADD || op == O_SUB || op == O_MUL || op == O_MAD || op == O_MIN || op == O_MAX ||
           op == O_SLT || op == O_SGE || op == O_LRP || op == O_CMP || op == O_CND || op == 35 /* abs */ ||
           op == O_FRC || op == 34 /* sgn */;
}
static int is_fuse_scalar(int op)
{
    return op == O_RCP || op == O_RSQ || op == O_EXP || op == O_LOG || op == O_EXPP || op == O_LOGP || op == 32 /* pow */;
}
typedef struct { uint32_t key; int wj, wk; } LastW;        /* register component -> its writer (a plain mul's) */
typedef struct { int wj, wk, n; int uj, usi, uk; } UseW;   /* a mul component's uses: the only one (u*), how many */
static void fusions(Sh *s)
{
    LastW *last = NULL;
    int nlast = 0, nuses = 0;
    UseW *uses = NULL;
    for (int g0 = 0; g0 < s->nprog; ) {
        int g1 = g0 + 1;
        while (g1 < s->nprog && s->prog[g1].co) g1++;     /* (a co-issued pair reads before either writes) */
        LastW writes[64];
        int nw = 0;
        for (int ii = g0; ii < g1; ii++) {
            Ins *i = &s->prog[ii];
            int mask = i->has_dst ? i->dst.mask : 15;
            for (int si = 0; si < i->nsrc; si++) {
                Src *x = &i->src[si];
                if ((x->typ != R_TEMP && x->typ != R_ADDR_TEX) || x->rel) continue;
                uint32_t rkey[4];
                int ruse[4], nr = 0;                       /* ruse: the use's component, -1 if it cannot fuse */
                uint32_t base = (uint32_t)x->typ << 16 | (uint32_t)x->num << 2;
                if (is_fuse_cw(i->op)) {
                    for (int k = 0; k < 4; k++)
                        if (mask >> k & 1) {
                            int ok = (i->op == O_ADD || i->op == O_SUB) && x->mod == 0;
                            rkey[nr] = base | (uint32_t)x->swz[k];
                            ruse[nr++] = ok ? k : -1;
                        }
                } else if (is_fuse_scalar(i->op)) {
                    rkey[nr] = base | (uint32_t)x->swz[0];
                    ruse[nr++] = -1;
                } else if (i->op == O_DP3 || i->op == O_DP4 || i->op == O_DP2ADD) {
                    int n = si < 2 ? (i->op == O_DP3 ? 3 : i->op == O_DP4 ? 4 : 2) : 1;
                    for (int k = 0; k < n; k++) { rkey[nr] = base | (uint32_t)x->swz[k]; ruse[nr++] = -1; }
                } else {
                    for (int k = 0; k < 4; k++) { rkey[nr] = base | (uint32_t)k; ruse[nr++] = -1; }
                }
                /* each read: its register component's last writer (a plain mul's) gets a use */
                for (int r = 0; r < nr; r++)
                    for (int l = 0; l < nlast; l++) {
                        if (last[l].key != rkey[r]) continue;
                        if (last[l].wj < 0) break;
                        int u = -1;
                        for (int q = 0; q < nuses; q++) if (uses[q].wj == last[l].wj && uses[q].wk == last[l].wk) u = q;
                        if (u < 0) {
                            uses = realloc(uses, (size_t)(nuses + 1) * sizeof *uses);
                            uses[nuses] = (UseW){last[l].wj, last[l].wk, 0, -1, 0, 0};
                            u = nuses++;
                        }
                        uses[u].n++;
                        if (ruse[r] >= 0) uses[u].uj = i->idx, uses[u].usi = si, uses[u].uk = ruse[r];
                        else uses[u].uj = -2;
                        break;
                    }
            }
            if (i->has_dst && (i->dst.typ == R_TEMP || i->dst.typ == R_ADDR_TEX || i->dst.typ == R_TEXOUT ||
                               i->dst.typ == R_ATTROUT || i->dst.typ == R_RASTOUT || i->dst.typ == R_COLOROUT)) {
                int plain = i->op == O_MUL && !i->dst.sat && !i->dst.shift;
                for (int k = 0; k < 4; k++)
                    if ((mask >> k & 1) && nw < 64)
                        writes[nw++] = (LastW){(uint32_t)i->dst.typ << 16 | (uint32_t)i->dst.num << 2 | (uint32_t)k,
                                               plain ? i->idx : -1, k};
            }
        }
        for (int w = 0; w < nw; w++) {
            int l;
            for (l = 0; l < nlast; l++) if (last[l].key == writes[w].key) break;
            if (l == nlast) { last = realloc(last, (size_t)(nlast + 1) * sizeof *last); nlast++; }
            last[l] = writes[w];
        }
        g0 = g1;
    }
    /* the single fusable uses; an add whose both sources fuse in a component keeps the first (DXVK) */
    for (int q = 0; q < nuses; q++) {
        if (uses[q].n != 1 || uses[q].uj < 0) continue;
        int dup = 0;
        for (int f = 0; f < s->nfuse; f++)
            if (s->fuse[f].j == uses[q].uj && s->fuse[f].k == uses[q].uk) {
                if (uses[q].usi < s->fuse[f].si) s->fuse[f] = (Fuse){uses[q].uj, uses[q].usi, uses[q].uk, uses[q].wj, uses[q].wk};
                dup = 1;
            }
        if (dup) continue;
        s->fuse = realloc(s->fuse, (size_t)(s->nfuse + 1) * sizeof *s->fuse);
        s->fuse[s->nfuse++] = (Fuse){uses[q].uj, uses[q].usi, uses[q].uk, uses[q].wj, uses[q].wk};
    }
    free(last);
    free(uses);
}

static const Fuse *fuse_of(Sh *s, int j, int si, int k)
{
    for (int f = 0; f < s->nfuse; f++) if (s->fuse[f].j == j && s->fuse[f].si == si && s->fuse[f].k == k) return &s->fuse[f];
    return NULL;
}
static int fuse_mul(Sh *s, int j, int k)
{
    for (int f = 0; f < s->nfuse; f++) if (s->fuse[f].mj == j && s->fuse[f].mk == k) return 1;
    return 0;
}
static Val add_fused(Sh *s, const Ins *i, Val x, Val y)
{
    Val comps[4];
    for (int k = 0; k < 4; k++) {
        Val xk = extract(s, x, k), yk = extract(s, y, k);
        const Fuse *f0 = fuse_of(s, i->idx, 0, k), *f1 = fuse_of(s, i->idx, 1, k);
        if (!(i->dst.mask >> k & 1) || !(f0 || f1)) {
            comps[k] = i->op == O_ADD ? add(s, xk, yk) : sub(s, xk, yk);
            continue;
        }
        const Fuse *f = f0 ? f0 : f1;
        int si = f0 ? 0 : 1;
        Val a = xk, b = xk;
        for (int c = 0; c < s->ncaps; c++)
            if (s->caps[c].j == f->mj && s->caps[c].k == f->mk) a = s->caps[c].a, b = s->caps[c].b;
        Val other = si == 0 ? yk : xk;
        if (i->op == O_SUB && si == 0) other = neg(s, other);     /* a x b - c */
        else if (i->op == O_SUB) a = neg(s, a);                    /* c - a x b */
        comps[k] = ext3(s, GLSL_Fma, a, b, other);
    }
    return construct(s, comps, 4);
}

/* ---- instructions */
static void mova(Sh *s, const Ins *i, Val v)
{
    Mod *m = &s->m;
    Val r = ext1(s, GLSL_Floor, splat(s, v, 4));
    uint32_t it = t_vec(m, t_i32(m), 4);
    uint32_t iv = OP(s, OpConvertFToS, it, r.id);
    uint32_t var = addr_var(s);
    uint32_t old = OP(s, OpLoad, it, var);
    uint32_t a[6] = {old, iv};
    for (int c = 0; c < 4; c++) a[2 + c] = (i->dst.mask >> c & 1) ? 4 + (uint32_t)c : (uint32_t)c;
    uint32_t nw = op1(s, OpVectorShuffle, it, a, 6);
    INS(&m->code, OpStore, var, nw);
}
static void texkill(Sh *s, const Ins *i)
{
    Mod *m = &s->m;
    s->kills = 1;
    Val v = texcoord_in(s, i->dst.num), mn = {0, 0, 0};
    int have = 0;
    for (int c = 0; c < 3; c++)
        if (i->dst.mask >> c & 1) {
            Val x = extract(s, v, c);
            mn = have ? ext2(s, GLSL_FMin, x, mn) : x;
            have = 1;
        }
    if (!have) return;
    Val cond = cmpv(s, OpFOrdLessThan, mn, cf(s, 0));
    uint32_t lt = mnew(m), merge = mnew(m);
    INS(&m->code, OpSelectionMerge, merge, 0);
    INS(&m->code, OpBranchConditional, cond.id, lt, merge);
    lab(s, lt);
    INS0(&m->code, OpDemoteToHelperInvocation);
    INS(&m->code, OpBranch, merge);
    lab(s, merge);
}
static void instr(Sh *s, const Ins *i)
{
    Mod *m = &s->m;
#define S(k) src(s, &i->src[k], 4)
    switch (i->op) {
    case O_NOP: return;
    case O_MOV: {
        Val v = S(0);
        if (i->dst.typ == R_ADDR_TEX && !s->ps) { mova(s, i, v); return; }
        result(s, i, v);
        return;
    }
    case O_MOVA: mova(s, i, S(0)); return;
    case O_ADD: case O_SUB: {
        int fused = 0;
        for (int si = 0; si < 2; si++) for (int k = 0; k < 4; k++) if (fuse_of(s, i->idx, si, k)) fused = 1;
        if (fused) { Val a = S(0), b = S(1); result(s, i, add_fused(s, i, a, b)); return; }
        Val a = S(0), b = S(1);
        result(s, i, i->op == O_ADD ? add(s, a, b) : sub(s, a, b));
        return;
    }
    case O_MUL: {
        int captured = 0;
        for (int k = 0; k < 4; k++) if (fuse_mul(s, i->idx, k)) captured = 1;
        Val a = S(0), b = S(1);
        if (captured) {
            Val a2, b2;
            legacy(s, a, b, &a2, &b2);
            for (int k = 0; k < 4; k++)
                if (fuse_mul(s, i->idx, k) && s->ncaps < 64)
                    s->caps[s->ncaps++] = (Cap){i->idx, k, extract(s, a2, k), extract(s, b2, k)};
            result(s, i, sv_fmul(s, a2, b2));
            return;
        }
        result(s, i, mul(s, a, b, 0));
        return;
    }
    case O_MAD: { Val a = S(0), b = S(1), c = S(2); result(s, i, mad(s, a, b, c, 0)); return; }
    case O_MIN: { Val a = S(0), b = S(1); result(s, i, ext2(s, GLSL_FMin, a, b)); return; }
    case O_MAX: { Val a = S(0), b = S(1); result(s, i, ext2(s, GLSL_FMax, a, b)); return; }
    case O_SLT: { Val a = S(0), b = S(1); result(s, i, sel(s, cmpv(s, OpFOrdLessThan, a, b), cf(s, 1), cf(s, 0))); return; }
    case O_SGE: { Val a = S(0), b = S(1); result(s, i, sel(s, cmpv(s, OpFOrdGreaterThanEqual, a, b), cf(s, 1), cf(s, 0))); return; }
    case O_LRP: { Val a = S(0), b = S(1), c = S(2); result(s, i, add(s, mul(s, a, sub(s, b, c), 0), c)); return; }
    case O_CMP: {
        Val a = S(0);
        Val c = cmpv(s, OpFOrdGreaterThanEqual, a, c4(s, 0, 0, 0, 0));
        Val b = S(1), d = S(2);
        result(s, i, sel(s, c, b, d));
        return;
    }
    case O_CND: {
        Val a = S(0);
        Val c = cmpv(s, OpFOrdGreaterThan, a, c4(s, 0.5f, 0.5f, 0.5f, 0.5f));
        Val b = S(1), d = S(2);
        result(s, i, sel(s, c, b, d));
        return;
    }
    case O_RCP: { Val x = S(0); result(s, i, sv_fdiv(s, cf(s, 1), x)); return; }
    case O_RSQ: { Val x = S(0); result(s, i, ext1(s, GLSL_InverseSqrt, ext1(s, GLSL_FAbs, x))); return; }
    case O_EXP: case O_EXPP: { Val x = S(0); result(s, i, ext1(s, GLSL_Exp2, x)); return; }
    case O_LOG: case O_LOGP: { Val x = S(0); result(s, i, ext1(s, GLSL_Log2, ext1(s, GLSL_FAbs, x))); return; }
    case O_FRC: { Val x = S(0); result(s, i, ext1(s, GLSL_Fract, x)); return; }
    case O_DP3: case O_DP4: { Val a = S(0), b = S(1); result(s, i, dot(s, a, b, i->op == O_DP3 ? 3 : 4, NULL)); return; }
    case O_DP2ADD: { Val a = S(0), b = S(1), c = extract(s, S(2), 0); result(s, i, dot(s, a, b, 2, &c)); return; }
    case O_M4X4: case O_M4X3: case O_M3X4: case O_M3X3: case O_M3X2: {
        int cols = i->op == O_M4X4 || i->op == O_M4X3 ? 4 : 3;
        int rows = i->op == O_M4X4 || i->op == O_M3X4 ? 4 : i->op == O_M3X2 ? 2 : 3;
        Val a = S(0), comps[4];
        for (int r = 0; r < rows; r++) {
            Src row = i->src[1];
            row.num += r;
            comps[r] = dot(s, a, src(s, &row, 4), cols, NULL);
        }
        for (int r = rows; r < 4; r++) comps[r] = cf(s, 0);
        Ins dd = *i;
        dd.dst.mask &= (1 << rows) - 1;
        dd.dst.centroid = 0;
        result(s, &dd, construct(s, comps, 4));
        return;
    }
    case O_LIT: {
        Val x0 = S(0);
        Val x = extract(s, x0, 0), y = extract(s, x0, 1), w = extract(s, x0, 3);
        Val pw = clampv(s, w, -127.9961f, 127.9961f);
        Val z = cf(s, 0), one = cf(s, 1);
        Val ok = {OP(s, OpLogicalAnd, t_bool(m), cmpv(s, OpFOrdGreaterThanEqual, x, z).id, cmpv(s, OpFOrdGreaterThanEqual, y, z).id),
                  t_bool(m), 1};
        Val pz = sel(s, ok, ext1(s, GLSL_Exp2, mul(s, pw, ext1(s, GLSL_Log2, y), 0)), z);
        Val c[4] = {one, ext2(s, GLSL_FMax, x, z), pz, one};
        result(s, i, construct(s, c, 4));
        return;
    }
    case O_TEX: {
        if (!s->ps || s->major != 1 || s->minor >= 4) { FAIL(s, "tex in this shader model"); return; }
        int sl = i->dst.num;
        Val coord = project(s, sl, texcoord_in(s, sl));
        result(s, i, sample(s, sl, coord));
        return;
    }
    case O_TEXCOORD: {
        Val t = saturate(s, texcoord_in(s, i->dst.num));
        Val c[4] = {extract(s, t, 0), extract(s, t, 1), extract(s, t, 2), cf(s, 1)};
        result(s, i, construct(s, c, 4));
        return;
    }
    case O_TEXKILL: texkill(s, i); return;
    case O_TEXBEM: {
        int sl = i->dst.num;
        if (sl >= 4) { FAIL(s, "texbem stage %d", sl); return; }
        Val coord = project(s, sl, texcoord_in(s, sl));
        Val t = S(0);
        Val mat = extra(s, 2 + sl);
        Val tx = extract(s, t, 0), ty = extract(s, t, 1);
        /* (DXVK 3.1.1 with larush-native's texbem patch, sm3_converter.cpp applyBumpMapping: the coordinate plus
           M00 du, plus M10 dv, each add fused with its legacy multiply by dxbc-spirv's ArithmeticPass fuseMad) */
        Val u = mad(s, extract(s, mat, 2), ty, mad(s, extract(s, mat, 0), tx, extract(s, coord, 0), 0), 0);
        Val v = mad(s, extract(s, mat, 3), ty, mad(s, extract(s, mat, 1), tx, extract(s, coord, 1), 0), 0);
        Val c[4] = {u, v, extract(s, coord, 2), extract(s, coord, 3)};
        result(s, i, sample(s, sl, construct(s, c, 4)));
        return;
    }
    case O_TEXM3X2PAD: case 73 /* texm3x3pad */: return;
    /* (the texm3x3 family, as dxbc-spirv's sm3_converter.cpp emitTexMatMul and handleTextureSample: the
       three texture coordinates' xyz dotted with the source's xyz (legacy dot products), then texm3x3tex samples at
       them, texm3x3spec / vspec at the reflection of the eye ray (spec: the second source; vspec: the three
       coordinates' w) about them, both normalised (x times the inverse square root of x.x), 2 (n.e / n.n) n - e
       with the multiply and the subtraction fused (dxbc-spirv's ArithmeticPass fuseMad: a * b - c is
       FMadLegacy(a, b, -c)); w 0 [verified: a chrome object one game draws with texm3x3spec, its pixel shader
       skipped before as "instruction 73", then the same as DXVK's in frame dumps]) */
    case 74: case 76: case 77: {
        int n = i->dst.num;
        if (n < 2) { FAIL(s, "texm3x3 to t%d", n); return; }
        Val tn = src(s, &i->src[0], 3);
        Val nc[3];
        for (int r = 0; r < 3; r++) nc[r] = dot(s, texcoord_in(s, n - 2 + r), tn, 3, NULL);
        Val coord;
        if (i->op == 74) {
            Val c[4] = {nc[0], nc[1], nc[2], cf(s, 0)};
            coord = construct(s, c, 4);
        } else {
            Val eye;
            if (i->op == 77) {
                Val e[3];
                for (int r = 0; r < 3; r++) e[r] = extract(s, texcoord_in(s, n - 2 + r), 3);
                eye = construct(s, e, 3);
            } else eye = src(s, &i->src[1], 3);
            Val nrm = construct(s, nc, 3);
            eye = mul(s, eye, splat(s, ext1(s, GLSL_InverseSqrt, dot(s, eye, eye, 3, NULL)), 3), 0);
            nrm = mul(s, nrm, splat(s, ext1(s, GLSL_InverseSqrt, dot(s, nrm, nrm, 3, NULL)), 3), 0);
            Val ne = dot(s, nrm, eye, 3, NULL), nn = dot(s, nrm, nrm, 3, NULL);
            Val two = mul(s, cf(s, 2), sv_fdiv(s, ne, nn), 0);
            Val r3 = mad(s, nrm, splat(s, two, 3), neg(s, eye), 0);
            Val c[4] = {extract(s, r3, 0), extract(s, r3, 1), extract(s, r3, 2), cf(s, 0)};
            coord = construct(s, c, 4);
        }
        result(s, i, sample(s, n, coord));
        return;
    }
    case O_TEXM3X2TEX: {
        int n = i->dst.num;
        Val tn = src(s, &i->src[0], 3);
        Val u = dot(s, texcoord_in(s, n - 1), tn, 3, NULL), w = dot(s, texcoord_in(s, n), tn, 3, NULL);
        Val c[4] = {u, w, cf(s, 0), cf(s, 0)};
        result(s, i, sample(s, n, construct(s, c, 4)));
        return;
    }
    }
#undef S
    FAIL(s, "instruction %d", i->op);
}

/* ---- the end of the program */
static Val fog_on(Sh *s, void *ctx, int arg)
{
    (void)arg;
    Val col = *(Val *)ctx;
    Mod *m = &s->m;
    Val e0 = extra(s, 0), e1 = extra(s, 1);
    Val fc = frag_coord(s);
    Val depth = sv_fdiv(s, cf(s, 1), extract(s, fc, 3));      /* (W fog: a vertex shader is never Z fog) */
    if (!s->vfog) {
        s->vfog = gvar(m, t_ptr(m, SC_Input, t_f32(m)), SC_Input);
        deco1(m, s->vfog, DEC_Location, VARY_FOG);
    }
    Val vf = {OP(s, OpLoad, t_f32(m), s->vfog), t_f32(m), 1};
    Val lin = mul(s, sub(s, extract(s, e1, 0), depth), extract(s, e1, 1), 0);
    Val d1 = mul(s, depth, extract(s, e0, 3), 0);
    Val ex = ext1(s, GLSL_Exp2, sv_fmul(s, d1, cf(s, -1.442695041f)));
    Val ex2 = ext1(s, GLSL_Exp2, sv_fmul(s, mul(s, d1, d1, 0), cf(s, -1.442695041f)));
    Val spec = *(Val *)((char *)ctx + sizeof(Val));
#define IS(k) ((Val){OP(s, OpIEqual, t_bool(m), spec.id, c_u32(m, (k)).id), t_bool(m), 1})
    Val c4v = IS(4), c3v = IS(3), c2v = IS(2);
    Val f = sel(s, c4v, lin, sel(s, c3v, ex2, sel(s, c2v, ex, vf)));
#undef IS
    f = ext2(s, GLSL_FMax, ext2(s, GLSL_FMin, f, cf(s, 1)), cf(s, 0));
    /* (the fog colour: its bytes (vk_d3d9.c consts_ps) divided by 255 here, as dxbc-spirv's emitFog reads
       DXVK's push data: OpFDiv on the GPU, not the CPU's correctly rounded quotient; with the CPU's,
       single pixels of fogged fly-by frames were a level off [verified: <PORT_ENV>_DRAWSHOT 1550 draw 30 with
       <PORT_ENV>_DRAWSHOT_RS=28=0 identical, DXVK's SPIR-V from DXVK_SHADER_DUMP_PATH]) */
    static const int rgb3[3] = {0, 1, 2};
    Val f255 = cf(s, 255.0f);
    Val fcc[3] = {sv_fdiv(s, extract(s, e0, 0), f255), sv_fdiv(s, extract(s, e0, 1), f255), sv_fdiv(s, extract(s, e0, 2), f255)};
    Val fcol = construct(s, fcc, 3);
    /* (sm3_converter.cpp emitFog: fogColor + (colour - fogColor) x factor, the add fused with the multiply
       by fuseMad: one rounding; unfused, single pixels of fogged frames were a level off [verified: frame
       dumps 1350-1550, <PORT_ENV>_DRAWSHOT]) */
    Val rgb = ext3(s, GLSL_Fma, sub(s, shuffle(s, col, rgb3, 3), fcol), splat(s, f, 3), fcol);
    Val c[4] = {extract(s, rgb, 0), extract(s, rgb, 1), extract(s, rgb, 2), extract(s, col, 3)};
    return construct(s, c, 4);
}
static Val fog_off(Sh *s, void *ctx, int arg) { (void)s; (void)arg; return *(Val *)ctx; }
static Val fog(Sh *s, Val col)
{
    Mod *m = &s->m;
    Val spec = spec_u32(m, VK_SPEC_FOG, 0);
    Val on = {OP(s, OpINotEqual, t_bool(m), spec.id, c_u32(m, 0).id), t_bool(m), 1};
    Val ctx[2] = {col, spec};
    Case cs[2] = {{on, 1, fog_on, 0}, {{0, 0, 0}, 0, fog_off, 0}};
    return branch(s, ctx, cs, 2);
}
static void alpha_test(Sh *s, Val col)
{
    Mod *m = &s->m;
    Val spec = spec_u32(m, VK_SPEC_ALPHA, 8);
    uint32_t on = OP(s, OpULessThan, t_bool(m), spec.id, c_u32(m, 8).id);
    uint32_t lt = mnew(m), merge = mnew(m);
    INS(&m->code, OpSelectionMerge, merge, 0);
    INS(&m->code, OpBranchConditional, on, lt, merge);
    lab(s, lt);
    Val e1 = extra(s, 1);
    Val a = extract(s, col, 3), scale = extract(s, e1, 3), ref = extract(s, e1, 2);
    Val ints = cmpv(s, OpFOrdGreaterThan, scale, cf(s, 0));
    a = sel(s, ints, ext1(s, GLSL_RoundEven, sv_fmul(s, a, scale)), a);
    static const struct { uint32_t k, op; } tests[6] = {{2, OpFOrdLessThan}, {3, OpFOrdEqual}, {4, OpFOrdLessThanEqual},
        {5, OpFOrdGreaterThan}, {6, OpFOrdNotEqual}, {7, OpFOrdGreaterThanEqual}};
    Val passed = c_bool(m, 0);
    for (int k = 0; k < 6; k++) {
        Val c = cmpv(s, tests[k].op, a, ref);
        uint32_t is = OP(s, OpIEqual, t_bool(m), spec.id, c_u32(m, tests[k].k).id);
        passed = (Val){OP(s, OpSelect, t_bool(m), is, c.id, passed.id), t_bool(m), 1};
    }
    uint32_t kill = mnew(m), merge2 = mnew(m);
    INS(&m->code, OpSelectionMerge, merge2, 0);
    INS(&m->code, OpBranchConditional, passed.id, merge2, kill);
    lab(s, kill);
    INS0(&m->code, OpDemoteToHelperInvocation);
    INS(&m->code, OpBranch, merge2);
    lab(s, merge2);
    INS(&m->code, OpBranch, merge);
    lab(s, merge);
}
static void finish_ps(Sh *s)
{
    Mod *m = &s->m;
    uint32_t out = gvar(m, t_ptr(m, SC_Output, fv(s, 4)), SC_Output);
    deco1(m, out, DEC_Location, 0);
    Val col = load_reg(s, R_TEMP, 0);
    col = fog(s, col);
    alpha_test(s, col);
    INS(&m->code, OpStore, out, col.id);
}
static void finish_vs(Sh *s)
{
    Mod *m = &s->m;
    uint32_t pos = gvar(m, t_ptr(m, SC_Output, fv(s, 4)), SC_Output);
    deco1(m, pos, DEC_BuiltIn, 0);
    uint32_t psz = gvar(m, t_ptr(m, SC_Output, t_f32(m)), SC_Output);
    deco1(m, psz, DEC_BuiltIn, 1);
    uint32_t outs[NVARY] = {0}, opos = 0, opsz = 0;
    for (int i = 0; i < s->nouts; i++) {
        int t = s->outs[i].tgt;
        if (t == TGT_POS) opos = s->outs[i].var;
        else if (t == TGT_PSIZE) opsz = s->outs[i].var;
        else outs[t] = s->outs[i].var;
    }
    if (opos) INS(&m->code, OpStore, pos, OP(s, OpLoad, fv(s, 4), opos));
    else INS(&m->code, OpStore, pos, c4(s, 0, 0, 0, 1).id);
    if (opsz) INS(&m->code, OpStore, psz, extract(s, load4(s, opsz), 0).id);
    else INS(&m->code, OpStore, psz, cf(s, 1).id);
    for (int loc = 0; loc < NVARY; loc++) {
        if (loc == VARY_FOG) {
            uint32_t var = gvar(m, t_ptr(m, SC_Output, t_f32(m)), SC_Output);
            deco1(m, var, DEC_Location, (uint32_t)loc);
            Val v = outs[loc] ? extract(s, load4(s, outs[loc]), 0) : cf(s, 1);
            INS(&m->code, OpStore, var, v.id);
            continue;
        }
        uint32_t var = gvar(m, t_ptr(m, SC_Output, fv(s, 4)), SC_Output);
        deco1(m, var, DEC_Location, (uint32_t)loc);
        Val v;
        if (outs[loc]) {
            v = load4(s, outs[loc]);
            if (loc < VARY_TEX) v = saturate(s, v);
        } else if (loc == VARY_COLOR) v = c4(s, 1, 1, 1, 1);
        else if (loc == VARY_COLOR + 1) v = c4(s, 0, 0, 0, 1);
        else v = c4(s, 0, 0, 0, 0);
        INS(&m->code, OpStore, var, v.id);
    }
}

/* the whole shader: its SPIR-V (NULL: it cannot be, s->m.err says why) */
static uint32_t *compile(Sh *s, const uint32_t *code, uint32_t ntok, uint32_t *nwords)
{
    Mod *m = &s->m;
    m->bound = 1;
    m->glsl = mnew(m);
    uint32_t v = code[0];
    s->ps = v >> 16 == 0xffff;
    s->major = (int)((v >> 8) & 0xff), s->minor = (int)(v & 0xff);
    if (s->major != 1 || (s->ps && s->minor >= 4)) { FAIL(s, "%s_%d_%d", s->ps ? "ps" : "vs", s->major, s->minor); return NULL; }
    wpush(&m->caps, 1);                                  /* Shader */
    if (s->ps) { wpush(&m->caps, 5302); wpush(&m->caps, 50); wpush(&m->caps, 5379); }
    /* decode */
    s->prog = calloc(ntok, sizeof *s->prog);
    for (uint32_t k = 1; k < ntok; ) {
        uint32_t t = code[k], op = t & 0xffff, n;
        if (t == 0xffff) break;
        if (op == 0xfffe) { k += 1 + ((t >> 16) & 0x7fff); continue; }
        if (op == O_DEF) n = 5;
        else for (n = 0; k + 1 + n < ntok && (code[k + 1 + n] & 0x80000000u); n++) ;
        Ins *i = &s->prog[s->nprog++];
        i->op = (int)op;
        i->co = (t >> 30) & 1;
        i->raw = code + k + 1;
        i->nraw = (int)n;
        if (op == O_DCL) {
            i->dcl = code[k + 1];
            i->dst = dst_of(code[k + 2]);
        } else if (op == O_DEF) {
            i->dst = dst_of(code[k + 1]);
        } else {
            int hd, ns;
            if (!op_info((int)op, &hd, &ns)) { FAIL(s, "opcode %u", op); return NULL; }
            uint32_t p = 0;
            if (hd && n) { i->has_dst = 1; i->dst = dst_of(code[k + 1]); p = 1; }
            while (p < n && i->nsrc < 4) i->src[i->nsrc++] = src_of(code[k + 1 + p++]);
        }
        k += 1 + n;
    }
    uint32_t vd = t_void(m);
    s->fn = mnew(m);
    s->entry_label = mnew(m);
    s->cur_label = s->entry_label;
    /* the constant buffer */
    uint32_t arr = t_array(m, fv(s, 4), s->ps ? VK_PS_EXTRA + VK_PS_CONSTS : VK_VS_CONSTS, 16);
    uint32_t st = mnew(m);
    INS(&m->glob, OpTypeStruct, st, arr);
    deco(m, st, DEC_Block);
    INS(&m->annot, OpMemberDecorate, st, 0, DEC_Offset, 0);
    if (s->ps) s->cbuf = gvar(m, t_ptr(m, SC_Uniform, st), SC_Uniform);
    else {
        uint32_t blocks = mnew(m);
        INS(&m->glob, OpTypeArray, blocks, st, c_u32(m, VK_VS_CHUNKS).id);
        s->cbuf = gvar(m, t_ptr(m, SC_Uniform, blocks), SC_Uniform);
    }
    deco1(m, s->cbuf, DEC_DescriptorSet, 1);
    deco1(m, s->cbuf, DEC_Binding, s->ps ? 1 : 0);
    deco(m, s->cbuf, DEC_NonWritable);
    /* declarations and definitions first, then the program without them */
    int n = 0;
    for (int k = 0; k < s->nprog; k++) {
        Ins *i = &s->prog[k];
        if (i->op == O_DCL) {
            if (s->ps) continue;                         /* (ps_1_x has none) */
            if (s->ndcls < 32) s->dcls[s->ndcls++] = (Dcl){i->dst.typ, i->dst.num, (int)(i->dcl & 31), (int)((i->dcl >> 16) & 15)};
            continue;
        }
        if (i->op == O_DEF) {
            if (s->ndefs < 96) s->defs[s->ndefs++] = (Def){i->dst.typ, i->dst.num, c_vecb(m, i->raw + 1, 4)};
            if (i->dst.typ == R_CONST && s->ndefl < 96) {
                s->defl[s->ndefl].reg = (uint32_t)i->dst.num;
                memcpy(s->defl[s->ndefl].v, i->raw + 1, 16);
                s->ndefl++;
            }
            continue;
        }
        s->prog[n] = *i;
        s->prog[n].idx = n;
        n++;
    }
    s->nprog = n;
    fusions(s);
    for (int k = 0; k < s->nprog && !m->err[0]; k++) {
        Ins *i = &s->prog[k];
        if (k + 1 < s->nprog && s->prog[k + 1].co) {
            s->defer = 1;
            s->ndeferred = 0;
            instr(s, i);
            instr(s, &s->prog[k + 1]);
            s->defer = 0;
            for (int d = 0; d < s->ndeferred; d++) dst_store(s, &s->deferred[d].d, s->deferred[d].v);
            k++;
            continue;
        }
        instr(s, i);
    }
    if (m->err[0]) return NULL;
    if (s->ps) finish_ps(s);
    else finish_vs(s);
    if (m->err[0]) return NULL;
    /* the function: its variables, the registers at zero and the outputs at DXVK's defaults, the body */
    W body = m->code;
    memset(&m->code, 0, sizeof m->code);
    W fnw = {0};
    INS(&fnw, OpFunction, vd, s->fn, 0, t_func(m, vd));
    INS(&fnw, OpLabel, s->entry_label);
    wcat(&fnw, &m->vars);
    Val z = c4(s, 0, 0, 0, 0);
    for (int r = 0; r < s->nregs; r++) INS(&fnw, OpStore, s->regs[r].var, z.id);
    for (int o = 0; o < s->nouts; o++) {
        int t = s->outs[o].tgt;
        Val d = t == VARY_COLOR ? c4(s, 1, 1, 1, 1) : t == VARY_COLOR + 1 ? c4(s, 0, 0, 0, 1) :
                t == VARY_FOG || t == TGT_PSIZE ? c4(s, 1, 0, 0, 0) : c4(s, 0, 0, 0, 0);
        INS(&fnw, OpStore, s->outs[o].var, d.id);
    }
    wcat(&fnw, &body);
    INS0(&fnw, OpReturn);
    INS0(&fnw, OpFunctionEnd);
    free(body.w);
    /* float controls as DXVK's shaders ask (vk_d3d9.c module_get keeps what the device has) */
    wpush(&m->caps, 4467);
    wpush(&m->caps, 4465);
    wpush(&m->caps, 6029);
    uint32_t tf = t_f32(m), fc2 = c_u32(m, FC2_DEFAULT).id;
    W out = {0};
    wpush(&out, 0x07230203);
    wpush(&out, 0x00010600);
    wpush(&out, 0x4c615275);
    wpush(&out, 0);                                      /* (the bound: below) */
    wpush(&out, 0);
    for (uint32_t c = 0; c < m->caps.n; c++) INS(&out, OpCapability, m->caps.w[c]);
    {   /* OpExtension "SPV_KHR_float_controls2" */
        static const uint32_t name[6] = {0x5f565053, 0x5f52484b, 0x616f6c66, 0x6f635f74, 0x6f72746e, 0x0032736c};
        ins_a(&out, OpExtension, name, 6);
    }
    {   /* OpExtInstImport "GLSL.std.450" */
        uint32_t a[5] = {m->glsl, 0x4c534c47, 0x6474732e, 0x3035342e, 0};
        ins_a(&out, OpExtInstImport, a, 5);
    }
    INS(&out, OpMemoryModel, 0, 1);
    {
        uint32_t a[64] = {s->ps ? 4u : 0u, s->fn, 0x6e69616d, 0};       /* "main" */
        uint32_t na = 4;
        for (uint32_t k = 0; k < m->iface.n && na < 64; k++) a[na++] = m->iface.w[k];
        ins_a(&out, OpEntryPoint, a, na);
    }
    if (s->ps) INS(&out, OpExecutionMode, s->fn, 7);     /* OriginUpperLeft */
    INS(&out, OpExecutionMode, s->fn, 4462, 32);         /* RoundingModeRTE */
    INS(&out, OpExecutionMode, s->fn, 4460, 32);         /* DenormFlushToZero */
    INS(&out, 331, s->fn, 6028, tf, fc2);                /* OpExecutionModeId FPFastMathDefault */
    wcat(&out, &m->annot);
    wcat(&out, &m->glob);
    wcat(&out, &fnw);
    free(fnw.w);
    out.w[3] = m->bound;
    *nwords = out.n;
    return out.w;
}

static void sh_free(Sh *s)
{
    Mod *m = &s->m;
    free(m->caps.w); free(m->exts.w); free(m->annot.w); free(m->glob.w); free(m->vars.w); free(m->code.w); free(m->iface.w);
    free(m->keys);
    free(s->prog);
    free(s->fuse);
    free(s->deferred);
}

/* ------------------------------------------------------------------ the registry: one per bytecode hash */
typedef struct { uint64_t hash; VkShaderMeta *meta; } Entry;
static Entry *reg_e;
static int nreg_e, capreg_e;
static pthread_mutex_t reg_lock = PTHREAD_MUTEX_INITIALIZER;
int vk_spv_count;

const VkShaderMeta *vk_spv_find(uint64_t hash)
{
    pthread_mutex_lock(&reg_lock);
    const VkShaderMeta *r = NULL;
    for (int i = 0; i < nreg_e; i++) if (reg_e[i].hash == hash) { r = reg_e[i].meta; break; }
    pthread_mutex_unlock(&reg_lock);
    return r;
}

/* THE SAVED SPIR-V: every shader compiled here is kept in the
   user folder (vk_spirv.bin: its bytecode hash, metadata, def'd constants and SPIR-V; made from the
   player's own files, never in the repository), read back at the start, so the pipelines listed from
   the last runs (vk_d3d9.c vk_pipelines.bin) that use the game's shaders are made by the workers while
   the game loads, not at their first draw, and the compiler does not run again. The file is tagged with
   the compiler's build (VK_SPV_BUILD: this file's hash, from the Makefile): another build's entries are
   not read. <PORT_ENV>_VK_SPIRV_CACHE=0: neither read nor written. */
#define SPV_MAGIC "BALSPV01"
#ifndef VK_SPV_BUILD
#define VK_SPV_BUILD 0u
#endif
static int spv_cache = -1;
static int spv_cache_on(void)
{
    if (spv_cache < 0) spv_cache = !(port_getenv("VK_SPIRV_CACHE") && *port_getenv("VK_SPIRV_CACHE") == '0');
    return spv_cache;
}
static void reg_add(uint64_t hash, VkShaderMeta *mt)
{
    if (nreg_e == capreg_e) {
        capreg_e = capreg_e ? capreg_e * 2 : 32;
        reg_e = realloc(reg_e, (size_t)capreg_e * sizeof *reg_e);
    }
    reg_e[nreg_e++] = (Entry){hash, mt};
}
typedef struct {
    uint64_t hash;
    uint8_t major, minor, flags, pad;
    uint32_t inputs, samplers, kinds, proj, cmax, ndef, nwords;
} SpvRec;
static int spv_loaded;

/* at the start (vk_d3d9.c, before the listed pipelines are made): the saved shaders into the registry */
void vk_spv_load(void)
{
    if (spv_loaded++ || !spv_cache_on()) return;
    char path[1200];
    snprintf(path, sizeof path, "%s/vk_spirv.bin", user_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char magic[8];
    uint32_t build = 0, n = 0, got = 0;
    if (fread(magic, 8, 1, f) != 1 || memcmp(magic, SPV_MAGIC, 8) || fread(&build, 4, 1, f) != 1 || fread(&n, 4, 1, f) != 1 ||
        build != (uint32_t)VK_SPV_BUILD) {
        LOG(LOG_ALWAYS, "vulkan renderer: %s is of another build of the compiler: not read", path);
        fclose(f);
        return;
    }
    pthread_mutex_lock(&reg_lock);
    for (uint32_t i = 0; i < n && i < 4096; i++) {
        SpvRec r;
        if (fread(&r, sizeof r, 1, f) != 1 || r.ndef > 256 || r.nwords > (1u << 20)) break;
        VkShaderMeta *mt = calloc(1, sizeof *mt);
        VkShaderDef *d = r.ndef ? malloc(r.ndef * sizeof *d) : NULL;
        uint32_t *w = malloc(r.nwords * 4u);
        if ((r.ndef && fread(d, sizeof *d, r.ndef, f) != r.ndef) || fread(w, 4, r.nwords, f) != r.nwords) {
            free(mt); free(d); free(w);
            break;
        }
        int dup = 0;
        for (int k = 0; k < nreg_e; k++) dup |= reg_e[k].hash == r.hash;
        if (dup) { free(mt); free(d); free(w); continue; }
        *mt = (VkShaderMeta){r.hash, w, r.nwords, r.major, r.minor, r.flags, r.inputs, r.samplers, r.kinds, r.proj, r.cmax,
                             d, r.ndef, NULL, 0, 0, 0};
        reg_add(r.hash, mt);
        got++;
    }
    pthread_mutex_unlock(&reg_lock);
    fclose(f);
    LOG(LOG_ALWAYS, "vulkan renderer: %u saved shaders read (%s)", got, path);
}

/* (under reg_lock) the registry written out, when a new shader was compiled */
static void spv_save_locked(void)
{
    if (!spv_cache_on() || !user_dir[0]) return;
    char path[1200], tmp[1300];
    snprintf(path, sizeof path, "%s/vk_spirv.bin", user_dir);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    uint32_t build = (uint32_t)VK_SPV_BUILD, n = (uint32_t)nreg_e;
    int ok = fwrite(SPV_MAGIC, 8, 1, f) == 1 && fwrite(&build, 4, 1, f) == 1 && fwrite(&n, 4, 1, f) == 1;
    for (int i = 0; ok && i < nreg_e; i++) {
        const VkShaderMeta *m = reg_e[i].meta;
        SpvRec r = {m->hash, m->major, m->minor, m->flags, 0, m->inputs, m->samplers, m->kinds, m->proj, m->cmax, m->ndef, m->nwords};
        ok = fwrite(&r, sizeof r, 1, f) == 1 && (!m->ndef || fwrite(m->defs, sizeof *m->defs, m->ndef, f) == m->ndef) &&
             fwrite(m->words, 4, m->nwords, f) == m->nwords;
    }
    if (fclose(f) == 0 && ok) rename(tmp, path);
    else remove(tmp);
}

const VkShaderMeta *vk_spv_compile(const uint32_t *code, uint32_t ntokens, uint64_t hash)
{
    pthread_mutex_lock(&reg_lock);
    for (int i = 0; i < nreg_e; i++)
        if (reg_e[i].hash == hash) {
            const VkShaderMeta *r = reg_e[i].meta;
            pthread_mutex_unlock(&reg_lock);
            return r;
        }
    pthread_mutex_unlock(&reg_lock);
    double t0 = vk_spv_ms();
    Sh *s = calloc(1, sizeof *s);
    uint32_t nw = 0;
    uint32_t *w = code && ntokens ? compile(s, code, ntokens, &nw) : NULL;
    VkShaderMeta *mt = NULL;
    if (!w) {
        LOG(LOG_ALWAYS, "vulkan renderer: shader %016llx cannot be compiled: %s", (unsigned long long)hash,
            s->m.err[0] ? s->m.err : "no code");
    } else {
        mt = calloc(1, sizeof *mt);
        mt->hash = hash;
        mt->words = w;
        mt->nwords = nw;
        mt->major = (uint8_t)s->major;
        mt->minor = (uint8_t)s->minor;
        mt->flags = (uint8_t)((s->ps ? VK_SH_PIXEL : 0) | (s->rel ? VK_SH_RELATIVE : 0) | (s->kills ? VK_SH_KILL : 0));
        mt->inputs = s->inputs;
        mt->samplers = s->samplers;
        mt->kinds = 0;                                   /* (ps_1_x: any, by the spec constant) */
        mt->proj = s->proj_used;
        mt->cmax = s->cmax;
        if (s->ndefl) {
            VkShaderDef *d = malloc((size_t)s->ndefl * sizeof *d);
            memcpy(d, s->defl, (size_t)s->ndefl * sizeof *d);
            mt->defs = d;
            mt->ndef = (uint32_t)s->ndefl;
        }
        LOG(LOG_ALWAYS, "vulkan renderer: %s_%d_%d shader %016llx compiled (%u words, %.2f ms)", s->ps ? "ps" : "vs", s->major,
            s->minor, (unsigned long long)hash, nw, vk_spv_ms() - t0);
    }
    sh_free(s);
    free(s);
    pthread_mutex_lock(&reg_lock);
    if (mt) {
        reg_add(hash, mt);
        vk_spv_count++;
        spv_save_locked();
    }
    pthread_mutex_unlock(&reg_lock);
    return mt;
}

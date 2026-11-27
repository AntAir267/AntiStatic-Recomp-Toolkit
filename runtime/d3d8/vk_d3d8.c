/* Our own Vulkan renderer: Direct3D 8. The IDirect3D8 objects runtime/d3d8.c and
   the generated wrappers (gen/d3d8_gen.c) call, as they called DXVK's, over our Direct3D 9 renderer
   (vk_d3d9.c, etm-native's). This is DXVK 3.1.1's d3d8 front end (third_party/dxvk/src/d3d8, the
   reference for each mapping) written in C, so the program sees the same objects and answers under
   both renderers (its memory must not depend on the renderer: a port's A/B runs):
   - The objects' references as DXVK's: public and private counts (ComObjectClamp), device children
     holding the device while referenced, a texture's surfaces made at the first ask and sharing the
     texture's count, the device's private references on what is bound (textures, streams, indices,
     targets) so a released object lives until it is unbound, private data (the bridge's death
     watchers) let go when the object goes; destruction in DXVK's order.
   - The state: render states and texture stage states mapped to Direct3D 9's (ZBIAS, the sampler
     states), SetIndices' base vertex, vertex shader handles and FVFs, D3D8 vertex shader declarations
     and bytecode turned into Direct3D 9's (dcl instructions, constants from the declaration as def,
     the scalar instructions' replicate swizzle: DXVK's TranslateVertexShader8), pixel shader handles.
   - Present parameters, caps (ConvertCaps8), surface descriptions with their sizes, CopyRects by the
     pools as DXVK's table.
   Not here: state blocks are passed to Direct3D 9's without DXVK's Direct3D 8 recording of textures,
   streams and indices (the route makes none: <PORT_ENV>_D3D_INVENTORY), volume textures and additional swap
   chains (our Direct3D 9 has none), DXVK's batcher and options (all off by default there).
   ALTERED SOURCE: a C rewrite of DXVK 3.1.1's src/d3d8 (zlib/libpng licence; DXVK's notice in NOTICE). */
#define _GNU_SOURCE
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "vk.h"
#include "d3d8_gen.h"
#include "options.h"
#include "win.h"

/* our Direct3D 9 (vk_d3d9.c): what DXVK's d3d8 front end asks of DXVK's through its bridge */
int vk9_format_supported(D3DFORMAT f);
HRESULT vk9_update_from_buffer(void *src, void *dst, const RECT *sr, const POINT *dp);
void vk9_set_vsync(int on);

#define V9(T, o) (((T *)(o))->lpVtbl)
#define UNSUPPORTED8(name) do { static int _s; if (!_s++) LOG(LOG_ALWAYS, "vulkan renderer: Direct3D 8 %s is not supported", name); } while (0)

/* ------------------------------------------------------------------ Direct3D 8's own types (the host's
   layout, as runtime/d3d8.c and the wrappers hand them over: DXVK's d3d8.h; this file has Direct3D 9's) */
typedef struct {
    UINT BackBufferWidth, BackBufferHeight;
    D3DFORMAT BackBufferFormat;
    UINT BackBufferCount;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    D3DSWAPEFFECT SwapEffect;
    HWND hDeviceWindow;
    WINBOOL Windowed, EnableAutoDepthStencil;
    D3DFORMAT AutoDepthStencilFormat;
    DWORD Flags;
    UINT FullScreen_RefreshRateInHz, FullScreen_PresentationInterval;
} PP8;
typedef struct { D3DFORMAT Format; D3DRESOURCETYPE Type; DWORD Usage; D3DPOOL Pool; UINT Size; D3DMULTISAMPLE_TYPE MultiSampleType;
                 UINT Width, Height; } SurfDesc8;
typedef struct {
    char Driver[512], Description[512];
    LARGE_INTEGER DriverVersion;
    DWORD VendorId, DeviceId, SubSysId, Revision;
    GUID DeviceIdentifier;
    DWORD WHQLLevel;
} AdapterId8;
/* D3DCAPS8 is D3DCAPS9 up to its PixelShader1xMaxValue (Direct3D 8's MaxPixelShaderValue) */
#define CAPS8_SIZE (offsetof(D3DCAPS9, PixelShader1xMaxValue) + sizeof(float))

/* Direct3D 8's render states and texture stage states that Direct3D 9 numbers otherwise */
enum { RS8_LINEPATTERN = 10, RS8_ZVISIBLE = 30, RS8_EDGEANTIALIAS = 40, RS8_ZBIAS = 47, RS8_SOFTWAREVERTEXPROCESSING = 153,
       RS8_PATCHSEGMENTS = 164 };
#define ZBIAS_SCALE (-1.0f / ((1u << 16) - 1))      /* (DXVK's d8caps: D16's precision) */
#define ZBIAS_SCALE_INV (1 / ZBIAS_SCALE)

static int sampler_state9(DWORD t)
{
    switch (t) {
    case 13: return D3DSAMP_ADDRESSU;
    case 14: return D3DSAMP_ADDRESSV;
    case 15: return D3DSAMP_BORDERCOLOR;
    case 16: return D3DSAMP_MAGFILTER;
    case 17: return D3DSAMP_MINFILTER;
    case 18: return D3DSAMP_MIPFILTER;
    case 19: return D3DSAMP_MIPMAPLODBIAS;
    case 20: return D3DSAMP_MAXMIPLEVEL;
    case 21: return D3DSAMP_MAXANISOTROPY;
    case 25: return D3DSAMP_ADDRESSW;
    }
    return -1;
}

static const GUID iid8_d3d = {0x1DD9E8DA, 0x1C77, 0x4D40, {0xB0, 0xCF, 0x98, 0xFE, 0xFD, 0xFF, 0x95, 0x12}};
static const GUID iid8_dev = {0x7385E5DF, 0x8FE8, 0x41D5, {0x86, 0xB6, 0xD7, 0xB4, 0x85, 0x47, 0xB6, 0xCF}};
static const GUID iid8_res = {0x1B36BB7B, 0x09B7, 0x410A, {0xB4, 0x45, 0x7D, 0x14, 0x30, 0xD7, 0xB3, 0x3F}};
static const GUID iid8_vb = {0x8AEEEAC7, 0x05F9, 0x44D4, {0xB5, 0x91, 0x00, 0x0B, 0x0D, 0xF1, 0xCB, 0x95}};
static const GUID iid8_surf = {0xB96EEBCA, 0xB326, 0x4EA5, {0x88, 0x2F, 0x2F, 0xF5, 0xBA, 0xE0, 0x21, 0xDD}};
static const GUID iid8_ib = {0x0E689C9A, 0x053D, 0x44A0, {0x9D, 0x92, 0xDB, 0x0E, 0x3D, 0x75, 0x0F, 0x86}};
static const GUID iid8_base = {0xB4211CFA, 0x51B9, 0x4A9F, {0xAB, 0x78, 0xDB, 0x99, 0xB2, 0xBB, 0x67, 0x8E}};
static const GUID iid8_tex = {0xE4CDD575, 0x2866, 0x4F01, {0xB1, 0x2E, 0x7E, 0xEC, 0xE1, 0xEC, 0x93, 0x58}};
static const GUID iid8_cube = {0x3EE5B968, 0x2ACA, 0x4C34, {0x8B, 0xB5, 0x7E, 0x0C, 0x3D, 0x19, 0xB7, 0x50}};
static const GUID iid_unknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

/* ------------------------------------------------------------------ objects */
typedef struct V8Priv { struct V8Priv *next; GUID guid; void *data; DWORD size; int iunk; } V8Priv;
enum { K8_D3D, K8_DEV, K8_TEX, K8_CUBE, K8_SURF, K8_VB, K8_IB };

typedef struct V8 {
    void **vtbl;
    long refs, priv;            /* DXVK's public and private counts */
    int kind;
    void *d9;                   /* the Direct3D 9 object (a public reference) */
    struct V8 *container;       /* a texture's surface: the texture */
    V8Priv *pd, *pd_last;       /* private data, in the order set */
    DWORD pool, priority, usage;
    struct V8 **subs;           /* textures: the surfaces (levels; cube: level * 6 + face), private references */
    uint32_t nsubs;
    void *blit;                 /* surfaces: CopyRects' stretch target (DXVK's GetBlitImage) */
} V8;

static void *vt8_d3d[16], *vt8_dev[97], *vt8_tex[19], *vt8_cube[19], *vt8_surf[11], *vt8_vb[14], *vt8_ib[14];

/* the device (one) */
typedef struct {
    void *decl, *vs;            /* Direct3D 9's declaration and shader (NULL: fixed function) */
    DWORD *declaration, *function;
    uint32_t ndecl, nfunc;
} VShader8;
static struct {
    V8 o;                       /* d9: the IDirect3DDevice9 */
    V8 *parent;                 /* the IDirect3D8 that made it (a public reference) */
    PP8 pp;
    DWORD line_pattern, zvisible;
    V8 *textures[8];
    struct { V8 *buf; UINT stride; } streams[16];
    V8 *indices;
    UINT base_vertex;
    V8 **backbufs;
    UINT nbackbufs;
    V8 *autods, *rt, *ds;
    VShader8 *vshaders;
    int nvs, capvs;
    void **pshaders;
    int nps, capps;
    DWORD cur_vs, cur_ps;
    DWORD flags;
    int recording;
    void **sblocks;             /* state blocks by token - 1 (Direct3D 9's) */
    DWORD nsblocks, token;
} D;

static void v8_destroy(V8 *o);
static void addref_priv(V8 *o) { if (o) o->priv++; }
static void release_priv(V8 *o)
{
    if (o && --o->priv == 0) v8_destroy(o);
}
/* a private reference slot set (DXVK's Com<T, false> assignment: the old one let go first) */
static void priv_set(V8 **slot, V8 *n)
{
    if (*slot == n) return;
    V8 *old = *slot;
    *slot = n;
    release_priv(old);
    addref_priv(n);
}

static ULONG dev_AddRef8(void *This);
static ULONG dev_Release8(void *This);

static ULONG obj_AddRef(void *This)
{
    V8 *o = This;
    if (o->container) return obj_AddRef(o->container);       /* (a texture's surface: the texture's count) */
    long r = o->refs++;
    if (!r) {
        o->priv++;
        if (o->kind != K8_D3D && o->kind != K8_DEV) dev_AddRef8(&D.o);
    }
    return (ULONG)(r + 1);
}

static ULONG obj_Release(void *This)
{
    V8 *o = This;
    if (o->container) return obj_Release(o->container);
    if (!o->refs) return 0;                                   /* (clamped at zero, as Direct3D's) */
    long r = --o->refs;
    if (!r) {
        int child = o->kind != K8_D3D && o->kind != K8_DEV;
        release_priv(o);
        if (child) dev_Release8(&D.o);
    }
    return (ULONG)r;
}

static void priv_free(V8 *o)
{
    V8Priv *p = o->pd;
    o->pd = o->pd_last = NULL;
    while (p) {
        V8Priv *n = p->next;
        if (p->iunk) { if (p->data) ((ULONG (*)(void *))(*(void ***)p->data)[2])(p->data); }
        else free(p->data);
        free(p);
        p = n;
    }
}

static V8 *v8_new(int kind, void **vtbl, void *d9, DWORD pool)
{
    V8 *o = calloc(1, sizeof *o);
    o->vtbl = vtbl;
    o->kind = kind;
    o->d9 = d9;
    o->pool = pool;
    return o;
}

static void v8_destroy(V8 *o)
{
    switch (o->kind) {
    case K8_TEX: case K8_CUBE:
        for (uint32_t i = 0; i < o->nsubs; i++) {     /* (~D3D8BaseTexture: the surfaces first) */
            V8 *s = o->subs[i];
            o->subs[i] = NULL;
            release_priv(s);
        }
        free(o->subs);
        break;
    case K8_SURF:
        if (o->blit) V9(IDirect3DSurface9, o->blit)->Release(o->blit);
        break;
    default:
        break;
    }
    priv_free(o);                                   /* (~D3D8Resource: the private data, the watchers) */
    if (o->d9) ((ULONG (*)(void *))(*(void ***)o->d9)[2])(o->d9);
    o->vtbl = NULL;
    free(o);
}

static HRESULT obj_QueryInterface(void *This, REFIID riid, void **out)
{
    V8 *o = This;
    if (!out) return E_POINTER;
    *out = NULL;
    int ok = !memcmp(riid, &iid_unknown, sizeof(GUID));
    switch (o->kind) {
    case K8_D3D: ok |= !memcmp(riid, &iid8_d3d, sizeof(GUID)); break;
    case K8_DEV: ok |= !memcmp(riid, &iid8_dev, sizeof(GUID)); break;
    case K8_TEX: ok |= !memcmp(riid, &iid8_tex, sizeof(GUID)) || !memcmp(riid, &iid8_base, sizeof(GUID)) ||
                       !memcmp(riid, &iid8_res, sizeof(GUID)); break;
    case K8_CUBE: ok |= !memcmp(riid, &iid8_cube, sizeof(GUID)) || !memcmp(riid, &iid8_base, sizeof(GUID)) ||
                        !memcmp(riid, &iid8_res, sizeof(GUID)); break;
    case K8_SURF: ok |= !memcmp(riid, &iid8_surf, sizeof(GUID)) || !memcmp(riid, &iid8_res, sizeof(GUID)); break;
    case K8_VB: ok |= !memcmp(riid, &iid8_vb, sizeof(GUID)) || !memcmp(riid, &iid8_res, sizeof(GUID)); break;
    case K8_IB: ok |= !memcmp(riid, &iid8_ib, sizeof(GUID)) || !memcmp(riid, &iid8_res, sizeof(GUID)); break;
    }
    if (!ok) return E_NOINTERFACE;
    obj_AddRef(This);
    *out = This;
    return S_OK;
}

static HRESULT res_GetDevice(void *This, void **out)
{
    (void)This;
    if (!out) return D3DERR_INVALIDCALL;
    dev_AddRef8(&D.o);
    *out = &D.o;
    return D3D_OK;
}

/* private data (DXVK's ComPrivateData: an interface is held, plain data copied; the same GUID replaces) */
static void priv_drop(V8 *o, REFGUID g)
{
    for (V8Priv **pp = &o->pd, *prev = NULL; *pp; prev = *pp, pp = &(*pp)->next)
        if (!memcmp(&(*pp)->guid, g, sizeof(GUID))) {
            V8Priv *p = *pp;
            *pp = p->next;
            if (o->pd_last == p) o->pd_last = prev;
            if (p->iunk) { if (p->data) ((ULONG (*)(void *))(*(void ***)p->data)[2])(p->data); }
            else free(p->data);
            free(p);
            return;
        }
}

static HRESULT res_SetPrivateData(void *This, REFGUID g, const void *data, DWORD size, DWORD flags)
{
    V8 *o = This;
    if ((flags & D3DSPD_IUNKNOWN) && size != sizeof(void *)) return D3DERR_INVALIDCALL;
    priv_drop(o, g);
    V8Priv *p = calloc(1, sizeof *p);
    memcpy(&p->guid, g, sizeof(GUID));
    if (flags & D3DSPD_IUNKNOWN) {
        p->iunk = 1;
        p->data = (void *)data;
        p->size = sizeof(void *);
        if (data) ((ULONG (*)(const void *))(*(void *const *const *)data)[1])(data);
    } else {
        p->data = malloc(size ? size : 1);
        if (size && data) memcpy(p->data, data, size);
        p->size = size;
    }
    if (o->pd_last) o->pd_last->next = p;
    else o->pd = p;
    o->pd_last = p;
    return D3D_OK;
}

static HRESULT res_GetPrivateData(void *This, REFGUID g, void *data, DWORD *size)
{
    V8 *o = This;
    if (!data && !size) return D3DERR_NOTFOUND;
    for (V8Priv *p = o->pd; p; p = p->next)
        if (!memcmp(&p->guid, g, sizeof(GUID))) {
            if (!size) return D3DERR_INVALIDCALL;
            if (!data) { *size = p->size; return D3D_OK; }
            if (*size < p->size) { *size = p->size; return D3DERR_MOREDATA; }
            if (p->iunk) {
                memcpy(data, &p->data, sizeof(void *));
                if (p->data) ((ULONG (*)(void *))(*(void ***)p->data)[1])(p->data);
            } else memcpy(data, p->data, p->size);
            *size = p->size;
            return D3D_OK;
        }
    return D3DERR_NOTFOUND;
}

static HRESULT res_FreePrivateData(void *This, REFGUID g) { priv_drop(This, g); return D3D_OK; }

static DWORD res_SetPriority(void *This, DWORD p)
{
    V8 *o = This;
    DWORD old = o->priority;
    if (o->pool == D3DPOOL_MANAGED) { o->priority = p; return old; }
    return o->priority;
}
static DWORD res_GetPriority(void *This) { return ((V8 *)This)->priority; }

/* the size Direct3D 8's descriptions give (DXVK's getSurfaceSize) */
static UINT fmt_stride8(D3DFORMAT f)
{
    switch ((int)f) {
    case D3DFMT_R3G3B2: case D3DFMT_A8: case D3DFMT_P8: case D3DFMT_L8: case D3DFMT_A4L4: return 1;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_A8R3G3B2:
    case D3DFMT_X4R4G4B4: case D3DFMT_A8P8: case D3DFMT_A8L8: case D3DFMT_V8U8: case D3DFMT_L6V5U5: case D3DFMT_D16_LOCKABLE:
    case D3DFMT_D15S1: case D3DFMT_D16: case D3DFMT_UYVY: case D3DFMT_YUY2: return 2;
    case D3DFMT_R8G8B8: return 3;
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A2B10G10R10: case D3DFMT_G16R16: case D3DFMT_X8L8V8U8:
    case D3DFMT_Q8W8V8U8: case D3DFMT_V16U16: case 65 /* W11V11U10 */: case D3DFMT_A2W10V10U10: case D3DFMT_D32:
    case D3DFMT_D24S8: case D3DFMT_D24X8: case D3DFMT_D24X4S4: return 4;
    case D3DFMT_DXT1: return 8;
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: return 16;
    }
    return 0;
}
static int is_dxt(D3DFORMAT f)
{
    return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5;
}
static UINT surface_size8(D3DFORMAT f, UINT w, UINT h)
{
    if (is_dxt(f)) w = (w + 3) >> 2, h = (h + 3) >> 2;
    return w * h * fmt_stride8(f);
}
static void desc8(const D3DSURFACE_DESC *s9, SurfDesc8 *d)
{
    d->Format = s9->Format;
    d->Type = s9->Type;
    d->Usage = s9->Usage;
    d->Pool = s9->Pool;
    d->Size = surface_size8(s9->Format, s9->Width, s9->Height);
    d->MultiSampleType = s9->MultiSampleType;
    d->Width = s9->Width;
    d->Height = s9->Height;
}

static int is_depth_fmt8(D3DFORMAT f)
{
    return f == D3DFMT_D16_LOCKABLE || f == D3DFMT_D16 || f == D3DFMT_D32 || f == D3DFMT_D15S1 || f == D3DFMT_D24X4S4 ||
           f == D3DFMT_D24S8 || f == D3DFMT_D24X8;
}
static int is_rt_fmt8(D3DFORMAT f)
{
    return f == D3DFMT_X1R5G5B5 || f == D3DFMT_R5G6B5 || f == D3DFMT_X8R8G8B8 || f == D3DFMT_A8R8G8B8 ||
           f == (D3DFORMAT)MAKEFOURCC('N', 'U', 'L', 'L');
}
static int is_d3d9_only_fmt(D3DFORMAT f)
{
    switch ((int)f) {
    case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8: case D3DFMT_A2R10G10B10: case D3DFMT_A16B16G16R16: case D3DFMT_L16:
    case D3DFMT_D32F_LOCKABLE: case D3DFMT_D24FS8: case 84 /* D32_LOCKABLE */: case 85 /* S8_LOCKABLE */:
    case D3DFMT_Q16W16V16U16: case D3DFMT_R16F: case D3DFMT_G16R16F: case D3DFMT_A16B16G16R16F: case D3DFMT_R32F:
    case D3DFMT_G32R32F: case D3DFMT_A32B32G32R32F: case D3DFMT_CxV8U8: case 118 /* A1 */: case 119 /* A2B10G10R10_XR_BIAS */:
        return 1;
    }
    return f == (D3DFORMAT)MAKEFOURCC('D', 'F', '1', '6') || f == (D3DFORMAT)MAKEFOURCC('D', 'F', '2', '4') ||
           f == (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z');
}

/* ------------------------------------------------------------------ IDirect3DSurface8 */
static V8 *surf8_new(void *s9, DWORD pool, V8 *container)
{
    V8 *s = v8_new(K8_SURF, vt8_surf, s9, pool);
    s->container = container;
    return s;
}
static HRESULT surf_GetContainer(void *This, REFIID riid, void **out)
{
    V8 *s = This;
    if (s->container) return obj_QueryInterface(s->container, riid, out);
    return obj_QueryInterface(&D.o, riid, out);
}
static HRESULT surf_GetDesc(void *This, SurfDesc8 *d)
{
    V8 *s = This;
    if (!d) return D3DERR_INVALIDCALL;
    D3DSURFACE_DESC s9;
    HRESULT r = V9(IDirect3DSurface9, s->d9)->GetDesc(s->d9, &s9);
    if (r < 0) return r;
    desc8(&s9, d);
    return D3D_OK;
}
static HRESULT surf_LockRect(void *This, D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags)
{
    V8 *s = This;
    return V9(IDirect3DSurface9, s->d9)->LockRect(s->d9, lr, rect, flags);
}
static HRESULT surf_UnlockRect(void *This)
{
    V8 *s = This;
    return V9(IDirect3DSurface9, s->d9)->UnlockRect(s->d9);
}
static void *surf_blit_image(V8 *s)
{
    if (!s->blit) {
        D3DSURFACE_DESC d;
        V9(IDirect3DSurface9, s->d9)->GetDesc(s->d9, &d);
        void *img = NULL;
        if (V9(IDirect3DDevice9, D.o.d9)->CreateRenderTarget(D.o.d9, d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
                                                             (IDirect3DSurface9 **)&img, NULL) < 0)
            return NULL;
        s->blit = img;
    }
    return s->blit;
}

/* ------------------------------------------------------------------ IDirect3DTexture8 / IDirect3DCubeTexture8 */
static V8 *tex8_new(int cube, void *t9, DWORD pool)
{
    V8 *t = v8_new(cube ? K8_CUBE : K8_TEX, cube ? vt8_cube : vt8_tex, t9, pool);
    DWORD levels = V9(IDirect3DBaseTexture9, t9)->GetLevelCount(t9);
    t->nsubs = levels * (cube ? 6 : 1);
    t->subs = calloc(t->nsubs ? t->nsubs : 1, sizeof *t->subs);
    return t;
}
static HRESULT tex_subresource(V8 *t, UINT i, void **out)
{
    if (!out) return D3DERR_INVALIDCALL;
    *out = NULL;
    if (i >= t->nsubs) return D3DERR_INVALIDCALL;
    if (!t->subs[i]) {
        void *s9 = NULL;
        HRESULT r = t->kind == K8_CUBE
            ? V9(IDirect3DCubeTexture9, t->d9)->GetCubeMapSurface(t->d9, (D3DCUBEMAP_FACES)(i % 6), i / 6, (IDirect3DSurface9 **)&s9)
            : V9(IDirect3DTexture9, t->d9)->GetSurfaceLevel(t->d9, i, (IDirect3DSurface9 **)&s9);
        if (r < 0 || !s9) return D3DERR_INVALIDCALL;
        t->subs[i] = surf8_new(s9, t->pool, t);
        addref_priv(t->subs[i]);
    }
    obj_AddRef(t->subs[i]);
    *out = t->subs[i];
    return D3D_OK;
}
static void tex_PreLoad(void *This) { (void)This; }
static D3DRESOURCETYPE tex_GetType(void *This) { return ((V8 *)This)->kind == K8_CUBE ? D3DRTYPE_CUBETEXTURE : D3DRTYPE_TEXTURE; }
static DWORD tex_SetLOD(void *This, DWORD l) { V8 *t = This; return V9(IDirect3DBaseTexture9, t->d9)->SetLOD(t->d9, l); }
static DWORD tex_GetLOD(void *This) { V8 *t = This; return V9(IDirect3DBaseTexture9, t->d9)->GetLOD(t->d9); }
static DWORD tex_GetLevelCount(void *This) { V8 *t = This; return V9(IDirect3DBaseTexture9, t->d9)->GetLevelCount(t->d9); }
static HRESULT tex_GetLevelDesc(void *This, UINT level, SurfDesc8 *d)
{
    V8 *t = This;
    if (!d) return D3DERR_INVALIDCALL;
    D3DSURFACE_DESC s9;
    HRESULT r = t->kind == K8_CUBE ? V9(IDirect3DCubeTexture9, t->d9)->GetLevelDesc(t->d9, level, &s9)
                                   : V9(IDirect3DTexture9, t->d9)->GetLevelDesc(t->d9, level, &s9);
    if (r < 0) return r;
    desc8(&s9, d);
    return D3D_OK;
}
static HRESULT tex_GetSurfaceLevel(void *This, UINT level, void **out) { return tex_subresource(This, level, out); }
static HRESULT tex_LockRect(void *This, UINT level, D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags)
{
    V8 *t = This;
    return V9(IDirect3DTexture9, t->d9)->LockRect(t->d9, level, lr, rect, flags);
}
static HRESULT tex_UnlockRect(void *This, UINT level) { V8 *t = This; return V9(IDirect3DTexture9, t->d9)->UnlockRect(t->d9, level); }
static HRESULT tex_AddDirtyRect(void *This, const RECT *r) { V8 *t = This; return V9(IDirect3DTexture9, t->d9)->AddDirtyRect(t->d9, r); }
static HRESULT cube_GetCubeMapSurface(void *This, D3DCUBEMAP_FACES face, UINT level, void **out)
{
    return tex_subresource(This, level * 6 + (UINT)face, out);
}
static HRESULT cube_LockRect(void *This, D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags)
{
    V8 *t = This;
    return V9(IDirect3DCubeTexture9, t->d9)->LockRect(t->d9, face, level, lr, rect, flags);
}
static HRESULT cube_UnlockRect(void *This, D3DCUBEMAP_FACES face, UINT level)
{
    V8 *t = This;
    return V9(IDirect3DCubeTexture9, t->d9)->UnlockRect(t->d9, face, level);
}
static HRESULT cube_AddDirtyRect(void *This, D3DCUBEMAP_FACES face, const RECT *r)
{
    V8 *t = This;
    return V9(IDirect3DCubeTexture9, t->d9)->AddDirtyRect(t->d9, face, r);
}

/* ------------------------------------------------------------------ buffers */
static D3DRESOURCETYPE vb_GetType(void *This) { (void)This; return D3DRTYPE_VERTEXBUFFER; }
static D3DRESOURCETYPE ib_GetType(void *This) { (void)This; return D3DRTYPE_INDEXBUFFER; }
static HRESULT buf_Lock(void *This, UINT off, UINT len, BYTE **data, DWORD flags)
{
    V8 *b = This;
    return V9(IDirect3DVertexBuffer9, b->d9)->Lock(b->d9, off, len, (void **)data, flags);
}
static HRESULT buf_Unlock(void *This) { V8 *b = This; return V9(IDirect3DVertexBuffer9, b->d9)->Unlock(b->d9); }
static HRESULT vb_GetDesc(void *This, D3DVERTEXBUFFER_DESC *d) { V8 *b = This; return V9(IDirect3DVertexBuffer9, b->d9)->GetDesc(b->d9, d); }
static HRESULT ib_GetDesc(void *This, D3DINDEXBUFFER_DESC *d) { V8 *b = This; return V9(IDirect3DIndexBuffer9, b->d9)->GetDesc(b->d9, d); }
static void buf_PreLoad(void *This) { (void)This; }

/* ------------------------------------------------------------------ IDirect3D8 */
static void *d3d9;              /* our IDirect3D9 (the interface's, as DXVK's m_d3d9) */
static UINT nmodes8;
static D3DDISPLAYMODE modes8[256];

static ULONG d3d_AddRef(void *This) { return obj_AddRef(This); }
static ULONG d3d_Release(void *This) { return obj_Release(This); }
static HRESULT d3d_RegisterSoftwareDevice(void *This, void *f) { (void)This; (void)f; return D3DERR_INVALIDCALL; }
static UINT d3d_GetAdapterCount(void *This) { (void)This; return V9(IDirect3D9, d3d9)->GetAdapterCount(d3d9); }
static HRESULT d3d_GetAdapterIdentifier(void *This, UINT a, DWORD flags, AdapterId8 *id)
{
    (void)This;
    if (!id) return D3DERR_INVALIDCALL;
    /* (Direct3D 8's flag means the opposite: DXVK's) */
    if (flags & 2 /* D3DENUM_NO_WHQL_LEVEL */) flags &= ~(DWORD)D3DENUM_WHQL_LEVEL;
    else flags |= D3DENUM_WHQL_LEVEL;
    D3DADAPTER_IDENTIFIER9 i9;
    memset(&i9, 0, sizeof i9);
    HRESULT r = V9(IDirect3D9, d3d9)->GetAdapterIdentifier(d3d9, a, flags, &i9);
    if (r < 0) return r;
    strncpy(id->Driver, i9.Driver, 512);
    strncpy(id->Description, i9.Description, 512);
    id->DriverVersion = i9.DriverVersion;
    id->VendorId = i9.VendorId;
    id->DeviceId = i9.DeviceId;
    id->SubSysId = i9.SubSysId;
    id->Revision = i9.Revision;
    id->DeviceIdentifier = i9.DeviceIdentifier;
    id->WHQLLevel = i9.WHQLLevel;
    return D3D_OK;
}
static UINT d3d_GetAdapterModeCount(void *This, UINT a) { (void)This; return a == 0 ? nmodes8 : 0; }
static HRESULT d3d_EnumAdapterModes(void *This, UINT a, UINT m, D3DDISPLAYMODE *out)
{
    (void)This;
    if (a || m >= nmodes8 || !out) return D3DERR_INVALIDCALL;
    *out = modes8[m];
    return D3D_OK;
}
static HRESULT d3d_GetAdapterDisplayMode(void *This, UINT a, D3DDISPLAYMODE *m)
{
    (void)This;
    return V9(IDirect3D9, d3d9)->GetAdapterDisplayMode(d3d9, a, m);
}
static HRESULT d3d_CheckDeviceType(void *This, UINT a, D3DDEVTYPE t, D3DFORMAT af, D3DFORMAT bf, WINBOOL w)
{
    (void)This; (void)w;
    return V9(IDirect3D9, d3d9)->CheckDeviceType(d3d9, a, t, af, bf, FALSE);    /* (DXVK's: always as full screen) */
}
static HRESULT d3d_CheckDeviceFormat(void *This, UINT a, D3DDEVTYPE t, D3DFORMAT af, DWORD usage, D3DRESOURCETYPE rt, D3DFORMAT f)
{
    (void)This;
    if (is_d3d9_only_fmt(f)) return D3DERR_NOTAVAILABLE;
    if ((usage & D3DUSAGE_RENDERTARGET) && !is_rt_fmt8(f)) return D3DERR_NOTAVAILABLE;
    return V9(IDirect3D9, d3d9)->CheckDeviceFormat(d3d9, a, t, af, usage, rt, f);
}
static HRESULT d3d_CheckDeviceMultiSampleType(void *This, UINT a, D3DDEVTYPE t, D3DFORMAT f, WINBOOL w, D3DMULTISAMPLE_TYPE ms)
{
    (void)This;
    return V9(IDirect3D9, d3d9)->CheckDeviceMultiSampleType(d3d9, a, t, f, w, ms, NULL);
}
static HRESULT d3d_CheckDepthStencilMatch(void *This, UINT a, D3DDEVTYPE t, D3DFORMAT af, D3DFORMAT rf, D3DFORMAT df)
{
    (void)This;
    if (is_d3d9_only_fmt(rf) || is_d3d9_only_fmt(df) || !is_rt_fmt8(rf)) return D3DERR_NOTAVAILABLE;
    return V9(IDirect3D9, d3d9)->CheckDepthStencilMatch(d3d9, a, t, af, rf, df);
}
/* DXVK's ConvertCaps8 */
static void caps8(const D3DCAPS9 *c9, void *out)
{
    D3DCAPS9 c;
    memset(&c, 0, sizeof c);
    memcpy(&c, c9, CAPS8_SIZE);
    c.VertexShaderVersion = D3DVS_VERSION(1, 1);
    if (c9->PixelShaderVersion != D3DPS_VERSION(0, 0)) c.PixelShaderVersion = D3DPS_VERSION(1, 4);
    c.Caps2 &= ~(DWORD)D3DCAPS2_CANAUTOGENMIPMAP;
    c.Caps3 &= ~(DWORD)(D3DCAPS3_LINEAR_TO_SRGB_PRESENTATION | D3DCAPS3_COPY_TO_VIDMEM | D3DCAPS3_COPY_TO_SYSTEMMEM);
    c.PrimitiveMiscCaps &= ~(DWORD)(D3DPMISCCAPS_INDEPENDENTWRITEMASKS | D3DPMISCCAPS_PERSTAGECONSTANT |
                                    D3DPMISCCAPS_FOGANDSPECULARALPHA | D3DPMISCCAPS_SEPARATEALPHABLEND |
                                    D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS | D3DPMISCCAPS_MRTPOSTPIXELSHADERBLENDING |
                                    D3DPMISCCAPS_FOGVERTEXCLAMPED | D3DPMISCCAPS_POSTBLENDSRGBCONVERT);
    c.RasterCaps &= ~(DWORD)(D3DPRASTERCAPS_SCISSORTEST | D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS | D3DPRASTERCAPS_DEPTHBIAS |
                             D3DPRASTERCAPS_MULTISAMPLE_TOGGLE);
    c.SrcBlendCaps &= ~(DWORD)D3DPBLENDCAPS_BLENDFACTOR;
    c.DestBlendCaps &= ~(DWORD)D3DPBLENDCAPS_BLENDFACTOR;
    c.LineCaps &= ~(DWORD)D3DLINECAPS_ANTIALIAS;
    c.StencilCaps &= ~(DWORD)D3DSTENCILCAPS_TWOSIDED;
    c.VertexProcessingCaps &= ~(DWORD)D3DVTXPCAPS_TEXGEN_SPHEREMAP;
    c.Caps2 |= 0x00080000 /* D3DCAPS2_CANRENDERWINDOWED */;
    c.RasterCaps |= 0x00004000 /* D3DPRASTERCAPS_ZBIAS */;
    memcpy(out, &c, CAPS8_SIZE);
}
static HRESULT d3d_GetDeviceCaps(void *This, UINT a, D3DDEVTYPE t, void *caps)
{
    (void)This;
    if (!caps) return D3DERR_INVALIDCALL;
    D3DCAPS9 c9;
    HRESULT r = V9(IDirect3D9, d3d9)->GetDeviceCaps(d3d9, a, t, &c9);
    if (r < 0) return r;
    caps8(&c9, caps);
    return D3D_OK;
}
static HMONITOR d3d_GetAdapterMonitor(void *This, UINT a) { (void)This; return V9(IDirect3D9, d3d9)->GetAdapterMonitor(d3d9, a); }

/* DXVK's ConvertPresentParameters9 (the 8's back buffer count and format corrected in place) */
static D3DPRESENT_PARAMETERS pp9_of(PP8 *p)
{
    if (p->BackBufferCount < 1) p->BackBufferCount = 1;
    if (p->BackBufferFormat == D3DFMT_UNKNOWN) p->BackBufferFormat = D3DFMT_X8R8G8B8;
    D3DPRESENT_PARAMETERS q;
    memset(&q, 0, sizeof q);
    q.BackBufferWidth = p->BackBufferWidth;
    q.BackBufferHeight = p->BackBufferHeight;
    q.BackBufferFormat = p->BackBufferFormat;
    q.BackBufferCount = p->BackBufferCount;
    q.MultiSampleType = p->MultiSampleType;
    q.MultiSampleQuality = 0;
    UINT interval = p->FullScreen_PresentationInterval;
    if (p->Windowed) interval = D3DPRESENT_INTERVAL_IMMEDIATE;
    D3DSWAPEFFECT se = p->SwapEffect;
    if (se == 4 /* D3DSWAPEFFECT_COPY_VSYNC */) {
        se = D3DSWAPEFFECT_COPY;
        if (p->Windowed || interval == D3DPRESENT_INTERVAL_IMMEDIATE) interval = D3DPRESENT_INTERVAL_ONE;
    }
    q.SwapEffect = se;
    q.hDeviceWindow = p->hDeviceWindow;
    q.Windowed = p->Windowed;
    q.EnableAutoDepthStencil = p->EnableAutoDepthStencil;
    q.AutoDepthStencilFormat = p->AutoDepthStencilFormat;
    q.Flags = p->Flags;
    q.FullScreen_RefreshRateInHz = p->FullScreen_RefreshRateInHz == 0x7fffffff /* UNLIMITED */ ? 0 : p->FullScreen_RefreshRateInHz;
    q.PresentationInterval = interval;
    return q;
}
static HRESULT validate_pp(const PP8 *p)
{
    if (!p) return D3DERR_INVALIDCALL;
    if (p->SwapEffect == D3DSWAPEFFECT_COPY && p->BackBufferCount > 1) return D3DERR_INVALIDCALL;
    if (p->Windowed && p->FullScreen_PresentationInterval != D3DPRESENT_INTERVAL_DEFAULT) return D3DERR_INVALIDCALL;
    return D3D_OK;
}

/* the device's back buffers and depth buffer as Direct3D 8 surfaces (DXVK's RecreateBackBuffersAndAutoDepthStencil) */
static void reset_state(void)
{
    if (D.pp.BackBufferCount < 1) D.pp.BackBufferCount = 1;
    D.line_pattern = D.zvisible = 0;
    for (int i = 0; i < 8; i++) priv_set(&D.textures[i], NULL);
    for (int i = 0; i < 16; i++) { priv_set(&D.streams[i].buf, NULL); D.streams[i].stride = 0; }
    priv_set(&D.indices, NULL);
    priv_set(&D.rt, NULL);
    priv_set(&D.ds, NULL);
    for (UINT i = 0; i < D.nbackbufs; i++) priv_set(&D.backbufs[i], NULL);
    free(D.backbufs);
    D.nbackbufs = D.pp.BackBufferCount;
    D.backbufs = calloc(D.nbackbufs, sizeof *D.backbufs);
    priv_set(&D.autods, NULL);
}
static void recreate_buffers(void)
{
    for (UINT i = 0; i < D.nbackbufs; i++) {
        void *s9 = NULL;
        V9(IDirect3DDevice9, D.o.d9)->GetBackBuffer(D.o.d9, 0, i, D3DBACKBUFFER_TYPE_MONO, (IDirect3DSurface9 **)&s9);
        priv_set(&D.backbufs[i], s9 ? surf8_new(s9, D3DPOOL_DEFAULT, NULL) : NULL);
    }
    void *ds9 = NULL;
    HRESULT r = V9(IDirect3DDevice9, D.o.d9)->GetDepthStencilSurface(D.o.d9, (IDirect3DSurface9 **)&ds9);
    priv_set(&D.autods, r >= 0 && ds9 ? surf8_new(ds9, D3DPOOL_DEFAULT, NULL) : NULL);
    priv_set(&D.rt, D.backbufs[0]);
    priv_set(&D.ds, D.autods);
}

static HRESULT d3d_CreateDevice(void *This, UINT a, D3DDEVTYPE t, HWND focus, DWORD flags, PP8 *pp, void **out)
{
    if (!out) return D3DERR_INVALIDCALL;
    *out = NULL;
    HRESULT r = validate_pp(pp);
    if (r < 0) return r;
    if (D.o.d9) {
        LOG(LOG_ALWAYS, "vulkan renderer: a second Direct3D 8 device (one only)");
        return D3DERR_NOTAVAILABLE;
    }
    D3DPRESENT_PARAMETERS p9 = pp9_of(pp);
    void *dev9 = NULL;
    r = V9(IDirect3D9, d3d9)->CreateDevice(d3d9, a, t, focus, flags, &p9, (IDirect3DDevice9 **)&dev9);
    if (r < 0) return r;
    memset(&D, 0, sizeof D);
    D.o.vtbl = vt8_dev;
    D.o.kind = K8_DEV;
    D.o.d9 = dev9;
    D.parent = This;
    obj_AddRef(This);
    D.pp = *pp;
    D.flags = flags;
    reset_state();
    recreate_buffers();
    dev_AddRef8(&D.o);
    *out = &D.o;
    return D3D_OK;
}

/* ------------------------------------------------------------------ IDirect3DDevice8 */
#define DEV9 V9(IDirect3DDevice9, D.o.d9)
#define d9dev (D.o.d9)
/* DIRECT CALLS: the hot methods call our Direct3D 9 device's functions by name
   (vk.h vk9_*) instead of through its vtable, and where Direct3D 8's method is Direct3D 9's (the
   transforms, the material, the lights, Clear, DrawPrimitive, the constants) this vtable holds that
   function itself, so the program's call reaches the device's state in one step from the bridge's
   wrapper. <PORT_ENV>_VK_DIRECT=0: through the vtables as in round 1 (for before/after measurements). */
static int direct = 1;
#define CALL9(m, ...) (direct ? vk9_##m(d9dev, __VA_ARGS__) : DEV9->m(d9dev, __VA_ARGS__))

static ULONG dev_AddRef8(void *This)
{
    V8 *o = This;
    long r = o->refs++;
    if (!r) o->priv++;
    return (ULONG)(r + 1);
}
static ULONG dev_Release8(void *This)
{
    V8 *o = This;
    if (!o->refs) return 0;
    long r = --o->refs;
    if (!r && --o->priv == 0) LOG(LOG_ALWAYS, "vulkan renderer: the Direct3D 8 device released (kept)");
    return (ULONG)r;
}
static HRESULT dev_TestCooperativeLevel(void *This) { (void)This; return DEV9->TestCooperativeLevel(d9dev); }
static UINT dev_GetAvailableTextureMem(void *This) { (void)This; return DEV9->GetAvailableTextureMem(d9dev); }
static HRESULT dev_ResourceManagerDiscardBytes(void *This, DWORD b) { (void)This; (void)b; return DEV9->EvictManagedResources(d9dev); }
static HRESULT dev_GetDirect3D(void *This, void **out)
{
    (void)This;
    if (!out) return D3DERR_INVALIDCALL;
    obj_AddRef(D.parent);
    *out = D.parent;
    return D3D_OK;
}
static HRESULT dev_GetDeviceCaps(void *This, void *caps)
{
    (void)This;
    D3DCAPS9 c9;
    HRESULT r = DEV9->GetDeviceCaps(d9dev, &c9);
    if (r < 0) return r;
    caps8(&c9, caps);
    return D3D_OK;
}
static HRESULT dev_GetDisplayMode(void *This, D3DDISPLAYMODE *m) { (void)This; return DEV9->GetDisplayMode(d9dev, 0, m); }
static HRESULT dev_GetCreationParameters(void *This, D3DDEVICE_CREATION_PARAMETERS *p) { (void)This; return DEV9->GetCreationParameters(d9dev, p); }
static HRESULT dev_SetCursorProperties(void *This, UINT x, UINT y, void *s)
{
    (void)This;
    return DEV9->SetCursorProperties(d9dev, x, y, s ? ((V8 *)s)->d9 : NULL);
}
static void dev_SetCursorPosition(void *This, int x, int y, DWORD f) { (void)This; DEV9->SetCursorPosition(d9dev, x, y, f); }
static WINBOOL dev_ShowCursor(void *This, WINBOOL b) { (void)This; return DEV9->ShowCursor(d9dev, b); }
static HRESULT dev_CreateAdditionalSwapChain(void *This, PP8 *pp, void **out)
{
    (void)This; (void)pp;
    if (out) *out = NULL;
    UNSUPPORTED8("CreateAdditionalSwapChain");
    return D3DERR_INVALIDCALL;
}
static HRESULT dev_Reset(void *This, PP8 *pp)
{
    (void)This;
    HRESULT r = validate_pp(pp);
    if (r < 0) return r;
    D.pp = *pp;
    reset_state();
    D3DPRESENT_PARAMETERS p9 = pp9_of(pp);
    r = DEV9->Reset(d9dev, &p9);
    if (r < 0) return r;
    recreate_buffers();
    return D3D_OK;
}
static HRESULT dev_Present(void *This, const RECT *s, const RECT *d, HWND w, const RGNDATA *rg)
{
    (void)This;
    return DEV9->Present(d9dev, s, d, w, rg);
}
static HRESULT dev_GetBackBuffer(void *This, UINT i, D3DBACKBUFFER_TYPE t, void **out)
{
    (void)This;
    if (!out) return D3DERR_INVALIDCALL;
    *out = NULL;
    if (i >= D.nbackbufs || !D.backbufs[i]) {
        void *s9 = NULL;
        HRESULT r = DEV9->GetBackBuffer(d9dev, 0, i, t, (IDirect3DSurface9 **)&s9);
        if (r < 0) return r;
        if (i >= D.nbackbufs) {          /* (DXVK's vector index past its size: kept the same way here) */
            V8 *s = surf8_new(s9, D3DPOOL_DEFAULT, NULL);
            obj_AddRef(s);
            *out = s;
            return D3D_OK;
        }
        priv_set(&D.backbufs[i], surf8_new(s9, D3DPOOL_DEFAULT, NULL));
    }
    obj_AddRef(D.backbufs[i]);
    *out = D.backbufs[i];
    return D3D_OK;
}
static HRESULT dev_GetRasterStatus(void *This, D3DRASTER_STATUS *rs) { (void)This; return DEV9->GetRasterStatus(d9dev, 0, rs); }
static void dev_SetGammaRamp(void *This, DWORD f, const D3DGAMMARAMP *r) { (void)This; DEV9->SetGammaRamp(d9dev, 0, f, r); }
static void dev_GetGammaRamp(void *This, D3DGAMMARAMP *r) { (void)This; DEV9->GetGammaRamp(d9dev, 0, r); }

static HRESULT dev_CreateTexture(void *This, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, void **out)
{
    (void)This;
    if (fmt == D3DFMT_UNKNOWN) return D3DERR_INVALIDCALL;
    if (out) *out = NULL;
    if (!out || is_d3d9_only_fmt(fmt)) return D3DERR_INVALIDCALL;
    void *t9 = NULL;
    HRESULT r = DEV9->CreateTexture(d9dev, w, h, levels, usage, fmt, pool, (IDirect3DTexture9 **)&t9, NULL);
    if (r < 0) return r;
    V8 *t = tex8_new(0, t9, pool);
    obj_AddRef(t);
    *out = t;
    return D3D_OK;
}
static HRESULT dev_CreateVolumeTexture(void *This, UINT w, UINT h, UINT d, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, void **out)
{
    (void)This; (void)w; (void)h; (void)d; (void)l; (void)u; (void)p;
    if (f == D3DFMT_UNKNOWN) return D3DERR_INVALIDCALL;
    if (out) *out = NULL;
    UNSUPPORTED8("CreateVolumeTexture");
    return D3DERR_INVALIDCALL;
}
static HRESULT dev_CreateCubeTexture(void *This, UINT edge, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, void **out)
{
    (void)This;
    if (fmt == D3DFMT_UNKNOWN) return D3DERR_INVALIDCALL;
    if (out) *out = NULL;
    if (!out || is_d3d9_only_fmt(fmt)) return D3DERR_INVALIDCALL;
    void *t9 = NULL;
    HRESULT r = DEV9->CreateCubeTexture(d9dev, edge, levels, usage, fmt, pool, (IDirect3DCubeTexture9 **)&t9, NULL);
    if (r < 0) return r;
    V8 *t = tex8_new(1, t9, pool);
    obj_AddRef(t);
    *out = t;
    return D3D_OK;
}
static HRESULT dev_CreateVertexBuffer(void *This, UINT len, DWORD usage, DWORD fvf, D3DPOOL pool, void **out)
{
    (void)This;
    if (!out) return D3DERR_INVALIDCALL;
    *out = NULL;
    void *b9 = NULL;
    HRESULT r = DEV9->CreateVertexBuffer(d9dev, len, usage, fvf, pool, (IDirect3DVertexBuffer9 **)&b9, NULL);
    if (r < 0) return r;
    V8 *b = v8_new(K8_VB, vt8_vb, b9, pool);
    b->usage = usage;
    obj_AddRef(b);
    *out = b;
    return D3D_OK;
}
static HRESULT dev_CreateIndexBuffer(void *This, UINT len, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, void **out)
{
    (void)This;
    if (!out) return D3DERR_INVALIDCALL;
    *out = NULL;
    void *b9 = NULL;
    HRESULT r = DEV9->CreateIndexBuffer(d9dev, len, usage, fmt, pool, (IDirect3DIndexBuffer9 **)&b9, NULL);
    if (r < 0) return r;
    V8 *b = v8_new(K8_IB, vt8_ib, b9, pool);
    b->usage = usage;
    obj_AddRef(b);
    *out = b;
    return D3D_OK;
}
static HRESULT dev_CreateRenderTarget(void *This, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, WINBOOL lockable, void **out)
{
    (void)This;
    if (fmt == D3DFMT_UNKNOWN) return D3DERR_INVALIDCALL;
    if (out) *out = NULL;
    if (!out || is_d3d9_only_fmt(fmt) || !is_rt_fmt8(fmt)) return D3DERR_INVALIDCALL;
    void *s9 = NULL;
    HRESULT r = DEV9->CreateRenderTarget(d9dev, w, h, fmt, ms, 0, lockable, (IDirect3DSurface9 **)&s9, NULL);
    if (r < 0) return r;
    V8 *s = surf8_new(s9, D3DPOOL_DEFAULT, NULL);
    obj_AddRef(s);
    *out = s;
    return D3D_OK;
}
static HRESULT dev_CreateDepthStencilSurface(void *This, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, void **out)
{
    (void)This;
    if (fmt == D3DFMT_UNKNOWN) return D3DERR_INVALIDCALL;
    if (out) *out = NULL;
    if (!out || is_d3d9_only_fmt(fmt)) return D3DERR_INVALIDCALL;
    void *s9 = NULL;
    HRESULT r = DEV9->CreateDepthStencilSurface(d9dev, w, h, fmt, ms, 0, FALSE, (IDirect3DSurface9 **)&s9, NULL);
    if (r < 0) return r;
    V8 *s = surf8_new(s9, D3DPOOL_DEFAULT, NULL);
    obj_AddRef(s);
    *out = s;
    return D3D_OK;
}
static HRESULT dev_CreateImageSurface(void *This, UINT w, UINT h, D3DFORMAT fmt, void **out)
{
    (void)This;
    if (out) *out = NULL;
    if (fmt == D3DFMT_UNKNOWN || !out || is_d3d9_only_fmt(fmt)) return D3DERR_INVALIDCALL;
    D3DPOOL pool = vk9_format_supported(fmt) ? D3DPOOL_SYSTEMMEM : D3DPOOL_SCRATCH;
    void *s9 = NULL;
    HRESULT r = DEV9->CreateOffscreenPlainSurface(d9dev, w, h, fmt, pool, (IDirect3DSurface9 **)&s9, NULL);
    if (r < 0) return r;
    V8 *s = surf8_new(s9, pool, NULL);
    obj_AddRef(s);
    *out = s;
    return D3D_OK;
}

/* DXVK's copyTextureBuffers: congruent rectangles between two lockable surfaces */
static HRESULT copy_buffers(V8 *src, V8 *dst, const D3DSURFACE_DESC *sd, const D3DSURFACE_DESC *dd, const RECT *sr, const RECT *dr)
{
    D3DLOCKED_RECT sl, dl;
    int compressed = is_dxt(sd->Format);
    HRESULT r = surf_LockRect(src, &sl, sr, D3DLOCK_READONLY);
    if (r < 0) return r;
    r = surf_LockRect(dst, &dl, dr, 0);
    if (r < 0) { surf_UnlockRect(src); return r; }
    LONG rows = sr->bottom - sr->top, cols = sr->right - sr->left;
    LONG bpp = sl.Pitch / (LONG)sd->Width;
    if (!compressed && sr->left == 0 && sr->right == (LONG)sd->Width && sd->Width == dd->Width && sl.Pitch == dl.Pitch) {
        memcpy(dl.pBits, sl.pBits, (size_t)sl.Pitch * (size_t)rows);
    } else {
        LONG amp = cols * bpp;
        if (compressed) {
            UINT tw = sd->Width / 4 ? sd->Width / 4 : 1;
            amp = (cols / 4) * (sl.Pitch / (LONG)tw);
            rows = rows / 4;
        }
        for (LONG i = 0; i < rows; i++)
            memcpy((uint8_t *)dl.pBits + (size_t)i * dl.Pitch, (uint8_t *)sl.pBits + (size_t)i * sl.Pitch, (size_t)amp);
    }
    r = surf_UnlockRect(dst);
    if (r < 0) { surf_UnlockRect(src); return r; }
    return surf_UnlockRect(src);
}

static HRESULT dev_CreateImageSurface(void *This, UINT w, UINT h, D3DFORMAT fmt, void **out);
static HRESULT dev_CopyRects(void *This, void *srcp, const RECT *rects, UINT n, void *dstp, const POINT *pts)
{
    V8 *src = srcp, *dst = dstp;
    if (!src || !dst || src == dst) return D3DERR_INVALIDCALL;
    obj_AddRef(src);                     /* (DXVK holds both for the call: Com<D3D8Surface>) */
    obj_AddRef(dst);
    HRESULT res = D3DERR_INVALIDCALL;
    D3DSURFACE_DESC sd, dd;
    V9(IDirect3DSurface9, src->d9)->GetDesc(src->d9, &sd);
    V9(IDirect3DSurface9, dst->d9)->GetDesc(dst->d9, &dd);
    if (sd.Format != dd.Format || is_depth_fmt8(sd.Format)) goto out;
    RECT whole;
    POINT zero = {0, 0};
    if (!rects) {
        n = 1;
        whole = (RECT){0, 0, (LONG)sd.Width, (LONG)sd.Height};
        rects = &whole;
        pts = &zero;
    }
    for (UINT i = 0; i < n; i++) {
        RECT sr = rects[i], dr;
        int asym = 0, stretch = 0;
        if (pts) {
            dr.left = pts[i].x;
            dr.right = dr.left + (sr.right - sr.left);
            dr.top = pts[i].y;
            dr.bottom = dr.top + (sr.bottom - sr.top);
            asym = dr.left != sr.left || dr.top != sr.top || dr.right != sr.right || dr.bottom != sr.bottom;
            stretch = (dr.right - dr.left) != (sr.right - sr.left) || (dr.bottom - dr.top) != (sr.bottom - sr.top);
        } else dr = sr;
        POINT dp = {dr.left, dr.top};
        /* (as DXVK's: every case returns after the first rectangle) */
        switch (dd.Pool) {
        case D3DPOOL_DEFAULT:
            switch (sd.Pool) {
            case D3DPOOL_DEFAULT: res = DEV9->StretchRect(d9dev, src->d9, &sr, dst->d9, &dr, D3DTEXF_NONE); goto out;
            case D3DPOOL_MANAGED: res = vk9_update_from_buffer(src->d9, dst->d9, &sr, &dp); goto out;
            case D3DPOOL_SYSTEMMEM: res = DEV9->UpdateSurface(d9dev, src->d9, &sr, dst->d9, &dp); goto out;
            case D3DPOOL_SCRATCH: {
                if (!vk9_format_supported(sd.Format)) goto out;
                V8 *tmp = NULL;
                res = dev_CreateImageSurface(This, sd.Width, sd.Height, sd.Format, (void **)&tmp);
                if (res < 0) goto out;
                res = copy_buffers(src, tmp, &sd, &sd, &sr, &sr);
                if (res >= 0) res = DEV9->UpdateSurface(d9dev, tmp->d9, &sr, dst->d9, &dp);
                obj_Release(tmp);
                goto out;
            }
            default: goto out;
            }
        case D3DPOOL_MANAGED:
            switch (sd.Pool) {
            case D3DPOOL_DEFAULT: {
                void *b = surf_blit_image(dst);
                if (!b) goto out;
                res = DEV9->StretchRect(d9dev, src->d9, &sr, b, &dr, D3DTEXF_NONE);
                if (res >= 0) res = DEV9->GetRenderTargetData(d9dev, b, dst->d9);
                goto out;
            }
            case D3DPOOL_MANAGED: case D3DPOOL_SYSTEMMEM: case D3DPOOL_SCRATCH:
                res = stretch ? D3DERR_INVALIDCALL : copy_buffers(src, dst, &sd, &dd, &sr, &dr);
                goto out;
            default: goto out;
            }
        case D3DPOOL_SYSTEMMEM: case D3DPOOL_SCRATCH:
            if (((sd.Usage & D3DUSAGE_RENDERTARGET) || D.rt == src) && sd.MultiSampleType == D3DMULTISAMPLE_NONE &&
                sd.Width == dd.Width && sd.Height == dd.Height && sd.Format == dd.Format && !asym) {
                res = DEV9->GetRenderTargetData(d9dev, src->d9, dst->d9);
                goto out;
            }
            switch (sd.Pool) {
            case D3DPOOL_DEFAULT: {
                void *b = surf_blit_image(dst);
                if (!b) goto out;
                res = DEV9->StretchRect(d9dev, src->d9, &sr, b, &dr, D3DTEXF_NONE);
                if (res >= 0) res = DEV9->GetRenderTargetData(d9dev, b, dst->d9);
                goto out;
            }
            case D3DPOOL_MANAGED: case D3DPOOL_SYSTEMMEM: case D3DPOOL_SCRATCH:
                res = stretch ? D3DERR_INVALIDCALL : copy_buffers(src, dst, &sd, &dd, &sr, &dr);
                goto out;
            default: goto out;
            }
        default: goto out;
        }
    }
out:
    obj_Release(dst);
    obj_Release(src);
    return res;
}
static HRESULT dev_UpdateTexture(void *This, void *s, void *d)
{
    (void)This;
    if (!s || !d) return D3DERR_INVALIDCALL;
    return DEV9->UpdateTexture(d9dev, ((V8 *)s)->d9, ((V8 *)d)->d9);
}
static HRESULT dev_GetFrontBuffer(void *This, void *d)
{
    (void)This;
    if (!d) return D3DERR_INVALIDCALL;
    return DEV9->GetFrontBufferData(d9dev, 0, ((V8 *)d)->d9);
}
static HRESULT dev_SetRenderTarget(void *This, void *rtp, void *zp)
{
    (void)This;
    V8 *rt = rtp, *z = zp;
    HRESULT r;
    if (rt) {
        r = DEV9->SetRenderTarget(d9dev, 0, rt->d9);
        if (r < 0) return r;
        priv_set(&D.rt, rt);
    }
    if (D.rt && z) {
        SurfDesc8 rd, zd;
        if ((r = surf_GetDesc(D.rt, &rd)) < 0) return r;
        if ((r = surf_GetDesc(z, &zd)) < 0) return r;
        if (zd.Width < rd.Width || zd.Height < rd.Height) return D3DERR_INVALIDCALL;
    }
    r = DEV9->SetDepthStencilSurface(d9dev, z ? z->d9 : NULL);
    if (r < 0) return r;
    priv_set(&D.ds, z);
    return D3D_OK;
}
static HRESULT dev_GetRenderTarget(void *This, void **out)
{
    (void)This;
    if (!out) return D3DERR_INVALIDCALL;
    *out = NULL;
    if (!D.rt) {
        void *s9 = NULL;
        HRESULT r = DEV9->GetRenderTarget(d9dev, 0, (IDirect3DSurface9 **)&s9);
        if (r < 0) return r;
        priv_set(&D.rt, surf8_new(s9, D3DPOOL_DEFAULT, NULL));
    }
    obj_AddRef(D.rt);
    *out = D.rt;
    return D3D_OK;
}
static HRESULT dev_GetDepthStencilSurface(void *This, void **out)
{
    (void)This;
    if (!out) return D3DERR_INVALIDCALL;
    *out = NULL;
    if (!D.ds) {
        void *s9 = NULL;
        HRESULT r = DEV9->GetDepthStencilSurface(d9dev, (IDirect3DSurface9 **)&s9);
        if (r < 0) return r;
        priv_set(&D.ds, surf8_new(s9, D3DPOOL_DEFAULT, NULL));
    }
    obj_AddRef(D.ds);
    *out = D.ds;
    return D3D_OK;
}
static HRESULT dev_BeginScene(void *This) { (void)This; return direct ? vk9_BeginScene(d9dev) : DEV9->BeginScene(d9dev); }
static HRESULT dev_EndScene(void *This) { (void)This; return direct ? vk9_EndScene(d9dev) : DEV9->EndScene(d9dev); }
static HRESULT dev_Clear(void *This, DWORD n, const D3DRECT *r, DWORD f, D3DCOLOR c, float z, DWORD s)
{
    (void)This;
    return CALL9(Clear, n, r, f, c, z, s);
}
static HRESULT dev_SetTransform(void *This, D3DTRANSFORMSTATETYPE t, const D3DMATRIX *m) { (void)This; return CALL9(SetTransform, t, m); }
static HRESULT dev_GetTransform(void *This, D3DTRANSFORMSTATETYPE t, D3DMATRIX *m) { (void)This; return CALL9(GetTransform, t, m); }
static HRESULT dev_MultiplyTransform(void *This, D3DTRANSFORMSTATETYPE t, const D3DMATRIX *m) { (void)This; return DEV9->MultiplyTransform(d9dev, t, m); }
static HRESULT dev_SetViewport(void *This, const D3DVIEWPORT9 *vp)
{
    (void)This;
    if (!vp || !D.rt) return D3DERR_INVALIDCALL;
    SurfDesc8 rd;
    if (surf_GetDesc(D.rt, &rd) >= 0 && (vp->X + vp->Width > rd.Width || vp->Y + vp->Height > rd.Height)) {
        int wider = vp->X + vp->Width == rd.Width + 1, taller = vp->Y + vp->Height == rd.Height + 1;
        if (!(D.pp.Windowed && (wider || taller))) return D3DERR_INVALIDCALL;
    }
    return CALL9(SetViewport, vp);
}
static HRESULT dev_GetViewport(void *This, D3DVIEWPORT9 *vp) { (void)This; return CALL9(GetViewport, vp); }
static HRESULT dev_SetMaterial(void *This, const D3DMATERIAL9 *m) { (void)This; return CALL9(SetMaterial, m); }
static HRESULT dev_GetMaterial(void *This, D3DMATERIAL9 *m) { (void)This; return CALL9(GetMaterial, m); }
static HRESULT dev_SetLight(void *This, DWORD i, const D3DLIGHT9 *l) { (void)This; return CALL9(SetLight, i, l); }
static HRESULT dev_GetLight(void *This, DWORD i, D3DLIGHT9 *l) { (void)This; return CALL9(GetLight, i, l); }
static HRESULT dev_LightEnable(void *This, DWORD i, WINBOOL e) { (void)This; return CALL9(LightEnable, i, e); }
static HRESULT dev_GetLightEnable(void *This, DWORD i, WINBOOL *e) { (void)This; return CALL9(GetLightEnable, i, e); }
static HRESULT dev_SetClipPlane(void *This, DWORD i, const float *p) { (void)This; return DEV9->SetClipPlane(d9dev, i, p); }
static HRESULT dev_GetClipPlane(void *This, DWORD i, float *p) { (void)This; return DEV9->GetClipPlane(d9dev, i, p); }
/* <PORT_ENV>_VK_TRACE_STATES=1: each render state / texture stage state value the game sets, once (a measuring aid) */
static void trace_state(int kind, DWORD stage, DWORD st, DWORD v)
{
    static int on = -1;
    if (on < 0) on = port_getenv("VK_TRACE_STATES") != NULL;
    if (!on) return;
    static struct { int kind; DWORD stage, st, v; } seen[4096];
    static int n;
    for (int i = 0; i < n; i++)
        if (seen[i].kind == kind && seen[i].stage == stage && seen[i].st == st && seen[i].v == v) return;
    if (n < 4096) seen[n++] = (typeof(seen[0])){kind, stage, st, v};
    LOG(LOG_ALWAYS, "vulkan renderer: %s %u %u = %08x (frame %lu)", kind ? "tss" : "rs", (unsigned)stage, (unsigned)st, (unsigned)v,
        frame_count);
}

static HRESULT dev_SetRenderState(void *This, DWORD st, DWORD v)
{
    (void)This;
    DWORD s9 = st;
    trace_state(0, 0, st, v);
    switch (st) {
    case RS8_LINEPATTERN:
        UNSUPPORTED8("D3DRS_LINEPATTERN");
        D.line_pattern = v;
        return D3D_OK;
    case RS8_ZVISIBLE:
        D.zvisible = v;
        return D3D_OK;
    case RS8_EDGEANTIALIAS:
        s9 = D3DRS_ANTIALIASEDLINEENABLE;
        break;
    case RS8_ZBIAS: {
        s9 = D3DRS_DEPTHBIAS;
        float f = (float)v * ZBIAS_SCALE;
        memcpy(&v, &f, 4);
        break;
    }
    case RS8_SOFTWAREVERTEXPROCESSING:
        if (!(D.flags & D3DCREATE_MIXED_VERTEXPROCESSING)) return D3D_OK;
        return DEV9->SetSoftwareVertexProcessing(d9dev, v);
    case RS8_PATCHSEGMENTS: {
        UNSUPPORTED8("D3DRS_PATCHSEGMENTS");
        float f;
        memcpy(&f, &v, 4);
        return DEV9->SetNPatchMode(d9dev, f);
    }
    }
    return CALL9(SetRenderState, (D3DRENDERSTATETYPE)s9, v);
}
static HRESULT dev_GetRenderState(void *This, DWORD st, DWORD *v)
{
    (void)This;
    if (!v) return D3DERR_INVALIDCALL;
    DWORD s9 = st;
    switch (st) {
    case RS8_LINEPATTERN: *v = D.line_pattern; return D3D_OK;
    case RS8_ZVISIBLE: *v = D.zvisible; return D3D_OK;
    case RS8_EDGEANTIALIAS: s9 = D3DRS_ANTIALIASEDLINEENABLE; break;
    case RS8_ZBIAS: {
        DWORD b = 0;
        HRESULT r = DEV9->GetRenderState(d9dev, D3DRS_DEPTHBIAS, &b);
        float f;
        memcpy(&f, &b, 4);
        *v = (DWORD)(f * ZBIAS_SCALE_INV);
        return r;
    }
    case RS8_SOFTWAREVERTEXPROCESSING: *v = DEV9->GetSoftwareVertexProcessing(d9dev); return D3D_OK;
    case RS8_PATCHSEGMENTS: { float f = DEV9->GetNPatchMode(d9dev); memcpy(v, &f, 4); return D3D_OK; }
    }
    return DEV9->GetRenderState(d9dev, (D3DRENDERSTATETYPE)s9, v);
}

/* state blocks: tokens of Direct3D 9's blocks [without DXVK's D3D8StateBlock's own capture of the 8's
   textures, streams and indices: the route makes none] */
static HRESULT sb_add(void *sb, DWORD *tok)
{
    if (D.token >= D.nsblocks) {
        D.nsblocks = D.nsblocks ? D.nsblocks * 2 : 16;
        D.sblocks = realloc(D.sblocks, D.nsblocks * sizeof *D.sblocks);
    }
    D.sblocks[D.token++] = sb;
    *tok = D.token;
    return D3D_OK;
}
static void *sb_of(DWORD tok) { return tok && tok <= D.token ? D.sblocks[tok - 1] : NULL; }
static HRESULT dev_BeginStateBlock(void *This)
{
    (void)This;
    if (D.recording) return D3DERR_INVALIDCALL;
    UNSUPPORTED8("recorded state blocks (Direct3D 9's alone)");
    HRESULT r = DEV9->BeginStateBlock(d9dev);
    if (r >= 0) D.recording = 1;
    return r;
}
static HRESULT dev_EndStateBlock(void *This, DWORD *tok)
{
    (void)This;
    if (!tok || !D.recording) return D3DERR_INVALIDCALL;
    void *sb = NULL;
    HRESULT r = DEV9->EndStateBlock(d9dev, (IDirect3DStateBlock9 **)&sb);
    D.recording = 0;
    if (r < 0) return r;
    return sb_add(sb, tok);
}
static HRESULT dev_ApplyStateBlock(void *This, DWORD tok)
{
    (void)This;
    if (D.recording) return D3DERR_INVALIDCALL;
    void *sb = sb_of(tok);
    return sb ? V9(IDirect3DStateBlock9, sb)->Apply(sb) : D3D_OK;
}
static HRESULT dev_CaptureStateBlock(void *This, DWORD tok)
{
    (void)This;
    if (D.recording) return D3DERR_INVALIDCALL;
    void *sb = sb_of(tok);
    return sb ? V9(IDirect3DStateBlock9, sb)->Capture(sb) : D3D_OK;
}
static HRESULT dev_DeleteStateBlock(void *This, DWORD tok)
{
    (void)This;
    if (D.recording) return D3DERR_INVALIDCALL;
    void *sb = sb_of(tok);
    if (!sb) return D3D_OK;
    V9(IDirect3DStateBlock9, sb)->Release(sb);
    D.sblocks[tok - 1] = NULL;
    if (D.token == tok) D.token--;
    return D3D_OK;
}
static HRESULT dev_CreateStateBlock(void *This, D3DSTATEBLOCKTYPE t, DWORD *tok)
{
    (void)This;
    if (!tok || D.recording) return D3DERR_INVALIDCALL;
    if (t != D3DSBT_ALL && t != D3DSBT_PIXELSTATE && t != D3DSBT_VERTEXSTATE) return D3DERR_INVALIDCALL;
    UNSUPPORTED8("state blocks (Direct3D 9's alone)");
    void *sb = NULL;
    HRESULT r = DEV9->CreateStateBlock(d9dev, t, (IDirect3DStateBlock9 **)&sb);
    if (r < 0) return r;
    return sb_add(sb, tok);
}
static HRESULT dev_SetClipStatus(void *This, const D3DCLIPSTATUS9 *c) { (void)This; return DEV9->SetClipStatus(d9dev, c); }
static HRESULT dev_GetClipStatus(void *This, D3DCLIPSTATUS9 *c) { (void)This; return DEV9->GetClipStatus(d9dev, c); }
static HRESULT dev_GetTexture(void *This, DWORD stage, void **out)
{
    (void)This;
    if (!out) return D3DERR_INVALIDCALL;
    *out = NULL;
    if (stage >= 8) return D3DERR_INVALIDCALL;
    if (D.textures[stage]) obj_AddRef(D.textures[stage]);
    *out = D.textures[stage];
    return D3D_OK;
}
static HRESULT dev_SetTexture(void *This, DWORD stage, void *tp)
{
    (void)This;
    if (stage >= 8) return D3DERR_INVALIDCALL;
    V8 *t = tp;
    if (D.textures[stage] == t) return D3D_OK;
    HRESULT r = CALL9(SetTexture, stage, t ? t->d9 : NULL);
    if (r < 0) return r;
    priv_set(&D.textures[stage], t);
    return D3D_OK;
}
static HRESULT dev_GetTextureStageState(void *This, DWORD stage, DWORD t, DWORD *v)
{
    (void)This;
    int s = sampler_state9(t);
    if (s >= 0) return DEV9->GetSamplerState(d9dev, stage, (D3DSAMPLERSTATETYPE)s, v);
    return DEV9->GetTextureStageState(d9dev, stage, (D3DTEXTURESTAGESTATETYPE)t, v);
}
static HRESULT dev_SetTextureStageState(void *This, DWORD stage, DWORD t, DWORD v)
{
    (void)This;
    trace_state(1, stage, t, v);
    int s = sampler_state9(t);
    if (s >= 0) return CALL9(SetSamplerState, stage, (D3DSAMPLERSTATETYPE)s, v);
    return CALL9(SetTextureStageState, stage, (D3DTEXTURESTAGESTATETYPE)t, v);
}
static HRESULT dev_ValidateDevice(void *This, DWORD *n) { (void)This; return DEV9->ValidateDevice(d9dev, n); }
static HRESULT dev_GetInfo(void *This, DWORD id, void *data, DWORD size)
{
    (void)This;
    if (!data || !size) return D3DERR_INVALIDCALL;
    switch (id) {
    case 0: case 1: case 2: case 3: return E_FAIL;     /* (the pre-Direct3D 8 queries) */
    case 4: {                                           /* D3DDEVINFOID_VCACHE: none here (as DXVK on AMD) */
        if (size < 16) return D3DERR_INVALIDCALL;
        memset(data, 0, 16);
        return S_FALSE;
    }
    }
    return E_FAIL;
}
static HRESULT dev_SetPaletteEntries(void *This, UINT p, const PALETTEENTRY *e) { (void)This; return DEV9->SetPaletteEntries(d9dev, p, e); }
static HRESULT dev_GetPaletteEntries(void *This, UINT p, PALETTEENTRY *e) { (void)This; return DEV9->GetPaletteEntries(d9dev, p, e); }
static HRESULT dev_SetCurrentTexturePalette(void *This, UINT p) { (void)This; return DEV9->SetCurrentTexturePalette(d9dev, p); }
static HRESULT dev_GetCurrentTexturePalette(void *This, UINT *p) { (void)This; return DEV9->GetCurrentTexturePalette(d9dev, p); }
static HRESULT dev_DrawPrimitive(void *This, D3DPRIMITIVETYPE p, UINT start, UINT n) { (void)This; return CALL9(DrawPrimitive, p, start, n); }
static HRESULT dev_DrawIndexedPrimitive(void *This, D3DPRIMITIVETYPE p, UINT minv, UINT nv, UINT starti, UINT n)
{
    (void)This;
    INT base = (INT)(D.base_vertex < 0x7fffffffu ? D.base_vertex : 0x7fffffffu);
    return CALL9(DrawIndexedPrimitive, p, base, minv, nv, starti, n);
}
static HRESULT dev_DrawPrimitiveUP(void *This, D3DPRIMITIVETYPE p, UINT n, const void *data, UINT stride)
{
    (void)This;
    priv_set(&D.streams[0].buf, NULL);
    D.streams[0].stride = 0;
    return DEV9->DrawPrimitiveUP(d9dev, p, n, data, stride);
}
static HRESULT dev_DrawIndexedPrimitiveUP(void *This, D3DPRIMITIVETYPE p, UINT minv, UINT nv, UINT n, const void *idx, D3DFORMAT f,
                                          const void *data, UINT stride)
{
    (void)This;
    priv_set(&D.streams[0].buf, NULL);
    D.streams[0].stride = 0;
    priv_set(&D.indices, NULL);
    D.base_vertex = 0;
    return DEV9->DrawIndexedPrimitiveUP(d9dev, p, minv, nv, n, idx, f, data, stride);
}
static HRESULT dev_ProcessVertices(void *This, UINT a, UINT b, UINT c, void *d, DWORD f)
{
    (void)This;
    return DEV9->ProcessVertices(d9dev, a, b, c, d ? ((V8 *)d)->d9 : NULL, NULL, f);
}

/* ---- vertex shaders: DXVK's TranslateVertexShader8 */
static const BYTE vs_regs[17][2] = {
    {D3DDECLUSAGE_POSITION, 0}, {D3DDECLUSAGE_BLENDWEIGHT, 0}, {D3DDECLUSAGE_BLENDINDICES, 0}, {D3DDECLUSAGE_NORMAL, 0},
    {D3DDECLUSAGE_PSIZE, 0}, {D3DDECLUSAGE_COLOR, 0}, {D3DDECLUSAGE_COLOR, 1}, {D3DDECLUSAGE_TEXCOORD, 0},
    {D3DDECLUSAGE_TEXCOORD, 1}, {D3DDECLUSAGE_TEXCOORD, 2}, {D3DDECLUSAGE_TEXCOORD, 3}, {D3DDECLUSAGE_TEXCOORD, 4},
    {D3DDECLUSAGE_TEXCOORD, 5}, {D3DDECLUSAGE_TEXCOORD, 6}, {D3DDECLUSAGE_TEXCOORD, 7}, {D3DDECLUSAGE_POSITION, 1},
    {D3DDECLUSAGE_NORMAL, 1}};
static const BYTE decl_sizes[18] = {4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 6, 6, 8, 16, 0};

typedef struct { DWORD *w; uint32_t n, cap; } Toks;
static void tk(Toks *t, DWORD v)
{
    if (t->n == t->cap) { t->cap = t->cap ? t->cap * 2 : 256; t->w = realloc(t->w, t->cap * 4); }
    t->w[t->n++] = v;
}
static DWORD dst_reg(DWORD type, DWORD reg)
{
    return (reg & 0x7ff) | ((type & 7) << 28) | (((type & 0x18) >> 3) << 11) | (0xfu << 16) | (1u << 31);
}

static HRESULT translate_vs(const DWORD *decl, const DWORD *fn, D3DVERTEXELEMENT9 *el, Toks *out)
{
    Toks defs = {0};
    DWORD inputs = 0;
    uint32_t ne = 0, i = 0;
    WORD stream = 0, offset = 0;
    DWORD token;
    do {
        token = decl[i++];
        switch ((token >> 29) & 7) {
        case 0: break;                                   /* NOP */
        case 1:                                          /* STREAM */
            if (token & (1u << 28)) UNSUPPORTED8("D3DVSD_STREAM_TESS");
            stream = (WORD)(token & 0xf);
            offset = 0;
            break;
        case 2:                                          /* STREAMDATA */
            if (token & 0x10000000) { offset += (WORD)(((token >> 16) & 0xf) * 4); break; }   /* SKIP */
            if (!((token >> 28) & 1)) {
                DWORD type = (token >> 16) & 0xf, reg = token & 0x1f;
                if (!fn && reg == 3 && type != 2) return D3DERR_INVALIDCALL;   /* (a fixed-function normal is FLOAT3) */
                if (ne < 32 && reg < 17 && type < 18) {
                    el[ne] = (D3DVERTEXELEMENT9){stream, offset, (BYTE)type, D3DDECLMETHOD_DEFAULT, vs_regs[reg][0], vs_regs[reg][1]};
                    ne++;
                    offset += decl_sizes[type];
                    inputs |= 1u << reg;
                }
            } else UNSUPPORTED8("D3DVSD_DATALOADTYPE 1");
            break;
        case 3: UNSUPPORTED8("D3DVSD_TOKEN_TESSELLATOR"); break;
        case 4: {                                        /* CONSTMEM */
            DWORD count = (token >> 25) & 0xf, addr = token & 0x7f;
            for (uint32_t j = 0; j < count * 4; j += 4) {
                tk(&defs, 81 /* D3DSIO_DEF */);
                tk(&defs, dst_reg(D3DSPR_CONST2, addr));
                for (int k = 0; k < 4; k++) tk(&defs, decl[i + j + k]);
                addr++;
            }
            i += count * 4;
            break;
        }
        case 5: break;                                   /* EXT */
        case 7: if (ne < 33) el[ne++] = (D3DVERTEXELEMENT9)D3DDECL_END(); break;
        default: LOG(LOG_ALWAYS, "vulkan renderer: vertex shader declaration token %08x not handled", (unsigned)token); break;
        }
    } while (token != 0xffffffffu);
    if (fn) {
        tk(out, fn[0]);
        for (uint32_t vn = 0; vn < 17; vn++)
            if (inputs & (1u << vn)) {
                tk(out, 31 /* D3DSIO_DCL */);
                tk(out, (vs_regs[vn][0] & 0x1f) | ((DWORD)vs_regs[vn][1] << 16) | (1u << 31));
                tk(out, dst_reg(D3DSPR_INPUT, vn));
            }
        for (uint32_t k = 0; k < defs.n; k++) tk(out, defs.w[k]);
        i = 1;
        do {
            token = fn[i++];
            DWORD op = token & 0xffff;
            if (!(token & 0x80000000u) && (op == D3DSIO_RSQ || op == D3DSIO_RCP || op == D3DSIO_EXP || op == D3DSIO_LOG ||
                                           op == D3DSIO_EXPP || op == D3DSIO_LOGP)) {
                tk(out, token);
                tk(out, fn[i++]);                        /* dest */
                token = fn[i++];                         /* src0: no swizzle reads w (DXVK's d8vk#43) */
                if ((token & D3DVS_NOSWIZZLE) == D3DVS_NOSWIZZLE) {
                    token &= ~(DWORD)D3DVS_SWIZZLE_MASK;
                    token |= D3DVS_X_W | D3DVS_Y_W | D3DVS_Z_W | D3DVS_W_W;
                }
            }
            tk(out, token);
        } while (token != 0x0000ffffu);
    }
    free(defs.w);
    return D3D_OK;
}

static HRESULT dev_CreateVertexShader(void *This, const DWORD *decl, const DWORD *fn, DWORD *handle, DWORD usage)
{
    (void)This; (void)usage;
    if (!decl || !handle) return D3DERR_INVALIDCALL;
    D3DVERTEXELEMENT9 el[34];
    memset(el, 0, sizeof el);
    Toks code = {0};
    HRESULT r = translate_vs(decl, fn, el, &code);
    if (r < 0) { free(code.w); return r; }
    void *d9 = NULL, *vs = NULL;
    r = DEV9->CreateVertexDeclaration(d9dev, el, (IDirect3DVertexDeclaration9 **)&d9);
    if (r < 0) { free(code.w); return r; }
    if (fn) {
        r = DEV9->CreateVertexShader(d9dev, code.w, (IDirect3DVertexShader9 **)&vs);
        if (r < 0) { free(code.w); V9(IDirect3DVertexDeclaration9, d9)->Release(d9); return r; }
    }
    free(code.w);
    if (D.nvs == D.capvs) {
        D.capvs = D.capvs ? D.capvs * 2 : 16;
        D.vshaders = realloc(D.vshaders, (size_t)D.capvs * sizeof *D.vshaders);
    }
    VShader8 *v = &D.vshaders[D.nvs++];
    memset(v, 0, sizeof *v);
    v->decl = d9;
    v->vs = vs;
    uint32_t n = 0;
    while (decl[n] != 0xffffffffu) n++;
    v->ndecl = n + 1;
    v->declaration = malloc(v->ndecl * 4);
    memcpy(v->declaration, decl, v->ndecl * 4);
    if (fn) {
        n = 0;
        while (fn[n] != 0x0000ffffu) n++;
        v->nfunc = n + 1;
        v->function = malloc(v->nfunc * 4);
        memcpy(v->function, fn, v->nfunc * 4);
    }
    *handle = ((DWORD)D.nvs << 1) | 1;              /* (the bit tells a shader from an FVF) */
    LOG(LOG_ALWAYS, "vulkan renderer: vertex shader %x: %s %016llx", (unsigned)*handle, vs ? "bytecode" : "fixed function, declaration", vs ? (unsigned long long)((VkShader *)vs)->hash : (unsigned long long)((VkDecl *)d9)->hash);
    return D3D_OK;
}
static VShader8 *vs_of(DWORD h)
{
    DWORD i = (h & 1) ? ((h & ~1u) >> 1) - 1 : h;
    if (i >= (DWORD)D.nvs) return NULL;
    VShader8 *v = &D.vshaders[i];
    return v->decl || v->vs ? v : NULL;
}
static HRESULT dev_SetVertexShader(void *This, DWORD h)
{
    (void)This;
    if (D.recording) UNSUPPORTED8("SetVertexShader while recording a state block");
    if (h & 1) {
        VShader8 *v = vs_of(h);
        if (!v) return D3DERR_INVALIDCALL;
        CALL9(SetVertexDeclaration, v->decl);
        HRESULT r = CALL9(SetVertexShader, v->vs);
        if (r < 0) return r;
        D.cur_vs = h;
    } else if (D.cur_vs != h) {
        CALL9(SetVertexShader, NULL);
        HRESULT r = CALL9(SetFVF, h);
        if (r < 0) return r;
        D.cur_vs = h;
    }
    return D3D_OK;
}
static HRESULT dev_GetVertexShader(void *This, DWORD *h) { (void)This; if (!h) return D3DERR_INVALIDCALL; *h = D.cur_vs; return D3D_OK; }
static HRESULT dev_DeleteVertexShader(void *This, DWORD h)
{
    (void)This;
    if (!(h & 1)) return D3D_OK;
    VShader8 *v = vs_of(h);
    if (!v) return D3DERR_INVALIDCALL;
    if (v->decl) V9(IDirect3DVertexDeclaration9, v->decl)->Release(v->decl);
    if (v->vs) V9(IDirect3DVertexShader9, v->vs)->Release(v->vs);
    free(v->declaration);
    free(v->function);
    memset(v, 0, sizeof *v);
    if (D.cur_vs == h) D.cur_vs = 0;
    return D3D_OK;
}
static HRESULT dev_SetVertexShaderConstant(void *This, DWORD r, const void *d, DWORD n)
{
    (void)This;
    return CALL9(SetVertexShaderConstantF, r, d, n);
}
static HRESULT dev_GetVertexShaderConstant(void *This, DWORD r, void *d, DWORD n) { (void)This; return DEV9->GetVertexShaderConstantF(d9dev, r, d, n); }
static HRESULT shader_bytes(const DWORD *src, uint32_t n, void *data, DWORD *size)
{
    UINT have = *size, need = n * 4;
    if (!data) { *size = need; return D3D_OK; }
    if (have < need) return D3DERR_INVALIDCALL;
    memcpy(data, src, need);
    return D3D_OK;
}
static HRESULT dev_GetVertexShaderDeclaration(void *This, DWORD h, void *data, DWORD *size)
{
    (void)This;
    VShader8 *v = vs_of(h);
    if (!v) return D3DERR_INVALIDCALL;
    return shader_bytes(v->declaration, v->ndecl, data, size);
}
static HRESULT dev_GetVertexShaderFunction(void *This, DWORD h, void *data, DWORD *size)
{
    (void)This;
    VShader8 *v = vs_of(h);
    if (!v) return D3DERR_INVALIDCALL;
    return shader_bytes(v->function, v->nfunc, data, size);
}
static HRESULT dev_SetStreamSource(void *This, UINT n, void *vbp, UINT stride)
{
    (void)This;
    if (n >= 16) return D3DERR_INVALIDCALL;
    if (D.recording) UNSUPPORTED8("SetStreamSource while recording a state block");
    V8 *b = vbp;
    HRESULT r = CALL9(SetStreamSource, n, b ? b->d9 : NULL, 0, stride);
    if (r < 0) return r;
    priv_set(&D.streams[n].buf, b);
    if (b) D.streams[n].stride = stride;           /* (a NULL buffer keeps the stride) */
    return D3D_OK;
}
static HRESULT dev_GetStreamSource(void *This, UINT n, void **out, UINT *stride)
{
    (void)This;
    if (out) *out = NULL;
    if (stride) *stride = 0;
    if (!out || !stride || n >= 16) return D3DERR_INVALIDCALL;
    if (D.streams[n].buf) obj_AddRef(D.streams[n].buf);
    *out = D.streams[n].buf;
    *stride = D.streams[n].stride;
    return D3D_OK;
}
static HRESULT dev_SetIndices(void *This, void *ibp, UINT base)
{
    (void)This;
    if (D.recording) UNSUPPORTED8("SetIndices while recording a state block");
    V8 *b = ibp;
    HRESULT r = CALL9(SetIndices, b ? b->d9 : NULL);
    if (r < 0) return r;
    priv_set(&D.indices, b);
    D.base_vertex = base;
    return D3D_OK;
}
static HRESULT dev_GetIndices(void *This, void **out, UINT *base)
{
    (void)This;
    if (out) *out = NULL;
    if (!out || !base) return D3DERR_INVALIDCALL;
    if (D.indices) obj_AddRef(D.indices);
    *out = D.indices;
    *base = D.base_vertex;
    return D3D_OK;
}

/* ---- pixel shaders: handles of Direct3D 9's (the same bytecode) */
static HRESULT dev_CreatePixelShader(void *This, const DWORD *fn, DWORD *handle)
{
    (void)This;
    if (!fn || !handle) return D3DERR_INVALIDCALL;
    void *ps = NULL;
    HRESULT r = DEV9->CreatePixelShader(d9dev, fn, (IDirect3DPixelShader9 **)&ps);
    if (r < 0) return r;
    if (D.nps == D.capps) {
        D.capps = D.capps ? D.capps * 2 : 16;
        D.pshaders = realloc(D.pshaders, (size_t)D.capps * sizeof *D.pshaders);
    }
    D.pshaders[D.nps++] = ps;
    *handle = ((DWORD)D.nps << 1) | 1;
    LOG(LOG_ALWAYS, "vulkan renderer: pixel shader %x: %016llx", (unsigned)*handle, (unsigned long long)((VkShader *)ps)->hash);
    return D3D_OK;
}
static void *ps_of(DWORD h)
{
    DWORD i = (h & 1) ? ((h & ~1u) >> 1) - 1 : h;
    return i < (DWORD)D.nps ? D.pshaders[i] : NULL;
}
static HRESULT dev_SetPixelShader(void *This, DWORD h)
{
    (void)This;
    if (D.recording) UNSUPPORTED8("SetPixelShader while recording a state block");
    if (!h) {
        D.cur_ps = 0;
        return CALL9(SetPixelShader, NULL);
    }
    void *ps = ps_of(h);
    if (!ps) return D3DERR_INVALIDCALL;
    HRESULT r = CALL9(SetPixelShader, ps);
    if (r < 0) return r;
    D.cur_ps = h;
    return D3D_OK;
}
static HRESULT dev_GetPixelShader(void *This, DWORD *h) { (void)This; if (!h) return D3DERR_INVALIDCALL; *h = D.cur_ps; return D3D_OK; }
static HRESULT dev_DeletePixelShader(void *This, DWORD h)
{
    (void)This;
    void *ps = ps_of(h);
    if (!ps) return D3DERR_INVALIDCALL;
    V9(IDirect3DPixelShader9, ps)->Release(ps);
    D.pshaders[((h & ~1u) >> 1) - 1] = NULL;
    if (D.cur_ps == h) D.cur_ps = 0;
    return D3D_OK;
}
static HRESULT dev_SetPixelShaderConstant(void *This, DWORD r, const void *d, DWORD n) { (void)This; return CALL9(SetPixelShaderConstantF, r, d, n); }
static HRESULT dev_GetPixelShaderConstant(void *This, DWORD r, void *d, DWORD n) { (void)This; return DEV9->GetPixelShaderConstantF(d9dev, r, d, n); }
static HRESULT dev_GetPixelShaderFunction(void *This, DWORD h, void *data, DWORD *size)
{
    (void)This;
    void *ps = ps_of(h);
    if (!ps) return D3DERR_INVALIDCALL;
    UINT have = *size, need = 0;
    V9(IDirect3DPixelShader9, ps)->GetFunction(ps, NULL, &need);
    if (!data) { *size = need; return D3D_OK; }
    if (have < need) return D3DERR_INVALIDCALL;
    return V9(IDirect3DPixelShader9, ps)->GetFunction(ps, data, &have);
}
static HRESULT dev_DrawRectPatch(void *This, UINT h, const float *s, const void *i) { (void)This; (void)h; (void)s; (void)i; return D3DERR_INVALIDCALL; }
static HRESULT dev_DrawTriPatch(void *This, UINT h, const float *s, const void *i) { (void)This; (void)h; (void)s; (void)i; return D3DERR_INVALIDCALL; }
static HRESULT dev_DeletePatch(void *This, UINT h) { (void)This; (void)h; return D3DERR_INVALIDCALL; }

/* ------------------------------------------------------------------ the vtables */
#define S(vt, iface, m, f) vt[VI_##iface##_##m] = (void *)(f)
#define RES8(vt, iface) S(vt, iface, QueryInterface, obj_QueryInterface); S(vt, iface, AddRef, obj_AddRef); \
    S(vt, iface, Release, obj_Release); S(vt, iface, GetDevice, res_GetDevice); S(vt, iface, SetPrivateData, res_SetPrivateData); \
    S(vt, iface, GetPrivateData, res_GetPrivateData); S(vt, iface, FreePrivateData, res_FreePrivateData)
#define PRIO8(vt, iface) S(vt, iface, SetPriority, res_SetPriority); S(vt, iface, GetPriority, res_GetPriority)
#define TEX8(vt, iface) RES8(vt, iface); PRIO8(vt, iface); S(vt, iface, PreLoad, tex_PreLoad); S(vt, iface, GetType, tex_GetType); \
    S(vt, iface, SetLOD, tex_SetLOD); S(vt, iface, GetLOD, tex_GetLOD); S(vt, iface, GetLevelCount, tex_GetLevelCount); \
    S(vt, iface, GetLevelDesc, tex_GetLevelDesc)

static void vtables8_init(void)
{
    static int done;
    if (done) return;
    done = 1;
    {
        const char *e = port_getenv("VK_DIRECT");
        direct = !(e && *e == '0');
        LOG(LOG_ALWAYS, "vulkan renderer: direct device calls %s", direct ? "on" : "off (<PORT_ENV>_VK_DIRECT=0)");
    }
#define I8(m) S(vt8_d3d, IDirect3D8, m, d3d_##m)
    S(vt8_d3d, IDirect3D8, QueryInterface, obj_QueryInterface);
    I8(AddRef); I8(Release); I8(RegisterSoftwareDevice); I8(GetAdapterCount); I8(GetAdapterIdentifier); I8(GetAdapterModeCount);
    I8(EnumAdapterModes); I8(GetAdapterDisplayMode); I8(CheckDeviceType); I8(CheckDeviceFormat); I8(CheckDeviceMultiSampleType);
    I8(CheckDepthStencilMatch); I8(GetDeviceCaps); I8(GetAdapterMonitor); I8(CreateDevice);
#undef I8
#define D8(m) S(vt8_dev, IDirect3DDevice8, m, dev_##m)
    S(vt8_dev, IDirect3DDevice8, QueryInterface, obj_QueryInterface);
    S(vt8_dev, IDirect3DDevice8, AddRef, dev_AddRef8);
    S(vt8_dev, IDirect3DDevice8, Release, dev_Release8);
    D8(TestCooperativeLevel); D8(GetAvailableTextureMem); D8(ResourceManagerDiscardBytes); D8(GetDirect3D); D8(GetDeviceCaps);
    D8(GetDisplayMode); D8(GetCreationParameters); D8(SetCursorProperties); D8(SetCursorPosition); D8(ShowCursor);
    D8(CreateAdditionalSwapChain); D8(Reset); D8(Present); D8(GetBackBuffer); D8(GetRasterStatus); D8(SetGammaRamp);
    D8(GetGammaRamp); D8(CreateTexture); D8(CreateVolumeTexture); D8(CreateCubeTexture); D8(CreateVertexBuffer);
    D8(CreateIndexBuffer); D8(CreateRenderTarget); D8(CreateDepthStencilSurface); D8(CreateImageSurface); D8(CopyRects);
    D8(UpdateTexture); D8(GetFrontBuffer); D8(SetRenderTarget); D8(GetRenderTarget); D8(GetDepthStencilSurface); D8(BeginScene);
    D8(EndScene); D8(Clear); D8(SetTransform); D8(GetTransform); D8(MultiplyTransform); D8(SetViewport); D8(GetViewport);
    D8(SetMaterial); D8(GetMaterial); D8(SetLight); D8(GetLight); D8(LightEnable); D8(GetLightEnable); D8(SetClipPlane);
    D8(GetClipPlane); D8(SetRenderState); D8(GetRenderState); D8(BeginStateBlock); D8(EndStateBlock); D8(ApplyStateBlock);
    D8(CaptureStateBlock); D8(DeleteStateBlock); D8(CreateStateBlock); D8(SetClipStatus); D8(GetClipStatus); D8(GetTexture);
    D8(SetTexture); D8(GetTextureStageState); D8(SetTextureStageState); D8(ValidateDevice); D8(GetInfo); D8(SetPaletteEntries);
    D8(GetPaletteEntries); D8(SetCurrentTexturePalette); D8(GetCurrentTexturePalette); D8(DrawPrimitive);
    D8(DrawIndexedPrimitive); D8(DrawPrimitiveUP); D8(DrawIndexedPrimitiveUP); D8(ProcessVertices); D8(CreateVertexShader);
    D8(SetVertexShader); D8(GetVertexShader); D8(DeleteVertexShader); D8(SetVertexShaderConstant); D8(GetVertexShaderConstant);
    D8(GetVertexShaderDeclaration); D8(GetVertexShaderFunction); D8(SetStreamSource); D8(GetStreamSource); D8(SetIndices);
    D8(GetIndices); D8(CreatePixelShader); D8(SetPixelShader); D8(GetPixelShader); D8(DeletePixelShader);
    D8(SetPixelShaderConstant); D8(GetPixelShaderConstant); D8(GetPixelShaderFunction); D8(DrawRectPatch); D8(DrawTriPatch);
    D8(DeletePatch);
#undef D8
    TEX8(vt8_tex, IDirect3DTexture8);
    if (direct) {
#define DIRECT8(m, f) S(vt8_dev, IDirect3DDevice8, m, f)
        DIRECT8(SetTransform, vk9_SetTransform); DIRECT8(GetTransform, vk9_GetTransform);
        DIRECT8(SetMaterial, vk9_SetMaterial); DIRECT8(GetMaterial, vk9_GetMaterial);
        DIRECT8(SetLight, vk9_SetLight); DIRECT8(GetLight, vk9_GetLight);
        DIRECT8(LightEnable, vk9_LightEnable); DIRECT8(GetLightEnable, vk9_GetLightEnable);
        DIRECT8(BeginScene, vk9_BeginScene); DIRECT8(EndScene, vk9_EndScene); DIRECT8(Clear, vk9_Clear);
        DIRECT8(DrawPrimitive, vk9_DrawPrimitive); DIRECT8(GetViewport, vk9_GetViewport);
        DIRECT8(SetVertexShaderConstant, vk9_SetVertexShaderConstantF);
        DIRECT8(SetPixelShaderConstant, vk9_SetPixelShaderConstantF);
#undef DIRECT8
    }
    S(vt8_tex, IDirect3DTexture8, GetSurfaceLevel, tex_GetSurfaceLevel);
    S(vt8_tex, IDirect3DTexture8, LockRect, tex_LockRect);
    S(vt8_tex, IDirect3DTexture8, UnlockRect, tex_UnlockRect);
    S(vt8_tex, IDirect3DTexture8, AddDirtyRect, tex_AddDirtyRect);
    TEX8(vt8_cube, IDirect3DCubeTexture8);
    S(vt8_cube, IDirect3DCubeTexture8, GetCubeMapSurface, cube_GetCubeMapSurface);
    S(vt8_cube, IDirect3DCubeTexture8, LockRect, cube_LockRect);
    S(vt8_cube, IDirect3DCubeTexture8, UnlockRect, cube_UnlockRect);
    S(vt8_cube, IDirect3DCubeTexture8, AddDirtyRect, cube_AddDirtyRect);
    RES8(vt8_surf, IDirect3DSurface8);
    S(vt8_surf, IDirect3DSurface8, GetContainer, surf_GetContainer);
    S(vt8_surf, IDirect3DSurface8, GetDesc, surf_GetDesc);
    S(vt8_surf, IDirect3DSurface8, LockRect, surf_LockRect);
    S(vt8_surf, IDirect3DSurface8, UnlockRect, surf_UnlockRect);
    RES8(vt8_vb, IDirect3DVertexBuffer8);
    PRIO8(vt8_vb, IDirect3DVertexBuffer8);
    S(vt8_vb, IDirect3DVertexBuffer8, PreLoad, buf_PreLoad);
    S(vt8_vb, IDirect3DVertexBuffer8, GetType, vb_GetType);
    S(vt8_vb, IDirect3DVertexBuffer8, Lock, buf_Lock);
    S(vt8_vb, IDirect3DVertexBuffer8, Unlock, buf_Unlock);
    S(vt8_vb, IDirect3DVertexBuffer8, GetDesc, vb_GetDesc);
    RES8(vt8_ib, IDirect3DIndexBuffer8);
    PRIO8(vt8_ib, IDirect3DIndexBuffer8);
    S(vt8_ib, IDirect3DIndexBuffer8, PreLoad, buf_PreLoad);
    S(vt8_ib, IDirect3DIndexBuffer8, GetType, ib_GetType);
    S(vt8_ib, IDirect3DIndexBuffer8, Lock, buf_Lock);
    S(vt8_ib, IDirect3DIndexBuffer8, Unlock, buf_Unlock);
    S(vt8_ib, IDirect3DIndexBuffer8, GetDesc, ib_GetDesc);
}

/* runtime/d3d8.c's buffer locks: the program's copy of a
   vertex or index buffer (in its memory, as many bytes, zeros as ours) is our buffer's host copy from its
   first lock on, so the bridge's copy at unlock is a copy onto itself (skipped there) and the bytes go
   from the program's copy to the GPU's once. <PORT_ENV>_VK_BUF_HOST=0: our own copy as in round 1. */
void vk8_buf_host(void *buf, void *host)
{
    V8 *b = buf;
    if (b && (b->kind == K8_VB || b->kind == K8_IB)) vk_buf_host(b->d9, host);
}

/* Direct3DCreate8 under renderer = vulkan (d3d8.c): NULL when our renderer cannot start (then DXVK) */
void *vk_create8(void)
{
    if (!d3d9) {
        d3d9 = vk_create9();
        if (!d3d9) return NULL;
        /* option vsync = on (default): as runtime/d3d8.c tells DXVK (d3d9.presentInterval = 1; the game asks
           for immediate presents); test runs and hidden ones never wait */
        vk9_set_vsync(option_bool("vsync", 1) && !deterministic && !headless);
        /* (DXVK's D3D8Interface caches the adapter's modes of X8R8G8B8 then R5G6B5) */
        static const D3DFORMAT afmts[2] = {D3DFMT_X8R8G8B8, D3DFMT_R5G6B5};
        for (int k = 0; k < 2; k++) {
            UINT n = V9(IDirect3D9, d3d9)->GetAdapterModeCount(d3d9, 0, afmts[k]);
            for (UINT m = 0; m < n && nmodes8 < 256; m++)
                if (V9(IDirect3D9, d3d9)->EnumAdapterModes(d3d9, 0, afmts[k], m, &modes8[nmodes8]) >= 0) nmodes8++;
        }
    }
    vtables8_init();
    V8 *o = v8_new(K8_D3D, vt8_d3d, NULL, 0);
    obj_AddRef(o);
    return o;
}

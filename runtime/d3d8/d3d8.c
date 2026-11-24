/* The Direct3D 8 bridge to DXVK (the AntiStatic Recomp Toolkit's, from the D3D8 ports' own copies:
   the program's D3D8 runs on DXVK's native Linux build, its d3d8 front end over its d3d9; on Windows
   on its own Direct3D 8 or DXVK's d3d8.dll). Forked from the toolkit's Direct3D 9 bridge.
   The program's COM objects are ours (com.c): their vtables point at runtime functions, mostly
   generated (gen/d3d8_gen.c), which call DXVK's object behind them. Written here: the objects'
   identity and reference counts, the methods whose arguments differ between the program's
   32-bit layout and DXVK's 64-bit one (present parameters, locked rectangles, window handles),
   and locks: the program cannot address DXVK's memory (above 4 GB), so a lock hands it a copy in
   its own memory (persistent for vertex and index buffers, per lock for textures and surfaces),
   copied into DXVK's at unlock. */
#define _GNU_SOURCE
#define INITGUID
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <strings.h>
#include <zlib.h>
#ifdef _WIN32
#include <SDL3/SDL.h>
#endif
#include "d3d8_bridge.h"
#include "win.h"
#include "options.h"

/* ------------------------------------------------------------------ the inventory */
int d3d_inventory = -1;
static struct { unsigned *slot; const char *name; } inv[512];
static int ninv;
static pthread_mutex_t inv_lock = PTHREAD_MUTEX_INITIALIZER;
void d3d_count(unsigned *slot, const char *name)
{
    if (!*slot) {
        pthread_mutex_lock(&inv_lock);
        if (ninv < 512) inv[ninv].slot = slot, inv[ninv++].name = name;
        pthread_mutex_unlock(&inv_lock);
    }
    __atomic_add_fetch(slot, 1, __ATOMIC_RELAXED);
}
void d3d_inventory_write(void)
{
    const char *p = port_getenv("D3D_INVENTORY");
    FILE *f = p ? fopen(p, "w") : NULL;
    if (!f) return;
    for (int i = 0; i < ninv; i++) fprintf(f, "%10u %s\n", *inv[i].slot, inv[i].name);
    fclose(f);
}
__attribute__((constructor)) static void inventory_init(void)
{
    d3d_inventory = port_getenv("D3D_INVENTORY") != NULL;
}

/* ------------------------------------------------------------------ objects */
typedef struct {
    void *host;
    uint32_t guest;
    int kind;
    /* vertex/index buffers: the program's copy */
    uint32_t shadow, size;
    int nlocks;
    struct { uint32_t off, len; void *hp; } locks[4];
    int watched;                         /* a death watcher is attached (ent_watch) */
} Ent;

#define HCAP 65536
static Ent *ents[HCAP];                  /* open addressing on the host pointer */
static pthread_mutex_t dlock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t hslot(void *h) { return (uint32_t)(((uintptr_t)h >> 4) * 2654435761u) & (HCAP - 1); }

static Ent *ent_find(void *h)
{
    for (uint32_t i = hslot(h), n = 0; n < HCAP; i = (i + 1) & (HCAP - 1), n++) {
        if (!ents[i]) return NULL;
        if (ents[i] != (Ent *)1 && ents[i]->host == h) return ents[i];
    }
    return NULL;
}

static void ent_put(Ent *e)
{
    for (uint32_t i = hslot(e->host), n = 0; n < HCAP; i = (i + 1) & (HCAP - 1), n++)
        if (!ents[i] || ents[i] == (Ent *)1) { ents[i] = e; return; }
    fatal("too many Direct3D objects");
}

static void ent_del(Ent *e)
{
    for (uint32_t i = hslot(e->host), n = 0; n < HCAP; i = (i + 1) & (HCAP - 1), n++) {
        if (!ents[i]) return;
        if (ents[i] == e) { ents[i] = (Ent *)1; return; }
    }
}

void *d3d_host(uint32_t guest) { return com_host(guest); }
void *d3d_host_or_null(uint32_t guest) { return guest ? com_host(guest) : NULL; }

static Ent *ent_of_guest(uint32_t guest)
{
    void *h = com_host(guest);
    pthread_mutex_lock(&dlock);
    Ent *e = ent_find(h);
    pthread_mutex_unlock(&dlock);
    return e;
}

/* a resource's own class from its type (the program may hold it as a base texture) */
static int kind_of_resource(void *h)
{
    uint32_t t = ((uint32_t (*)(void *))VT(h)[VI_IDirect3DResource8_GetType])(h);
    switch (t) {
    case 1: return DK_IDirect3DSurface8;
    case 2: return DK_IDirect3DVolume8;
    case 3: return DK_IDirect3DTexture8;
    case 4: return DK_IDirect3DVolumeTexture8;
    case 5: return DK_IDirect3DCubeTexture8;
    case 6: return DK_IDirect3DVertexBuffer8;
    case 7: return DK_IDirect3DIndexBuffer8;
    }
    fatal("Direct3D resource of unknown type %u", t);
}

/* DEATH WATCHERS (as in the D3D9 bridge). A resource can die without the program releasing it (a
   texture's surfaces go with the texture). Each resource carries a small object of ours as
   private data (D3DSPD_IUNKNOWN); DXVK releases it when the resource is destroyed, and the entry
   goes then. Without them a dead entry was only noticed when DXVK put a new object at the same
   address, which happened or not depending on DXVK's allocator, so the program's memory differed
   from run to run (an extra wrapper object at a race's load [verified: runtime allocation logs of
   two runs, 2026-11-04]). */
static void ent_free_locked(Ent *e);
typedef struct { void **vt; Ent *e; volatile long ref; } Watcher;
static long __stdcall watcher_qi(Watcher *w, const GUID *g, void **o) { (void)g; *o = w; w->ref++; return 0; }
static unsigned long __stdcall watcher_addref(Watcher *w) { return (unsigned long)++w->ref; }
static unsigned long __stdcall watcher_release(Watcher *w)
{
    long r = --w->ref;
    if (r == 0) {
        pthread_mutex_lock(&dlock);
        if (w->e) ent_free_locked(w->e);
        pthread_mutex_unlock(&dlock);
        free(w);
    }
    return (unsigned long)r;
}
static void *watcher_vt[3] = {(void *)watcher_qi, (void *)watcher_addref, (void *)watcher_release};
static const GUID watcher_guid = {0x4c615275, 0x7368, 0x4e61, {0x74, 0x69, 0x76, 0x65, 0x44, 0x65, 0x61, 0x64}};

static void ent_watch(Ent *e)
{
    switch (e->kind) {
    case DK_IDirect3DSurface8: case DK_IDirect3DVolume8: case DK_IDirect3DTexture8:
    case DK_IDirect3DVolumeTexture8: case DK_IDirect3DCubeTexture8: case DK_IDirect3DVertexBuffer8:
    case DK_IDirect3DIndexBuffer8: break;
    default: return;
    }
    Watcher *w = calloc(1, sizeof *w);
    w->vt = watcher_vt;
    w->e = e;
    w->ref = 1;
    /* (SetPrivateData is method 4 of every interface that has it) */
    HRESULT r = ((HRESULT (*)(void *, const GUID *, const void *, DWORD, DWORD))VT(e->host)[4])(
        e->host, &watcher_guid, w, sizeof(void *), 1 /* D3DSPD_IUNKNOWN: the data is the object */);
    if (r >= 0) e->watched = 1;
    else w->e = NULL;
    watcher_release(w);                  /* DXVK holds the only reference now */
}

/* under dlock: the entry goes (released, destroyed, or found dead) */
static void ent_free_locked(Ent *e)
{
    ent_del(e);
    LOG(LOG_D3D, "%s %08x gone", d3d_classes[e->kind].iface, e->guest);
    if (e->shadow) gfree(e->shadow);
    W32(e->guest, 0);                    /* a use after it finds no vtable */
    free(e);
}

/* An entry left for a host object that died without the program releasing it (a texture's
   surfaces go with the texture) must not be handed out for a new object DXVK puts at the same
   address: objects from Create* calls are always new (d3d_wrap_new), and an entry of another
   kind is stale (the D3D9 bridge's lesson). */
static uint32_t wrap(int kind, void *host, int is_new)
{
    if (!host) return 0;
    pthread_mutex_lock(&dlock);
    Ent *e = ent_find(host);
    if (e && (is_new || (kind != DK_IDirect3DResource8 && kind != DK_IDirect3DBaseTexture8 &&
                         kind != DK_IUnknown && e->kind != kind))) {
        LOG(LOG_D3D, "%s %p: the old %s %08x there is gone", d3d_classes[kind].iface, host,
            d3d_classes[e->kind].iface, e->guest);
        ent_free_locked(e);
        e = NULL;
    }
    if (e) {
        pthread_mutex_unlock(&dlock);
        return e->guest;
    }
    pthread_mutex_unlock(&dlock);
    if (kind == DK_IDirect3DResource8 || kind == DK_IDirect3DBaseTexture8)
        kind = kind_of_resource(host);
    e = calloc(1, sizeof *e);
    e->host = host;
    e->kind = kind;
    e->guest = com_new(&d3d_classes[kind], host);
    pthread_mutex_lock(&dlock);
    Ent *again = ent_find(host);         /* another thread wrapped it meanwhile */
    if (again) {
        pthread_mutex_unlock(&dlock);
        free(e);
        return again->guest;
    }
    ent_put(e);
    pthread_mutex_unlock(&dlock);
    ent_watch(e);
    LOG(LOG_D3D, "%s %p -> %08x", d3d_classes[kind].iface, host, e->guest);
    return e->guest;
}

uint32_t d3d_wrap(int kind, void *host) { return wrap(kind, host, 0); }
uint32_t d3d_wrap_new(int kind, void *host) { return wrap(kind, host, 1); }

void *d3d_hwnd(uint32_t hwnd)
{
    (void)hwnd;                          /* every window is the host window */
#ifdef _WIN32
    /* (Windows' Direct3D takes the window's own handle; DXVK native an SDL_Window) */
    void *w = host_sdl_window();
    return w ? SDL_GetPointerProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL) : NULL;
#else
    return host_sdl_window();
#endif
}

float d3d_argf(CPU *c, int n)
{
    uint32_t u = ARG(n);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

void d3d_unsupported(CPU *c, const char *name, int nargs)
{
    LOG(LOG_ALWAYS, "%s is not supported (called from %08x)", name, R32(c->esp));
    api_return(c, 0x8876086cu, nargs);   /* D3DERR_INVALIDCALL */
}

static pthread_mutex_t device_lock;
static pthread_once_t device_lock_once = PTHREAD_ONCE_INIT;
static void device_lock_init(void)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&device_lock, &a);
}
void d3d_lock(void)
{
    pthread_once(&device_lock_once, device_lock_init);
    pthread_mutex_lock(&device_lock);
}
void d3d_unlock(void) { pthread_mutex_unlock(&device_lock); }

/* ------------------------------------------------------------------ IUnknown */
static const struct { const GUID *iid; int kind; } iids[] = {
    {&IID_IDirect3D8, DK_IDirect3D8}, {&IID_IDirect3DDevice8, DK_IDirect3DDevice8},
    {&IID_IDirect3DResource8, DK_IDirect3DResource8}, {&IID_IDirect3DVertexBuffer8, DK_IDirect3DVertexBuffer8},
    {&IID_IDirect3DVolume8, DK_IDirect3DVolume8}, {&IID_IDirect3DSwapChain8, DK_IDirect3DSwapChain8},
    {&IID_IDirect3DSurface8, DK_IDirect3DSurface8}, {&IID_IDirect3DIndexBuffer8, DK_IDirect3DIndexBuffer8},
    {&IID_IDirect3DBaseTexture8, DK_IDirect3DBaseTexture8}, {&IID_IDirect3DTexture8, DK_IDirect3DTexture8},
    {&IID_IDirect3DCubeTexture8, DK_IDirect3DCubeTexture8}, {&IID_IDirect3DVolumeTexture8, DK_IDirect3DVolumeTexture8},
};

static int kind_of_iid(const GUID *g, int dflt)
{
    for (size_t i = 0; i < sizeof iids / sizeof *iids; i++)
        if (!memcmp(g, iids[i].iid, sizeof *g)) return iids[i].kind;
    return dflt;
}

HOSTFN(sp_any_QueryInterface, 3)
{
    void *self = d3d_host(ARG(0));
    Ent *me = ent_of_guest(ARG(0));
    GUID g;
    memcpy(&g, gptr(ARG(1)), sizeof g);
    void *o = NULL;
    uint32_t r = (uint32_t)((HRESULT (*)(void *, const GUID *, void **))VT(self)[0])(self, &g, &o);
    if ((int32_t)r < 0 || !o) {
        if (ARG(2)) W32(ARG(2), 0);
        LOG(LOG_D3D, "QueryInterface(" GUID_FMT ") on %s -> %08x", GUID_ARGS(ARG(1)), me ? d3d_classes[me->kind].iface : "?", r);
        return r;
    }
    int kind = kind_of_iid(&g, me ? me->kind : DK_IDirect3DResource8);
    if (o == self && me)
        kind = me->kind;                 /* the same object: keep its class */
    W32(ARG(2), d3d_wrap(kind, o));
    return r;
}

HOSTFN(sp_any_AddRef, 1)
{
    void *self = d3d_host(ARG(0));
    return ((uint32_t (*)(void *))VT(self)[1])(self);
}

HOSTFN(sp_any_Release, 1)
{
    void *self = d3d_host(ARG(0));
    Ent *e = ent_of_guest(ARG(0));
    int watched = e && e->watched;       /* (its watcher frees it as the object dies) */
    uint32_t r = ((uint32_t (*)(void *))VT(self)[2])(self);
    if (r == 0 && e && !watched) {
        pthread_mutex_lock(&dlock);
        ent_free_locked(e);
        pthread_mutex_unlock(&dlock);
    }
    return r;
}

/* SetPrivateData with D3DSPD_IUNKNOWN would hand DXVK a program object to reference count: kept
   out of DXVK (the data is stored as plain bytes instead, the pointer included). The method sits
   at the same index in every interface that has it. */
HOSTFN(sp_any_SetPrivateData, 5)
{
    void *self = d3d_host(ARG(0));
    GUID g;
    memcpy(&g, gptr(ARG(1)), sizeof g);
    uint32_t flags = ARG(4) & ~1u;
    return (uint32_t)((HRESULT (*)(void *, const GUID *, const void *, DWORD, DWORD))VT(self)[4])(
        self, &g, gptr(ARG(2)), ARG(3), flags);
}

HOSTFN(sp_any_GetPrivateData, 4)
{
    void *self = d3d_host(ARG(0));
    GUID g;
    memcpy(&g, gptr(ARG(1)), sizeof g);
    DWORD n = ARG(3) ? R32(ARG(3)) : 0;
    uint32_t r = (uint32_t)((HRESULT (*)(void *, const GUID *, void *, DWORD *))VT(self)[5])(
        self, &g, ARG(2) ? gptr(ARG(2)) : NULL, &n);
    if (ARG(3)) W32(ARG(3), n);
    return r;
}

/* ------------------------------------------------------------------ present parameters */
/* the program's D3DPRESENT_PARAMETERS (Direct3D 8): 13 dwords, hDeviceWindow at +24 */
static void pp_in(uint32_t g, D3DPRESENT_PARAMETERS *pp, uint32_t focus)
{
    memset(pp, 0, sizeof *pp);
    pp->BackBufferWidth = R32(g);
    pp->BackBufferHeight = R32(g + 4);
    pp->BackBufferFormat = (D3DFORMAT)R32(g + 8);
    pp->BackBufferCount = R32(g + 12);
    pp->MultiSampleType = (D3DMULTISAMPLE_TYPE)R32(g + 16);
    pp->SwapEffect = (D3DSWAPEFFECT)R32(g + 20);
    pp->hDeviceWindow = d3d_hwnd(R32(g + 24) ? R32(g + 24) : focus);
    pp->Windowed = R32(g + 28);
    pp->EnableAutoDepthStencil = R32(g + 32);
    pp->AutoDepthStencilFormat = (D3DFORMAT)R32(g + 36);
    pp->Flags = R32(g + 40);
    /* shots asked for (--shot): a back buffer that can be locked, for drivers whose CopyRects
       needs D3DX (d3d8to9 on Windows' Direct3D 9 without the DirectX runtime) */
    extern int shots_requested(void);
    if (shots_requested()) {
        /* (a multisampled back buffer can't be locked: shot runs draw without multisampling;
           Penguins! asks for 2x) */
        pp->MultiSampleType = D3DMULTISAMPLE_NONE;
        pp->Flags |= D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    }
    pp->FullScreen_RefreshRateInHz = R32(g + 44);
    pp->FullScreen_PresentationInterval = R32(g + 48);
    LOG(LOG_ALWAYS, "present parameters: %ux%u format %u x%u, ms %u, swap %u, %s, depth %u (format %u), "
        "flags %x, %u Hz, interval %x", pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat,
        pp->BackBufferCount, pp->MultiSampleType, pp->SwapEffect, pp->Windowed ? "windowed" : "full screen",
        pp->EnableAutoDepthStencil, pp->AutoDepthStencilFormat, pp->Flags, pp->FullScreen_RefreshRateInHz,
        pp->FullScreen_PresentationInterval);
    /* the desktop's mode is never changed: full screen becomes a window of the back buffer's size
       (the host window goes full screen on its own, borderless) */
    if (!pp->Windowed) {
        pp->Windowed = TRUE;
        pp->FullScreen_RefreshRateInHz = 0;
        pp->FullScreen_PresentationInterval = 0;
        if (pp->SwapEffect == D3DSWAPEFFECT_COPY_VSYNC) pp->SwapEffect = D3DSWAPEFFECT_COPY;
    }
    if (pp->BackBufferWidth && pp->BackBufferHeight)
        host_window_resize((int)pp->BackBufferWidth, (int)pp->BackBufferHeight);
}

static void pp_out(uint32_t g, const D3DPRESENT_PARAMETERS *pp)
{
    W32(g, pp->BackBufferWidth);
    W32(g + 4, pp->BackBufferHeight);
    W32(g + 8, pp->BackBufferFormat);
    W32(g + 12, pp->BackBufferCount);
}

/* ------------------------------------------------------------------ IDirect3D8 */
/* THE CARD AS THE PROGRAM SEES IT (2026-11-24, as AntiStatic v0.2.46's D3D9 bridge does): under --test
   a test card, the same on every PC, so a recorded run doesn't depend on the PC's card (a game can keep
   the adapter's whole D3DADAPTER_IDENTIFIER8 and the device's capabilities in its memory: one port's
   runs parted between PCs from frame 10 on them). Option test_card = nvidia (default) |
   amd | intel | real (the PC's own: runs that can't be compared between PCs), test_vram = MB (default
   2047); the same cards as the toolkit's runtime/d3d9/d3d9.c, so the D3D9 and D3D8 ports answer alike.
   max_point: D3DCAPS8.MaxPointSize, the one capability DXVK takes from the Vulkan driver
   (pointSizeRange). Outside --test always the PC's own card. */
typedef struct { const char *name; uint32_t vendor, device; const char *pci_vendor, *pci_device, *desc, *driver; float max_point; } TestCard;
static const TestCard test_cards[] = {
    {"nvidia", 0x10de, 0x2487, "10DE", "2487", "NVIDIA GeForce RTX 3060", "nvd3dum.dll", 2047.9375f},
    {"amd", 0x1002, 0x73df, "1002", "73DF", "AMD Radeon RX 6700 XT", "aticfx32.dll", 8191.875f},
    {"intel", 0x8086, 0x3e92, "8086", "3E92", "Intel(R) UHD Graphics 630", "igdumd32.dll", 255.875f},
};
static const TestCard *test_card(void)
{
    static const TestCard *card;
    static int known;
    if (!deterministic)
        return NULL;
    if (!known) {
        const char *v = option_str("test_card", "nvidia");
        known = 1;
        for (size_t i = 0; i < sizeof test_cards / sizeof *test_cards; i++)
            if (!strcasecmp(v, test_cards[i].name))
                card = &test_cards[i];
        if (!card && strcasecmp(v, "real"))
            fatal("option test_card = %s: nvidia, amd, intel or real", v);
        LOG(LOG_ALWAYS, "test card: %s", card ? card->desc : "the PC's own");
    }
    return card;
}

static void *dxvk;
static void *(*dxvk_create8)(UINT);

/* a port's own Vulkan renderer for D3D8 (its vk_d3d8.c, option renderer = vulkan), when it has one:
   weak defaults here, so a port without one runs on DXVK */
__attribute__((weak)) void *vk_create8(void) { return NULL; }                  /* its IDirect3D8 */
__attribute__((weak)) void vk8_buf_host(void *buf, void *host) { (void)buf; (void)host; }   /* a buffer's host copy */
static int vk_renderer;                 /* our own renderer is the one running */
int d3d8_vulkan_running(void) { return vk_renderer; }
__attribute__((weak)) void vk_debug8(void *d3d8) { (void)d3d8; }   /* a port's measuring aids (optional) */

API(D3D8, Direct3DCreate8, 1)
{
    static int inventory_kept;               /* (written at exit: nothing called it before) */
    if (d3d_inventory && !inventory_kept) {
        inventory_kept = 1;
        runtime_at_exit(d3d_inventory_write);
    }
    /* option renderer = dxvk (the default) | vulkan (a port's own Vulkan renderer, vk_d3d8.c, when it has one,
       linked in): the same COM interfaces, so everything here calls either the same way; it falls
       back to DXVK if it cannot start */
    static int vulkan = -1;
    if (vulkan < 0)
        vulkan = !strcasecmp(option_str("renderer", "dxvk"), "vulkan");
    if (vulkan) {
        void *d = vk_create8();
        if (d) {
            if (!vk_renderer) d3d_classes_init();   /* (once, as on DXVK's path) */
            vk_renderer = 1;
            LOG(LOG_ALWAYS, "Direct3DCreate8(%u) -> %p (our own Vulkan renderer)", ARG(0), d);
            vk_debug8(d);
            return d3d_wrap_new(DK_IDirect3D8, d);
        }
        LOG(LOG_ALWAYS, "renderer = vulkan: our renderer could not start; DXVK instead");
        vulkan = 0;
    }
#ifdef _WIN32
    /* Windows: its own Direct3D 8 (d3d8.dll in the system folder), or the DLL the option
       d3d8_dll names (e.g. DXVK's d3d8.dll beside the program) */
    if (!dxvk) {
        char path[1200];
        /* the order: option d3d8_dll; DXVK's d3d8.dll beside the program (the Windows download
           carries it with its d3d9.dll: Windows 11 26H2 has no d3d8.dll [verified:
           a Windows 11 PC, 2026-11-12]); Windows' own */
        const char *opt = option_str("d3d8_dll", "");
        char dir[1024];
        if (opt[0])
            snprintf(path, sizeof path, "%s", opt);
        else if (win_exe_dir(dir, sizeof dir) &&
                 (snprintf(path, sizeof path, "%s/d3d8.dll", dir), access(path, 0) == 0))
            ;
        else {
            UINT n = GetSystemDirectoryA(path, sizeof path - 16);
            snprintf(path + n, sizeof path - n, "\\d3d8.dll");
        }
        dxvk = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!dxvk)
            fatal("cannot load Direct3D 8 (%s): %s", path, dlerror());
        dxvk_create8 = (void *(*)(UINT))dlsym(dxvk, "Direct3DCreate8");
        if (!dxvk_create8)
            fatal("%s has no Direct3DCreate8", path);
        LOG(LOG_ALWAYS, "Direct3D 8: %s", path);
        d3d_classes_init();
    }
#else
    if (!dxvk) {
        char dir[1024], path[1200];
        ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 1);
        dir[n > 0 ? n : 0] = 0;
        char *slash = strrchr(dir, '/');
        if (slash) *slash = 0;
        setenv("DXVK_WSI_DRIVER", "SDL3", 0);
        static char logdir[1100];
        snprintf(logdir, sizeof logdir, "%s", user_dir);
        setenv("DXVK_LOG_PATH", logdir, 0);
        setenv("DXVK_STATE_CACHE_PATH", logdir, 0);
        /* option vsync = on (default) | off: frames wait for the screen's refresh. The game asks
           for immediate presents (D3DPRESENT_INTERVAL_IMMEDIATE) and, unsynced, ran at
           thousands of frames a second [verified: 4000 frames in a second on a 2024 handheld]. Test runs (--test) and hidden ones never wait. */
        /* option gamma_flashes = on (default) | off: the game's flashes are gamma ramps, which DXVK
           shows only on a full-screen swap chain, and the port's are windowed: DXVK with
           (the toolkit's third_party/dxvk/dxvk-windowed-gamma.patch) applies them in a window when asked */
        /* option anisotropic = off (default: the game's own filtering) | 2 | 4 | 8 | 16: DXVK's
           d3d9.samplerAnisotropy */
        {
            static char conf[512];
            int vs = option_bool("vsync", 1) && !deterministic && !headless;
            int an = atoi(option_str("anisotropic", "off"));
            int n = snprintf(conf, sizeof conf, "d3d9.presentInterval = %d; d3d9.windowedGammaRamp = %s;", vs ? 1 : 0,
                             option_bool("gamma_flashes", 1) ? "True" : "False");
            if (an == 2 || an == 4 || an == 8 || an == 16)
                n += snprintf(conf + n, sizeof conf - (size_t)n, " d3d9.samplerAnisotropy = %d;", an);
            /* (under --test DXVK's own vendor-dependent answers follow the test card too, below) */
            const TestCard *tc = test_card();
            if (tc)
                snprintf(conf + n, sizeof conf - (size_t)n, " d3d9.customVendorId = %s; d3d9.customDeviceId = %s;"
                         " d3d9.customDeviceDesc = \"%s\";", tc->pci_vendor, tc->pci_device, tc->desc);
            setenv("DXVK_CONFIG", conf, 0);
        }
        /* DXVK's d3d8 front end loads its d3d9 by name: that one first, from our lib folder */
        snprintf(path, sizeof path, "%s/lib/libdxvk_d3d9.so.0", dir);
        if (!dlopen(path, RTLD_NOW | RTLD_GLOBAL))
            fatal("cannot load DXVK (%s): %s", path, dlerror());
        snprintf(path, sizeof path, "%s/lib/libdxvk_d3d8.so.0", dir);
        dxvk = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!dxvk)
            fatal("cannot load DXVK (%s): %s", path, dlerror());
        dxvk_create8 = (void *(*)(UINT))dlsym(dxvk, "Direct3DCreate8");
        if (!dxvk_create8)
            fatal("DXVK has no Direct3DCreate8");
        d3d_classes_init();
    }
#endif
    if (!host_sdl_window())
        LOG(LOG_ALWAYS, "Direct3DCreate8 without a host window (headless run): DXVK cannot present");
    void *d = dxvk_create8(ARG(0));
    LOG(LOG_ALWAYS, "Direct3DCreate8(%u) -> %p", ARG(0), d);
    if (d) vk_debug8(d);
    return d3d_wrap_new(DK_IDirect3D8, d);
}

HOSTFN(sp_IDirect3D8_CreateDevice, 7)
{
    void *self = d3d_host(ARG(0));
    uint32_t flags = ARG(4);
    D3DPRESENT_PARAMETERS pp;
    pp_in(ARG(5), &pp, ARG(3));
    /* Direct3D sets the FPU to single precision unless told to preserve it: so does this, on the
       program's control word; DXVK always preserves (the host FPU is not its to change) */
    if (!(flags & D3DCREATE_FPU_PRESERVE))
        fpu_setcw(c, c->fcw & ~0x300u);
    void *dev = NULL;
    uint32_t r = (uint32_t)((HRESULT (*)(void *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, void **))
                            VT(self)[VI_IDirect3D8_CreateDevice])(
        self, ARG(1), (D3DDEVTYPE)ARG(2), d3d_hwnd(ARG(3)), flags | D3DCREATE_FPU_PRESERVE, &pp, &dev);
    LOG(LOG_ALWAYS, "CreateDevice(adapter %u, type %u, flags %x) -> %08x", ARG(1), ARG(2), flags, r);
    pp_out(ARG(5), &pp);
    if (ARG(6)) W32(ARG(6), (int32_t)r >= 0 ? d3d_wrap_new(DK_IDirect3DDevice8, dev) : 0);
    return r;
}

/* THE DISPLAY MODES: a fixed list under --test (the screens plugged in must not change the program's
   memory between runs), on a PC with no screen (SDL's offscreen driver), and always with
   PORT_D3D8_FIXED_MODES 1 (LEGO Island Xtreme Stunts: it asks for 640x480 R5G6B5 and, when the list
   lacks it, takes the nearest mode and rebuilds its device from its loading thread while the main
   thread still uses the old one, a crash at the first CreateTexture after it [verified]). Otherwise
   DXVK's, or with PORT_D3D8_MODES_FILTER 1 only those of option resolution's size (below). Direct3D 8
   lists every format's modes together. */
#ifndef PORT_D3D8_FIXED_MODES
#define PORT_D3D8_FIXED_MODES 0
#endif
#ifndef PORT_D3D8_MODES_FILTER
#define PORT_D3D8_MODES_FILTER 0
#endif
static int fixed_modes(void)
{
    static int none = -1;
    if (deterministic || PORT_D3D8_FIXED_MODES)
        return 1;
    if (none < 0) {
        extern int host_no_screen(void);
        none = host_no_screen();
        if (none)
            LOG(LOG_ALWAYS, "D3D8: no screen (SDL offscreen): the fixed modes and a 1920x1080 desktop");
    }
    return none;
}
static const uint16_t test_modes[][2] = {
    {640, 480}, {800, 600}, {1024, 768}, {1152, 864}, {1280, 720}, {1280, 768}, {1280, 800}, {1280, 960},
    {1280, 1024}, {1360, 768}, {1366, 768}, {1440, 900}, {1600, 900}, {1600, 1200}, {1680, 1050},
    {1920, 1080}, {1920, 1200}, {2560, 1440}, {2560, 1600}};
#define NTEST_MODES ((UINT)(sizeof test_modes / sizeof *test_modes))

/* THE RESOLUTION (PORT_D3D8_MODES_FILTER 1): option resolution = desktop (default) | WxH. For a game
   that picks its mode from the adapter's list in a dialog the port never shows (one port's own default
   was 1280x720 whatever the screen [verified: runs with DXVK's 24 modes up to 2560x1600]): the list
   holds only the modes of the size wanted (all of them if the adapter has none of that size). A
   changed option makes a saved mode fail the game's check against the list, and the game chooses again. */
static UINT nmodes_shown;
static UINT modes_shown[64];             /* indices into the adapter's own list */

static void modes_filter(void *self)
{
    static int done;
    if (done) return;
    done = 1;
    int w = 0, h = 0;
    const char *r = option_str("resolution", "desktop");
    if (!strcasecmp(r, "desktop"))
        host_desktop_size(&w, &h);
    else if (sscanf(r, "%dx%d", &w, &h) != 2)
        w = h = 0;
    UINT n = ((UINT (*)(void *, UINT))VT(self)[VI_IDirect3D8_GetAdapterModeCount])(self, 0);
    for (int pass = 0; pass < 2 && !nmodes_shown; pass++)
        for (UINT i = 0; i < n && nmodes_shown < 64; i++) {
            D3DDISPLAYMODE m;
            if (((HRESULT (*)(void *, UINT, UINT, D3DDISPLAYMODE *))VT(self)[VI_IDirect3D8_EnumAdapterModes])(self, 0, i, &m) < 0)
                continue;
            if (pass == 1 || ((int)m.Width == w && (int)m.Height == h))
                modes_shown[nmodes_shown++] = i;
        }
    LOG(LOG_ALWAYS, "display modes: %u of the adapter's %u shown (resolution %s = %dx%d)", nmodes_shown, n, r, w, h);
}

HOSTFN(sp_IDirect3D8_GetAdapterModeCount, 2)
{
    if (fixed_modes())
        return ARG(1) == 0 ? 2 * NTEST_MODES : 0;
    void *self = d3d_host(ARG(0));
    if (!PORT_D3D8_MODES_FILTER)
        return ((UINT (*)(void *, UINT))VT(self)[VI_IDirect3D8_GetAdapterModeCount])(self, ARG(1));
    if (ARG(1) != 0) return 0;
    modes_filter(self);
    return nmodes_shown;
}

HOSTFN(sp_IDirect3D8_EnumAdapterModes, 4)
{
    if (fixed_modes()) {
        if (ARG(1) != 0 || ARG(2) >= 2 * NTEST_MODES || !ARG(3))
            return D3DERR_INVALIDCALL;
        uint32_t i = ARG(2) % NTEST_MODES;
        D3DDISPLAYMODE m = {test_modes[i][0], test_modes[i][1], 60, ARG(2) < NTEST_MODES ? D3DFMT_R5G6B5 : D3DFMT_X8R8G8B8};
        memcpy(gptr(ARG(3)), &m, sizeof m);
        return D3D_OK;
    }
    void *self = d3d_host(ARG(0));
    if (!PORT_D3D8_MODES_FILTER)
        return (uint32_t)((HRESULT (*)(void *, UINT, UINT, D3DDISPLAYMODE *))VT(self)[VI_IDirect3D8_EnumAdapterModes])(
            self, ARG(1), ARG(2), ARG(3) ? (D3DDISPLAYMODE *)gptr(ARG(3)) : NULL);
    modes_filter(self);
    if (ARG(1) != 0 || ARG(2) >= nmodes_shown || !ARG(3))
        return D3DERR_INVALIDCALL;
    return (uint32_t)((HRESULT (*)(void *, UINT, UINT, D3DDISPLAYMODE *))VT(self)[VI_IDirect3D8_EnumAdapterModes])(
        self, 0, modes_shown[ARG(2)], (D3DDISPLAYMODE *)gptr(ARG(3)));
}

/* The program's D3DADAPTER_IDENTIFIER8 is 0x42c bytes (1068) and the host's 1072 (padded to the
   LARGE_INTEGER's 8): DXVK fills a host copy and the program gets its 1068 (DXVK wrote 4 bytes past
   the program's structure before, into the next vector element or the heap). */
#define ADAPTER_ID8_GUEST 0x42cu
HOSTFN(sp_IDirect3D8_GetAdapterIdentifier, 4)
{
    uint32_t out = ARG(3);
    if (!out)
        return D3DERR_INVALIDCALL;
    D3DADAPTER_IDENTIFIER8 id;
    memset(&id, 0, sizeof id);
    uint32_t r;
    const TestCard *tc = test_card();
    if (tc) {
        if (ARG(1) != 0)
            return D3DERR_INVALIDCALL;
        snprintf(id.Driver, sizeof id.Driver, "%s", tc->driver);
        snprintf(id.Description, sizeof id.Description, "%s", tc->desc);
        id.DriverVersion.QuadPart = INT64_MAX;
        id.VendorId = tc->vendor;
        id.DeviceId = tc->device;
        static const uint8_t guid[16] = {0x41, 0x6e, 0x74, 0x69, 0x53, 0x74, 0x61, 0x74,
                                         0x69, 0x63, 0x20, 0x74, 0x65, 0x73, 0x74, 0x21};
        memcpy(&id.DeviceIdentifier, guid, 16);
        r = D3D_OK;
    } else {
        void *self = d3d_host(ARG(0));
        r = (uint32_t)((HRESULT (*)(void *, UINT, DWORD, D3DADAPTER_IDENTIFIER8 *))VT(self)[VI_IDirect3D8_GetAdapterIdentifier])(
            self, ARG(1), ARG(2), &id);
        if ((int32_t)r < 0)
            return r;
    }
    memcpy(gptr(out), &id, ADAPTER_ID8_GUEST);
    return r;
}

/* the capabilities: DXVK's (D3DCAPS8 is all 4-byte fields, the same size on both sides), with the card's
   own MaxPointSize the test card's under --test */
static void caps_test(uint32_t caps, uint32_t r)
{
    const TestCard *tc = test_card();
    if (tc && caps && (int32_t)r >= 0)
        ((D3DCAPS8 *)gptr(caps))->MaxPointSize = tc->max_point;
}
HOSTFN(sp_IDirect3D8_GetDeviceCaps, 4)
{
    void *self = d3d_host(ARG(0));
    uint32_t r = (uint32_t)((HRESULT (*)(void *, UINT, D3DDEVTYPE, D3DCAPS8 *))VT(self)[VI_IDirect3D8_GetDeviceCaps])(
        self, ARG(1), (D3DDEVTYPE)ARG(2), ARG(3) ? (D3DCAPS8 *)gptr(ARG(3)) : NULL);
    caps_test(ARG(3), r);
    return r;
}
HOSTFN(sp_IDirect3DDevice8_GetDeviceCaps, 2)
{
    void *self = d3d_host(ARG(0));
    uint32_t r = (uint32_t)((HRESULT (*)(void *, D3DCAPS8 *))VT(self)[VI_IDirect3DDevice8_GetDeviceCaps])(
        self, ARG(1) ? (D3DCAPS8 *)gptr(ARG(1)) : NULL);
    caps_test(ARG(1), r);
    return r;
}
HOSTFN(sp_IDirect3DDevice8_GetAvailableTextureMem, 1)
{
    if (test_card()) {
        long mb = atol(option_str("test_vram", "2047"));
        return mb < 1 ? 1u << 20 : mb > 4095 ? 4095u << 20 : (uint32_t)mb << 20;
    }
    void *self = d3d_host(ARG(0));
    return ((UINT (*)(void *))VT(self)[VI_IDirect3DDevice8_GetAvailableTextureMem])(self);
}

/* the desktop's mode: under --test (a test card) and with no screen 1920x1080 at 60, as the toolkit's
   D3D9 bridge answers (a game can keep it beside the adapter: one port's runs on a headless desktop of
   1024x768 and a hidden display of 1920x1080 parted from frame 10 [verified: their memory]) */
HOSTFN(sp_IDirect3D8_GetAdapterDisplayMode, 3)
{
    if (test_card() || (!deterministic && fixed_modes() && !PORT_D3D8_FIXED_MODES)) {   /* (test_card = real: the PC's own) */
        if (ARG(1) != 0 || !ARG(2))
            return D3DERR_INVALIDCALL;
        D3DDISPLAYMODE m = {1920, 1080, 60, D3DFMT_X8R8G8B8};
        memcpy(gptr(ARG(2)), &m, sizeof m);
        return D3D_OK;
    }
    void *self = d3d_host(ARG(0));
    return (uint32_t)((HRESULT (*)(void *, UINT, D3DDISPLAYMODE *))VT(self)[VI_IDirect3D8_GetAdapterDisplayMode])(
        self, ARG(1), ARG(2) ? (D3DDISPLAYMODE *)gptr(ARG(2)) : NULL);
}

HOSTFN(sp_IDirect3D8_GetAdapterMonitor, 2) { return 0x00120001u; }
HOSTFN(sp_IDirect3D8_RegisterSoftwareDevice, 2) { return D3DERR_INVALIDCALL; }

/* ------------------------------------------------------------------ IDirect3DDevice8 */
HOSTFN(sp_IDirect3DDevice8_Reset, 2)
{
    void *self = d3d_host(ARG(0));
    D3DPRESENT_PARAMETERS pp;
    pp_in(ARG(1), &pp, 0);
    uint32_t r = (uint32_t)((HRESULT (*)(void *, D3DPRESENT_PARAMETERS *))VT(self)[VI_IDirect3DDevice8_Reset])(self, &pp);
    LOG(LOG_ALWAYS, "Reset -> %08x", r);
    pp_out(ARG(1), &pp);
    return r;
}

HOSTFN(sp_IDirect3DDevice8_CreateAdditionalSwapChain, 3)
{
    void *self = d3d_host(ARG(0));
    D3DPRESENT_PARAMETERS pp;
    pp_in(ARG(1), &pp, 0);
    void *sc = NULL;
    uint32_t r = (uint32_t)((HRESULT (*)(void *, D3DPRESENT_PARAMETERS *, void **))
                            VT(self)[VI_IDirect3DDevice8_CreateAdditionalSwapChain])(self, &pp, &sc);
    pp_out(ARG(1), &pp);
    if (ARG(2)) W32(ARG(2), (int32_t)r >= 0 ? d3d_wrap_new(DK_IDirect3DSwapChain8, sc) : 0);
    return r;
}

HOSTFN(sp_IDirect3DDevice8_GetCreationParameters, 2)
{
    void *self = d3d_host(ARG(0));
    D3DDEVICE_CREATION_PARAMETERS p;
    uint32_t r = (uint32_t)((HRESULT (*)(void *, D3DDEVICE_CREATION_PARAMETERS *))
                            VT(self)[VI_IDirect3DDevice8_GetCreationParameters])(self, &p);
    W32(ARG(1), p.AdapterOrdinal);
    W32(ARG(1) + 4, p.DeviceType);
    W32(ARG(1) + 8, main_hwnd);
    W32(ARG(1) + 12, p.BehaviorFlags & ~(uint32_t)D3DCREATE_FPU_PRESERVE);
    return r;
}

/* --shot FRAME FILE (main.c): the back buffer of that presented frame, read before the present
   (after it the buffer is undefined), written by d3d_write_png */
static uint8_t *shot_rgb;
static uint32_t shot_w, shot_h;

static void shot_capture(void *dev)
{
    void *bb = NULL, *sys = NULL;
    if (((HRESULT (*)(void *, UINT, D3DBACKBUFFER_TYPE, void **))VT(dev)[VI_IDirect3DDevice8_GetBackBuffer])(
            dev, 0, D3DBACKBUFFER_TYPE_MONO, &bb) < 0 || !bb)
        return;
    D3DSURFACE_DESC d;
    ((HRESULT (*)(void *, D3DSURFACE_DESC *))VT(bb)[VI_IDirect3DSurface8_GetDesc])(bb, &d);
    if (((HRESULT (*)(void *, UINT, UINT, D3DFORMAT, void **))VT(dev)[VI_IDirect3DDevice8_CreateImageSurface])(
            dev, d.Width, d.Height, d.Format, &sys) >= 0 && sys &&
        ((HRESULT (*)(void *, void *, const RECT *, UINT, void *, const POINT *))VT(dev)[VI_IDirect3DDevice8_CopyRects])(
            dev, bb, NULL, 0, sys, NULL) < 0) {
        /* the copy failed: read the (lockable, pp_in) back buffer itself */
        ((ULONG (*)(void *))VT(sys)[2])(sys);
        sys = bb;
        ((ULONG (*)(void *))VT(bb)[1])(bb);
    }
    if (sys) {
        D3DLOCKED_RECT lr;
        if (((HRESULT (*)(void *, D3DLOCKED_RECT *, const RECT *, DWORD))VT(sys)[VI_IDirect3DSurface8_LockRect])(
                sys, &lr, NULL, D3DLOCK_READONLY) >= 0) {
            free(shot_rgb);
            shot_w = d.Width, shot_h = d.Height;
            shot_rgb = malloc((size_t)shot_w * shot_h * 3);
            for (uint32_t y = 0; y < shot_h; y++)
                for (uint32_t x = 0; x < shot_w; x++) {
                    const uint8_t *row = (const uint8_t *)lr.pBits + (size_t)y * lr.Pitch;
                    uint8_t *o = shot_rgb + ((size_t)y * shot_w + x) * 3;
                    if (d.Format == D3DFMT_R5G6B5) {
                        uint16_t p = ((const uint16_t *)row)[x];
                        o[0] = (uint8_t)((p >> 11) * 255 / 31), o[1] = (uint8_t)(((p >> 5) & 63) * 255 / 63), o[2] = (uint8_t)((p & 31) * 255 / 31);
                    } else if (d.Format == D3DFMT_X1R5G5B5 || d.Format == D3DFMT_A1R5G5B5) {
                        uint16_t p = ((const uint16_t *)row)[x];
                        o[0] = (uint8_t)(((p >> 10) & 31) * 255 / 31), o[1] = (uint8_t)(((p >> 5) & 31) * 255 / 31), o[2] = (uint8_t)((p & 31) * 255 / 31);
                    } else {
                        const uint8_t *p = row + 4 * x;
                        o[0] = p[2], o[1] = p[1], o[2] = p[0];
                    }
                }
            ((HRESULT (*)(void *))VT(sys)[VI_IDirect3DSurface8_UnlockRect])(sys);
        }
    }
    if (sys) ((ULONG (*)(void *))VT(sys)[2])(sys);
    ((ULONG (*)(void *))VT(bb)[2])(bb);
}

static void png_chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n)
{
    uint8_t h[8] = {n >> 24, n >> 16, n >> 8, n, type[0], type[1], type[2], type[3]};
    fwrite(h, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    uint32_t crc = crc32(0, (const uint8_t *)type, 4);
    if (n) crc = crc32(crc, d, n);
    uint8_t t[4] = {crc >> 24, crc >> 16, crc >> 8, crc};
    fwrite(t, 1, 4, f);
}

int d3d_write_png(const char *path)
{
    if (!shot_rgb) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t rowb = (size_t)shot_w * 3 + 1, raw = rowb * shot_h;
    uint8_t *buf = malloc(raw);
    for (uint32_t y = 0; y < shot_h; y++) {
        buf[y * rowb] = 0;
        memcpy(buf + y * rowb + 1, shot_rgb + (size_t)y * shot_w * 3, (size_t)shot_w * 3);
    }
    uLongf zn = compressBound(raw);
    uint8_t *z = malloc(zn);
    compress2(z, &zn, buf, raw, 6);
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ih[13] = {shot_w >> 24, shot_w >> 16, shot_w >> 8, shot_w, shot_h >> 24, shot_h >> 16, shot_h >> 8, shot_h, 8, 2, 0, 0, 0};
    png_chunk(f, "IHDR", ih, 13);
    png_chunk(f, "IDAT", z, (uint32_t)zn);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(buf);
    free(z);
    return 1;
}

HOSTFN(sp_IDirect3DDevice8_Present, 5)
{
    void *self = d3d_host(ARG(0));
    if (shot_due()) shot_capture(self);
    uint32_t r = (uint32_t)((HRESULT (*)(void *, const RECT *, const RECT *, HWND, const RGNDATA *))
                            VT(self)[VI_IDirect3DDevice8_Present])(
        self, (const RECT *)gptr(ARG(1)), (const RECT *)gptr(ARG(2)), NULL, (const RGNDATA *)gptr(ARG(4)));
    frame_presented();
    return r;
}

HOSTFN(sp_IDirect3DSwapChain8_Present, 5)
{
    void *self = d3d_host(ARG(0));
    uint32_t r = (uint32_t)((HRESULT (*)(void *, const RECT *, const RECT *, HWND, const RGNDATA *))
                            VT(self)[VI_IDirect3DSwapChain8_Present])(
        self, (const RECT *)gptr(ARG(1)), (const RECT *)gptr(ARG(2)), NULL, (const RGNDATA *)gptr(ARG(4)));
    frame_presented();
    return r;
}

/* ------------------------------------------------------------------ buffer locks */
/* Lock(offset, size, BYTE **data, flags); GetDesc's Size at 16 for both buffer kinds */
static uint32_t buffer_lock(CPU *c, int vi_lock, int vi_desc)
{
    void *self = d3d_host(ARG(0));
    Ent *e = ent_of_guest(ARG(0));
    uint32_t off = ARG(1), len = ARG(2), flags = ARG(4);
    if (!e) fatal("buffer lock on an unknown object");
    if (!e->shadow) {
        uint8_t desc[32];
        ((HRESULT (*)(void *, void *))VT(self)[vi_desc])(self, desc);
        memcpy(&e->size, desc + 16, 4);
        e->shadow = galloc(e->size + 16);
        memset(gptr(e->shadow), 0, e->size);
        if (vk_renderer)                /* (our renderer's host copy: this one, vk_d3d8.c) */
            vk8_buf_host(self, gptr(e->shadow));
    }
    if (off > e->size) off = e->size;
    if (!len || off + len > e->size) len = e->size - off;
    /* A plain lock (no DISCARD or NOOVERWRITE) covers the whole buffer: on Windows the pointer is
       the buffer's own memory, so bytes written past the range asked for count. Penguins! asks for
       its vertex count in bytes and fills the whole buffer (Lock(0, 9996) of 9996 x 36 bytes)
       [verified: lock log, and the bound buffer's vertices all zero past the range] */
    uint32_t want = off;
    if (!(flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE | D3DLOCK_READONLY))) {
        off = 0;
        len = e->size;
    }
    BYTE *hp = NULL;
    uint32_t r = (uint32_t)((HRESULT (*)(void *, UINT, UINT, BYTE **, DWORD))VT(self)[vi_lock])(self, off, len, &hp, flags);
    if ((int32_t)r < 0 || !hp) {
        W32(ARG(3), 0);
        return r;
    }
    if ((flags & D3DLOCK_READONLY) && hp != (BYTE *)gptr(e->shadow + off))   /* (our renderer: the same bytes) */
        memcpy(gptr(e->shadow + off), hp, len);
    if (e->nlocks < 4)
        e->locks[e->nlocks] = (typeof(e->locks[0])){off, (flags & D3DLOCK_READONLY) ? 0 : len, hp};
    e->nlocks++;
    W32(ARG(3), e->shadow + want);
    LOG(LOG_DRAW, "buffer %08x lock %u+%u of %u, flags %x -> %08x", ARG(0), ARG(1), ARG(2), e->size, flags, e->shadow + want);
    return r;
}

static uint32_t buffer_unlock(CPU *c, int vi_unlock)
{
    void *self = d3d_host(ARG(0));
    Ent *e = ent_of_guest(ARG(0));
    if (e && e->nlocks > 0) {
        e->nlocks--;
        if (e->nlocks < 4 && e->locks[e->nlocks].len && e->locks[e->nlocks].hp != gptr(e->shadow + e->locks[e->nlocks].off))
            memcpy(e->locks[e->nlocks].hp, gptr(e->shadow + e->locks[e->nlocks].off), e->locks[e->nlocks].len);
    }
    return (uint32_t)((HRESULT (*)(void *))VT(self)[vi_unlock])(self);
}

HOSTFN(sp_IDirect3DVertexBuffer8_Lock, 5) { return buffer_lock(c, VI_IDirect3DVertexBuffer8_Lock, VI_IDirect3DVertexBuffer8_GetDesc); }
HOSTFN(sp_IDirect3DVertexBuffer8_Unlock, 1) { return buffer_unlock(c, VI_IDirect3DVertexBuffer8_Unlock); }
HOSTFN(sp_IDirect3DIndexBuffer8_Lock, 5) { return buffer_lock(c, VI_IDirect3DIndexBuffer8_Lock, VI_IDirect3DIndexBuffer8_GetDesc); }
HOSTFN(sp_IDirect3DIndexBuffer8_Unlock, 1) { return buffer_unlock(c, VI_IDirect3DIndexBuffer8_Unlock); }

/* ------------------------------------------------------------------ texture and surface locks */
/* bytes per block and block size of a format (4 for the DXT formats, else 1) */
static int fmt_info(uint32_t f, int *block)
{
    *block = 1;
    switch (f) {
    case D3DFMT_DXT1: *block = 4; return 8;
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: *block = 4; return 16;
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A2B10G10R10: case D3DFMT_G16R16:
    case D3DFMT_Q8W8V8U8: case D3DFMT_V16U16: case D3DFMT_X8L8V8U8: case D3DFMT_W11V11U10:
    case D3DFMT_A2W10V10U10: case D3DFMT_D32: case D3DFMT_D24S8: case D3DFMT_D24X8: case D3DFMT_D24X4S4:
    case D3DFMT_INDEX32:
        return 4;
    case D3DFMT_R8G8B8: return 3;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4: case D3DFMT_A8R3G3B2: case D3DFMT_A8L8: case D3DFMT_V8U8: case D3DFMT_L6V5U5:
    case D3DFMT_D16: case D3DFMT_D16_LOCKABLE: case D3DFMT_D15S1: case D3DFMT_A8P8: case D3DFMT_INDEX16:
    case D3DFMT_UYVY: case D3DFMT_YUY2:
        return 2;
    case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_P8: case D3DFMT_R3G3B2: case D3DFMT_A4L4:
        return 1;
    }
    return 4;
}

typedef struct Lk { struct Lk *next; void *host; uint32_t sub; uint32_t shadow, bytes; void *hp; int readonly; } Lk;
static Lk *lks;

static void lk_push(void *self, uint32_t sub, uint32_t sh, uint32_t bytes, void *hp, int readonly)
{
    Lk *k = malloc(sizeof *k);
    *k = (Lk){NULL, self, sub, sh, bytes, hp, readonly};
    pthread_mutex_lock(&dlock);
    k->next = lks;
    lks = k;
    pthread_mutex_unlock(&dlock);
}

static uint32_t rect_lock(void *self, uint32_t sub, const D3DSURFACE_DESC *d, uint32_t lr, uint32_t rect,
                          uint32_t flags, HRESULT (*lock)(void *, uint32_t, D3DLOCKED_RECT *, const RECT *, DWORD))
{
    D3DLOCKED_RECT hl;
    /* the whole surface on the host: the program's copy covers it, and a rectangle is an offset */
    HRESULT r = lock(self, sub, &hl, NULL, flags & ~(uint32_t)D3DLOCK_NO_DIRTY_UPDATE);
    if (r < 0) { W32(lr, 0); W32(lr + 4, 0); return (uint32_t)r; }
    int block, bpb = fmt_info(d->Format, &block);
    uint32_t rows = (d->Height + block - 1) / block;
    uint32_t bytes = (uint32_t)hl.Pitch * rows;
    uint32_t sh = galloc(bytes + 16);
    if (!(flags & D3DLOCK_DISCARD))
        memcpy(gptr(sh), hl.pBits, bytes);
    uint32_t off = 0;
    if (rect) {
        int32_t x = (int32_t)R32(rect), y = (int32_t)R32(rect + 4);
        off = (uint32_t)(y / block) * (uint32_t)hl.Pitch + (uint32_t)(x / block) * (uint32_t)bpb;
    }
    lk_push(self, sub, sh, bytes, hl.pBits, (flags & D3DLOCK_READONLY) != 0);
    W32(lr, (uint32_t)hl.Pitch);
    W32(lr + 4, sh + off);
    return (uint32_t)r;
}

static void rect_unlock_copy(void *self, uint32_t sub)
{
    pthread_mutex_lock(&dlock);
    Lk **pk = &lks, *k = NULL;
    for (; *pk; pk = &(*pk)->next)
        if ((*pk)->host == self && (*pk)->sub == sub) { k = *pk; *pk = k->next; break; }
    pthread_mutex_unlock(&dlock);
    if (!k) {
        LOG(LOG_ALWAYS, "unlock of %p (%u) that was not locked", self, sub);
        return;
    }
    if (!k->readonly)
        memcpy(k->hp, gptr(k->shadow), k->bytes);
    gfree(k->shadow);
    free(k);
}

/* the per-interface lock functions, with the subresource in a common form */
static HRESULT tex_lock(void *s, uint32_t sub, D3DLOCKED_RECT *l, const RECT *r, DWORD f)
{ return ((HRESULT (*)(void *, UINT, D3DLOCKED_RECT *, const RECT *, DWORD))VT(s)[VI_IDirect3DTexture8_LockRect])(s, sub, l, r, f); }
static HRESULT cube_lock(void *s, uint32_t sub, D3DLOCKED_RECT *l, const RECT *r, DWORD f)
{ return ((HRESULT (*)(void *, D3DCUBEMAP_FACES, UINT, D3DLOCKED_RECT *, const RECT *, DWORD))VT(s)[VI_IDirect3DCubeTexture8_LockRect])(s, (D3DCUBEMAP_FACES)(sub >> 16), sub & 0xffff, l, r, f); }
static HRESULT surf_lock(void *s, uint32_t sub, D3DLOCKED_RECT *l, const RECT *r, DWORD f)
{ (void)sub; return ((HRESULT (*)(void *, D3DLOCKED_RECT *, const RECT *, DWORD))VT(s)[VI_IDirect3DSurface8_LockRect])(s, l, r, f); }

HOSTFN(sp_IDirect3DTexture8_LockRect, 5)
{
    void *self = d3d_host(ARG(0));
    D3DSURFACE_DESC d;
    ((HRESULT (*)(void *, UINT, D3DSURFACE_DESC *))VT(self)[VI_IDirect3DTexture8_GetLevelDesc])(self, ARG(1), &d);
    return rect_lock(self, ARG(1), &d, ARG(2), ARG(3), ARG(4), tex_lock);
}

HOSTFN(sp_IDirect3DTexture8_UnlockRect, 2)
{
    void *self = d3d_host(ARG(0));
    rect_unlock_copy(self, ARG(1));
    return (uint32_t)((HRESULT (*)(void *, UINT))VT(self)[VI_IDirect3DTexture8_UnlockRect])(self, ARG(1));
}

HOSTFN(sp_IDirect3DCubeTexture8_LockRect, 6)
{
    void *self = d3d_host(ARG(0));
    D3DSURFACE_DESC d;
    ((HRESULT (*)(void *, UINT, D3DSURFACE_DESC *))VT(self)[VI_IDirect3DCubeTexture8_GetLevelDesc])(self, ARG(2), &d);
    return rect_lock(self, (ARG(1) << 16) | ARG(2), &d, ARG(3), ARG(4), ARG(5), cube_lock);
}

HOSTFN(sp_IDirect3DCubeTexture8_UnlockRect, 3)
{
    void *self = d3d_host(ARG(0));
    rect_unlock_copy(self, (ARG(1) << 16) | ARG(2));
    return (uint32_t)((HRESULT (*)(void *, D3DCUBEMAP_FACES, UINT))VT(self)[VI_IDirect3DCubeTexture8_UnlockRect])(
        self, (D3DCUBEMAP_FACES)ARG(1), ARG(2));
}

HOSTFN(sp_IDirect3DSurface8_LockRect, 4)
{
    void *self = d3d_host(ARG(0));
    D3DSURFACE_DESC d;
    ((HRESULT (*)(void *, D3DSURFACE_DESC *))VT(self)[VI_IDirect3DSurface8_GetDesc])(self, &d);
    return rect_lock(self, 0, &d, ARG(1), ARG(2), ARG(3), surf_lock);
}

HOSTFN(sp_IDirect3DSurface8_UnlockRect, 1)
{
    void *self = d3d_host(ARG(0));
    rect_unlock_copy(self, 0);
    return (uint32_t)((HRESULT (*)(void *))VT(self)[VI_IDirect3DSurface8_UnlockRect])(self);
}

/* volumes: boxes of slices */
static uint32_t box_lock(void *self, uint32_t sub, const D3DVOLUME_DESC *d, uint32_t lb, uint32_t box,
                         uint32_t flags, HRESULT (*lock)(void *, uint32_t, D3DLOCKED_BOX *, const D3DBOX *, DWORD))
{
    D3DLOCKED_BOX hb;
    HRESULT r = lock(self, sub, &hb, NULL, flags);
    if (r < 0) { W32(lb, 0); W32(lb + 4, 0); W32(lb + 8, 0); return (uint32_t)r; }
    int block, bpb = fmt_info(d->Format, &block);
    uint32_t bytes = (uint32_t)hb.SlicePitch * d->Depth;
    uint32_t sh = galloc(bytes + 16);
    if (!(flags & D3DLOCK_DISCARD))
        memcpy(gptr(sh), hb.pBits, bytes);
    uint32_t off = 0;
    if (box)
        off = R32(box + 16) * (uint32_t)hb.SlicePitch + (R32(box + 4) / block) * (uint32_t)hb.RowPitch +
              (R32(box) / block) * (uint32_t)bpb;
    lk_push(self, sub, sh, bytes, hb.pBits, (flags & D3DLOCK_READONLY) != 0);
    W32(lb, (uint32_t)hb.RowPitch);
    W32(lb + 4, (uint32_t)hb.SlicePitch);
    W32(lb + 8, sh + off);
    return (uint32_t)r;
}

static HRESULT voltex_lock(void *s, uint32_t sub, D3DLOCKED_BOX *l, const D3DBOX *b, DWORD f)
{ return ((HRESULT (*)(void *, UINT, D3DLOCKED_BOX *, const D3DBOX *, DWORD))VT(s)[VI_IDirect3DVolumeTexture8_LockBox])(s, sub, l, b, f); }
static HRESULT vol_lock(void *s, uint32_t sub, D3DLOCKED_BOX *l, const D3DBOX *b, DWORD f)
{ (void)sub; return ((HRESULT (*)(void *, D3DLOCKED_BOX *, const D3DBOX *, DWORD))VT(s)[VI_IDirect3DVolume8_LockBox])(s, l, b, f); }

HOSTFN(sp_IDirect3DVolumeTexture8_LockBox, 5)
{
    void *self = d3d_host(ARG(0));
    D3DVOLUME_DESC d;
    ((HRESULT (*)(void *, UINT, D3DVOLUME_DESC *))VT(self)[VI_IDirect3DVolumeTexture8_GetLevelDesc])(self, ARG(1), &d);
    return box_lock(self, ARG(1), &d, ARG(2), ARG(3), ARG(4), voltex_lock);
}

HOSTFN(sp_IDirect3DVolumeTexture8_UnlockBox, 2)
{
    void *self = d3d_host(ARG(0));
    rect_unlock_copy(self, ARG(1));
    return (uint32_t)((HRESULT (*)(void *, UINT))VT(self)[VI_IDirect3DVolumeTexture8_UnlockBox])(self, ARG(1));
}

HOSTFN(sp_IDirect3DVolume8_LockBox, 4)
{
    void *self = d3d_host(ARG(0));
    D3DVOLUME_DESC d;
    ((HRESULT (*)(void *, D3DVOLUME_DESC *))VT(self)[VI_IDirect3DVolume8_GetDesc])(self, &d);
    return box_lock(self, 0, &d, ARG(1), ARG(2), ARG(3), vol_lock);
}

HOSTFN(sp_IDirect3DVolume8_UnlockBox, 1)
{
    void *self = d3d_host(ARG(0));
    rect_unlock_copy(self, 0);
    return (uint32_t)((HRESULT (*)(void *))VT(self)[VI_IDirect3DVolume8_UnlockBox])(self);
}

/* ------------------------------------------------------------------ the rest */
static uint32_t get_container(CPU *c, int index)
{
    void *self = d3d_host(ARG(0));
    GUID g;
    memcpy(&g, gptr(ARG(1)), sizeof g);
    void *o = NULL;
    uint32_t r = (uint32_t)((HRESULT (*)(void *, const GUID *, void **))VT(self)[index])(self, &g, &o);
    W32(ARG(2), (int32_t)r >= 0 && o ? d3d_wrap(kind_of_iid(&g, DK_IDirect3DResource8), o) : 0);
    return r;
}

HOSTFN(sp_IDirect3DSurface8_GetContainer, 3) { return get_container(c, VI_IDirect3DSurface8_GetContainer); }
HOSTFN(sp_IDirect3DVolume8_GetContainer, 3) { return get_container(c, VI_IDirect3DVolume8_GetContainer); }

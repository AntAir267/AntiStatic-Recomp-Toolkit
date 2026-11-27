/* <PORT_ENV>_DUMP_CAPS=1 (a measuring aid for our renderer): the answers the game can ask of an
   IDirect3D8 at the start (caps, adapter, format, depth, device type and multisample checks, modes),
   from whichever renderer runs, as "capdump" lines in the log, so the two renderers' are diffed (the
   game's memory must not depend on the renderer: etm-native's lesson). */
#define _GNU_SOURCE
#include <d3d8.h>
#undef LOG
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/syscall.h>
#endif
#include "runtime.h"
#include "d3d8_gen.h"

#define VT(o) (*(void ***)(o))

static void vk_dump_caps8(void *d);

/* <PORT_ENV>_DRAWSHOT=F:A:B[:DIR] (a measuring aid for our renderer): the back buffer after each draw A..B of the
   frame presented as F, as DIR/drawshot_<n>.ppm (default work/vk/ds), from whichever renderer runs: the
   host device's vtable is wrapped (the program's objects and memory untouched), so the first draw whose
   picture differs between the two renderers is found. */
static void **orig_d3d, **orig_dev;
static void *hook_d3d[64], *hook_dev[256];
static unsigned long ds_frame;
static int ds_a, ds_b, ds_n;
static unsigned long ds_last_frame = ~0ul;
static char ds_dir[512] = "work/vk/ds";

static void ds_capture(void *dev, int n)
{
    void *bb = NULL, *sys = NULL;
    if (((HRESULT (*)(void *, UINT, D3DBACKBUFFER_TYPE, void **))orig_dev[VI_IDirect3DDevice8_GetBackBuffer])(
            dev, 0, D3DBACKBUFFER_TYPE_MONO, &bb) < 0 || !bb)
        return;
    D3DSURFACE_DESC sd;
    ((HRESULT (*)(void *, D3DSURFACE_DESC *))VT(bb)[VI_IDirect3DSurface8_GetDesc])(bb, &sd);
    if (((HRESULT (*)(void *, UINT, UINT, D3DFORMAT, void **))orig_dev[VI_IDirect3DDevice8_CreateImageSurface])(
            dev, sd.Width, sd.Height, sd.Format, &sys) >= 0 && sys &&
        ((HRESULT (*)(void *, void *, const RECT *, UINT, void *, const POINT *))orig_dev[VI_IDirect3DDevice8_CopyRects])(
            dev, bb, NULL, 0, sys, NULL) >= 0) {
        D3DLOCKED_RECT lr;
        if (((HRESULT (*)(void *, D3DLOCKED_RECT *, const RECT *, DWORD))VT(sys)[VI_IDirect3DSurface8_LockRect])(sys, &lr, NULL, D3DLOCK_READONLY) >= 0) {
            uint64_t h = 0xcbf29ce484222325ull;
            for (UINT y = 0; y < sd.Height; y++)
                for (UINT x = 0; x < sd.Width; x++) {
                    const uint8_t *p = (const uint8_t *)lr.pBits + (size_t)y * lr.Pitch + 4 * x;
                    for (int c = 0; c < 3; c++) h = (h ^ p[c]) * 0x100000001b3ull;
                }
            LOG(LOG_ALWAYS, "drawshot %d: picture %016llx", n, (unsigned long long)h);
            char path[700];
            snprintf(path, sizeof path, "%s/drawshot_%05d.ppm", ds_dir, n);
            FILE *f = port_getenv("DRAWSHOT_PPM") ? fopen(path, "wb") : NULL;
            if (f) {
                fprintf(f, "P6\n%u %u\n255\n", sd.Width, sd.Height);
                for (UINT y = 0; y < sd.Height; y++)
                    for (UINT x = 0; x < sd.Width; x++) {
                        const uint8_t *p = (const uint8_t *)lr.pBits + (size_t)y * lr.Pitch + 4 * x;
                        uint8_t o[3] = {p[2], p[1], p[0]};
                        fwrite(o, 1, 3, f);
                    }
                fclose(f);
            }
            ((HRESULT (*)(void *))VT(sys)[VI_IDirect3DSurface8_UnlockRect])(sys);
        }
    }
    if (sys) ((ULONG (*)(void *))VT(sys)[2])(sys);
    ((ULONG (*)(void *))VT(bb)[2])(bb);
}

static void ds_after_draw(void *dev, const char *what, UINT a, UINT b)
{
    if (frame_count != ds_last_frame) { ds_last_frame = frame_count; ds_n = 0; }
    int n = ds_n++;
    if (frame_count + 1 != ds_frame || n < ds_a || n > ds_b) return;
    DWORD vs = 0, ps = 0;
    ((HRESULT (*)(void *, DWORD *))orig_dev[VI_IDirect3DDevice8_GetVertexShader])(dev, &vs);
    ((HRESULT (*)(void *, DWORD *))orig_dev[VI_IDirect3DDevice8_GetPixelShader])(dev, &ps);
    DWORD fog = 0, table = 0, light = 0, blend = 0;
    ((HRESULT (*)(void *, D3DRENDERSTATETYPE, DWORD *))orig_dev[VI_IDirect3DDevice8_GetRenderState])(dev, D3DRS_FOGENABLE, &fog);
    ((HRESULT (*)(void *, D3DRENDERSTATETYPE, DWORD *))orig_dev[VI_IDirect3DDevice8_GetRenderState])(dev, D3DRS_FOGTABLEMODE, &table);
    ((HRESULT (*)(void *, D3DRENDERSTATETYPE, DWORD *))orig_dev[VI_IDirect3DDevice8_GetRenderState])(dev, D3DRS_LIGHTING, &light);
    ((HRESULT (*)(void *, D3DRENDERSTATETYPE, DWORD *))orig_dev[VI_IDirect3DDevice8_GetRenderState])(dev, D3DRS_ALPHABLENDENABLE, &blend);
    LOG(LOG_ALWAYS, "drawshot %d: %s %u %u (vertex shader %x, pixel shader %x; fog %u table %u, lighting %u, blend %u)", n, what,
        a, b, (unsigned)vs, (unsigned)ps, (unsigned)fog, (unsigned)table, (unsigned)light, (unsigned)blend);
    if (port_getenv("DRAWSHOT_XF")) {    /* <PORT_ENV>_DRAWSHOT_XF=1: the transforms and the draw's first vertices */
        static const D3DTRANSFORMSTATETYPE ts[3] = { D3DTS_WORLD, D3DTS_VIEW, D3DTS_PROJECTION };
        static const char *tn[3] = { "world", "view", "proj" };
        for (int k = 0; k < 3; k++) {
            D3DMATRIX m;
            memset(&m, 0, sizeof m);
            ((HRESULT (*)(void *, D3DTRANSFORMSTATETYPE, D3DMATRIX *))orig_dev[VI_IDirect3DDevice8_GetTransform])(dev, ts[k], &m);
            const float *f = &m._11;
            LOG(LOG_ALWAYS, "drawshot %d: %s %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g", n, tn[k], f[0], f[1], f[2],
                f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
        }
        void *vb = NULL;
        UINT stride = 0;
        ((HRESULT (*)(void *, UINT, void **, UINT *))orig_dev[VI_IDirect3DDevice8_GetStreamSource])(dev, 0, &vb, &stride);
        BYTE *data = NULL;
        if (vb && stride && !strcmp(what, "DrawPrimitive") &&
            ((HRESULT (*)(void *, UINT, UINT, BYTE **, DWORD))(*(void ***)vb)[11])(vb, a * stride, 4 * stride, &data, 0x10) >= 0 && data) {
            for (int v = 0; v < 4; v++) {
                const float *f = (const float *)(data + v * stride);
                LOG(LOG_ALWAYS, "drawshot %d: vertex %d (stride %u): %g %g %g  %g %g %g  %08x  %g %g", n, v, stride, f[0], f[1],
                    f[2], f[3], f[4], f[5], ((const uint32_t *)f)[6], f[7], f[8]);
            }
            ((HRESULT (*)(void *))(*(void ***)vb)[12])(vb);
        }
        if (vb) ((ULONG (*)(void *))(*(void ***)vb)[2])(vb);
    }
    ds_capture(dev, n);
}
/* <PORT_ENV>_DRAWSHOT_RS=S=V[,S=V...] (with <PORT_ENV>_DRAWSHOT): render states set before every draw of frame F, in
   either renderer (e.g. 28=0: fog off, 137=0: lighting off), so a difference is narrowed to a stage;
   <PORT_ENV>_DRAWSHOT_TSS=T:S=V[,...] the same for texture stage states (stage T) */
static void ds_override(void *dev)
{
    if (frame_count + 1 != ds_frame) return;
    const char *e = port_getenv("DRAWSHOT_RS");
    for (unsigned st, v; e && sscanf(e, "%u=%u", &st, &v) == 2; e = strchr(e, ','), e = e ? e + 1 : NULL)
        ((HRESULT (*)(void *, DWORD, DWORD))orig_dev[VI_IDirect3DDevice8_SetRenderState])(dev, st, v);
    e = port_getenv("DRAWSHOT_TSS");
    for (unsigned t, st, v; e && sscanf(e, "%u:%u=%u", &t, &st, &v) == 3; e = strchr(e, ','), e = e ? e + 1 : NULL)
        ((HRESULT (*)(void *, DWORD, DWORD, DWORD))orig_dev[VI_IDirect3DDevice8_SetTextureStageState])(dev, t, st, v);
}
static HRESULT hk_DrawPrimitive(void *dev, D3DPRIMITIVETYPE p, UINT s, UINT n)
{
    ds_override(dev);
    HRESULT r = ((HRESULT (*)(void *, D3DPRIMITIVETYPE, UINT, UINT))orig_dev[VI_IDirect3DDevice8_DrawPrimitive])(dev, p, s, n);
    ds_after_draw(dev, "DrawPrimitive", s, n);
    return r;
}
static HRESULT hk_DrawIndexedPrimitive(void *dev, D3DPRIMITIVETYPE p, UINT mv, UINT nv, UINT si, UINT n)
{
    ds_override(dev);
    HRESULT r = ((HRESULT (*)(void *, D3DPRIMITIVETYPE, UINT, UINT, UINT, UINT))orig_dev[VI_IDirect3DDevice8_DrawIndexedPrimitive])(
        dev, p, mv, nv, si, n);
    ds_after_draw(dev, "DrawIndexedPrimitive", si, n);
    return r;
}
/* <PORT_ENV>_FRAMETIME=A:B (a measuring aid): from the present of frame A to that of frame B,
   each frame's wall time (present to present), the presenting (game) thread's CPU time and the whole
   process's, logged at B as "frametime" lines (mean, median, 95th / 99th percentile, worst), from either
   renderer. Headless --test runs never wait for the clock, so the wall time is what a frame costs.
   <PORT_ENV>_PROF=A:B[:FILE] (a measuring aid): the presenting thread sampled on its CPU clock (every 250 us of
   its CPU) from frame A to B; FILE (default work/vk/prof.txt) gets each sample's code address as module
   and offset, with a short frame-pointer walk (addr2line names them). */
static unsigned long ft_a, ft_b;
static double *ft_wall, *ft_cpu, *ft_pcpu;
static double ft_last[3];
static double clk(clockid_t id)
{
    struct timespec ts;
    clock_gettime(id, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static int dcmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static void ft_report(const char *what, double *v, int n)
{
    double sum = 0;
    int over16 = 0, over33 = 0;
    for (int i = 0; i < n; i++) sum += v[i], over16 += v[i] > 1000.0 / 60, over33 += v[i] > 1000.0 / 30;
    qsort(v, (size_t)n, sizeof *v, dcmp);
    LOG(LOG_ALWAYS, "frametime %s over %d frames: mean %.3f ms, median %.3f, p95 %.3f, p99 %.3f, worst %.3f; %d over 16.7 ms, %d over 33.3",
        what, n, sum / n, v[n / 2], v[(int)(n * 0.95)], v[(int)(n * 0.99)], v[n - 1], over16, over33);
}
static void ft_present(void)
{
    unsigned long f = frame_count + 1;          /* (the frame being presented) */
    if (!ft_b || f < ft_a || f > ft_b) return;
    double now[3] = {clk(CLOCK_MONOTONIC), clk(CLOCK_THREAD_CPUTIME_ID), clk(CLOCK_PROCESS_CPUTIME_ID)};
    if (f > ft_a) {
        int i = (int)(f - ft_a - 1);
        ft_wall[i] = now[0] - ft_last[0];
        ft_cpu[i] = now[1] - ft_last[1];
        ft_pcpu[i] = now[2] - ft_last[2];
    }
    memcpy(ft_last, now, sizeof now);
    if (f == ft_b) {
        int n = (int)(ft_b - ft_a);
        ft_report("wall", ft_wall, n);
        ft_report("game thread CPU", ft_cpu, n);
        ft_report("process CPU", ft_pcpu, n);
    }
}

#define PROF_MAX (1 << 20)
#define PROF_DEPTH 6
static unsigned long pf_a, pf_b;
static char pf_path[512] = "work/vk/prof.txt";
static uintptr_t (*pf_s)[PROF_DEPTH];
static volatile int pf_n, pf_on;
static uintptr_t pf_lo, pf_hi;                  /* the thread's stack */
#ifndef _WIN32
static timer_t pf_timer;
static void pf_handler(int sig, siginfo_t *si, void *uc_)
{
    (void)sig; (void)si;
    if (!pf_on || pf_n >= PROF_MAX) return;
    ucontext_t *uc = uc_;
    uintptr_t *o = pf_s[pf_n];
    memset(o, 0, sizeof pf_s[0]);
    o[0] = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    uintptr_t fp = (uintptr_t)uc->uc_mcontext.gregs[REG_RBP];
    for (int d = 1; d < PROF_DEPTH; d++) {
        if (fp < pf_lo || fp + 16 > pf_hi || (fp & 7)) break;
        uintptr_t next = ((uintptr_t *)fp)[0];
        o[d] = ((uintptr_t *)fp)[1];
        if (next <= fp) break;
        fp = next;
    }
    pf_n++;
}
static void pf_start(void)
{
    pthread_attr_t at;
    void *base;
    size_t sz;
    pthread_getattr_np(pthread_self(), &at);
    pthread_attr_getstack(&at, &base, &sz);
    pthread_attr_destroy(&at);
    pf_lo = (uintptr_t)base, pf_hi = (uintptr_t)base + sz;
    pf_s = calloc(PROF_MAX, sizeof *pf_s);
    struct sigaction sa = {0};
    sa.sa_sigaction = pf_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    struct sigevent ev = {0};
    ev.sigev_notify = SIGEV_THREAD_ID;
    ev.sigev_signo = SIGPROF;
    ev._sigev_un._tid = (pid_t)syscall(SYS_gettid);
    clockid_t cid;
    pthread_getcpuclockid(pthread_self(), &cid);
    if (timer_create(cid, &ev, &pf_timer) < 0) { LOG(LOG_ALWAYS, "prof: no timer"); return; }
    struct itimerspec it = {{0, 250000}, {0, 250000}};
    pf_on = 1;
    timer_settime(pf_timer, 0, &it, NULL);
}
static void pf_stop(void)
{
    pf_on = 0;
    timer_delete(pf_timer);
    FILE *f = fopen(pf_path, "w");
    if (!f) { LOG(LOG_ALWAYS, "prof: cannot write %s", pf_path); return; }
    for (int i = 0; i < pf_n; i++) {
        for (int d = 0; d < PROF_DEPTH && pf_s[i][d]; d++) {
            Dl_info di;
            if (dladdr((void *)pf_s[i][d], &di) && di.dli_fname)
                fprintf(f, "%s%s+%lx", d ? " " : "", di.dli_fname, (unsigned long)(pf_s[i][d] - (uintptr_t)di.dli_fbase));
            else
                fprintf(f, "%s?+%lx", d ? " " : "", (unsigned long)pf_s[i][d]);
        }
        fputc('\n', f);
    }
    fclose(f);
    LOG(LOG_ALWAYS, "prof: %d samples (frames %lu-%lu) in %s", pf_n, pf_a, pf_b, pf_path);
}
#else
/* (Windows: the sampling profiler is Linux's; not here) */
static void pf_start(void) { LOG(LOG_ALWAYS, "prof: not available on Windows"); }
static void pf_stop(void) {}
#endif
static void pf_present(void)
{
    unsigned long f = frame_count + 1;
    if (!pf_b) return;
    if (f == pf_a) pf_start();
    else if (f == pf_b) pf_stop();
}
static HRESULT hk_Present(void *dev, const RECT *a, const RECT *b, HWND w, const RGNDATA *r)
{
    ft_present();
    pf_present();
    return ((HRESULT (*)(void *, const RECT *, const RECT *, HWND, const RGNDATA *))orig_dev[VI_IDirect3DDevice8_Present])(dev, a, b, w, r);
}

static HRESULT hk_CreateDevice(void *d, UINT a, D3DDEVTYPE t, HWND w, DWORD f, D3DPRESENT_PARAMETERS *pp, void **out)
{
    HRESULT r = ((HRESULT (*)(void *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, void **))orig_d3d[VI_IDirect3D8_CreateDevice])(
        d, a, t, w, f, pp, out);
    if (r >= 0 && out && *out && !orig_dev) {
        orig_dev = VT(*out);
        memcpy(hook_dev, orig_dev, sizeof hook_dev);      /* (past the COM methods: C++'s own, copied as they are) */
        if (ds_frame) {
            hook_dev[VI_IDirect3DDevice8_DrawPrimitive] = (void *)hk_DrawPrimitive;
            hook_dev[VI_IDirect3DDevice8_DrawIndexedPrimitive] = (void *)hk_DrawIndexedPrimitive;
        }
        if (ft_b || pf_b) hook_dev[VI_IDirect3DDevice8_Present] = (void *)hk_Present;
        *(void ***)*out = hook_dev;
    }
    return r;
}

void vk_debug8(void *d)
{
    if (port_getenv("DUMP_CAPS")) vk_dump_caps8(d);
    if (orig_d3d) return;
    const char *e = port_getenv("DRAWSHOT");
    if (e && sscanf(e, "%lu:%d:%d:%511s", &ds_frame, &ds_a, &ds_b, ds_dir) < 3) ds_frame = 0;
    if ((e = port_getenv("FRAMETIME")) && sscanf(e, "%lu:%lu", &ft_a, &ft_b) == 2 && ft_b > ft_a) {
        ft_wall = calloc(ft_b - ft_a, sizeof *ft_wall);
        ft_cpu = calloc(ft_b - ft_a, sizeof *ft_cpu);
        ft_pcpu = calloc(ft_b - ft_a, sizeof *ft_pcpu);
    } else ft_b = 0;
    if (!((e = port_getenv("PROF")) && sscanf(e, "%lu:%lu:%511s", &pf_a, &pf_b, pf_path) >= 2 && pf_b > pf_a)) pf_b = 0;
    if (!ds_frame && !ft_b && !pf_b) return;
    orig_d3d = VT(d);
    memcpy(hook_d3d, orig_d3d, sizeof hook_d3d);
    hook_d3d[VI_IDirect3D8_CreateDevice] = (void *)hk_CreateDevice;
    *(void ***)d = hook_d3d;
    if (ds_frame) LOG(LOG_ALWAYS, "drawshot: frame %lu, draws %d to %d into %s", ds_frame, ds_a, ds_b, ds_dir);
}

static void vk_dump_caps8(void *d)
{
    D3DCAPS8 caps;
    char hex[2 * sizeof caps + 1];
    memset(&caps, 0, sizeof caps);
    HRESULT r = ((HRESULT (*)(void *, UINT, D3DDEVTYPE, D3DCAPS8 *))VT(d)[VI_IDirect3D8_GetDeviceCaps])(d, 0, D3DDEVTYPE_HAL, &caps);
    for (size_t i = 0; i < sizeof caps; i++) sprintf(hex + 2 * i, "%02x", ((uint8_t *)&caps)[i]);
    LOG(LOG_ALWAYS, "capdump caps %08x %zu bytes: %s", (unsigned)r, sizeof caps, hex);
    for (int t = 1; t <= 3; t++) {
        r = ((HRESULT (*)(void *, UINT, D3DDEVTYPE, D3DCAPS8 *))VT(d)[VI_IDirect3D8_GetDeviceCaps])(d, 0, (D3DDEVTYPE)t, &caps);
        LOG(LOG_ALWAYS, "capdump caps type %d -> %08x", t, (unsigned)r);
    }
    D3DADAPTER_IDENTIFIER8 id;
    for (DWORD fl = 0; fl < 3; fl++) {
        memset(&id, 0, sizeof id);
        r = ((HRESULT (*)(void *, UINT, DWORD, D3DADAPTER_IDENTIFIER8 *))VT(d)[VI_IDirect3D8_GetAdapterIdentifier])(d, 0, fl, &id);
        LOG(LOG_ALWAYS, "capdump adapter flags %u -> %08x: driver '%s' description '%s' vendor %04x device %04x subsys %08x "
            "revision %u whql %u version %llx guid %08x", (unsigned)fl, (unsigned)r, id.Driver, id.Description, (unsigned)id.VendorId,
            (unsigned)id.DeviceId, (unsigned)id.SubSysId, (unsigned)id.Revision, (unsigned)id.WHQLLevel,
            (unsigned long long)id.DriverVersion.QuadPart, (unsigned)id.DeviceIdentifier.Data1);
    }
    LOG(LOG_ALWAYS, "capdump adapters %u", ((UINT (*)(void *))VT(d)[VI_IDirect3D8_GetAdapterCount])(d));
    static const uint32_t fmts[] = {D3DFMT_R8G8B8, D3DFMT_A8R8G8B8, D3DFMT_X8R8G8B8, D3DFMT_R5G6B5, D3DFMT_X1R5G5B5, D3DFMT_A1R5G5B5,
        D3DFMT_A4R4G4B4, D3DFMT_R3G3B2, D3DFMT_A8, D3DFMT_A8R3G3B2, D3DFMT_X4R4G4B4, D3DFMT_A2B10G10R10, D3DFMT_G16R16, D3DFMT_A8P8,
        D3DFMT_P8, D3DFMT_L8, D3DFMT_A8L8, D3DFMT_A4L4, D3DFMT_V8U8, D3DFMT_L6V5U5, D3DFMT_X8L8V8U8, D3DFMT_Q8W8V8U8, D3DFMT_V16U16,
        D3DFMT_W11V11U10, D3DFMT_A2W10V10U10, D3DFMT_UYVY, D3DFMT_YUY2, D3DFMT_DXT1, D3DFMT_DXT2, D3DFMT_DXT3, D3DFMT_DXT4,
        D3DFMT_DXT5, D3DFMT_D16_LOCKABLE, D3DFMT_D32, D3DFMT_D15S1, D3DFMT_D24S8, D3DFMT_D16, D3DFMT_D24X8, D3DFMT_D24X4S4};
    static const uint32_t afmts[] = {D3DFMT_X8R8G8B8, D3DFMT_R5G6B5, D3DFMT_A8R8G8B8, D3DFMT_X1R5G5B5};
    static const uint32_t usages[] = {0, D3DUSAGE_RENDERTARGET, D3DUSAGE_DEPTHSTENCIL, D3DUSAGE_DYNAMIC};
    int n = 0;
    for (size_t a = 0; a < sizeof afmts / sizeof *afmts; a++)
        for (size_t k = 0; k < sizeof fmts / sizeof *fmts; k++) {
            char line[512];
            int p = 0;
            for (size_t u = 0; u < 4; u++)
                for (uint32_t rt = D3DRTYPE_SURFACE; rt <= D3DRTYPE_CUBETEXTURE; rt++) {
                    r = ((HRESULT (*)(void *, UINT, D3DDEVTYPE, D3DFORMAT, DWORD, D3DRESOURCETYPE, D3DFORMAT))VT(d)[VI_IDirect3D8_CheckDeviceFormat])(
                        d, 0, D3DDEVTYPE_HAL, (D3DFORMAT)afmts[a], usages[u], (D3DRESOURCETYPE)rt, (D3DFORMAT)fmts[k]);
                    p += snprintf(line + p, sizeof line - (size_t)p, " %x", (unsigned)r & 0xfff);
                    n++;
                }
            LOG(LOG_ALWAYS, "capdump format %u on %u:%s", fmts[k], afmts[a], line);
        }
    for (size_t a = 0; a < sizeof afmts / sizeof *afmts; a++)
        for (size_t b = 0; b < sizeof afmts / sizeof *afmts; b++)
            for (int w = 0; w < 2; w++)
                LOG(LOG_ALWAYS, "capdump devicetype %u %u %d -> %08x", afmts[a], afmts[b], w,
                    (unsigned)((HRESULT (*)(void *, UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, BOOL))VT(d)[VI_IDirect3D8_CheckDeviceType])(
                        d, 0, D3DDEVTYPE_HAL, (D3DFORMAT)afmts[a], (D3DFORMAT)afmts[b], w));
    for (size_t a = 0; a < sizeof afmts / sizeof *afmts; a++)
        for (size_t k = 0; k < sizeof fmts / sizeof *fmts; k++)
            LOG(LOG_ALWAYS, "capdump depthmatch %u %u -> %08x", afmts[a], fmts[k],
                (unsigned)((HRESULT (*)(void *, UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, D3DFORMAT))VT(d)[VI_IDirect3D8_CheckDepthStencilMatch])(
                    d, 0, D3DDEVTYPE_HAL, (D3DFORMAT)afmts[a], (D3DFORMAT)afmts[a], (D3DFORMAT)fmts[k]));
    static const uint32_t msf[] = {D3DFMT_A8R8G8B8, D3DFMT_X8R8G8B8, D3DFMT_R5G6B5, D3DFMT_D24S8, D3DFMT_D24X8, D3DFMT_D16};
    for (size_t k = 0; k < sizeof msf / sizeof *msf; k++)
        for (uint32_t ms = 0; ms <= 16; ms++)
            for (int w = 0; w < 2; w++)
                LOG(LOG_ALWAYS, "capdump multisample %u %u %d -> %08x", msf[k], ms, w,
                    (unsigned)((HRESULT (*)(void *, UINT, D3DDEVTYPE, D3DFORMAT, BOOL, D3DMULTISAMPLE_TYPE))VT(d)[VI_IDirect3D8_CheckDeviceMultiSampleType])(
                        d, 0, D3DDEVTYPE_HAL, (D3DFORMAT)msf[k], w, (D3DMULTISAMPLE_TYPE)ms));
    UINT nm = ((UINT (*)(void *, UINT))VT(d)[VI_IDirect3D8_GetAdapterModeCount])(d, 0);
    LOG(LOG_ALWAYS, "capdump modes %u", nm);
    for (UINT i = 0; i < nm && i < 64; i++) {
        D3DDISPLAYMODE m;
        memset(&m, 0, sizeof m);
        r = ((HRESULT (*)(void *, UINT, UINT, D3DDISPLAYMODE *))VT(d)[VI_IDirect3D8_EnumAdapterModes])(d, 0, i, &m);
        LOG(LOG_ALWAYS, "capdump mode %u -> %08x %ux%u %u Hz format %u", i, (unsigned)r, m.Width, m.Height, m.RefreshRate, m.Format);
    }
    D3DDISPLAYMODE dm;
    memset(&dm, 0, sizeof dm);
    r = ((HRESULT (*)(void *, UINT, D3DDISPLAYMODE *))VT(d)[VI_IDirect3D8_GetAdapterDisplayMode])(d, 0, &dm);
    LOG(LOG_ALWAYS, "capdump display mode %08x %ux%u %u Hz format %u (%d format checks)", (unsigned)r, dm.Width, dm.Height,
        dm.RefreshRate, dm.Format, n);
}

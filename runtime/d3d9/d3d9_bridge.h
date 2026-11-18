/* The Direct3D 9 bridge: the program's 32-bit D3D9 calls go to DXVK (native Linux build,
   Vulkan) through COM objects of our own (runtime/d3d9/d3d9.c, the port's generated
   gen/d3d9_gen.c: tools/d3d9/d3d9gen.py). README.md in this folder says what a port supplies. */
#pragma once
#include <d3d9.h>
#undef LOG
#include "runtime.h"
#include "d3d9_gen.h"

#define VT(o) (*(void ***)(o))

/* One program thread at a time in the device: a game may draw its loading frames from a second
   thread while its main thread makes the level's textures and buffers (Enter the Matrix
   [verified: frame logs]), and a renderer recording both into one command buffer crashes in the
   driver (etm-native, 2026-10-30). Every Direct3D call from the program comes through the
   macros below. The lock is recursive; d3d_present_done() lets go of it entirely around
   frame_presented(), which in --test hands the turn to another thread (det_yield): a thread
   waiting on the device would otherwise wait for a lock its holder never lets go of while it
   waits for its turn (etm-native's lobby route: "--test: the turn is stuck" at frame 2280). */
void d3d_lock(void);
void d3d_unlock(void);
void d3d_present_done(void);                 /* frame_presented() outside the device lock */
#undef HOSTFN
#define HOSTFN(fname, nargs) \
    static uint32_t fname##_body(CPU *c); \
    void fname(CPU *c) { d3d_lock(); uint32_t _r = fname##_body(c); d3d_unlock(); api_return(c, _r, nargs); } \
    static uint32_t fname##_body(CPU *c)
#undef METHOD
#define METHOD(fname, nargs) \
    static uint32_t fname##_body(CPU *c); \
    static void fname(CPU *c) { d3d_lock(); uint32_t _r = fname##_body(c); d3d_unlock(); api_return(c, _r, nargs); } \
    static uint32_t fname##_body(CPU *c)

/* the DXVK object behind a program object (inline, as com_host) */
static inline void *d3d_host(uint32_t guest) { return com_host(guest); }
static inline void *d3d_host_or_null(uint32_t guest) { return guest ? com_host(guest) : NULL; }
uint32_t d3d_wrap(int kind, void *host);     /* the program object for a DXVK object (one each) */
uint32_t d3d_wrap_new(int kind, void *host); /* the same for an object just created */
void *d3d_hwnd(uint32_t hwnd);               /* a window handle as DXVK takes it (SDL_Window *) */
float d3d_argf(CPU *c, int n);               /* a float argument */
void d3d_unsupported(CPU *c, const char *name, int nargs);
extern ComClass d3d_classes[DK_COUNT];
void d3d_classes_init(void);
void msaa_device(void *dev);                 /* d3d9_msaa.c: after CreateDevice / Reset */
void msaa_reset(void);                       /* before Reset */
uint32_t msaa_render_state(uint32_t state, uint32_t value);   /* MULTISAMPLEANTIALIAS set */

/* ------------------------------------------------------------------ the port's hooks
   Weak defaults in d3d9.c; a port defines the strong one where its game needs it. */
/* Another renderer's IDirect3D9 for Direct3DCreate9 (the port's own Vulkan renderer, say, chosen
   by an option), or NULL for DXVK. Asked at each Direct3DCreate9 until it first returns NULL. */
void *port_d3d9_renderer(uint32_t sdk_version);
/* That renderer's buffers: the program's copy of a vertex or index buffer, made at its first lock,
   handed to it (etm-native's vk_buf_host: its renderer reads the program's copy in place) */
void port_d3d9_buffer_host(void *buffer, void *host_copy);
/* The back buffer's size, at CreateDevice / Reset / CreateAdditionalSwapChain */
void port_d3d9_screen(uint32_t w, uint32_t h);
/* SetViewport: the viewport the game set, changed in place if the port wants another */
void port_d3d9_viewport(uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h);
/* SetRenderState: the value the device gets (the default: the game's) */
uint32_t port_d3d9_render_state(uint32_t state, uint32_t value);
/* Screen-space draws from memory (FVF XYZRHW, DrawPrimitiveUP / DrawIndexedPrimitiveUP): given the
   caller (the return address) and the vertices' x and y range, 1 and a scale and offset for x and
   y to lay the 2D out differently (widescreen), or 0 to draw them as they are */
int port_d3d9_ui2d_layout(uint32_t caller, float minx, float maxx, float miny, float maxy, float *xscale,
                          float *xoffset, float *yscale, float *yoffset);
/* More DXVK settings (DXVK_CONFIG syntax, "key = value;" each), or NULL */
const char *port_d3d9_dxvk_config(void);

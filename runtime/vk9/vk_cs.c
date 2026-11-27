/* RECORDING ON A THREAD OF ITS OWN. The game thread's calls into the
   driver's command recording (vkCmd*, vkBegin/EndCommandBuffer, vkQueueSubmit2, vkQueuePresentKHR) are the
   largest part of the renderer's cost on the game thread [verified: <PORT_ENV>_PROF on an NVIDIA desktop GPU, a race:
   the NVIDIA driver 3.0 % of the game thread's samples, our own renderer code 2.5 %]. With this on, those
   calls are written into a queue as they come (their arguments copied, arrays and structures included) and a
   worker thread makes them, in the same order, on the same command buffers: the command stream the driver sees
   is the one it saw before, so the pictures are the same [verified: the frame dumps].
   How it stays correct:
   - Vulkan's function pointers (vk.h) are swapped for the recorders below once the device is made
     (vk_cs_install), so vk_core.c and vk_d3d9.c record as they always did.
   - Whatever needs the GPU's or the queue's state now first waits for the worker to have made everything
     before it (vk_cs_flush): vkQueueWaitIdle, vkDeviceWaitIdle. A fence wait needs nothing more: the fence
     signals once the worker has submitted its work and the GPU has done it.
   - vkAcquireNextImageKHR stays on the game thread (the image's index is needed at once); the swapchain is
     used by one thread at a time (sc_lock: the worker's present, the game thread's acquire).
   - A present's result (out of date, suboptimal, lost) marks the swapchain broken from the worker; the game
     thread makes it again at its next present, as before, one frame later.
   - A submit that fails is fatal on the worker as it was on the game thread.
   - Resources are destroyed only after their frame's fence (vk_core.c's "later" lists), so a command still in
     the queue never meets a destroyed object.
   Option-free switch for measuring: <PORT_ENV>_VK_CS=0 records on the game thread (round 2's way). */
#include <stdatomic.h>
#include <sched.h>
#include "vk.h"

#define CS_RING (32u << 20)              /* bytes of queued commands (a race frame is ~60 KB) */
typedef void (*CsFn)(uint8_t *p);
typedef struct { CsFn fn; uint32_t size; uint32_t pad; } CsHead;   /* size: header and payload, 16-aligned */

static uint8_t *ring;
static size_t wpos;                      /* the game thread's write position (bytes since the start) */
static _Atomic size_t wpub;              /* published: the worker may read up to here */
static _Atomic size_t rdone;             /* the worker has made everything up to here */
static _Atomic int sleeping;
static pthread_mutex_t wake_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER, drained = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t sc_lock = PTHREAD_MUTEX_INITIALIZER;
static int cs_on;
static void fence_at(VkFence f, size_t pos);

#define REAL(n) static PFN_vk##n real_##n;
REAL(BeginCommandBuffer) REAL(EndCommandBuffer) REAL(QueueSubmit2) REAL(QueuePresentKHR) REAL(QueueWaitIdle)
REAL(DeviceWaitIdle) REAL(AcquireNextImageKHR) REAL(WaitForFences) REAL(ResetFences) REAL(GetFenceStatus)
REAL(CmdBeginRendering) REAL(CmdEndRendering) REAL(CmdBindPipeline) REAL(CmdBindDescriptorSets) REAL(CmdPushConstants)
REAL(CmdBindVertexBuffers2) REAL(CmdBindIndexBuffer) REAL(CmdDraw) REAL(CmdDrawIndexed) REAL(CmdSetViewport)
REAL(CmdSetScissor) REAL(CmdSetCullMode) REAL(CmdSetFrontFace) REAL(CmdSetPrimitiveTopology) REAL(CmdSetDepthTestEnable)
REAL(CmdSetDepthWriteEnable) REAL(CmdSetDepthCompareOp) REAL(CmdSetDepthBiasEnable) REAL(CmdSetDepthBias)
REAL(CmdSetStencilTestEnable) REAL(CmdSetStencilOp) REAL(CmdSetStencilCompareMask) REAL(CmdSetStencilWriteMask)
REAL(CmdSetStencilReference) REAL(CmdSetBlendConstants) REAL(CmdPipelineBarrier2) REAL(CmdCopyBufferToImage)
REAL(CmdCopyImageToBuffer) REAL(CmdCopyImage) REAL(CmdBlitImage) REAL(CmdResolveImage) REAL(CmdClearAttachments)
REAL(CmdClearColorImage) REAL(CmdClearDepthStencilImage) REAL(CmdCopyBuffer) REAL(CmdWriteTimestamp2)
REAL(CmdSetColorBlendEnableEXT) REAL(CmdSetColorBlendEquationEXT) REAL(CmdSetColorWriteMaskEXT)
#undef REAL

/* ------------------------------------------------------------------ the queue */
static void wake_worker(void)
{
    if (atomic_load_explicit(&sleeping, memory_order_seq_cst)) {
        pthread_mutex_lock(&wake_lock);
        pthread_cond_signal(&wake);
        pthread_mutex_unlock(&wake_lock);
    }
}

/* (the worker is handed the queue in batches, as DXVK's CS chunks: at CS_BATCH bytes, at each submit and
   present, and whenever the game thread waits for it; between them it sleeps. Handed every command as it came,
   it spun or woke all frame long: 0.3 ms more process CPU a frame than DXVK on a race [verified: a
   port's speed run on a desktop PC, 2.666 against 2.342 ms]) */
#define CS_BATCH (16u << 10)
static size_t wlast;
static void publish(void)
{
    wlast = wpos;
    atomic_store_explicit(&wpub, wpos, memory_order_seq_cst);
    wake_worker();
}

/* room for an op of n payload bytes: its payload (the header written, the op published by cs_end) */
static uint8_t *cs_begin(CsFn fn, size_t n)
{
    size_t need = (sizeof(CsHead) + n + 15) & ~(size_t)15;
    size_t off = wpos % CS_RING;
    if (off + need > CS_RING) {         /* (no room before the ring's end: a jump to its start) */
        size_t pad = CS_RING - off;
        while (wpos + pad - atomic_load_explicit(&rdone, memory_order_acquire) > CS_RING) { publish(); sched_yield(); }
        ((CsHead *)(ring + off))->fn = NULL;
        ((CsHead *)(ring + off))->size = (uint32_t)pad;
        wpos += pad;
        off = 0;
    }
    while (wpos + need - atomic_load_explicit(&rdone, memory_order_acquire) > CS_RING) { publish(); sched_yield(); }
    CsHead *h = (CsHead *)(ring + off);
    h->fn = fn;
    h->size = (uint32_t)need;
    return (uint8_t *)(h + 1);
}

static void cs_end(void)
{
    wpos += ((CsHead *)(ring + wpos % CS_RING))->size;
    if (wpos - wlast >= CS_BATCH) publish();
}

static void *worker(void *arg)
{
    (void)arg;
    size_t r = 0;
    for (;;) {
        size_t w = atomic_load_explicit(&wpub, memory_order_acquire);
        if (r == w) {
            /* (a short spin, then sleep until the game thread writes again) */
            int spun = 0;
            while (r == w && spun++ < 200) {
                __builtin_ia32_pause();
                w = atomic_load_explicit(&wpub, memory_order_acquire);
            }
            if (r == w) {
                pthread_mutex_lock(&wake_lock);
                pthread_cond_broadcast(&drained);
                atomic_store_explicit(&sleeping, 1, memory_order_seq_cst);
                while ((w = atomic_load_explicit(&wpub, memory_order_seq_cst)) == r)
                    pthread_cond_wait(&wake, &wake_lock);
                atomic_store_explicit(&sleeping, 0, memory_order_relaxed);
                pthread_mutex_unlock(&wake_lock);
            }
        }
        while (r != w) {
            CsHead *h = (CsHead *)(ring + r % CS_RING);
            uint32_t sz = h->size;
            if (h->fn) h->fn((uint8_t *)(h + 1));
            r += sz;
            atomic_store_explicit(&rdone, r, memory_order_release);
        }
    }
    return NULL;
}

/* everything queued made (the queue and the GPU's submissions in their place) */
void vk_cs_flush(void)
{
    if (!cs_on) return;
    publish();
    if (atomic_load_explicit(&rdone, memory_order_acquire) == wpos) return;
    pthread_mutex_lock(&wake_lock);
    while (atomic_load_explicit(&rdone, memory_order_acquire) != wpos) {
        pthread_cond_signal(&wake);
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200000;
        if (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
        pthread_cond_timedwait(&drained, &wake_lock, &ts);
    }
    pthread_mutex_unlock(&wake_lock);
}

/* ------------------------------------------------------------------ copying arguments */
typedef struct { uint8_t *p; } Cur;
static void *put(Cur *c, const void *src, size_t n)
{
    if (!n || !src) return NULL;
    void *d = c->p;
    memcpy(d, src, n);
    c->p += (n + 7) & ~(size_t)7;
    return d;
}
#define SZ(n) ((((size_t)(n)) + 7) & ~(size_t)7)

/* ------------------------------------------------------------------ the recorders */
/* (simple ones: the command buffer and plain values) */
#define CS_SIMPLE(name, decl, args, ...) \
    typedef struct { VkCommandBuffer cb; __VA_ARGS__ } A_##name; \
    static void x_##name(uint8_t *p) { A_##name *a = (A_##name *)p; (void)a; real_##name args; } \
    static VKAPI_ATTR void VKAPI_CALL r_##name decl { \
        A_##name *a = (A_##name *)cs_begin(x_##name, sizeof *a); \
        CS_FILL_##name; \
        cs_end(); \
    }

#define CS_FILL_CmdEndRendering (a->cb = cb)
CS_SIMPLE(CmdEndRendering, (VkCommandBuffer cb), (a->cb))
#define CS_FILL_CmdBindPipeline (a->cb = cb, a->bp = bp, a->p = pl)
CS_SIMPLE(CmdBindPipeline, (VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline pl), (a->cb, a->bp, a->p),
          VkPipelineBindPoint bp; VkPipeline p;)
#define CS_FILL_CmdBindIndexBuffer (a->cb = cb, a->b = b, a->o = o, a->t = t)
CS_SIMPLE(CmdBindIndexBuffer, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkIndexType t), (a->cb, a->b, a->o, a->t),
          VkBuffer b; VkDeviceSize o; VkIndexType t;)
#define CS_FILL_CmdDraw (a->cb = cb, a->v = v, a->i = i, a->fv = fv, a->fi = fi)
CS_SIMPLE(CmdDraw, (VkCommandBuffer cb, uint32_t v, uint32_t i, uint32_t fv, uint32_t fi), (a->cb, a->v, a->i, a->fv, a->fi),
          uint32_t v, i, fv, fi;)
#define CS_FILL_CmdDrawIndexed (a->cb = cb, a->n = n, a->i = i, a->fx = fx, a->vo = vo, a->fi = fi)
CS_SIMPLE(CmdDrawIndexed, (VkCommandBuffer cb, uint32_t n, uint32_t i, uint32_t fx, int32_t vo, uint32_t fi),
          (a->cb, a->n, a->i, a->fx, a->vo, a->fi), uint32_t n, i, fx; int32_t vo; uint32_t fi;)
#define CS_FILL_CmdSetCullMode (a->cb = cb, a->m = m)
CS_SIMPLE(CmdSetCullMode, (VkCommandBuffer cb, VkCullModeFlags m), (a->cb, a->m), VkCullModeFlags m;)
#define CS_FILL_CmdSetFrontFace (a->cb = cb, a->f = f)
CS_SIMPLE(CmdSetFrontFace, (VkCommandBuffer cb, VkFrontFace f), (a->cb, a->f), VkFrontFace f;)
#define CS_FILL_CmdSetPrimitiveTopology (a->cb = cb, a->t = t)
CS_SIMPLE(CmdSetPrimitiveTopology, (VkCommandBuffer cb, VkPrimitiveTopology t), (a->cb, a->t), VkPrimitiveTopology t;)
#define CS_FILL_CmdSetDepthTestEnable (a->cb = cb, a->e = e)
CS_SIMPLE(CmdSetDepthTestEnable, (VkCommandBuffer cb, VkBool32 e), (a->cb, a->e), VkBool32 e;)
#define CS_FILL_CmdSetDepthWriteEnable (a->cb = cb, a->e = e)
CS_SIMPLE(CmdSetDepthWriteEnable, (VkCommandBuffer cb, VkBool32 e), (a->cb, a->e), VkBool32 e;)
#define CS_FILL_CmdSetDepthCompareOp (a->cb = cb, a->o = o)
CS_SIMPLE(CmdSetDepthCompareOp, (VkCommandBuffer cb, VkCompareOp o), (a->cb, a->o), VkCompareOp o;)
#define CS_FILL_CmdSetDepthBiasEnable (a->cb = cb, a->e = e)
CS_SIMPLE(CmdSetDepthBiasEnable, (VkCommandBuffer cb, VkBool32 e), (a->cb, a->e), VkBool32 e;)
#define CS_FILL_CmdSetDepthBias (a->cb = cb, a->c = c, a->cl = cl, a->s = s)
CS_SIMPLE(CmdSetDepthBias, (VkCommandBuffer cb, float c, float cl, float s), (a->cb, a->c, a->cl, a->s), float c, cl, s;)
#define CS_FILL_CmdSetStencilTestEnable (a->cb = cb, a->e = e)
CS_SIMPLE(CmdSetStencilTestEnable, (VkCommandBuffer cb, VkBool32 e), (a->cb, a->e), VkBool32 e;)
#define CS_FILL_CmdSetStencilOp (a->cb = cb, a->f = f, a->fo = fo, a->po = po, a->dfo = dfo, a->co = co)
CS_SIMPLE(CmdSetStencilOp, (VkCommandBuffer cb, VkStencilFaceFlags f, VkStencilOp fo, VkStencilOp po, VkStencilOp dfo, VkCompareOp co),
          (a->cb, a->f, a->fo, a->po, a->dfo, a->co), VkStencilFaceFlags f; VkStencilOp fo, po, dfo; VkCompareOp co;)
#define CS_FILL_CmdSetStencilCompareMask (a->cb = cb, a->f = f, a->m = m)
CS_SIMPLE(CmdSetStencilCompareMask, (VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t m), (a->cb, a->f, a->m),
          VkStencilFaceFlags f; uint32_t m;)
#define CS_FILL_CmdSetStencilWriteMask (a->cb = cb, a->f = f, a->m = m)
CS_SIMPLE(CmdSetStencilWriteMask, (VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t m), (a->cb, a->f, a->m),
          VkStencilFaceFlags f; uint32_t m;)
#define CS_FILL_CmdSetStencilReference (a->cb = cb, a->f = f, a->m = m)
CS_SIMPLE(CmdSetStencilReference, (VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t m), (a->cb, a->f, a->m),
          VkStencilFaceFlags f; uint32_t m;)
#define CS_FILL_CmdSetBlendConstants (a->cb = cb, memcpy(a->c, c, sizeof a->c))
CS_SIMPLE(CmdSetBlendConstants, (VkCommandBuffer cb, const float c[4]), (a->cb, a->c), float c[4];)
#define CS_FILL_CmdWriteTimestamp2 (a->cb = cb, a->s = s, a->q = q, a->i = i)
CS_SIMPLE(CmdWriteTimestamp2, (VkCommandBuffer cb, VkPipelineStageFlags2 s, VkQueryPool q, uint32_t i), (a->cb, a->s, a->q, a->i),
          VkPipelineStageFlags2 s; VkQueryPool q; uint32_t i;)
#define CS_FILL_EndCommandBuffer (a->cb = cb)
typedef struct { VkCommandBuffer cb; } A_EndCommandBuffer;
static void x_EndCommandBuffer(uint8_t *p) { real_EndCommandBuffer(((A_EndCommandBuffer *)p)->cb); }
static VKAPI_ATTR VkResult VKAPI_CALL r_EndCommandBuffer(VkCommandBuffer cb)
{
    A_EndCommandBuffer *a = (A_EndCommandBuffer *)cs_begin(x_EndCommandBuffer, sizeof *a);
    a->cb = cb;
    cs_end();
    return VK_SUCCESS;
}
typedef struct { VkCommandBuffer cb; VkCommandBufferUsageFlags f; } A_BeginCommandBuffer;
static void x_BeginCommandBuffer(uint8_t *p)
{
    A_BeginCommandBuffer *a = (A_BeginCommandBuffer *)p;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, a->f, NULL};
    real_BeginCommandBuffer(a->cb, &bi);
}
static VKAPI_ATTR VkResult VKAPI_CALL r_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi)
{
    A_BeginCommandBuffer *a = (A_BeginCommandBuffer *)cs_begin(x_BeginCommandBuffer, sizeof *a);
    a->cb = cb;
    a->f = bi->flags;
    cs_end();
    return VK_SUCCESS;
}

/* (with arrays) */
typedef struct { VkCommandBuffer cb; VkPipelineBindPoint bp; VkPipelineLayout l; uint32_t first, n, nd; VkDescriptorSet *s; uint32_t *d; } A_BDS;
static void x_BDS(uint8_t *p)
{
    A_BDS *a = (A_BDS *)p;
    real_CmdBindDescriptorSets(a->cb, a->bp, a->l, a->first, a->n, a->s, a->nd, a->d);
}
static VKAPI_ATTR void VKAPI_CALL r_CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first,
                                                          uint32_t n, const VkDescriptorSet *s, uint32_t nd, const uint32_t *d)
{
    A_BDS *a = (A_BDS *)cs_begin(x_BDS, sizeof(A_BDS) + SZ(n * sizeof *s) + SZ(nd * 4));
    Cur c = {(uint8_t *)(a + 1)};
    *a = (A_BDS){cb, bp, l, first, n, nd, NULL, NULL};
    a->s = put(&c, s, n * sizeof *s);
    a->d = put(&c, d, nd * 4);
    cs_end();
}

typedef struct { VkCommandBuffer cb; VkPipelineLayout l; VkShaderStageFlags st; uint32_t off, size; } A_PC;
static void x_PC(uint8_t *p)
{
    A_PC *a = (A_PC *)p;
    real_CmdPushConstants(a->cb, a->l, a->st, a->off, a->size, a + 1);
}
static VKAPI_ATTR void VKAPI_CALL r_CmdPushConstants(VkCommandBuffer cb, VkPipelineLayout l, VkShaderStageFlags st, uint32_t off,
                                                     uint32_t size, const void *v)
{
    A_PC *a = (A_PC *)cs_begin(x_PC, sizeof(A_PC) + size);
    *a = (A_PC){cb, l, st, off, size};
    memcpy(a + 1, v, size);
    cs_end();
}

typedef struct { VkCommandBuffer cb; uint32_t first, n; VkBuffer *b; VkDeviceSize *o, *s, *st; } A_BVB;
static void x_BVB(uint8_t *p)
{
    A_BVB *a = (A_BVB *)p;
    real_CmdBindVertexBuffers2(a->cb, a->first, a->n, a->b, a->o, a->s, a->st);
}
static VKAPI_ATTR void VKAPI_CALL r_CmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b,
                                                          const VkDeviceSize *o, const VkDeviceSize *s, const VkDeviceSize *st)
{
    size_t k = SZ(n * 8);
    A_BVB *a = (A_BVB *)cs_begin(x_BVB, sizeof(A_BVB) + 4 * k);
    Cur c = {(uint8_t *)(a + 1)};
    *a = (A_BVB){cb, first, n, NULL, NULL, NULL, NULL};
    a->b = put(&c, b, n * sizeof *b);
    a->o = put(&c, o, n * 8);
    a->s = put(&c, s, n * 8);
    a->st = put(&c, st, n * 8);
    cs_end();
}

/* (the colour blend states, R.dyn_blend: one attachment each, as vk_d3d9.c sets them) */
typedef struct { VkCommandBuffer cb; uint32_t first, n; VkBool32 v[8]; } A_CBE;
static void x_CBE(uint8_t *p) { A_CBE *a = (A_CBE *)p; real_CmdSetColorBlendEnableEXT(a->cb, a->first, a->n, a->v); }
static VKAPI_ATTR void VKAPI_CALL r_CmdSetColorBlendEnableEXT(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBool32 *v)
{
    A_CBE *a = (A_CBE *)cs_begin(x_CBE, sizeof *a);
    a->cb = cb, a->first = first, a->n = n > 8 ? 8 : n;
    memcpy(a->v, v, a->n * sizeof *v);
    cs_end();
}
typedef struct { VkCommandBuffer cb; uint32_t first, n; VkColorBlendEquationEXT v[8]; } A_CBQ;
static void x_CBQ(uint8_t *p) { A_CBQ *a = (A_CBQ *)p; real_CmdSetColorBlendEquationEXT(a->cb, a->first, a->n, a->v); }
static VKAPI_ATTR void VKAPI_CALL r_CmdSetColorBlendEquationEXT(VkCommandBuffer cb, uint32_t first, uint32_t n,
                                                                const VkColorBlendEquationEXT *v)
{
    A_CBQ *a = (A_CBQ *)cs_begin(x_CBQ, sizeof *a);
    a->cb = cb, a->first = first, a->n = n > 8 ? 8 : n;
    memcpy(a->v, v, a->n * sizeof *v);
    cs_end();
}
typedef struct { VkCommandBuffer cb; uint32_t first, n; VkColorComponentFlags v[8]; } A_CWM;
static void x_CWM(uint8_t *p) { A_CWM *a = (A_CWM *)p; real_CmdSetColorWriteMaskEXT(a->cb, a->first, a->n, a->v); }
static VKAPI_ATTR void VKAPI_CALL r_CmdSetColorWriteMaskEXT(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkColorComponentFlags *v)
{
    A_CWM *a = (A_CWM *)cs_begin(x_CWM, sizeof *a);
    a->cb = cb, a->first = first, a->n = n > 8 ? 8 : n;
    memcpy(a->v, v, a->n * sizeof *v);
    cs_end();
}

typedef struct { VkCommandBuffer cb; uint32_t first, n; VkViewport v[]; } A_VP;
static void x_VP(uint8_t *p) { A_VP *a = (A_VP *)p; real_CmdSetViewport(a->cb, a->first, a->n, a->v); }
static VKAPI_ATTR void VKAPI_CALL r_CmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport *v)
{
    A_VP *a = (A_VP *)cs_begin(x_VP, sizeof(A_VP) + n * sizeof *v);
    a->cb = cb, a->first = first, a->n = n;
    memcpy(a->v, v, n * sizeof *v);
    cs_end();
}
typedef struct { VkCommandBuffer cb; uint32_t first, n; VkRect2D r[]; } A_SC;
static void x_SC(uint8_t *p) { A_SC *a = (A_SC *)p; real_CmdSetScissor(a->cb, a->first, a->n, a->r); }
static VKAPI_ATTR void VKAPI_CALL r_CmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D *r)
{
    A_SC *a = (A_SC *)cs_begin(x_SC, sizeof(A_SC) + n * sizeof *r);
    a->cb = cb, a->first = first, a->n = n;
    memcpy(a->r, r, n * sizeof *r);
    cs_end();
}

typedef struct { VkCommandBuffer cb; VkRenderingInfo ri; } A_BR;
static void x_BR(uint8_t *p) { A_BR *a = (A_BR *)p; real_CmdBeginRendering(a->cb, &a->ri); }
static VKAPI_ATTR void VKAPI_CALL r_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri)
{
    if (ri->pNext) LOG(LOG_ALWAYS, "vulkan: the recording thread drops a VkRenderingInfo chain");
    size_t ca = SZ(ri->colorAttachmentCount * sizeof(VkRenderingAttachmentInfo)), at = SZ(sizeof(VkRenderingAttachmentInfo));
    A_BR *a = (A_BR *)cs_begin(x_BR, sizeof(A_BR) + ca + 2 * at);
    Cur c = {(uint8_t *)(a + 1)};
    a->cb = cb;
    a->ri = *ri;
    a->ri.pNext = NULL;
    a->ri.pColorAttachments = put(&c, ri->pColorAttachments, ri->colorAttachmentCount * sizeof(VkRenderingAttachmentInfo));
    a->ri.pDepthAttachment = put(&c, ri->pDepthAttachment, ri->pDepthAttachment ? sizeof(VkRenderingAttachmentInfo) : 0);
    a->ri.pStencilAttachment = put(&c, ri->pStencilAttachment, ri->pStencilAttachment ? sizeof(VkRenderingAttachmentInfo) : 0);
    cs_end();
}

typedef struct { VkCommandBuffer cb; VkDependencyInfo di; } A_PB;
static void x_PB(uint8_t *p) { A_PB *a = (A_PB *)p; real_CmdPipelineBarrier2(a->cb, &a->di); }
static VKAPI_ATTR void VKAPI_CALL r_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *di)
{
    size_t m = SZ(di->memoryBarrierCount * sizeof(VkMemoryBarrier2)), b = SZ(di->bufferMemoryBarrierCount * sizeof(VkBufferMemoryBarrier2)),
           i = SZ(di->imageMemoryBarrierCount * sizeof(VkImageMemoryBarrier2));
    A_PB *a = (A_PB *)cs_begin(x_PB, sizeof(A_PB) + m + b + i);
    Cur c = {(uint8_t *)(a + 1)};
    a->cb = cb;
    a->di = *di;
    a->di.pMemoryBarriers = put(&c, di->pMemoryBarriers, di->memoryBarrierCount * sizeof(VkMemoryBarrier2));
    a->di.pBufferMemoryBarriers = put(&c, di->pBufferMemoryBarriers, di->bufferMemoryBarrierCount * sizeof(VkBufferMemoryBarrier2));
    a->di.pImageMemoryBarriers = put(&c, di->pImageMemoryBarriers, di->imageMemoryBarrierCount * sizeof(VkImageMemoryBarrier2));
    cs_end();
}

/* (copies: the command buffer, up to two objects and layouts, a filter, and n regions) */
typedef struct { VkCommandBuffer cb; uint64_t src, dst; VkImageLayout sl, dl; uint32_t n; VkFilter f; } A_CP;
#define CS_COPY(name, RT, decl, srcv, slv, dstv, dlv, fv, call) \
    static void x_##name(uint8_t *p) { A_CP *a = (A_CP *)p; const RT *rg = (const RT *)(a + 1); (void)rg; call; } \
    static VKAPI_ATTR void VKAPI_CALL r_##name decl { \
        A_CP *a = (A_CP *)cs_begin(x_##name, sizeof(A_CP) + n * sizeof(RT)); \
        *a = (A_CP){cb, (uint64_t)(srcv), (uint64_t)(dstv), (slv), (dlv), n, (fv)}; \
        memcpy(a + 1, rg, n * sizeof(RT)); \
        cs_end(); \
    }
CS_COPY(CmdCopyBufferToImage, VkBufferImageCopy,
        (VkCommandBuffer cb, VkBuffer s, VkImage d, VkImageLayout dl, uint32_t n, const VkBufferImageCopy *rg),
        s, 0, d, dl, 0, real_CmdCopyBufferToImage(a->cb, (VkBuffer)a->src, (VkImage)a->dst, a->dl, a->n, rg))
CS_COPY(CmdCopyImageToBuffer, VkBufferImageCopy,
        (VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkBuffer d, uint32_t n, const VkBufferImageCopy *rg),
        s, sl, d, 0, 0, real_CmdCopyImageToBuffer(a->cb, (VkImage)a->src, a->sl, (VkBuffer)a->dst, a->n, rg))
CS_COPY(CmdCopyImage, VkImageCopy,
        (VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageCopy *rg),
        s, sl, d, dl, 0, real_CmdCopyImage(a->cb, (VkImage)a->src, a->sl, (VkImage)a->dst, a->dl, a->n, rg))
CS_COPY(CmdBlitImage, VkImageBlit,
        (VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageBlit *rg, VkFilter f),
        s, sl, d, dl, f, real_CmdBlitImage(a->cb, (VkImage)a->src, a->sl, (VkImage)a->dst, a->dl, a->n, rg, a->f))
CS_COPY(CmdResolveImage, VkImageResolve,
        (VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageResolve *rg),
        s, sl, d, dl, 0, real_CmdResolveImage(a->cb, (VkImage)a->src, a->sl, (VkImage)a->dst, a->dl, a->n, rg))
CS_COPY(CmdCopyBuffer, VkBufferCopy, (VkCommandBuffer cb, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy *rg),
        s, 0, d, 0, 0, real_CmdCopyBuffer(a->cb, (VkBuffer)a->src, (VkBuffer)a->dst, a->n, rg))

typedef struct { VkCommandBuffer cb; uint32_t na, nr; } A_CA;
static void x_CA(uint8_t *p)
{
    A_CA *a = (A_CA *)p;
    const VkClearAttachment *at = (const VkClearAttachment *)(a + 1);
    const VkClearRect *r = (const VkClearRect *)((const uint8_t *)at + SZ(a->na * sizeof *at));
    real_CmdClearAttachments(a->cb, a->na, at, a->nr, r);
}
static VKAPI_ATTR void VKAPI_CALL r_CmdClearAttachments(VkCommandBuffer cb, uint32_t na, const VkClearAttachment *at, uint32_t nr,
                                                        const VkClearRect *r)
{
    A_CA *a = (A_CA *)cs_begin(x_CA, sizeof(A_CA) + SZ(na * sizeof *at) + SZ(nr * sizeof *r));
    Cur c = {(uint8_t *)(a + 1)};
    *a = (A_CA){cb, na, nr};
    put(&c, at, na * sizeof *at);
    put(&c, r, nr * sizeof *r);
    cs_end();
}

typedef struct { VkCommandBuffer cb; VkImage i; VkImageLayout l; VkClearColorValue v; VkClearDepthStencilValue d; uint32_t n; } A_CI;
static void x_CCI(uint8_t *p)
{
    A_CI *a = (A_CI *)p;
    real_CmdClearColorImage(a->cb, a->i, a->l, &a->v, a->n, (const VkImageSubresourceRange *)(a + 1));
}
static VKAPI_ATTR void VKAPI_CALL r_CmdClearColorImage(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearColorValue *v,
                                                       uint32_t n, const VkImageSubresourceRange *rg)
{
    A_CI *a = (A_CI *)cs_begin(x_CCI, sizeof(A_CI) + n * sizeof *rg);
    a->cb = cb, a->i = i, a->l = l, a->v = *v, a->n = n;
    memcpy(a + 1, rg, n * sizeof *rg);
    cs_end();
}
static void x_CDI(uint8_t *p)
{
    A_CI *a = (A_CI *)p;
    real_CmdClearDepthStencilImage(a->cb, a->i, a->l, &a->d, a->n, (const VkImageSubresourceRange *)(a + 1));
}
static VKAPI_ATTR void VKAPI_CALL r_CmdClearDepthStencilImage(VkCommandBuffer cb, VkImage i, VkImageLayout l,
                                                              const VkClearDepthStencilValue *v, uint32_t n,
                                                              const VkImageSubresourceRange *rg)
{
    A_CI *a = (A_CI *)cs_begin(x_CDI, sizeof(A_CI) + n * sizeof *rg);
    a->cb = cb, a->i = i, a->l = l, a->d = *v, a->n = n;
    memcpy(a + 1, rg, n * sizeof *rg);
    cs_end();
}

/* (the queue: submissions of one VkSubmitInfo2 each, as vk_core.c makes them, and presents) */
typedef struct { VkQueue q; VkFence f; uint32_t nw, nc, ns; } A_SUB;
static void x_SUB(uint8_t *p)
{
    A_SUB *a = (A_SUB *)p;
    const VkSemaphoreSubmitInfo *w = (const VkSemaphoreSubmitInfo *)(a + 1);
    const VkCommandBufferSubmitInfo *cbs = (const VkCommandBufferSubmitInfo *)((const uint8_t *)w + SZ(a->nw * sizeof *w));
    const VkSemaphoreSubmitInfo *s = (const VkSemaphoreSubmitInfo *)((const uint8_t *)cbs + SZ(a->nc * sizeof *cbs));
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, a->nw, w, a->nc, cbs, a->ns, s};
    VkResult r = real_QueueSubmit2(a->q, 1, &si, a->f);
    if (r != VK_SUCCESS) fatal("vulkan: submit failed (%d; the recording thread)", r);
}
static VKAPI_ATTR VkResult VKAPI_CALL r_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *si, VkFence f)
{
    if (n != 1 || si->pNext) {           /* (not one vk_core.c makes: made here, after the queue) */
        vk_cs_flush();
        return real_QueueSubmit2(q, n, si, f);
    }
    size_t w = SZ(si->waitSemaphoreInfoCount * sizeof(VkSemaphoreSubmitInfo)),
           c = SZ(si->commandBufferInfoCount * sizeof(VkCommandBufferSubmitInfo)),
           s = SZ(si->signalSemaphoreInfoCount * sizeof(VkSemaphoreSubmitInfo));
    A_SUB *a = (A_SUB *)cs_begin(x_SUB, sizeof(A_SUB) + w + c + s);
    Cur cur = {(uint8_t *)(a + 1)};
    *a = (A_SUB){q, f, si->waitSemaphoreInfoCount, si->commandBufferInfoCount, si->signalSemaphoreInfoCount};
    /* (each array at its rounded size, even when empty, as x_SUB finds them) */
    put(&cur, si->pWaitSemaphoreInfos, si->waitSemaphoreInfoCount * sizeof(VkSemaphoreSubmitInfo));
    cur.p = (uint8_t *)(a + 1) + w;
    put(&cur, si->pCommandBufferInfos, si->commandBufferInfoCount * sizeof(VkCommandBufferSubmitInfo));
    cur.p = (uint8_t *)(a + 1) + w + c;
    put(&cur, si->pSignalSemaphoreInfos, si->signalSemaphoreInfoCount * sizeof(VkSemaphoreSubmitInfo));
    cs_end();
    if (f) fence_at(f, wpos);
    publish();                          /* (a batch ends at each submit) */
    return VK_SUCCESS;
}

typedef struct { VkQueue q; VkSemaphore wait; VkSwapchainKHR sc; uint32_t idx, nw; } A_PR;
static void x_PR(uint8_t *p)
{
    A_PR *a = (A_PR *)p;
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, a->nw, &a->wait, 1, &a->sc, &a->idx, NULL};
    pthread_mutex_lock(&sc_lock);
    VkResult r = real_QueuePresentKHR(a->q, &pi);
    pthread_mutex_unlock(&sc_lock);
    if (r != VK_SUCCESS) __atomic_store_n(&R.sc_broken, 1, __ATOMIC_RELEASE);
}
static VKAPI_ATTR VkResult VKAPI_CALL r_QueuePresentKHR(VkQueue q, const VkPresentInfoKHR *pi)
{
    if (pi->swapchainCount != 1 || pi->waitSemaphoreCount > 1 || pi->pResults || pi->pNext) {
        vk_cs_flush();
        pthread_mutex_lock(&sc_lock);
        VkResult r = real_QueuePresentKHR(q, pi);
        pthread_mutex_unlock(&sc_lock);
        return r;
    }
    A_PR *a = (A_PR *)cs_begin(x_PR, sizeof(A_PR));
    *a = (A_PR){q, pi->waitSemaphoreCount ? pi->pWaitSemaphores[0] : VK_NULL_HANDLE, pi->pSwapchains[0], pi->pImageIndices[0],
                pi->waitSemaphoreCount};
    cs_end();
    publish();
    return VK_SUCCESS;                  /* (its result reaches R.sc_broken from the worker) */
}

/* (fences: a fence is the submit's, so the game thread's wait, reset or status of one first waits for the
   worker to have made the submit that uses it, and no further: the frame-end wait is for the frame before,
   long submitted; the validation layer's "THREADING ERROR ... VkFence" without this [verified: lavapipe]) */
#define NFENCES 16
static struct { VkFence f; size_t pos; } fpos[NFENCES];
static void fence_at(VkFence f, size_t pos)
{
    int free_i = -1;
    for (int i = 0; i < NFENCES; i++) {
        if (fpos[i].f == f) { fpos[i].pos = pos; return; }
        if (!fpos[i].f && free_i < 0) free_i = i;
    }
    if (free_i >= 0) fpos[free_i].f = f, fpos[free_i].pos = pos;
    else vk_cs_flush();                  /* (more fences than kept: everything made, the oldest forgotten) */
}
static void fence_wait_submitted(uint32_t n, const VkFence *fs)
{
    size_t need = 0;
    for (uint32_t k = 0; k < n; k++)
        for (int i = 0; i < NFENCES; i++)
            if (fpos[i].f == fs[k] && fpos[i].pos > need) need = fpos[i].pos;
    if (!need || atomic_load_explicit(&rdone, memory_order_acquire) >= need) return;
    publish();
    while (atomic_load_explicit(&rdone, memory_order_acquire) < need) sched_yield();
}

static VKAPI_ATTR VkResult VKAPI_CALL r_WaitForFences(VkDevice d, uint32_t n, const VkFence *fs, VkBool32 all, uint64_t t)
{
    fence_wait_submitted(n, fs);
    return real_WaitForFences(d, n, fs, all, t);
}
static VKAPI_ATTR VkResult VKAPI_CALL r_ResetFences(VkDevice d, uint32_t n, const VkFence *fs)
{
    fence_wait_submitted(n, fs);
    return real_ResetFences(d, n, fs);
}
static VKAPI_ATTR VkResult VKAPI_CALL r_GetFenceStatus(VkDevice d, VkFence f)
{
    fence_wait_submitted(1, &f);
    return real_GetFenceStatus(d, f);
}

/* (waits: everything queued first) */
static VKAPI_ATTR VkResult VKAPI_CALL r_QueueWaitIdle(VkQueue q) { vk_cs_flush(); return real_QueueWaitIdle(q); }
static VKAPI_ATTR VkResult VKAPI_CALL r_DeviceWaitIdle(VkDevice d) { vk_cs_flush(); return real_DeviceWaitIdle(d); }
static VKAPI_ATTR VkResult VKAPI_CALL r_AcquireNextImageKHR(VkDevice d, VkSwapchainKHR sc, uint64_t t, VkSemaphore s, VkFence f,
                                                            uint32_t *i)
{
    pthread_mutex_lock(&sc_lock);
    VkResult r = real_AcquireNextImageKHR(d, sc, t, s, f, i);
    pthread_mutex_unlock(&sc_lock);
    return r;
}

/* ------------------------------------------------------------------ a forked child (fncheck's) */
/* The per-function checks fork the game (the toolkit's runtime/rewrite/fncheck.c); a child has no recording thread,
   so it makes what was queued itself and records on its own thread from then on (Vulkan's own pointers back) */
static void after_fork_child(void)
{
    if (!cs_on) return;
    pthread_mutex_init(&wake_lock, NULL);
    pthread_mutex_init(&sc_lock, NULL);
    pthread_cond_init(&wake, NULL);
    pthread_cond_init(&drained, NULL);
    for (size_t r = atomic_load_explicit(&rdone, memory_order_acquire); r != wpos;) {
        CsHead *h = (CsHead *)(ring + r % CS_RING);
        uint32_t sz = h->size;
        if (h->fn) h->fn((uint8_t *)(h + 1));
        r += sz;
    }
    atomic_store(&rdone, wpos);
#define UNSWAP(n) vk##n = real_##n;
    UNSWAP(BeginCommandBuffer) UNSWAP(EndCommandBuffer) UNSWAP(QueueSubmit2) UNSWAP(QueuePresentKHR) UNSWAP(QueueWaitIdle)
    UNSWAP(DeviceWaitIdle) UNSWAP(AcquireNextImageKHR) UNSWAP(WaitForFences) UNSWAP(ResetFences) UNSWAP(GetFenceStatus)
    UNSWAP(CmdBeginRendering) UNSWAP(CmdEndRendering) UNSWAP(CmdBindPipeline) UNSWAP(CmdBindDescriptorSets)
    UNSWAP(CmdPushConstants) UNSWAP(CmdBindVertexBuffers2) UNSWAP(CmdBindIndexBuffer) UNSWAP(CmdDraw) UNSWAP(CmdDrawIndexed)
    UNSWAP(CmdSetViewport) UNSWAP(CmdSetScissor) UNSWAP(CmdSetCullMode) UNSWAP(CmdSetFrontFace) UNSWAP(CmdSetPrimitiveTopology)
    UNSWAP(CmdSetDepthTestEnable) UNSWAP(CmdSetDepthWriteEnable) UNSWAP(CmdSetDepthCompareOp) UNSWAP(CmdSetDepthBiasEnable)
    UNSWAP(CmdSetDepthBias) UNSWAP(CmdSetStencilTestEnable) UNSWAP(CmdSetStencilOp) UNSWAP(CmdSetStencilCompareMask)
    UNSWAP(CmdSetStencilWriteMask) UNSWAP(CmdSetStencilReference) UNSWAP(CmdSetBlendConstants) UNSWAP(CmdPipelineBarrier2)
    UNSWAP(CmdCopyBufferToImage) UNSWAP(CmdCopyImageToBuffer) UNSWAP(CmdCopyImage) UNSWAP(CmdBlitImage) UNSWAP(CmdResolveImage)
    UNSWAP(CmdClearAttachments) UNSWAP(CmdClearColorImage) UNSWAP(CmdClearDepthStencilImage) UNSWAP(CmdCopyBuffer)
    UNSWAP(CmdWriteTimestamp2) UNSWAP(CmdSetColorBlendEnableEXT) UNSWAP(CmdSetColorBlendEquationEXT) UNSWAP(CmdSetColorWriteMaskEXT)
#undef UNSWAP
    cs_on = 0;
}

/* ------------------------------------------------------------------ switching it on */
int vk_cs_install(void)
{
    const char *e = port_getenv("VK_CS");
    if (e && *e == '0') {
        LOG(LOG_ALWAYS, "vulkan renderer: commands recorded on the game thread (<PORT_ENV>_VK_CS=0)");
        return 0;
    }
    ring = aligned_alloc(64, CS_RING);
    if (!ring) return 0;
#define SWAP(n) real_##n = vk##n, vk##n = r_##n;
    SWAP(BeginCommandBuffer) SWAP(EndCommandBuffer) SWAP(QueueSubmit2) SWAP(QueuePresentKHR) SWAP(QueueWaitIdle)
    SWAP(DeviceWaitIdle) SWAP(AcquireNextImageKHR) SWAP(WaitForFences) SWAP(ResetFences) SWAP(GetFenceStatus)
    SWAP(CmdBeginRendering) SWAP(CmdEndRendering) SWAP(CmdBindPipeline) SWAP(CmdBindDescriptorSets) SWAP(CmdPushConstants)
    SWAP(CmdBindVertexBuffers2) SWAP(CmdBindIndexBuffer) SWAP(CmdDraw) SWAP(CmdDrawIndexed) SWAP(CmdSetViewport)
    SWAP(CmdSetScissor) SWAP(CmdSetCullMode) SWAP(CmdSetFrontFace) SWAP(CmdSetPrimitiveTopology) SWAP(CmdSetDepthTestEnable)
    SWAP(CmdSetDepthWriteEnable) SWAP(CmdSetDepthCompareOp) SWAP(CmdSetDepthBiasEnable) SWAP(CmdSetDepthBias)
    SWAP(CmdSetStencilTestEnable) SWAP(CmdSetStencilOp) SWAP(CmdSetStencilCompareMask) SWAP(CmdSetStencilWriteMask)
    SWAP(CmdSetStencilReference) SWAP(CmdSetBlendConstants) SWAP(CmdPipelineBarrier2) SWAP(CmdCopyBufferToImage)
    SWAP(CmdCopyImageToBuffer) SWAP(CmdCopyImage) SWAP(CmdBlitImage) SWAP(CmdResolveImage) SWAP(CmdClearAttachments)
    SWAP(CmdClearColorImage) SWAP(CmdClearDepthStencilImage) SWAP(CmdCopyBuffer) SWAP(CmdWriteTimestamp2)
    SWAP(CmdSetColorBlendEnableEXT) SWAP(CmdSetColorBlendEquationEXT) SWAP(CmdSetColorWriteMaskEXT)
#undef SWAP
    pthread_t th;
    if (pthread_create(&th, NULL, worker, NULL) != 0) fatal("vulkan: no recording thread");
    pthread_detach(th);
    cs_on = 1;
    pthread_atfork(NULL, NULL, after_fork_child);
    LOG(LOG_ALWAYS, "vulkan renderer: commands recorded on a thread of their own");
    return 1;
}

/* STATIC THREAD-LOCAL STORAGE, as Windows XP's loader gives it (__declspec(thread) variables, and
   TLS callbacks). A module with a TLS directory (data directory 9: read from its loaded headers,
   after relocation) gets the next TLS index, written into its index variable (the program first,
   index 0). Every thread gets an array of pointers at TEB+0x2c (ThreadLocalStoragePointer), one
   per index, each to its own copy of the module's template [start, end) followed by zero_fill
   zeros; compiled code reads its variables through fs:[0x2c] and the index.

   TLS callbacks (void __stdcall cb(base, reason, 0)) run as Windows runs them: a DLL's before its
   DllMain (PROCESS_ATTACH), the program's after every DLL started and before its entry point,
   and every module's on each thread's start (THREAD_ATTACH, before DllMain's) and end
   (THREAD_DETACH). As on XP, a DLL loaded later (LoadLibrary) gets no static TLS: logged.

   (recomp-kit's runtime/loader.cpp and SwARc's process.cpp do the same; written for this runtime.) */
#include <string.h>
#include "runtime.h"

#define TLS_MAX 16                      /* modules with TLS (Windows reserves a 64-slot array too) */
typedef struct { const ModuleEntry *m; uint32_t start, end, zero_fill, callbacks; } TlsModule;
static TlsModule tls_mods[TLS_MAX];
static int tls_count;
static int tls_has_callbacks;

/* the module's TLS directory from its loaded headers, 0 if none */
static uint32_t tls_dir(const ModuleEntry *m)
{
    uint32_t nt = m->base + R32(m->base + 0x3c);
    if (R32(nt) != 0x4550u)                 /* "PE\0\0" */
        return 0;
    uint32_t opt = nt + 24;
    if (R32(opt + 92) <= 9)                 /* NumberOfRvaAndSizes */
        return 0;
    uint32_t rva = R32(opt + 96 + 8 * 9);
    return rva ? m->base + rva : 0;
}

/* once, after the modules are loaded and relocated (runtime_setup), before the first thread */
void tls_setup(void)
{
    for (int i = 0; i < module_count; i++) {
        const ModuleEntry *m = &module_table[i];
        uint32_t d = tls_dir(m);
        if (!d)
            continue;
        if (m->is_dll && !m->at_start) {
            LOG(LOG_ALWAYS, "%s has static TLS but is loaded later: none given (as Windows XP)", m->name);
            continue;
        }
        if (tls_count == TLS_MAX)
            fatal("more than %d modules with static TLS", TLS_MAX);
        TlsModule *t = &tls_mods[tls_count];
        t->m = m;
        t->start = R32(d);
        t->end = R32(d + 4);
        t->zero_fill = R32(d + 16);
        t->callbacks = R32(d + 12);
        if (t->end < t->start)
            t->end = t->start;
        uint32_t index = R32(d + 8);
        if (index)
            W32(index, (uint32_t)tls_count);
        if (t->callbacks && R32(t->callbacks))
            tls_has_callbacks = 1;
        LOG(LOG_ALWAYS, "%s: static TLS index %d, %u bytes%s", m->name, tls_count,
            t->end - t->start + t->zero_fill, t->callbacks && R32(t->callbacks) ? ", callbacks" : "");
        tls_count++;
    }
}

/* a new thread's blocks (cpu_new): the pointer array at TEB+0x2c */
/* reuse: a finished thread's array (cpu_release), its blocks filled from the templates again */
void tls_thread_init(CPU *c, uint32_t reuse)
{
    if (!tls_count)
        return;
    uint32_t arr = reuse;
    if (!arr) {
        arr = galloc(4 * 64);
        memset(gptr(arr), 0, 4 * 64);
    }
    for (int i = 0; i < tls_count; i++) {
        TlsModule *t = &tls_mods[i];
        uint32_t n = t->end - t->start, size = n + t->zero_fill;
        uint32_t b = reuse ? R32(arr + 4 * (uint32_t)i) : 0;
        if (!b)
            b = galloc(size ? size : 4);
        memcpy(gptr(b), gptr(t->start), n);
        memset(gptr(b + n), 0, t->zero_fill);
        W32(arr + 4 * (uint32_t)i, b);
    }
    W32(c->fs_base + 0x2c, arr);
}

/* one module's callbacks, for reason (1 PROCESS_ATTACH, 2 THREAD_ATTACH, 3 THREAD_DETACH) */
void tls_callbacks(CPU *c, const ModuleEntry *m, uint32_t reason)
{
    if (!tls_has_callbacks)
        return;
    for (int i = 0; i < tls_count; i++) {
        if (tls_mods[i].m != m || !tls_mods[i].callbacks)
            continue;
        for (uint32_t a = tls_mods[i].callbacks, cb; (cb = R32(a)) != 0; a += 4) {
            LOG(LOG_THREAD, "%s: TLS callback %08x (reason %u)", m->name, cb, reason);
            guest_call(c, cb, 3, m->base, reason, 0u);
        }
    }
}

/* a thread's end: THREAD_DETACH callbacks, the DLLs' then the program's, as XP's
   LdrShutdownThread (only those: the DLLs' DllMain gets no THREAD_DETACH, as before) */
void tls_thread_end(CPU *c)
{
    if (!tls_has_callbacks)
        return;
    for (int i = 1; i < module_count; i++)
        tls_callbacks(c, &module_table[i], 3);
    tls_callbacks(c, &module_table[0], 3);
}

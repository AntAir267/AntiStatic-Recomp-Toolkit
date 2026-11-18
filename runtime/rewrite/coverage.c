/* COVERAGE: every translated function a run enters, written at exit to
   the file <PORT_ENV>_COVERAGE names, one address a line: which functions a test route reaches,
   so rewrites can be checked on them and batches chosen (tools/rewrite/modules.py). A coverage
   build compiles the generated code with -finstrument-functions -DREWRITE_COVERAGE_BUILD (the
   runtime is not instrumented) and links -rdynamic, so the F_<address> names resolve. Without
   REWRITE_COVERAGE_BUILD, coverage_write does nothing. */
#ifdef REWRITE_COVERAGE_BUILD
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *port_getenv(const char *name);

#define CAP (1 << 17)
static void *seen[CAP];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

__attribute__((no_instrument_function)) void __cyg_profile_func_enter(void *fn, void *site)
{
    (void)site;
    uint32_t h = (uint32_t)(((uintptr_t)fn >> 4) * 2654435761u) & (CAP - 1);
    for (int n = 0; n < CAP; n++, h = (h + 1) & (CAP - 1)) {
        void *s = __atomic_load_n(&seen[h], __ATOMIC_RELAXED);
        if (s == fn) return;
        if (!s) {
            pthread_mutex_lock(&lock);
            if (!seen[h]) { __atomic_store_n(&seen[h], fn, __ATOMIC_RELAXED); pthread_mutex_unlock(&lock); return; }
            pthread_mutex_unlock(&lock);
            if (seen[h] == fn) return;
        }
    }
}
__attribute__((no_instrument_function)) void __cyg_profile_func_exit(void *fn, void *site) { (void)fn; (void)site; }

__attribute__((no_instrument_function)) void coverage_write(void)
{
    const char *path = port_getenv("COVERAGE");
    if (!path) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < CAP; i++) {
        Dl_info d;
        if (seen[i] && dladdr(seen[i], &d) && d.dli_sname && !strncmp(d.dli_sname, "F_", 2) &&
            !strstr(d.dli_sname, "_orig"))
            fprintf(f, "%.8s\n", d.dli_sname + 2);
    }
    fclose(f);
}
#else
void coverage_write(void) {}
#endif

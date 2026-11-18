/* Shared declarations of the Windows/DirectX stand-in. */
#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include "cpu.h"

/* ------------------------------------------------------------------ generated tables */
#include "tables.h"
const ModuleEntry *module_by_name(const char *name);   /* "MFC71.DLL", "mfc71" ... or NULL */
const ModuleEntry *module_at(uint32_t addr);
int loader_module_at(int i, uint32_t *base, uint32_t *size);
void drop_key_messages(void);        /* key messages waiting in the queue thrown away */   /* module i's place, -1 past the last */
int module_started(const ModuleEntry *m);              /* a DLL loaded later: not until LoadLibrary */
void module_attach(const ModuleEntry *m, CPU *c);      /* its DllMain(PROCESS_ATTACH), once */
void dlls_thread_notify(CPU *c, int reason);           /* DLL_THREAD_ATTACH 2 / DETACH 3 */

/* the port's description of its game (port.h; see port_example.h) and its environment
   variables: port_getenv("KEYS") reads <PORT_ENV>_KEYS */
#include "port.h"
const char *port_getenv(const char *name);
const char *port_exe_path(void);            /* the program file inside PORT_VROOT */
const char *port_cmdline_exe(void);         /* the program as its command line names it (PORT_CMDLINE_EXE) */
int port_file_hidden(const char *winpath);

/* ------------------------------------------------------------------ logging */
enum { LOG_API = 1, LOG_FILE = 2, LOG_D3D = 4, LOG_DDRAW = 8, LOG_SOUND = 16, LOG_INPUT = 32,
       LOG_WIN = 64, LOG_MEM = 128, LOG_THREAD = 256, LOG_REG = 512, LOG_DRAW = 1024,
       LOG_ALWAYS = 0x40000000 };
extern unsigned log_mask;
int mem_region_count(void);          /* (captured states: capture.c) */
void mem_region(int i, uint32_t *base, uint32_t *size);
int mem_ranges(uint32_t *base, uint32_t *size, int max);   /* every region, up to max */
/* (a forked check child's: the image, DLL images, regions (kind 0-2) and stacks (3), no lock taken;
   the heap block holding an address) */
void mem_walk(void (*fn)(void *u, uint32_t base, uint32_t size, int kind), void *u);
int mem_heap_block(uint32_t a, uint32_t *start, uint32_t *size);
extern unsigned long mem_heap_ops;   /* virtual-memory and heap operations so far */
int cpu_dead_stacks(uint32_t *lo, uint32_t *hi, int max); /* each thread's [stack_lo, esp) */
void logmsg(unsigned cat, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
#define LOGON(cat) (log_mask & (cat))
#define LOG(cat, ...) do { if (log_mask & (cat)) logmsg(cat, __VA_ARGS__); } while (0)

/* ------------------------------------------------------------------ calling conventions */
#define ARG(n) R32(c->esp + 4 + 4 * (n))
extern unsigned long api_calls;                /* every return from the runtime to the program */
extern int api_trace_on;
/* --hash-calls: memory hash at chosen calls; --fncheck: a check child's record of each call (its
   nargs arguments are still in memory just below esp) */
void api_trace(CPU *c, int nargs);
static inline void api_return(CPU *c, uint32_t r, int nargs)
{
    c->eax = r;
    c->ret = R32(c->esp);
    c->esp += 4 + 4 * (uint32_t)nargs;
    api_calls++;
    if (__builtin_expect(api_trace_on, 0))
        api_trace(c, nargs);
}
/* A stdcall function the program calls (an import or a COM method): the body returns eax;
   nargs dwords are popped (for COM methods nargs includes `this`). */
/* Every stand-in's name and function goes into one table (the api_names section), which
   api_by_name searches: GetProcAddress for a function the program did not import finds it
   there, on Linux and Windows alike, with no export table or -rdynamic. */
typedef struct { const char *name; void (*fn)(CPU *c); } ApiName;
#ifdef _WIN32
#define API_NAMES_SECTION ".apinm$m"   /* between platform.c's $a and $z markers (COFF sorts by suffix) */
#else
#define API_NAMES_SECTION "api_names"  /* __start_api_names .. __stop_api_names */
#endif
#if defined(__has_attribute) && __has_attribute(retain) && !defined(_WIN32)
#define API_NAMES_KEEP __attribute__((used, retain))
#else
#define API_NAMES_KEEP __attribute__((used))
#endif
#define API_NAME(fname) \
    void fname(CPU *c); \
    static const ApiName fname##_apiname API_NAMES_KEEP \
        __attribute__((section(API_NAMES_SECTION), aligned(sizeof(void *)))) = {#fname, fname};
void (*api_by_name(const char *name))(CPU *c);   /* NULL if no stand-in has that name */
#define HOSTFN(fname, nargs) \
    API_NAME(fname) \
    static uint32_t fname##_body(CPU *c); \
    void fname(CPU *c) { uint32_t _r = fname##_body(c); api_return(c, _r, nargs); } \
    static uint32_t fname##_body(CPU *c)
#define API(dll, name, nargs) HOSTFN(w_##dll##_##name, nargs)
/* A default a port may replace with its own (its definition wins at link time) */
#define API_WEAK(dll, name, nargs) \
    __attribute__((weak)) void w_##dll##_##name(CPU *c); \
    API_NAME(w_##dll##_##name) \
    static uint32_t w_##dll##_##name##_body(CPU *c); \
    __attribute__((weak)) void w_##dll##_##name(CPU *c) { uint32_t _r = w_##dll##_##name##_body(c); api_return(c, _r, nargs); } \
    static uint32_t w_##dll##_##name##_body(CPU *c)
/* Same, file-local: COM methods and callbacks handed to the program through thunks. */
#define METHOD(fname, nargs) \
    static uint32_t fname##_body(CPU *c); \
    static void fname(CPU *c) { uint32_t _r = fname##_body(c); api_return(c, _r, nargs); } \
    static uint32_t fname##_body(CPU *c)
/* cdecl: the caller pops the arguments */
#define API_CDECL(dll, name) HOSTFN(w_##dll##_##name, 0)

/* Call a function of the program from the runtime (window procedure, thread start,
   callbacks). Arguments are pushed right to left; returns eax. */
uint32_t guest_call(CPU *c, uint32_t fn, int nargs, ...);
uint32_t guest_callv(CPU *c, uint32_t fn, int nargs, const uint32_t *args);
extern void (*icall_hook)(CPU *c, uint32_t target);
int call_thunk(CPU *c, uint32_t target);     /* run a runtime function by its thunk address */
/* A program address standing for a runtime function (vtables, IAT, GetProcAddress). */
uint32_t thunk_addr(void (*fn)(CPU *c), const char *name);
const char *thunk_name(uint32_t addr);

/* ------------------------------------------------------------------ memory */
#define ARENA_START PORT_ARENA_START   /* above the translated modules (port.h) */
#define ARENA_END   0x74000000u   /* below the stand-in system DLLs (sysdlls[], kernel32.c: 0x75000000 up) */
#define THUNK_BASE  0x7f000000u
#define THUNK_MAX   0x10000u
void mem_init(void);
uint32_t vm_alloc(uint32_t addr, uint32_t size, int reserve_only);   /* 64 KB granular */
uint32_t vm_alloc_table(uint32_t bytes);   /* for a moved table: ends at an inaccessible page */
int vm_free(uint32_t addr, uint32_t size, int release);
uint32_t vm_query_base(uint32_t addr);
uint32_t heap_create(uint32_t initial, uint32_t max);
int heap_destroy(uint32_t heap);
uint32_t heap_alloc(uint32_t heap, uint32_t size, int zero);
int heap_free(uint32_t heap, uint32_t p);
uint32_t heap_realloc(uint32_t heap, uint32_t p, uint32_t size, int zero, int in_place_only);
uint32_t heap_size(uint32_t heap, uint32_t p);
extern uint32_t process_heap;
/* runtime-owned allocations visible to the program */
uint64_t mem_hash(void);                     /* hash of program-visible memory (tests) */
void mem_dump(const char *path);              /* all program memory to a file (tests) */
extern int deterministic;                     /* --test */
uint32_t galloc(uint32_t size);
int mem_find_all(const void *needle, uint32_t n, uint32_t *found, int max);
void gfree(uint32_t p);
uint32_t gstrdup(const char *s);
static inline char *gptr(uint32_t a) { return (char *)(uintptr_t)a; }
static inline const char *gstr(uint32_t a) { return a ? (const char *)(uintptr_t)a : NULL; }
void gstrncpy(uint32_t dst, const char *src, uint32_t n);   /* always terminates if n > 0 */
/* the program's UTF-16 strings: read into a host buffer (Latin-1 range kept, others '?') and
   written back (cap in characters, always terminated; returns the length written) */
const char *gwstr(uint32_t w, char *buf, size_t n);
uint32_t gwput(uint32_t dst, const char *s, uint32_t cap);

/* ------------------------------------------------------------------ process / threads */
typedef struct Thread Thread;
extern CPU *main_cpu;
extern char game_dir[];      /* host folder holding Game.exe and Data/ */
extern char user_dir[];      /* host folder for saves, settings, logs */
extern uint32_t image_base;
extern int headless;
extern int hidden_window;
CPU *cpu_new(uint32_t stack_size);
void runtime_setup(void);
void set_current_cpu(CPU *c);
CPU *current_cpu(void);
/* crashes (main.c): a line of a crash report, safe in a signal handler on Linux (write() only, no
   lock); the port's own lines go in port_crash_report (optional), called after the faulting pc's line
   on any thread. log_fd: the log file's descriptor, or -1. */
void crash_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void port_crash_report(int guest_thread);
int log_fd(void);
uint32_t current_thread_id(void);
void set_last_error(uint32_t e);
uint32_t get_last_error(void);
void process_exit(int code) __attribute__((noreturn));
void game_exit(int code) __attribute__((noreturn));   /* the game quitting: 0 to the launcher in real runs */
void frame_limit_options(void);     /* main.c: "frame_limit" read again */
int frame_limit_screen(void);       /* ... its "screen" rate: 3 under the screen's refresh */
int host_refresh_hz(void);          /* win.c: the screen's refresh rate (60 when unknown) */
double now_ms(void);         /* milliseconds since start */

/* ------------------------------------------------------------------ handles */
enum { H_FREE = 0, H_FILE, H_EVENT, H_MUTEX, H_THREAD, H_FIND, H_SEMAPHORE, H_MISC };
typedef struct HandleObj HandleObj;
uint32_t handle_new(int type, void *obj);
void *handle_get(uint32_t h, int type);
int handle_type(uint32_t h);
void handle_close(uint32_t h);
int wait_handle(uint32_t h, uint32_t ms);              /* 0 = signalled, 0x102 = timeout */
int wait_handles(int n, const uint32_t *h, int all, uint32_t ms);

/* ------------------------------------------------------------------ files */
/* Translate a Windows path the program uses to a host path. For reading, a file present in
   the user folder wins over the game folder; writing always goes to the user folder. */
int path_to_host(const char *winpath, char *out, size_t outsz, int for_write);
/* The game's CD (optional: port.h PORT_CD_LABEL): drive PORT_CD_DRIVE ("D:" by default), read only,
   its files in cd_dir (<game>/CD unless --cd DIR) */
#ifdef PORT_CD_LABEL
#ifndef PORT_CD_DRIVE
#define PORT_CD_DRIVE "D:"
#endif
#endif
extern char cd_dir[];
const char *virtual_game_root(void);   /* e.g. "C:\\1NSANE" */

/* ------------------------------------------------------------------ resources */
int resource_find(uint32_t type, uint32_t name, uint32_t *data, uint32_t *size);

/* ------------------------------------------------------------------ registry/settings */
void settings_load(void);
void settings_save(void);
/* The runtime's own reads and writes of the store, e.g. keeping values of its own under another
   key. settings_get returns the value's size (0 if there is none) and copies up to cap bytes. */
uint32_t settings_get(const char *key, const char *name, uint32_t *type, void *buf, uint32_t cap);
int settings_has(const char *key, const char *name);
void settings_put(const char *key, const char *name, uint32_t type, const void *data, uint32_t size);
void settings_delete(const char *key, const char *name);

/* ------------------------------------------------------------------ COM objects */
/* A COM object as the program sees it: [vtable][host object pointer (low 32 bits unused)].
   The host side keeps its own struct; com_host() finds it from the program's pointer. */
typedef struct { const char *iface; int nmethods; uint32_t vtbl; } ComClass;
uint32_t com_new(ComClass *cls, void *host);
#define COM_MAGIC 0x4d4f4321u    /* "!COM" */
#define COM_HOST_CHUNK 4096
extern void **com_hosts[];
extern uint32_t com_nhosts;
void com_host_bad(uint32_t obj) __attribute__((noreturn, cold));
static inline void *com_host(uint32_t obj)   /* (inline: from L.A. Rush's runtime, 2.21 -> 2.12 ms a frame) */
{
    uint32_t id = obj ? R32(obj + 8) : 0;
    if (__builtin_expect(!obj || R32(obj + 4) != COM_MAGIC || id >= __atomic_load_n(&com_nhosts, __ATOMIC_ACQUIRE), 0))
        com_host_bad(obj);
    return com_hosts[id / COM_HOST_CHUNK][id % COM_HOST_CHUNK];
}
void com_set_class(uint32_t obj, ComClass *cls);
void com_class_init(ComClass *cls, const char *iface, int n, void (*const *methods)(CPU *),
                    const char *const *names);
#define GUID_FMT "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x"
#define GUID_ARGS(a) R32(a), R16((a) + 4), R16((a) + 6), R8((a) + 8), R8((a) + 9), R8((a) + 10), \
    R8((a) + 11), R8((a) + 12), R8((a) + 13), R8((a) + 14), R8((a) + 15)
int guid_eq(uint32_t a, uint32_t d1, uint16_t d2, uint16_t d3, const uint8_t d4[8]);

/* Called by the display layer once per presented frame (Flip, or a Blt to the primary
   surface): counts frames, applies --frames/--seconds limits, pumps host window events. */
double frame_clock_ms(void);           /* the clock frames move by (the virtual one in --test) */
void win_post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);   /* post a message to a window */
void win_rumble_set(int on);           /* the rumble option changed (once called, win.c no longer reads "rumble") */
int host_fullscreen(void);             /* win.c: the window is (or is about to go) full screen */
void sync_fork_child(int old_tid);     /* sync.c: in a forked child, the forking thread's critical sections as its own */
/* dialog.c: a dialog's own procedure run with no window (a port's port_dialog_box, for test switches):
   WM_INITDIALOG with param, then msgs[i] = {msg, wParam, lParam} until EndDialog; its result */
uint32_t dialog_drive(CPU *c, uint32_t proc, uint32_t param, const uint32_t (*msgs)[3], int n);
void port_frame_end(unsigned long frame);   /* weak: after the frame's input and window pump */
int shot_due(void);                  /* --shot wants the frame being presented now (main.c) */
int d3d_write_png(const char *path); /* the display layer: that frame as a PNG */
void frame_presented(void);
/* The program waits for messages (GetMessage, WaitMessage): the run limits still apply. */
void frame_presented_idle(void);
uint32_t current_thread_id(void);
/* The program reads an input device (DirectInput): if it does so without presenting frames, it
   is waiting for a key, and input must keep flowing (main.c). */
void input_wait(void);
extern int run_frames_limit;
extern double run_seconds_limit;
extern unsigned long frame_count;
extern unsigned long game_frames;   /* the game's frames (port_frame_ticks): input, --hash, --frames */
/* the draw-stream hash (drawhash.c), on with --hash */
extern int draw_hashing;
extern int dev_recording;          /* devrec.c: a captured function's calls being recorded */
void devrec_upload(uint32_t addr, uint32_t len);
extern uint64_t draw_hash;
void draw_hash_call(CPU *c, const char *method);
void draw_hash_bytes(const void *p, size_t n);

/* windows / input / display shared state */
void win_init(void);
void win_pump(void);            /* host events -> window messages (no-op when headless) */
extern uint32_t main_hwnd;
void ddraw_init(void);
void dsound_init(void);
void dinput_init(void);
void ifc_init(void);
void video_init(void);
void video_skip_press(void);         /* video.c: Escape or a controller button while an intro video plays */
int video_skip_taken(void);           /* ... one is waiting for the game (taken once) */

/* static thread-local storage (tls.c) */
void tls_callbacks(CPU *c, const ModuleEntry *m, uint32_t reason);
void tls_thread_end(CPU *c);

/* pacing (pacing.c): sleeps to an absolute time, the frame pacer, the clock-spin guard */
typedef struct { uint64_t next, period; } Pacer;
uint64_t mono_ns(void);
void sleep_until_ns(uint64_t t);
uint64_t pacer_wait(Pacer *p, double fps);
uint32_t clock_spin_poll(uint32_t ms, uint32_t (*read)(void));

/* exit work (main.c process_exit), run before the process ends with _exit: last registered first */
void runtime_at_exit(void (*fn)(void));
void port_at_exit(void);                /* a port's own (weak default: nothing) */

/* a DLL the port supplies in C (kernel32.c): LoadLibrary finds it by name, GetProcAddress its
   functions by name */
typedef struct { const char *name; void (*fn)(CPU *c); } HostExport;
void loader_add_host_module(const char *file, const HostExport *exports, int n);

/* the registry (advapi32.c): values a port seeds before its game starts */
void registry_set_str(const char *key, const char *name, const char *s);
void registry_set(const char *key, const char *name, uint32_t type, const void *data, uint32_t size);
void registry_set_dword(const char *key, const char *name, uint32_t v);

double host_ms(void);                    /* the host's clock in ms, whatever --test does (main.c) */
extern double test_frame_ms;             /* --test: the virtual clock's step this frame */
extern void (*api_call_hook)(CPU *c, const char *name, int after);   /* every thunk call (main.c) */
extern void (*port_thunk_hook)(CPU *c, uint32_t index, const char *name, int after);   /* the same, the port's, with the thunk's index */
int shots_requested(void);               /* --shot frames asked for (main.c) */

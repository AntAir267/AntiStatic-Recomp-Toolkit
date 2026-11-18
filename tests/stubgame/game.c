/* AntiStatic's stub game: a small 32-bit Windows program written for the toolkit (no game in it),
   so the public repository can build and run the whole chain (translator -> generated C -> runtime
   core) with no game files. It uses no C runtime: just the Windows functions a game's start-up
   uses, and writes what it saw to result.txt, which tests/stubgame/run.sh checks.

   Built with any compiler targeting 32-bit Windows (zig cc -target x86-windows-gnu, mingw).
   The idea of a stub game that tests a recompiler with no game files is recomp-kit's
   (https://github.com/veritr1x/recomp-kit, MIT); this program is our own. */
#include <windows.h>

static char out[4096];
static int outn;

static void put(const char *s) { while (*s && outn < (int)sizeof out - 1) out[outn++] = *s++; }
static void put_u(unsigned v)
{
    char b[12]; int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) { char c[2] = {b[--n], 0}; put(c); }
}
static void put_hex(unsigned v)
{
    for (int i = 28; i >= 0; i -= 4) { char c[2] = {"0123456789abcdef"[(v >> i) & 15], 0}; put(c); }
}
static void line(const char *k, unsigned v) { put(k); put(" "); put_u(v); put("\n"); }

static volatile LONG counter;
static HANDLE done;

/* STATIC TLS. With no C runtime linked, the TLS directory is ours (as mingw's tlssup.c defines
   it): the template between _tls_start and _tls_end (the compiler puts __thread variables in
   .tls$), the index the loader fills, and a callback list between __xl_a and __xl_z. */
ULONG _tls_index;
__attribute__((section(".tls"))) char _tls_start = 0;
__attribute__((section(".tls$ZZZ"))) char _tls_end = 0;
__attribute__((section(".CRT$XLA"))) PIMAGE_TLS_CALLBACK __xl_a = 0;
__attribute__((section(".CRT$XLZ"))) PIMAGE_TLS_CALLBACK __xl_z = 0;
static volatile LONG tls_attach[4];      /* callbacks seen, by reason */
static void NTAPI tls_cb(PVOID module, DWORD reason, PVOID reserved);
__attribute__((section(".CRT$XLB"), used)) PIMAGE_TLS_CALLBACK tls_cb_entry = tls_cb;
__attribute__((used)) const IMAGE_TLS_DIRECTORY32 _tls_used = {
    (DWORD)(UINT_PTR)&_tls_start, (DWORD)(UINT_PTR)&_tls_end, (DWORD)(UINT_PTR)&_tls_index,
    (DWORD)(UINT_PTR)(&__xl_a + 1), 0, 0};
static __thread unsigned tls_value = 0x1234;
static __thread unsigned tls_zeroed[8];
static void NTAPI tls_cb(PVOID module, DWORD reason, PVOID reserved)
{
    (void)reserved;
    if (reason < 4 && module == GetModuleHandleA(NULL))
        InterlockedIncrement(&tls_attach[reason]);
    if (reason == DLL_THREAD_ATTACH)
        tls_zeroed[7] = 0x77;           /* the new thread's own block, before its start function */
}
static unsigned worker_tls, worker_tls7;

static DWORD WINAPI worker(LPVOID arg)
{
    worker_tls = tls_value;              /* this thread's copy: the template's value */
    worker_tls7 = tls_zeroed[7];
    tls_value = 99;
    for (int i = 0; i < 1000; i++)
        InterlockedIncrement(&counter);
    InterlockedExchangeAdd(&counter, (LONG)(UINT_PTR)arg);
    SetEvent(done);
    return 7;
}

/* x87 and integer arithmetic with results known exactly */
static unsigned maths(void)
{
    volatile double a = 2.0, b = 3.0;
    double r = 1.0;
    for (int i = 0; i < 20; i++)
        r = r * a / b + 0.125;
    volatile float f = (float)r;
    unsigned h = 2166136261u;
    for (int i = 0; i < 64; i++)
        h = (h ^ (unsigned)i) * 16777619u;
    return (unsigned)(f * 1000000.0f) ^ h;
}

/* two functions the rewrite test replaces (tests/stubgame/port/rewrite.c): exported, so the
   test finds their addresses in the program's export table. mix is rewritten by hand as C; it
   calls rotl, which stays translated (stdcall, so the rewrite pops what the original pops). */
__declspec(dllexport) __attribute__((noinline)) unsigned __stdcall stub_rotl(unsigned x, unsigned n)
{
    return (x << (n & 31)) | (x >> ((32 - n) & 31));
}
__declspec(dllexport) __attribute__((noinline)) unsigned __cdecl stub_mix(unsigned a, unsigned b)
{
    unsigned h = a * 0x9e3779b1u;
    for (unsigned i = 0; i < 8; i++)
        h = stub_rotl(h ^ b, i + 5) + i;
    return h;
}

/* SETJMP / LONGJMP, both ways a game has them: imported from its C runtime (the runtime core's
   own pair), and its own copy linked in (here my_setjmp / my_longjmp, MSVCRT's layout, found by
   run.sh in the export table and listed in the build config as SETJMP / LONGJMP). A longjmp
   from five calls down comes back to the place after the setjmp with its value. */
typedef unsigned jb_t[16];
__declspec(dllimport) int __cdecl _setjmp3(jb_t, int, ...);
__declspec(dllimport) void __cdecl longjmp(jb_t, int);
int __cdecl my_setjmp(jb_t);
void __cdecl my_longjmp(jb_t, int);
__asm__(".text\n"
        ".globl _my_setjmp\n_my_setjmp:\n"
        "  mov 4(%esp), %edx\n  mov %ebp, (%edx)\n  mov %ebx, 4(%edx)\n  mov %edi, 8(%edx)\n"
        "  mov %esi, 12(%edx)\n  mov %esp, 16(%edx)\n  mov (%esp), %eax\n  mov %eax, 20(%edx)\n"
        "  xor %eax, %eax\n  ret\n"
        ".globl _my_longjmp\n_my_longjmp:\n"
        "  mov 4(%esp), %edx\n  mov 8(%esp), %eax\n  mov (%edx), %ebp\n  mov 4(%edx), %ebx\n"
        "  mov 8(%edx), %edi\n  mov 12(%edx), %esi\n  mov 16(%edx), %esp\n  add $4, %esp\n"
        "  cmp $1, %eax\n  adc $0, %eax\n  jmp *20(%edx)\n"
        ".section .drectve\n.ascii \" -export:my_setjmp -export:my_longjmp\"\n.text\n");
static jb_t jb;
static volatile int unwound;
static __attribute__((noinline)) void deep(int n, int own)
{
    if (n == 0) {
        if (own) my_longjmp(jb, 0);     /* (0 comes back as 1) */
        else longjmp(jb, 7);
    }
    deep(n - 1, own);
    unwound++;                          /* never: the jump skips every return */
}
static __attribute__((noinline)) unsigned setjmp_test(int own)
{
    volatile unsigned passes = 0;
    volatile int keep = 40;             /* a local the jump must find as it was */
    int r = own ? my_setjmp(jb) : _setjmp3(jb, 0);
    passes++;
    if (r == 0) {
        keep = 41;
        deep(5, own);
    }
    return (unsigned)r * 1000 + passes * 100 + (unsigned)keep + (unsigned)unwound;
}

/* DIRECTSOUND, the way games use it: a buffer filled by Lock, played looping, its play position
   read back while time passes (the game's clock), stopped, played once to its end, faster.
   The checks are on what a game relies on: positions advance at the buffer's own rate, the
   write position leads the play position, a stopped buffer holds still, a one-shot ends. */
typedef struct { DWORD size, flags, bytes, reserved; WAVEFORMATEX *fmt; } DSBDESC;
typedef struct { void **vt; } COMOBJ;
__declspec(dllimport) HRESULT WINAPI DirectSoundCreate(void *guid, COMOBJ **ds, void *outer);
typedef HRESULT (WINAPI *M0)(COMOBJ *);
typedef HRESULT (WINAPI *M1)(COMOBJ *, DWORD);
typedef HRESULT (WINAPI *M2)(COMOBJ *, DWORD, DWORD);
typedef HRESULT (WINAPI *M3)(COMOBJ *, DWORD, DWORD, DWORD);
typedef HRESULT (WINAPI *MCREATE)(COMOBJ *, DSBDESC *, COMOBJ **, void *);
typedef HRESULT (WINAPI *MPOS)(COMOBJ *, DWORD *, DWORD *);
typedef HRESULT (WINAPI *MSTATUS)(COMOBJ *, DWORD *);
typedef HRESULT (WINAPI *MLOCK)(COMOBJ *, DWORD, DWORD, void **, DWORD *, void **, DWORD *, DWORD);
typedef HRESULT (WINAPI *MUNLOCK)(COMOBJ *, void *, DWORD, void *, DWORD);
static void wait_ms(DWORD ms)           /* (as a game's loop does: the clock, not Sleep) */
{
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < ms) {}
}
static void sound_test(void)
{
    COMOBJ *ds = 0, *b = 0;
    line("ds_create", DirectSoundCreate(NULL, &ds, NULL) == 0 && ds);
    if (!ds) return;
    ((M2)ds->vt[6])(ds, 0, 2);                                      /* SetCooperativeLevel(PRIORITY) */
    WAVEFORMATEX f = {WAVE_FORMAT_PCM, 1, 22050, 44100, 2, 16, 0};   /* 22 kHz 16-bit mono: 44100 bytes a second */
    DSBDESC d = {sizeof d, 0x80 | 0x20 | 0x10000, 44100, 0, &f};    /* CTRLVOLUME|CTRLFREQUENCY|GETCURRENTPOSITION2, 1 s */
    line("ds_buffer", ((MCREATE)ds->vt[3])(ds, &d, &b, NULL) == 0 && b);
    if (!b) return;
    void *p1, *p2; DWORD n1, n2;
    line("ds_lock", ((MLOCK)b->vt[11])(b, 0, 44100, &p1, &n1, &p2, &n2, 0) == 0 && n1 == 44100);
    for (DWORD i = 0; i < n1 / 2; i++) ((short *)p1)[i] = (short)((i & 63) * 512 - 16384);
    ((MUNLOCK)b->vt[19])(b, p1, n1, p2, n2);
    ((M3)b->vt[12])(b, 0, 0, 1);                                    /* Play(LOOPING) */
    DWORD st = 0, play = 0, write = 0, play2 = 0;
    ((MSTATUS)b->vt[9])(b, &st);                                     /* GetStatus */
    line("ds_status_playing_looping", (st & 5) == 5);
    wait_ms(250);
    ((MPOS)b->vt[4])(b, &play, &write);
    line("ds_quarter_second_played", play > 9000 && play < 13500);  /* ~11025 */
    line("ds_write_ahead", ((write - play + 44100) % 44100) > 0 && ((write - play + 44100) % 44100) < 4410);
    wait_ms(1000);                                                  /* a lap: the loop wraps */
    ((MPOS)b->vt[4])(b, &play2, NULL);
    line("ds_loop_wraps", play2 < 44100 && (play2 > play ? play2 - play : play - play2) < 2205);
    ((M0)b->vt[18])(b);                                             /* Stop */
    ((MPOS)b->vt[4])(b, &play, NULL);
    wait_ms(300);
    ((MPOS)b->vt[4])(b, &play2, NULL);
    line("ds_stopped_holds", play == play2);
    ((M1)b->vt[13])(b, 0);                                          /* SetCurrentPosition(0) */
    ((M1)b->vt[17])(b, 44100);                                      /* SetFrequency: twice as fast */
    ((M3)b->vt[12])(b, 0, 0, 0);                                    /* Play once */
    wait_ms(250);
    ((MPOS)b->vt[4])(b, &play, NULL);
    line("ds_double_rate", play > 19000 && play < 25000);           /* ~22050 */
    wait_ms(600);
    ((MSTATUS)b->vt[9])(b, &st);
    line("ds_one_shot_ended", (st & 1) == 0);
    ((M0)b->vt[2])(b);
    ((M0)ds->vt[2])(ds);
}

/* MILES (runtime/music/mss32.c): the driver opened, a WAV image in memory played as a sample,
   playing while it lasts and done after it, as a game polls it */
__declspec(dllimport) int __stdcall AIL_startup(void);
__declspec(dllimport) int __stdcall AIL_waveOutOpen(void **drv, void **out, int id, WAVEFORMATEX *f);
__declspec(dllimport) void *__stdcall AIL_allocate_sample_handle(void *drv);
__declspec(dllimport) int __stdcall AIL_set_sample_file(void *s, const void *image, int block);
__declspec(dllimport) void __stdcall AIL_start_sample(void *s);
__declspec(dllimport) int __stdcall AIL_sample_status(void *s);
__declspec(dllimport) void __stdcall AIL_set_sample_volume(void *s, int volume);
__declspec(dllimport) void __stdcall AIL_release_sample_handle(void *s);
__declspec(dllimport) void __stdcall AIL_shutdown(void);
static unsigned char wav[44 + 11025];   /* 0.25 s of 22050 Hz 16-bit mono */
static void put4(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void miles_test(void)
{
    WAVEFORMATEX f = {WAVE_FORMAT_PCM, 2, 22050, 88200, 4, 16, 0};
    void *drv = 0, *out = 0;
    line("ail_startup", AIL_startup() != 0);
    line("ail_driver", AIL_waveOutOpen(&drv, &out, -1, &f) == 0 && drv);
    unsigned data = sizeof wav - 44;
    memcpy(wav, "RIFF", 4); put4(wav + 4, 36 + data); memcpy(wav + 8, "WAVEfmt ", 8);
    put4(wav + 16, 16); put4(wav + 20, 1 | 1u << 16); put4(wav + 24, 22050); put4(wav + 28, 44100);
    put4(wav + 32, 2 | 16u << 16); memcpy(wav + 36, "data", 4); put4(wav + 40, data);
    for (unsigned i = 0; i < data / 2; i++) ((short *)(wav + 44))[i] = (short)((i & 31) * 1024 - 16384);
    void *smp = AIL_allocate_sample_handle(drv);
    line("ail_sample", smp != 0 && AIL_set_sample_file(smp, wav, -1) != 0);
    AIL_set_sample_volume(smp, 100);
    AIL_start_sample(smp);
    line("ail_playing", AIL_sample_status(smp) == 4);     /* SMP_PLAYING */
    wait_ms(100);
    line("ail_still_playing", AIL_sample_status(smp) == 4);
    wait_ms(400);
    line("ail_done", AIL_sample_status(smp) == 2);        /* SMP_DONE */
    AIL_release_sample_handle(smp);
    AIL_shutdown();
}

/* A DLL only the port stands in for (port/ext.c defines it in a file that includes the
   translation's funcs.h, as ports do), and a runtime stand-in the game does not import, found
   by GetProcAddress (the runtime's name table, no export table) */
__declspec(dllimport) int __stdcall StubExtValue(void);
typedef BOOL (WINAPI *IPFP)(DWORD);
/* A dialog run by the port with no window (runtime/win32/dialog.c dialog_drive, from port_dialog_box):
   its object in GWL_USERDATA, a command message, and EndDialog's value back to DialogBoxParamA */
static unsigned dlg_seen;
static INT_PTR CALLBACK dlg_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_INITDIALOG) { SetWindowLongA(h, GWL_USERDATA, (LONG)l); return TRUE; }
    if (m == WM_COMMAND && LOWORD(w) == 5) {
        RECT r;
        GetWindowRect(h, &r);
        dlg_seen = (unsigned)GetWindowLongA(h, GWL_USERDATA) + (unsigned)(r.right - r.left);
        return TRUE;
    }
    if (m == WM_COMMAND && LOWORD(w) == IDOK) { EndDialog(h, 77); return TRUE; }
    return FALSE;
}
/* The host window follows the game's windows: an owned popup shown first (a film window) becomes
   the main window, the game's own window is shown, then the popup is hidden and destroyed: the
   game's window takes over (WindowFromPoint answers the main window). Then that one is hidden with
   nothing else showing, and a new window shown takes over. */
static void window_test(void)
{
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "stubwin";
    RegisterClassA(&wc);
    HWND game = CreateWindowExA(0, "stubwin", "game", WS_POPUP, 0, 0, 640, 480, NULL, NULL, wc.hInstance, NULL);
    HWND film = CreateWindowExA(0, "stubwin", "film", WS_POPUP, 0, 0, 640, 480, game, NULL, wc.hInstance, NULL);
    POINT pt = {10, 10};
    ShowWindow(film, SW_SHOW);
    line("window_first_shown_is_main", WindowFromPoint(pt) == film);
    ShowWindow(game, SW_SHOW);
    ShowWindow(film, SW_HIDE);
    line("window_hidden_main_handed_over", WindowFromPoint(pt) == game);
    DestroyWindow(film);
    line("window_after_destroy", WindowFromPoint(pt) == game);
    ShowWindow(game, SW_HIDE);
    HWND later = CreateWindowExA(0, "stubwin", "later", WS_POPUP, 0, 0, 640, 480, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(later, SW_SHOW);
    line("window_shown_after_hidden_main", WindowFromPoint(pt) == later);
    DestroyWindow(later);
    DestroyWindow(game);
}

/* Threads after threads: a finished thread's stack and TLS go to the next one (the same stack
   address, the TLS template's value again); a thread that ends holding a mutex abandons it
   (WAIT_ABANDONED); GetCurrentThread() duplicated is a real handle to that thread (a waiter
   on the main thread's sees it running) */
static volatile DWORD seen_local[2], seen_tls[2];
static HANDLE mtx, main_dup;
static DWORD WINAPI reuse_worker(LPVOID arg)
{
    volatile int here = 0;
    int k = (int)(INT_PTR)arg;
    seen_local[k] = (DWORD)(UINT_PTR)&here;
    seen_tls[k] = tls_value;
    tls_value = 7;
    return 0;
}
static DWORD WINAPI mutex_worker(LPVOID arg)
{
    (void)arg;
    WaitForSingleObject(mtx, INFINITE);
    return WaitForSingleObject(main_dup, 0);       /* (the main thread runs: not signalled) */
}
static void thread_test(void)
{
    for (int k = 0; k < 2; k++) {
        HANDLE t = CreateThread(NULL, 0, reuse_worker, (LPVOID)(INT_PTR)k, 0, NULL);
        WaitForSingleObject(t, 10000);
        CloseHandle(t);
    }
    line("thread_stack_reused", seen_local[0] == seen_local[1]);
    line("thread_tls_fresh", seen_tls[0] == 0x1234 && seen_tls[1] == 0x1234);
    mtx = CreateMutexA(NULL, FALSE, NULL);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &main_dup, 0, FALSE, 2);
    line("dup_is_new_handle", main_dup != GetCurrentThread());
    HANDLE t = CreateThread(NULL, 0, mutex_worker, NULL, 0, NULL);
    WaitForSingleObject(t, 10000);
    DWORD code = 0;
    GetExitCodeThread(t, &code);
    line("dup_main_not_signalled", code == WAIT_TIMEOUT);
    line("mutex_abandoned", WaitForSingleObject(mtx, 1000));
    ReleaseMutex(mtx);
    line("pseudo_close_ok", CloseHandle(GetCurrentThread()) != 0);
    CloseHandle(t);
    CloseHandle(main_dup);
    CloseHandle(mtx);
}

static void link_test(void)
{
    line("port_import", StubExtValue());
    line("default_import", GetPriorityClass(GetCurrentProcess()));   /* a replaceable default (API_WEAK) */
    IPFP ipfp = (IPFP)GetProcAddress(GetModuleHandleA("kernel32.dll"), "IsProcessorFeaturePresent");
    line("getprocaddress_unimported", ipfp != NULL);
    line("unimported_call", ipfp ? ipfp(PF_MMX_INSTRUCTIONS_AVAILABLE) + 1 : 0);
    line("dialog_result", (unsigned)DialogBoxParamA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(9), NULL, dlg_proc, 1000));
    line("dialog_seen", dlg_seen);
}

void __stdcall start(void)
{
    put("stub game\n");
    const char *cmd = GetCommandLineA();
    put("cmdline "); put(cmd); put("\n");
    line("module_is_image", GetModuleHandleA(NULL) == (HMODULE)0x400000);

    HANDLE heap = GetProcessHeap();
    unsigned *p = HeapAlloc(heap, HEAP_ZERO_MEMORY, 4096);
    unsigned sum = 0;
    for (int i = 0; i < 1024; i++) { p[i] = (unsigned)i * 3u; sum += p[i]; }
    line("heap_sum", sum);
    line("heap_free", HeapFree(heap, 0, p) != 0);

    unsigned char *v = VirtualAlloc(NULL, 1 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    line("virtualalloc_zeroed", v && v[0] == 0 && v[(1 << 20) - 1] == 0);
    VirtualFree(v, 0, MEM_RELEASE);

    done = CreateEventA(NULL, TRUE, FALSE, NULL);
    DWORD tid;
    HANDLE t = CreateThread(NULL, 0, worker, (LPVOID)5, 0, &tid);
    line("event_wait", WaitForSingleObject(done, 10000));
    WaitForSingleObject(t, 10000);
    DWORD code = 0;
    GetExitCodeThread(t, &code);
    line("thread_exit_code", code);
    line("counter", (unsigned)counter);
    tls_value += 1;
    line("tls_main", tls_value);
    line("tls_worker", worker_tls);
    line("tls_worker_from_callback", worker_tls7);
    line("tls_main_zeroed", tls_zeroed[7]);
    line("tls_callbacks_process_thread_detach", tls_attach[1] * 100 + tls_attach[2] * 10 + tls_attach[3]);
    CloseHandle(t);

    HKEY k;
    DWORD val = 0x1234, type = 0, size = sizeof val, back = 0;
    RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\AntiStatic\\Stub", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k, NULL);
    RegSetValueExA(k, "Value", 0, REG_DWORD, (const BYTE *)&val, sizeof val);
    RegQueryValueExA(k, "Value", NULL, &type, (BYTE *)&back, &size);
    RegCloseKey(k);
    line("registry_back", back);

    DWORD t0 = GetTickCount();
    Sleep(20);
    line("tick_moves", GetTickCount() >= t0);
    LARGE_INTEGER f;
    line("qpc_freq_nonzero", QueryPerformanceFrequency(&f) && f.QuadPart != 0);

    put("maths "); put_hex(maths()); put("\n");
    put("mix "); put_hex(stub_mix(0x12345678u, 0xcafef00du)); put("\n");
    line("setjmp_imported", setjmp_test(0));
    line("setjmp_own", setjmp_test(1));
    sound_test();
    miles_test();
    link_test();
    window_test();
    thread_test();

    /* the game's CD (port.h PORT_CD_LABEL): drive D:, its label, a file on it */
    line("cd_drive_type", GetDriveTypeA("D:\\"));
    char label[32] = {0};
    GetVolumeInformationA("D:\\", label, sizeof label, NULL, NULL, NULL, NULL, 0);
    put("cd_label "); put(label); put("\n");
    HANDLE cd = CreateFileA("D:\\disc.txt", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    char buf[16] = {0};
    DWORD got = 0;
    ReadFile(cd, buf, 15, &got, NULL);
    CloseHandle(cd);
    put("cd_file "); put(buf); put("\n");
    line("cd_read_only", (GetFileAttributesA("D:\\disc.txt") & FILE_ATTRIBUTE_READONLY) != 0);

    HANDLE h = CreateFileA("result.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD wrote = 0;
    WriteFile(h, out, (DWORD)outn, &wrote, NULL);
    CloseHandle(h);
    ExitProcess(wrote == (DWORD)outn ? 42 : 1);
}

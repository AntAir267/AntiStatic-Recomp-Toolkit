/* More Windows calls, from penguins-native's winpenguins.c and advapi32.c (2026-11-12): file
   attributes, GUID strings, OLE security and automation stand-ins, a network that is never there
   (WININET, iphlpapi: nothing is ever sent), the shell refusing to open pages, and access-control
   calls that succeed (there is no access control here). */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "runtime.h"

static void put_filetime(uint32_t at, time_t t)
{
    uint64_t ft = ((uint64_t)t + 11644473600ull) * 10000000ull;
    W32(at, (uint32_t)ft);
    W32(at + 4, (uint32_t)(ft >> 32));
}

static void guid_text(uint32_t g, char out[39])
{
    snprintf(out, 39, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", R32(g), R32(g + 4) & 0xffff,
             R32(g + 6) & 0xffff, R8(g + 8), R8(g + 9), R8(g + 10), R8(g + 11), R8(g + 12), R8(g + 13),
             R8(g + 14), R8(g + 15));
}
static void put_wide(uint32_t at, const char *s)
{
    for (; *s; s++, at += 2) W16(at, (uint8_t)*s);
    W16(at, 0);
}


/* iphlpapi GetAdaptersInfo (a machine id for WildTangent): no adapters (ERROR_NO_DATA) */
API_WEAK(IPHLPAPI, GetAdaptersInfo, 2) { if (ARG(1)) W32(ARG(1), 0); return 232; }

/* GetFileAttributesExA(name, GetFileExInfoStandard, WIN32_FILE_ATTRIBUTE_DATA *) */
API_WEAK(KERNEL32, GetFileAttributesExA, 3)
{
    char p[2048];
    const char *name = gstr(ARG(0));
    struct stat st;
    if (!name || ARG(1) != 0 || !path_to_host(name, p, sizeof p, 0) || stat(p, &st) != 0) {
        LOG(LOG_FILE, "GetFileAttributesExA(\"%s\") -> none", name ? name : "");
        set_last_error(2);
        return 0;
    }
    uint32_t d = ARG(2);
    W32(d, S_ISDIR(st.st_mode) ? 0x10 : 0x20);
    put_filetime(d + 4, st.st_mtime);
    put_filetime(d + 12, st.st_atime);
    put_filetime(d + 20, st.st_mtime);
    W32(d + 28, (uint32_t)((uint64_t)st.st_size >> 32));
    W32(d + 32, (uint32_t)st.st_size);
    LOG(LOG_FILE, "GetFileAttributesExA(\"%s\") -> %lld bytes", name, (long long)st.st_size);
    return 1;
}

API_WEAK(OLE32, CoInitializeSecurity, 9) { return 0; }

API_WEAK(OLE32, CoSetProxyBlanket, 8) { return 0; }

API_WEAK(OLE32, StringFromCLSID, 2)
{
    char t[39];
    guid_text(ARG(0), t);
    uint32_t w = galloc(80);
    put_wide(w, t);
    W32(ARG(1), w);
    return 0;
}

API_WEAK(OLEAUT32, 200, 2) { if (ARG(1)) W32(ARG(1), 0); return 1; }    /* GetErrorInfo: S_FALSE, none */

/* #31 CreateDispTypeInfo (Penguins! 0x52b140): the game describes a scriptable object (properties like
   "Event", "Change value to post an event") for the HTML panels Internet Explorer would show and
   call into. There is no browser here, so nothing ever asks it; it only has to succeed (a
   failure is thrown as a C++ exception, and its unwinding runs a __finally block this runtime
   can't return from [verified: a run stopped at 0x582e73, _local_unwind2]). The ITypeInfo
   handed back answers every method with E_NOTIMPL. */
#define TI_STUB(name, n) METHOD(ti_##name, n) { LOG(LOG_API, "ITypeInfo::" #name ": not implemented"); return 0x80004001u; }
METHOD(ti_QueryInterface, 3) { if (ARG(2)) W32(ARG(2), 0); return 0x80004002u; }
METHOD(ti_AddRef, 1) { return 1; }
METHOD(ti_Release, 1) { return 0; }
TI_STUB(GetTypeAttr, 2) TI_STUB(GetTypeComp, 2) TI_STUB(GetFuncDesc, 3) TI_STUB(GetVarDesc, 3)
TI_STUB(GetNames, 5) TI_STUB(GetRefTypeOfImplType, 3) TI_STUB(GetImplTypeFlags, 3)
TI_STUB(GetIDsOfNames, 4) TI_STUB(Invoke, 8) TI_STUB(GetDocumentation, 6) TI_STUB(GetDllEntry, 6)
TI_STUB(GetRefTypeInfo, 3) TI_STUB(AddressOfMember, 4) TI_STUB(CreateInstance, 4) TI_STUB(GetMops, 3)
TI_STUB(GetContainingTypeLib, 3)
METHOD(ti_ReleaseTypeAttr, 2) { return 0; }
METHOD(ti_ReleaseFuncDesc, 2) { return 0; }
METHOD(ti_ReleaseVarDesc, 2) { return 0; }
static void (*const ti_methods[])(CPU *) = {
    ti_QueryInterface, ti_AddRef, ti_Release, ti_GetTypeAttr, ti_GetTypeComp, ti_GetFuncDesc,
    ti_GetVarDesc, ti_GetNames, ti_GetRefTypeOfImplType, ti_GetImplTypeFlags, ti_GetIDsOfNames,
    ti_Invoke, ti_GetDocumentation, ti_GetDllEntry, ti_GetRefTypeInfo, ti_AddressOfMember,
    ti_CreateInstance, ti_GetMops, ti_GetContainingTypeLib, ti_ReleaseTypeAttr, ti_ReleaseFuncDesc,
    ti_ReleaseVarDesc };
static const char *const ti_names[] = {
    "QueryInterface", "AddRef", "Release", "GetTypeAttr", "GetTypeComp", "GetFuncDesc", "GetVarDesc",
    "GetNames", "GetRefTypeOfImplType", "GetImplTypeFlags", "GetIDsOfNames", "Invoke",
    "GetDocumentation", "GetDllEntry", "GetRefTypeInfo", "AddressOfMember", "CreateInstance",
    "GetMops", "GetContainingTypeLib", "ReleaseTypeAttr", "ReleaseFuncDesc", "ReleaseVarDesc" };
API_WEAK(OLEAUT32, 31, 3)
{
    static ComClass cls;
    static int ready;
    if (!ready) { com_class_init(&cls, "ITypeInfo", 22, ti_methods, ti_names); ready = 1; }
    static int dummy;
    if (ARG(2)) W32(ARG(2), com_new(&cls, &dummy));
    LOG(LOG_API, "CreateDispTypeInfo -> stand-in");
    return 0;
}

/* SHELL32 ShellExecuteExA: web pages and the like; never opened, logged */
API_WEAK(SHELL32, ShellExecuteExA, 1)
{
    uint32_t i = ARG(0);
    const char *file = i ? gstr(R32(i + 16)) : NULL;
    LOG(LOG_ALWAYS, "ShellExecuteExA(%s): not opened", file ? file : "?");
    set_last_error(1155);
    return 0;
}

API_WEAK(WININET, InternetCloseHandle, 1) { return 1; }

/* ------------------------------------------------------------------ WININET: offline
   The game's online parts (WildTangent's statistics check-in, the update list, leaderboards):
   there is no internet here. InternetOpenA fails as with no connection, and the rest fail too
   if reached. Nothing is ever sent. */
API_WEAK(WININET, InternetOpenA, 5)
{
    LOG(LOG_API, "InternetOpenA(%s): offline", ARG(0) ? gstr(ARG(0)) : "");
    set_last_error(12029);
    return 0;
}

API_WEAK(ADVAPI32, ImpersonateSelf, 1) { return 1; }

API_WEAK(ADVAPI32, InitializeSecurityDescriptor, 2)
{
    if (ARG(0)) memset(gptr(ARG(0)), 0, 20), W8(ARG(0), 1);   /* SECURITY_DESCRIPTOR_REVISION */
    return 1;
}

API_WEAK(ADVAPI32, RegSetKeySecurity, 3) { return 0; }

API_WEAK(ADVAPI32, RevertToSelf, 0) { return 1; }

/* ------------------------------------------------------------------ security (Penguins!)
   WildTangent's CDA code makes its shared objects open to everyone (SetEntriesInAclA, a
   security descriptor, RegSetKeySecurity). There is no access control here: each succeeds;
   the ACL is an empty one on the process heap (the program frees it with LocalFree). */
API_WEAK(ADVAPI32, SetEntriesInAclA, 4)
{
    extern uint32_t process_heap;
    uint32_t acl = heap_alloc(process_heap, 8, 1);
    W8(acl, 2); W8(acl + 1, 0); W16(acl + 2, 8); W16(acl + 4, 0); W16(acl + 6, 0);   /* ACL_REVISION, size 8, 0 ACEs */
    if (ARG(3)) W32(ARG(3), acl);
    return 0;
}

API_WEAK(ADVAPI32, SetSecurityDescriptorDacl, 4)
{
    if (ARG(0)) { W16(ARG(0) + 2, ARG(1) ? 4 : 0); W32(ARG(0) + 16, ARG(2)); }   /* SE_DACL_PRESENT, Dacl */
    return 1;
}

/* From lixs-native's lixs_win32.c and penguins-native's winpenguins.c (2026-11-12): the
   priority class, the shell's special folders, OLE calls a game makes and doesn't need, its
   development tools' file / colour pickers and AVI capture ("not available"). */
#define AVIERR_UNSUPPORTED 0x80044065u
API_WEAK(KERNEL32, GetPriorityClass, 1) { return 0x20; }             /* NORMAL_PRIORITY_CLASS */
API_WEAK(OLE32, CoFreeUnusedLibraries, 0) { return 0; }
API_WEAK(OLE32, GetRunningObjectTable, 2) { if (ARG(1)) W32(ARG(1), 0); return 0x80004005u; } /* E_FAIL */
API_WEAK(OLE32, CreateItemMoniker, 3) { if (ARG(2)) W32(ARG(2), 0); return 0x80004005u; }
API_WEAK(COMDLG32, GetOpenFileNameA, 1) { return 0; }                 /* cancelled */
API_WEAK(COMDLG32, GetSaveFileNameA, 1) { return 0; }
API_WEAK(COMDLG32, ChooseColorA, 1) { return 0; }
API_WEAK(AVIFIL32, AVIFileInit, 0) { return 0; }
API_WEAK(AVIFIL32, AVIFileExit, 0) { return 0; }
API_WEAK(AVIFIL32, AVIFileOpenA, 4) { if (ARG(0)) W32(ARG(0), 0); return AVIERR_UNSUPPORTED; }
API_WEAK(AVIFIL32, AVIFileRelease, 1) { return 0; }
API_WEAK(AVIFIL32, AVIFileCreateStreamA, 3) { if (ARG(1)) W32(ARG(1), 0); return AVIERR_UNSUPPORTED; }
API_WEAK(AVIFIL32, AVIMakeCompressedStream, 4) { if (ARG(0)) W32(ARG(0), 0); return AVIERR_UNSUPPORTED; }
API_WEAK(AVIFIL32, AVIStreamRelease, 1) { return 0; }
API_WEAK(AVIFIL32, AVIStreamSetFormat, 4) { return AVIERR_UNSUPPORTED; }
API_WEAK(AVIFIL32, AVIStreamWrite, 8) { return AVIERR_UNSUPPORTED; }
/* MSVFW32 ordinal 2: VideoForWindowsVersion() [verified: lixs 0x41f570, one call, no arguments] */
API_WEAK(MSVFW32, 2, 0) { return 0x010a0000u; }

/* SHGetSpecialFolderPathA: folders under the user folder's drive_c (CSIDL_APPDATA 0x1a,
   CSIDL_PERSONAL 0x05, CSIDL_COMMON_APPDATA 0x23; any other: the profile) */
API_WEAK(SHELL32, SHGetSpecialFolderPathA, 4)
{
    const char *p = ARG(2) == 0x1a ? "C:\\Users\\Player\\Application Data"
                  : ARG(2) == 0x23 ? "C:\\ProgramData"
                  : ARG(2) == 0x05 ? "C:\\Users\\Player\\My Documents"
                  : "C:\\Users\\Player";
    gstrncpy(ARG(1), p, 260);
    LOG(LOG_FILE, "SHGetSpecialFolderPathA(%#x) -> %s", ARG(2), p);
    return 1;
}

/* From nitronic-native's port_nitronic.c (2026-11-13; Nitronic Rush and its Visual C++ 2008
   runtime): the wide event, the alertable waits (no queued routines are run: the program queues
   none [guess]), the Windows folder, GetWindowInfo from the core's own calls, and a window that
   takes dropped files (nothing is ever dropped). Weak: a port's own copy wins. */
uint32_t event_create(int manual, int initial);   /* sync.c */

/* (security, manual reset, initial state, name): events are not named in the core (CreateEventA) */
API_WEAK(KERNEL32, CreateEventW, 4)
{
    uint32_t h = event_create(ARG(1) != 0, ARG(2) != 0);
    LOG(LOG_THREAD, "CreateEventW(manual=%d, initial=%d) -> %x", ARG(1) != 0, ARG(2) != 0, h);
    set_last_error(0);
    return h;
}

/* (count, handles, wait all, ms, alertable): no queued routines are run for the alertable form
   [guess: the program queues none; the core's WaitForSingleObjectEx runs them, sync.c] */
API_WEAK(KERNEL32, WaitForMultipleObjectsEx, 5)
{
    uint32_t n = ARG(0), hs[64];
    if (n > 64) n = 64;
    for (uint32_t i = 0; i < n; i++)
        hs[i] = R32(ARG(1) + 4 * i);
    LOG(LOG_THREAD, "WaitForMultipleObjectsEx(%u, all=%d, %d, alertable=%d)", n, ARG(2) != 0, (int)ARG(3), ARG(4) != 0);
    return wait_handles(n, hs, ARG(2) != 0, ARG(3));
}

/* (buffer, size in characters) -> length without the terminator */
API_WEAK(KERNEL32, GetSystemWindowsDirectoryW, 2)
{
    static const char dir[] = "C:\\WINDOWS";
    uint32_t dst = ARG(0), n = ARG(1);
    if (n < sizeof dir)
        return sizeof dir;                  /* the size needed, terminator included */
    for (uint32_t i = 0; i < sizeof dir; i++)
        W16(dst + 2 * i, (uint8_t)dir[i]);
    return sizeof dir - 1;
}

/* ------------------------------------------------------------------ calling the core's own */
/* One of the core's stdcall functions called from here: its arguments pushed on the program's
   stack as the program would, and popped by it. */
typedef void (*api_fn)(CPU *);
static uint32_t call_api(CPU *c, api_fn f, int n, const uint32_t *args)
{
    c->esp -= 4 * (n + 1);
    W32(c->esp, 0);                         /* (a return address nothing reads) */
    for (int i = 0; i < n; i++)
        W32(c->esp + 4 + 4 * i, args[i]);
    f(c);
    return c->eax;
}
void w_KERNEL32_Sleep(CPU *c);
void w_USER32_GetWindowRect(CPU *c);
void w_USER32_GetClientRect(CPU *c);
void w_USER32_ClientToScreen(CPU *c);
void w_USER32_GetWindowLongA(CPU *c);

/* (ms, alertable): no queued routines to run */
API_WEAK(KERNEL32, SleepEx, 2)
{
    uint32_t a[1] = {ARG(0)};
    call_api(c, w_KERNEL32_Sleep, 1, a);
    return 0;
}

/* ------------------------------------------------------------------ windows */
/* (hwnd, WINDOWINFO*): cbSize, rcWindow, rcClient (screen coordinates), dwStyle, dwExStyle,
   dwWindowStatus, cxWindowBorders, cyWindowBorders, atomWindowType, wCreatorVersion */
API_WEAK(USER32, GetWindowInfo, 2)
{
    uint32_t h = ARG(0), wi = ARG(1);
    uint32_t a[2] = {h, wi + 4};
    if (!call_api(c, w_USER32_GetWindowRect, 2, a))
        return 0;
    a[1] = wi + 20;
    call_api(c, w_USER32_GetClientRect, 2, a);
    uint32_t cw = R32(wi + 28), ch = R32(wi + 32);
    W32(wi + 20, 0); W32(wi + 24, 0);
    call_api(c, w_USER32_ClientToScreen, 2, a);         /* the client area's origin on the screen */
    W32(wi + 28, R32(wi + 20) + cw); W32(wi + 32, R32(wi + 24) + ch);
    uint32_t b[2] = {h, (uint32_t)-16};                 /* GWL_STYLE */
    W32(wi + 36, call_api(c, w_USER32_GetWindowLongA, 2, b));
    b[1] = (uint32_t)-20;                               /* GWL_EXSTYLE */
    W32(wi + 40, call_api(c, w_USER32_GetWindowLongA, 2, b));
    W32(wi + 44, 1);                                    /* WS_ACTIVECAPTION */
    W32(wi + 48, 0); W32(wi + 52, 0);
    W16(wi + 56, 0); W16(wi + 58, 0x0500);
    LOG(LOG_WIN, "GetWindowInfo(%x): window %d,%d-%d,%d client %d,%d-%d,%d", h, (int)R32(wi + 4), (int)R32(wi + 8),
        (int)R32(wi + 12), (int)R32(wi + 16), (int)R32(wi + 20), (int)R32(wi + 24), (int)R32(wi + 28), (int)R32(wi + 32));
    return 1;
}

API_WEAK(SHELL32, DragAcceptFiles, 2) { return 0; }

/* Less common Windows imports (from Hard Truck 2 and Off-Road: MFC programs): string
   resources, a few GDI and thread functions, and the parts a port does without: network play (WSOCK32: Windows Sockets is
   reported as not ready, as on a PC with no network), printing (WINSPOOL), drag and drop
   (SHELL32) and OLE structured storage. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"

/* ------------------------------------------------------------------ USER32 */
/* LoadStringA: string tables are RT_STRING (6) blocks of 16, block id / 16 + 1, each string a
   UTF-16 count and its characters. English strings: Latin-1 bytes, others '?' */
API(USER32, LoadStringA, 4)
{
    uint32_t id = ARG(1), buf = ARG(2), cap = ARG(3);
    uint32_t data, size;
    if (!cap) return 0;
    W8(buf, 0);
    if (!resource_find(6, id / 16 + 1, &data, &size)) {
        LOG(LOG_WIN, "LoadStringA(%u): no such string", id);
        return 0;
    }
    uint32_t p = data;
    for (uint32_t k = 0; k < (id & 15); k++)
        p += 2 + 2 * R16(p);
    uint32_t n = R16(p), i;
    for (i = 0; i < n && i + 1 < cap; i++) {
        uint32_t ch = R16(p + 2 + 2 * i);
        W8(buf + i, ch < 0x100 ? ch : '?');
    }
    W8(buf + i, 0);
    LOG(LOG_WIN, "LoadStringA(%u) -> \"%s\"", id, gstr(buf));
    return i;
}

int wsprintf_impl(char *out, size_t cap, const char *fmt, uint32_t args);
API(USER32, wvsprintfA, 3)
{
    char buf[1025];
    int n = wsprintf_impl(buf, sizeof buf, gstr(ARG(1)), ARG(2));
    memcpy(gptr(ARG(0)), buf, n + 1);
    return n;
}

/* ------------------------------------------------------------------ GDI32 */
API(GDI32, CreatePalette, 1) { return 0x00016000u; }
API(GDI32, RealizePalette, 1) { return 0; }
API(GDI32, SetDIBitsToDevice, 12) { return ARG(8); }    /* the lines asked for */

/* ------------------------------------------------------------------ KERNEL32 */
API(KERNEL32, GetThreadPriority, 1) { return 0; }
API(KERNEL32, GetProcessVersion, 1) { return 0x00040000u; }  /* a 4.0 program */
/* a crash handler's tools: not available */
API(KERNEL32, GetThreadContext, 2) { set_last_error(5); return 0; }
API(KERNEL32, ReadProcessMemory, 5)
{
    uint32_t n = ARG(3);
    memmove(gptr(ARG(2)), gptr(ARG(1)), n);
    if (ARG(4)) W32(ARG(4), n);
    return 1;
}

/* ------------------------------------------------------------------ OLE */

/* ------------------------------------------------------------------ SHELL32, WINSPOOL
   (these and WSOCK32's are defaults: a port with its own stand-ins replaces them) */
API_WEAK(SHELL32, DragQueryFileA, 4) { return 0; }
API_WEAK(SHELL32, DragFinish, 1) { return 0; }
API_WEAK(WINSPOOL, OpenPrinterA, 3) { if (ARG(1)) W32(ARG(1), 0); return 0; }
API_WEAK(WINSPOOL, ClosePrinter, 1) { return 0; }
API_WEAK(WINSPOOL, DocumentPropertiesA, 6) { return 0xffffffffu; }

/* ------------------------------------------------------------------ WSOCK32 (by ordinal) */
/* No network: Winsock refuses to start, or with PORT_WINSOCK_STARTS (Penguins!, whose game needs
   it started) it starts and every socket fails with WSAENETDOWN. Nothing is ever sent. */
#define WSASYSNOTREADY 10091
#define WSAENETDOWN 10050
#if defined(PORT_WINSOCK_STARTS)
static uint32_t wsa_error;
#else
static uint32_t wsa_error = WSAENETDOWN;
#endif
API_WEAK(WSOCK32, 115, 2)                    /* WSAStartup(version, WSADATA *) */
{
#if defined(PORT_WINSOCK_STARTS)
    uint32_t d = ARG(1);
    if (d) {
        memset(gptr(d), 0, 400);
        W16(d, 0x0101);                      /* wVersion */
        W16(d + 2, 0x0202);                  /* wHighVersion */
        gstrncpy(d + 4, "WinSock 2.0", 257);
        gstrncpy(d + 261, "Running", 129);
        W16(d + 390, 32767);                 /* iMaxSockets */
        W16(d + 392, 65467);                 /* iMaxUdpDg */
    }
    LOG(LOG_API, "WSAStartup(%x): started, without a network", ARG(0));
    return 0;
#else
    LOG(LOG_ALWAYS, "WSAStartup(%x): no network play in this port", ARG(0));
    return WSASYSNOTREADY;
#endif
}
API_WEAK(WSOCK32, 116, 0) { return 0; }                          /* WSACleanup */
API_WEAK(WSOCK32, 111, 0) { return wsa_error; }                  /* WSAGetLastError */
#define WSA_FAIL { wsa_error = WSAENETDOWN; return 0xffffffffu; }
API_WEAK(WSOCK32, 23, 3)                                          /* socket: INVALID_SOCKET */
{
    LOG(LOG_API, "socket(%u, %u, %u): no network", ARG(0), ARG(1), ARG(2));
    WSA_FAIL
}
API_WEAK(WSOCK32, 2, 3) WSA_FAIL                                  /* bind */
API_WEAK(WSOCK32, 13, 2) WSA_FAIL                                 /* listen */
API_WEAK(WSOCK32, 1, 3) WSA_FAIL                                  /* accept */
API_WEAK(WSOCK32, 16, 4) WSA_FAIL                                 /* recv */
API_WEAK(WSOCK32, 18, 5) WSA_FAIL                                 /* select */
API_WEAK(WSOCK32, 8, 1) { uint32_t v = ARG(0); return (v >> 24) | ((v >> 8) & 0xff00u) | ((v << 8) & 0xff0000u) | (v << 24); } /* htonl */
API_WEAK(WSOCK32, 14, 1) { uint32_t v = ARG(0); return (v >> 24) | ((v >> 8) & 0xff00u) | ((v << 8) & 0xff0000u) | (v << 24); } /* ntohl */
API_WEAK(WSOCK32, 15, 1) { uint32_t v = ARG(0) & 0xffff; return ((v & 0xff) << 8) | (v >> 8); }   /* ntohs */
API_WEAK(WSOCK32, 57, 2)                     /* gethostname(buf, len): the computer's name */
{
    extern const char *computer_name(void);
    if (ARG(1) <= strlen(computer_name())) { wsa_error = 10014; return 0xffffffffu; }   /* WSAEFAULT */
    gstrncpy(ARG(0), computer_name(), ARG(1));
    return 0;
}
API_WEAK(WSOCK32, 52, 1)                     /* gethostbyname(name): this computer, 127.0.0.1 */
{
    extern const char *computer_name(void);
    static uint32_t he;
    if (!he) {
        he = galloc(64);
        uint32_t name = he + 16, addr = he + 32, list = he + 40, alias = he + 48;
        gstrncpy(name, computer_name(), 16);
        W32(addr, 0x0100007fu);
        W32(list, addr);
        W32(list + 4, 0);
        W32(alias, 0);
        W32(he, name);
        W32(he + 4, alias);
        W16(he + 8, 2);                      /* AF_INET */
        W16(he + 10, 4);
        W32(he + 12, list);
    }
    return he;
}
API_WEAK(WSOCK32, 3, 1) WSA_FAIL                                  /* closesocket */
API_WEAK(WSOCK32, 4, 3) WSA_FAIL                                  /* connect */
API_WEAK(WSOCK32, 19, 4) WSA_FAIL                                 /* send */
API_WEAK(WSOCK32, 9, 1) { uint32_t v = ARG(0) & 0xffff; return ((v & 0xff) << 8) | (v >> 8); }   /* htons */
API_WEAK(WSOCK32, 10, 1) { return 0xffffffffu; }                  /* inet_addr: INADDR_NONE */

/* WS2_32 exports the same functions at the same ordinals */
#define WS2_ALIAS(n) void w_WSOCK32_##n(CPU *c); __attribute__((weak)) void w_WS2_32_##n(CPU *c) { w_WSOCK32_##n(c); }
WS2_ALIAS(1) WS2_ALIAS(2) WS2_ALIAS(3) WS2_ALIAS(4) WS2_ALIAS(8) WS2_ALIAS(9) WS2_ALIAS(10)
WS2_ALIAS(13) WS2_ALIAS(14) WS2_ALIAS(15) WS2_ALIAS(16) WS2_ALIAS(18) WS2_ALIAS(19) WS2_ALIAS(23)
WS2_ALIAS(52) WS2_ALIAS(57) WS2_ALIAS(111) WS2_ALIAS(115) WS2_ALIAS(116)

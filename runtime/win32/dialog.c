/* dialog_drive: a dialog's own procedure run as Windows would run it, with no window on screen, for
   test switches that exercise a game's dialog code (from one port's dialog route, made
   game-agnostic). A port calls it from its port_dialog_box answer.

   The dialog gets a handle of its own outside the window table (user32.c's windows are 0x10020 up,
   512 of them), so the core's window functions leave it alone. WM_INITDIALOG with param is sent,
   then each of the n messages {msg, wParam, lParam} until EndDialog, then WM_COMMAND IDOK if it has
   not ended. The result is the EndDialog value (1 when nothing ended it). The dialog's controls
   answer nothing (GetDlgItem / SendDlgItemMessageA give 0, so lists read as their first entries).

   Five calls answer for the dialog's handle: GetWindowRect (400x300), SetWindowPos (taken),
   EndDialog (the result kept), Get / SetWindowLongA (the procedure, instance, a popup style and
   GWL_USERDATA). While the procedure runs every call into the runtime goes through its thunk
   (api_via_thunk, as --trace-calls does) and the call hook fixes those five up after they return:
   their stdcall arguments are still just below esp. Nothing changes for a run that doesn't call it. */
#include <string.h>
#include "runtime.h"

#define DLG_HWND 0x00011000u
extern int api_via_thunk;
static uint32_t dlg_result, dlg_userdata, dlg_proc;
static int dlg_ended;
static void (*dlg_hook0)(CPU *, const char *, int);    /* the hook in place before (--trace-calls') */

static void dialog_api(CPU *c, const char *name, int after)
{
    if (dlg_hook0) dlg_hook0(c, name, after);
    const char *n = name ? strchr(name, '!') : NULL;
    if (!after || !n) return;
    n++;
    if (!strcmp(n, "EndDialog") && R32(c->esp - 8) == DLG_HWND) {
        dlg_ended = 1;
        dlg_result = R32(c->esp - 4);
    } else if (!strcmp(n, "GetWindowRect") && R32(c->esp - 8) == DLG_HWND) {
        uint32_t r = R32(c->esp - 4);
        W32(r, 0); W32(r + 4, 0); W32(r + 8, 400); W32(r + 12, 300);
        c->eax = 1;
    } else if (!strcmp(n, "SetWindowPos") && R32(c->esp - 28) == DLG_HWND) {
        LOG(LOG_WIN, "SetWindowPos(dialog %x, %dx%d): no window", DLG_HWND, (int)R32(c->esp - 12), (int)R32(c->esp - 8));
        c->eax = 1;
    } else if (!strcmp(n, "SetWindowLongA") && R32(c->esp - 12) == DLG_HWND) {
        if ((int32_t)R32(c->esp - 8) == -21) {      /* GWL_USERDATA: the old value back */
            c->eax = dlg_userdata;
            dlg_userdata = R32(c->esp - 4);
        } else
            c->eax = 0;
    } else if (!strcmp(n, "GetWindowLongA") && R32(c->esp - 8) == DLG_HWND) {
        switch ((int32_t)R32(c->esp - 4)) {
        case -4: c->eax = dlg_proc; break;                               /* GWL_WNDPROC */
        case -6: c->eax = module_count ? module_table[0].base : 0x00400000u; break;   /* GWL_HINSTANCE */
        case -16: c->eax = 0x80000000u; break;                           /* GWL_STYLE: WS_POPUP */
        case -21: c->eax = dlg_userdata; break;                          /* GWL_USERDATA */
        default: c->eax = 0; break;
        }
    }
}

uint32_t dialog_drive(CPU *c, uint32_t proc, uint32_t param, const uint32_t (*msgs)[3], int n)
{
    void (*hook0)(CPU *, const char *, int) = api_call_hook;
    int thunk0 = api_via_thunk;
    dlg_hook0 = hook0;
    api_call_hook = dialog_api;
    api_via_thunk = 1;
    dlg_ended = 0;
    dlg_result = 0;
    dlg_userdata = 0;
    dlg_proc = proc;
    LOG(LOG_ALWAYS, "dialog %x: procedure %08x run", DLG_HWND, proc);
    guest_call(c, proc, 4, DLG_HWND, 0x0110, 0, param);          /* WM_INITDIALOG */
    for (int i = 0; i < n && !dlg_ended; i++) {
        LOG(LOG_ALWAYS, "dialog %x: message %x (%x, %x)", DLG_HWND, msgs[i][0], msgs[i][1], msgs[i][2]);
        guest_call(c, proc, 4, DLG_HWND, msgs[i][0], msgs[i][1], msgs[i][2]);
    }
    if (!dlg_ended)
        guest_call(c, proc, 4, DLG_HWND, 0x0111, 1, 0);           /* WM_COMMAND IDOK */
    api_call_hook = hook0;
    api_via_thunk = thunk0;
    uint32_t r = dlg_ended ? dlg_result : 1;
    LOG(LOG_ALWAYS, "dialog: ended with %u", r);
    return r;
}

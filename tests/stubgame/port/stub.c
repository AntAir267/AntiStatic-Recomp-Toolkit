/* The stub game's layers: no display (a test program; controllers, DirectInput and DirectSound
   are the runtime core's). */
#include <string.h>
#include "runtime.h"
void game_init(void) {}
void game_frame(unsigned long frame) { (void)frame; }

/* the game's dialog 9: its own procedure run with no window (runtime/win32/dialog.c), with one
   command (id 5) before OK */
uint32_t port_dialog_box(CPU *c, uint32_t template_id, uint32_t proc, uint32_t param)
{
    static const uint32_t msgs[][3] = {{0x0111, 5, 0}};     /* WM_COMMAND 5 */
    if (template_id != 9)
        return 0xffffffffu;
    return dialog_drive(c, proc, param, msgs, 1);
}

/* Do-nothing defaults for what the runtime core calls in a port's input, video and display
   layers, so a port links without the ones it doesn't have (a port's own definitions win:
   these are weak). The DirectInput layer (keyboard, mouse, controllers) normally provides the
   input_* functions; a movie player provides video_skip_press. */
#include "runtime.h"
#include "input.h"

__attribute__((weak)) void input_key(int dik, int down) { (void)dik; (void)down; }
__attribute__((weak)) void input_focus(int focused) { (void)focused; }
__attribute__((weak)) int input_dik_from_hid(unsigned usage) { (void)usage; return 0; }
__attribute__((weak)) void input_mouse_move(int dx, int dy) { (void)dx; (void)dy; }
__attribute__((weak)) void input_mouse_button(int button, int down) { (void)button; (void)down; }
__attribute__((weak)) void input_mouse_wheel(int delta) { (void)delta; }
__attribute__((weak)) int input_mouse_captured(void) { return 0; }
__attribute__((weak)) void input_set_rumble_handler(InputRumbleFn fn) { (void)fn; }
__attribute__((weak)) void input_frame(unsigned long frame) { (void)frame; }
__attribute__((weak)) void (*input_script_key_hook)(int dik, int down);
__attribute__((weak)) void input_joy_names(int slot, const char *const *axes, const char *const *buttons)
{ (void)slot; (void)axes; (void)buttons; }
__attribute__((weak)) long input_script_mark_frame(void) { return -1; }
__attribute__((weak)) void input_script_mark_now(void) {}
/* the frame --shot asked for, as a PNG (the display layer writes it) */
__attribute__((weak)) int d3d_write_png(const char *path) { (void)path; return 0; }
__attribute__((weak)) void video_skip_press(void) {}
/* the port's game: once before the program starts, once per presented frame */
__attribute__((weak)) void game_init(void) {}
__attribute__((weak)) void game_frame(unsigned long frame) { (void)frame; }
__attribute__((weak)) void frame_rate_reapply(void) {}

/* force feedback a port plays as rumble (an IFC21 / Immersion stand-in, input.h): nothing here */
__attribute__((weak)) void ifc_frame(unsigned long frame) { (void)frame; }
__attribute__((weak)) void ifc_poll(struct CPU *c) { (void)c; }

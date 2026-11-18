/* Game controllers (pad.c): how the computer's controllers become the DirectInput joysticks the
   game sees, by the rules in GAME-GOALS ("Every controller works, whenever it is plugged in"). */
#pragma once
#include <stdint.h>

/* Buttons and axes, numbered as SDL3's SDL_GamepadButton and SDL_GamepadAxis (win.c checks). */
enum { PAD_A, PAD_B, PAD_X, PAD_Y, PAD_BACK, PAD_GUIDE, PAD_START, PAD_LSTICK, PAD_RSTICK, PAD_LB,
       PAD_RB, PAD_DUP, PAD_DDOWN, PAD_DLEFT, PAD_DRIGHT, PAD_MISC1, PAD_BUTTONS = 26 };
enum { PAD_LX, PAD_LY, PAD_RX, PAD_RY, PAD_LT, PAD_RT, PAD_AXES };
#define PAD_MAX_PLAYERS 4

/* From the window code (win.c), one call per real controller, and from the scripted one */
int pad_attach(const char *id, const char *name);   /* id: the controller's identity; returns its slot, -1 if none free */
void pad_detach(int slot);
void pad_axis(int slot, int axis, int value);        /* sticks -32768..32767 (+ = right/down), triggers 0..32767 */
void pad_button(int slot, int button, int down);

/* Every connected controller together (menu navigation in a port's own code) */
int pad_held(int button);
int pad_axis_value(int axis);                         /* the one furthest from rest */
int pad_count(void);                                  /* controllers connected */
int pad_active_slot(void);                            /* the one used last, -1 none */
int pad_prompts(void);                                /* a controller was the device used last */
int pad_rumbles(int joy, int slot);
int pad_game_joy(int j);                              /* player j's game joystick (-1 none): XInput's rumble */                   /* does controller `slot` shake with game joystick `joy`? */

/* Players. Outside split screen every controller drives player 1 (and every other joystick the
   game has, so any controller works anywhere). pad_players(n) (2..4) starts split screen: the
   controllers take players 2..n in the order they were connected, each tied to its identity,
   so unplugging one doesn't reshuffle the others; its player waits for it, and a new controller
   plugged in late takes a waiting player. pad_players(1) is back to the menus: every controller
   is player 1's again. */
void pad_players(int n);
int pad_player_of(int slot);                          /* 0-based player of a controller, -1 none */
void pad_two_players(int on);                         /* (= pad_players(on ? 2 : 1)) */
int pad_player_count(void);                           /* players now: 1 outside split screen */

/* Start-up and per frame */
void pad_init(void);                                  /* reads controller_deadzone; the scripted controller */
void pad_options(void);                               /* controller_deadzone changed */
void pad_frame(unsigned long frame);                  /* per presented frame (input_frame) */
void pad_film_tick(void);                             /* per wait while a film plays (no frames) */

/* A port's layout, when the game wants another than Windows' own for an Xbox controller (below):
   a strong port_pad_send sends joystick `joy` from `st` itself (input_joy_axis / _button / _pov)
   and returns 1. st has the dead zone applied and the triggers 0..32767. */
typedef struct { uint8_t button[PAD_BUTTONS]; int axis[PAD_AXES]; } PadState;
/* a player's controller(s) merged (0 if none connected): for a player the game has no joystick
   for, read by the port's own code (e.g. as an Xbox pad), or a port's own menus */
int pad_player_state(int player, PadState *st);
int port_pad_send(int joy, const PadState *st);
const char *port_pad_name(int joy);                   /* the name the game lists joystick `joy` by */
void port_pad_frame(unsigned long frame);             /* per frame, after the scripted controller */

/* <PORT_ENV>_PAD: a scripted controller for tests, connected at start-up, fed through the same
   calls as SDL's gamepad events: "frame:BUTTON*hold,frame:AXIS=value*hold,...", e.g.
   "900:A*10,1600:LX=-32768*60,1700:RT=32767*300". Buttons: A B X Y BACK GUIDE START LS RS LB
   RB UP DOWN LEFT RIGHT (the D-pad) MISC; axes: LX LY RX RY LT RT (back to rest after the hold).
   "frame:JOIN" connects a second controller alongside, and "frame:2.A" / "frame:P2.RT=32767*60"
   are its inputs (also connected by its first input); "JOIN3", "3.A" likewise a third, fourth.
   "frame:PLUG", "frame:UNPLUG", "frame:PLUG2" (another controller, swapped in): if the first of
   these plugs one in, there is no controller at start-up. "+N:" counts from the scripted input's
   mark (input_script_mark, a port's race start); "~N:" counts the port's film ticks instead
   (pad_film_tick: the waits while a film plays, when no frames are presented). */

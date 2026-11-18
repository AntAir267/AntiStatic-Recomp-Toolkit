/* tests/pad: runtime/win32/pad.c on its own, with a fake DirectInput that records the joysticks the
   game would see: controllers merged, plugged in late and unplugged, split screen players tied to
   a controller's identity, the scripted controller. */
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "input.h"
#include "pad.h"

int deterministic;
unsigned long frame_count;
static const char *pad_env;
const char *port_getenv(const char *name) { return !strcmp(name, "PAD") ? pad_env : NULL; }
const char *option_str(const char *key, const char *def) { (void)key; return def; }

#define J 4
static int njoy, axis[J][8], button[J][32], pov[J];
static char names[J][64];
int input_joy_attach(const char *id, const char *name, int na, int nb, int np)
{
    (void)id; (void)na; (void)nb; (void)np;
    snprintf(names[njoy], sizeof names[njoy], "%s", name);
    pov[njoy] = -1;
    return njoy++;
}
void input_joy_set_ff(int slot, int capable) { (void)slot; (void)capable; }
void input_joy_axis(int s, int a, int v) { axis[s][a] = v; }
void input_joy_button(int s, int b, int d) { button[s][b] = d; }
void input_joy_pov(int s, int p, int v) { (void)p; pov[s] = v; }
void input_joy_rumble(int s, uint16_t l, uint16_t h, uint32_t ms) { (void)s; (void)l; (void)h; (void)ms; }
static int gone[J];
void input_joy_disconnect(int s) { gone[s] = 1; }

static int fails;
static void check(int ok, const char *what)
{
    printf("%-64s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) fails++;
}

/* the scripted controller (<PORT_ENV>_PAD): a second one joining, its own inputs, split screen,
   a late plug-in of another controller */
static int script_test(void)
{
    pad_env = "5:A*2,10:JOIN,12:2.B*2,30:UNPLUG,40:PLUG2,42:X*2,50:P2.RT=32767*3";
    pad_init();
    check(pad_count() == 1, "script: one controller at start");
    int seen_a = 0, seen_b = 0, seen_x = 0, seen_rt = 0;
    for (unsigned long f = 0; f < 60; f++) {
        if (f == 11) pad_players(2);
        pad_frame(f);
        seen_a |= button[0][0];
        PadState p2;
        pad_player_state(1, &p2);
        seen_b |= PORT_PAD_JOYS >= 2 ? button[1][1] : p2.button[PAD_B] && !button[0][1];
        seen_x |= button[0][2];
        seen_rt |= PORT_PAD_JOYS >= 2 ? axis[1][INPUT_AXIS_Z] < -30000 : p2.axis[PAD_RT] > 30000;
        if (f == 20) check(pad_count() == 2, "  JOIN: a second controller connected");
        if (f == 35) check(pad_count() == 1, "  UNPLUG: the first one gone");
        if (f == 45) check(pad_count() == 2, "  PLUG2: a different controller plugged in late");
    }
    check(seen_a, "  the first controller's A reached joystick 0");
    check(seen_b, "  player 2's B reached player 2 (its joystick, or pad_player_state)");
    check(seen_x, "  the late controller drives player 1");
    check(seen_rt, "  player 2's trigger reached player 2 alone");
    printf(fails ? "pad script: FAILED\n" : "pad script: ok\n");
    return fails != 0;
}

/* a press timed by a film's ticks ("~3:A*5") whose film ends (skipped) before its release tick: let
   go at the next frame, so a later scripted A is a new press */
static int film_test(void)
{
    pad_env = "~3:A*5,6:A*2";
    pad_init();
    for (int t = 0; t < 4; t++) pad_film_tick();     /* the film: A down at its tick 3, then it ends */
    check(button[0][0] == 1, "film: A held from the film's tick 3");
    pad_frame(0);
    check(button[0][0] == 0, "  the film over: A let go at the next frame");
    int up_before = 0, pressed = 0;
    for (unsigned long f = 1; f < 10; f++) {
        pad_frame(f);
        if (f == 5) up_before = !button[0][0];
        if (f == 6) pressed = button[0][0];
    }
    check(up_before && pressed, "  the next scripted A is a new press (up, then down)");
    printf(fails ? "pad film: FAILED\n" : "pad film: ok\n");
    return fails != 0;
}

/* split screen chosen with no controller connected: the first plugged in is player 1's */
static int split_first_test(void)
{
    pad_init();
    pad_players(2);
    int a = pad_attach("guid-a/", "Pad A");
    int b = pad_attach("guid-b/", "Pad B");
    check(pad_player_of(a) == 0 && pad_player_of(b) == 1, "split first: the first controller is player 1, the next player 2");
    printf(fails ? "pad split first: FAILED\n" : "pad split first: ok\n");
    return fails != 0;
}

/* PORT_PAD_JOYS -1: one merged joystick, there while any controller is (1NSANE) */
static int merged_late_test(void)
{
    pad_init();
    check(njoy == 0, "merged on plug-in: no joystick before a controller");
    int a = pad_attach("guid-a/", "Pad A");
    check(njoy == 1 && !strcmp(names[0], "Pad A"), "  the first controller: one joystick, named after it");
    int b = pad_attach("guid-b/", "Pad B");
    check(njoy == 1, "  a second controller: still the one joystick");
    pad_axis(b, PAD_RT, 32767);
    check(axis[0][INPUT_AXIS_Z] < -30000, "  the second controller's trigger reaches it");
    pad_axis(b, PAD_RT, 0);
    pad_button(a, PAD_A, 1);
    check(button[0][0] == 1, "  the first controller's A reaches it");
    pad_button(a, PAD_A, 0);
    pad_detach(a);
    check(!gone[0], "  one unplugged, one left: the joystick stays");
    pad_axis(b, PAD_LX, -30000);
    check(axis[0][INPUT_AXIS_X] < -28000, "  the one left drives it");
    pad_detach(b);
    check(gone[0], "  the last unplugged: the joystick goes");
    int c = pad_attach("guid-c/", "Pad C");
    check(c >= 0 && njoy == 2 && !strcmp(names[1], "Pad C"), "  plugged in again: a joystick again");
    printf(fails ? "pad merged on plug-in: FAILED\n" : "pad merged on plug-in: ok\n");
    return fails != 0;
}

/* PORT_PAD_JOYS 0: a joystick per controller, made on plug-in, gone on unplug (1NSANE) */
static int per_controller_test(void)
{
    pad_init();
    check(njoy == 0, "per controller: no joystick before a controller");
    int a = pad_attach("guid-a/", "Pad A"), b = pad_attach("guid-b/", "Pad B");
    check(njoy == 2 && !strcmp(names[0], "Pad A") && !strcmp(names[1], "Pad B"),
          "  each controller its own joystick, named after it");
    pad_button(b, PAD_A, 1);
    check(button[1][0] == 1 && button[0][0] == 0, "  B's button reaches B's joystick only");
    pad_axis(a, PAD_LX, -30000);
    check(axis[0][INPUT_AXIS_X] < -28000 && axis[1][INPUT_AXIS_X] == 0, "  A's stick reaches A's joystick only");
    check(pad_rumbles(0, a) && !pad_rumbles(0, b), "  A's joystick shakes A alone");
    pad_detach(b);
    check(gone[1], "  unplugged: its joystick disconnected");
    int c = pad_attach("guid-c/", "Pad C");
    check(c >= 0 && njoy == 3 && !strcmp(names[2], "Pad C"), "  a late controller: a new joystick");
    printf(fails ? "pad per controller: FAILED\n" : "pad per controller: ok\n");
    return fails != 0;
}

int main(int argc, char **argv)
{
    if (PORT_PAD_JOYS < 0)
        return argc > 1 ? 0 : merged_late_test();
    if (PORT_PAD_JOYS == 0)
        return argc > 1 ? 0 : per_controller_test();
    if (argc > 1 && !strcmp(argv[1], "script"))
        return script_test();
    if (argc > 1 && !strcmp(argv[1], "film"))
        return film_test();
    if (argc > 1 && !strcmp(argv[1], "split"))
        return split_first_test();
    pad_init();
    check(njoy == PORT_PAD_JOYS, "the game's joysticks exist before any controller");
    check(!strcmp(names[0], "Controller (XBOX 360 For Windows)"), "  named as Windows names an Xbox controller");

    /* two controllers merged */
    int a = pad_attach("guid-a/", "Pad A"), b = pad_attach("guid-b/", "Pad B");
    pad_button(b, PAD_A, 1);
    check(button[0][0] == 1, "button A on the second controller reaches joystick 0");
    pad_axis(a, PAD_LX, 8000);
    pad_axis(b, PAD_LX, -30000);
    check(axis[0][INPUT_AXIS_X] < -28000, "the stick pushed furthest wins (B's -30000)");
    pad_axis(a, PAD_LT, 32767);
    check(axis[0][INPUT_AXIS_Z] == 32767, "triggers on Z: LT +");
    pad_axis(a, PAD_LT, 0);
    pad_axis(a, PAD_RT, 32767);
    check(axis[0][INPUT_AXIS_Z] == -32767, "  RT -");
    pad_axis(a, PAD_RT, 0);
    pad_axis(a, PAD_LY, 1000);
    pad_axis(b, PAD_LX, 0);
    pad_axis(a, PAD_LX, 0);
    check(axis[0][INPUT_AXIS_Y] == 0, "a resting drift (1000) is inside the dead zone");
    pad_button(a, PAD_DUP, 1);
    pad_button(a, PAD_DRIGHT, 1);
    check(pov[0] == 4500, "D-pad up+right is POV 45 degrees");
    pad_button(a, PAD_DUP, 0);
    pad_button(a, PAD_DRIGHT, 0);

    /* unplugging lets go of what it held */
    pad_detach(b);
    check(button[0][0] == 0, "unplugging B releases its held A");
    /* a late plug-in, the same controller back */
    b = pad_attach("guid-b/", "Pad B");
    pad_button(b, PAD_B, 1);
    check(button[0][1] == 1, "a controller plugged in late drives player 1");
    pad_button(b, PAD_B, 0);

    /* twins: two identical controllers with no serial get different identities */
    int c = pad_attach("guid-b/", "Pad B twin");
    check(c >= 0 && pad_count() == 3, "an identical twin connects as its own controller");

    /* split screen: two players */
    pad_players(2);
    check(pad_player_of(a) == 0 && pad_player_of(b) == 1 && pad_player_of(c) == 0,
          "split: first connected stays P1, next is P2, the rest P1");
#if PORT_PAD_JOYS >= 2
    pad_button(b, PAD_X, 1);
    check(button[1][2] == 1 && button[0][2] == 0, "  P2's button reaches joystick 1 only");
    pad_button(b, PAD_X, 0);
#endif
    pad_button(c, PAD_Y, 1);
    check(button[0][3] == 1, "  a third controller stays with player 1");
    pad_button(c, PAD_Y, 0);
    pad_detach(b);
    int d = pad_attach("guid-d/", "Pad D");
    check(pad_player_of(d) == 0, "  P2 unplugged: a new controller doesn't take P2's place");
    b = pad_attach("guid-b/", "Pad B");
    check(pad_player_of(b) == 1, "  P2's own controller back: it is P2 again");
    pad_players(3);
    int e = pad_attach("guid-e/", "Pad E");
    check(pad_player_of(e) == 2 || pad_player_of(c) == 2 || pad_player_of(d) == 2,
          "3 players: the third player is taken");
    pad_players(1);
    check(pad_player_of(b) == 0 && pad_player_of(e) == 0, "back to the menus: everyone is player 1");
    printf(fails ? "pad: FAILED\n" : "pad: ok\n");

    /* the scripted controller: JOIN and 2. inputs, with split on */
    return fails != 0;
}

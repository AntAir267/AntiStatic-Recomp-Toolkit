/* Controllers as DirectInput joysticks (pad.h): the mapping SDL's gamepad events go through
   (win.c), and <PORT_ENV>_PAD, a scripted controller for headless tests that goes through the
   same calls. From offroad-native's pad.c (two joysticks, players, late plug-in) and etm-native's
   (one merged joystick), made general.

   ANY CONTROLLER WORKS (GAME-GOALS). Games list their joysticks once, at start-up, and bind a
   player to one of them, so pads[] holds each real controller and the game sees PORT_PAD_JOYS
   joysticks (default 1, up to 4) that are always there, plugged in or not. Each is the
   controllers it has merged: buttons held on any, each stick from the controller pushing it
   furthest, each trigger likewise, rumble on all. So a controller works whenever it is plugged
   in, in any order, and a handheld's several devices for one controller (one handheld PC shows a
   Generic X-Box pad, a Microsoft X-Box 360 pad and a Handheld Daemon Controller, some silent)
   cannot leave the game on a silent one. Outside split screen every joystick has every
   controller; in split screen (pad_players) joystick k is player k+1's controller alone.

   THE LAYOUT is the one Windows gives an Xbox controller under DirectInput, so games that know
   it work as they did: X, Y the left stick, Z the triggers together (LT +, RT -), Rx, Ry the
   right stick, the D-pad as POV 0, buttons 0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 6 Back, 7 Start,
   8 left stick, 9 right stick, 10 Guide. A port wanting another one has port_pad_send.

   Both sticks get a round dead zone against resting drift (controller_deadzone in options.txt,
   percent, default 6), the rest of the travel stretched over the range. */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "runtime.h"
#include "input.h"
#include "options.h"
#include "pad.h"

/* the joysticks the game sees: axes and buttons (port.h; TDOR3's had 4 and 8, which its
   controls page lists) */
#ifndef PORT_PAD_AXES
#define PORT_PAD_AXES 6
#endif
#ifndef PORT_PAD_BUTTONS
#define PORT_PAD_BUTTONS 16
#endif

#ifndef PORT_PAD_JOYS
#define PORT_PAD_JOYS 1
#endif
/* PORT_PAD_JOYS 0: one joystick per controller instead, made when it is plugged in (named after
   it), gone when it is unplugged, each with only its own controller's state: the game sees what
   is plugged in, none with none (1NSANE: its --test runs and its own controller set-up for a late
   controller, padgame.c's claim_joysticks). Rumble goes to that controller alone. */
#define PER_CONTROLLER (PORT_PAD_JOYS == 0)
/* PORT_PAD_JOYS -1: ONE merged joystick that comes with the first controller plugged in and goes
   with the last one unplugged: every controller drives it (as the default's), and a session with
   no controller has no joystick at all, as the game shipped (1NSANE: the game drives a race from
   the one joystick it was set up with, so a joystick per controller left a second controller
   steering nothing; its --test runs and its own late-controller set-up need "none with none").
   Named after the controller that made it appear. */
#define MERGED_LATE (PORT_PAD_JOYS < 0)
#define GAME_JOYS (PORT_PAD_JOYS < 1 ? 1 : PORT_PAD_JOYS > PAD_MAX_PLAYERS ? PAD_MAX_PLAYERS : PORT_PAD_JOYS)

typedef struct {
    int connected;
    unsigned seq;                       /* connection order */
    char id[112];                       /* identity (GUID/serial; a twin gets "#2") */
    uint8_t button[PAD_BUTTONS];
    int axis[PAD_AXES];
    int joy;                            /* (PORT_PAD_JOYS 0) its own game joystick, -1 none */
} Pad;

static pthread_mutex_t pad_lock = PTHREAD_MUTEX_INITIALIZER;
static Pad pads[INPUT_MAX_JOYS];
static unsigned next_seq = 1;
static double stick_dead = 0.06 * 32767;
static int game_joy[PAD_MAX_PLAYERS] = {-1, -1, -1, -1};
static int nplayers = 1;
static char player_id[PAD_MAX_PLAYERS][112];   /* players 2..n: their controller's identity ("" waiting for any) */

/* SDL button -> joystick button (-1: the D-pad, the POV) */
static const int joy_button[PAD_BUTTONS] = {0, 1, 2, 3, 6, 10, 7, 8, 9, 4, 5, -1, -1, -1, -1, 11,
                                            12, 13, 14, 15, -1, -1, -1, -1, -1, -1};

static int clamp16(double v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : (int)lround(v); }

static int last_controller;             /* the device used last was a controller */
static int active_slot = -1;            /* the controller used last */
int pad_active_slot(void) { return active_slot; }
void input_used(int controller) { last_controller = controller != 0; }
int input_controller_last(void) { return last_controller; }

/* ------------------------------------------------------------------ port defaults */
__attribute__((weak)) int port_pad_send(int joy, const PadState *st) { (void)joy; (void)st; return 0; }
__attribute__((weak)) const char *port_pad_name(int joy)
{
    /* (what DirectInput calls an Xbox 360 controller; a second one the same) */
    (void)joy;
    return "Controller (XBOX 360 For Windows)";
}
__attribute__((weak)) void port_pad_frame(unsigned long frame) { (void)frame; }
__attribute__((weak)) int ifc_rumble_wanted(void) { return !deterministic; }

/* the game's joysticks exist from the start (pad_init), so it has them when it lists them */
static void game_joys_ensure(void)
{
    if (PER_CONTROLLER || MERGED_LATE)
        return;                         /* (made when a controller is plugged in) */
    for (int j = 0; j < GAME_JOYS; j++)
        if (game_joy[j] < 0) {
            char id[32];
            snprintf(id, sizeof id, j ? "controllers-%d" : "controllers", j + 1);
            game_joy[j] = input_joy_attach(id, port_pad_name(j), PORT_PAD_AXES, PORT_PAD_BUTTONS, 1);
            if (game_joy[j] >= 0)
                input_joy_set_ff(game_joy[j], ifc_rumble_wanted());    /* force feedback plays as rumble */
        }
}

/* ------------------------------------------------------------------ players (pad_lock held) */
static int player_of_locked(int s)
{
    if (s < 0 || s >= INPUT_MAX_JOYS || !pads[s].connected)
        return -1;
    for (int p = 1; p < nplayers; p++)
        if (player_id[p][0] && !strcmp(player_id[p], pads[s].id))
            return p;
    return 0;
}

/* a controller with no player yet takes the first player waiting for any controller */
static void claim_locked(int s)
{
    if (nplayers <= 1 || player_of_locked(s) != 0)
        return;
    /* player 1 first: a controller plugged in while player 1 has none is player 1's (split screen
       chosen with no controller connected gave the first one to player 2: L.A. Rush) */
    int p1 = 0;
    for (int t = 0; t < INPUT_MAX_JOYS && !p1; t++)
        p1 = t != s && pads[t].connected && player_of_locked(t) == 0;
    if (!p1)
        return;
    for (int p = 1; p < nplayers; p++)
        if (!player_id[p][0]) {
            snprintf(player_id[p], sizeof player_id[p], "%s", pads[s].id);
            LOG(LOG_INPUT, "controller %d (%s) is player %d", s, pads[s].id, p + 1);
            return;
        }
}

int pad_player_of(int slot)
{
    pthread_mutex_lock(&pad_lock);
    int p = player_of_locked(slot);
    pthread_mutex_unlock(&pad_lock);
    return p;
}

/* does controller s feed joystick j? */
static int member(int j, int s)
{
    if (!pads[s].connected)
        return 0;
    if (nplayers <= 1)
        return 1;                       /* every joystick has every controller */
    int p = player_of_locked(s);
    return j < nplayers ? p == j : p == 0;
}

/* the game joystick of player j (0-based), -1 none: XInput's rumble goes to it (xinput.c) */
int pad_game_joy(int j)
{
    return j >= 0 && j < GAME_JOYS ? game_joy[j] : -1;
}

int pad_rumbles(int joy, int slot)
{
    int r = 0;
    pthread_mutex_lock(&pad_lock);
    if (PER_CONTROLLER) {
        r = slot >= 0 && slot < INPUT_MAX_JOYS && pads[slot].connected && pads[slot].joy == joy;
        pthread_mutex_unlock(&pad_lock);
        return r;
    }
    for (int j = 0; j < GAME_JOYS; j++)
        if (game_joy[j] == joy && slot >= 0 && slot < INPUT_MAX_JOYS)
            r = member(j, slot);
    pthread_mutex_unlock(&pad_lock);
    return r;
}

/* ------------------------------------------------------------------ the joysticks' state */
/* the merged state of the controllers `in` picks (pad_lock taken here) */
static void gather(PadState *st, int (*in)(int k, int s), int k)
{
    memset(st, 0, sizeof *st);
    double best[2] = {0, 0};
    pthread_mutex_lock(&pad_lock);
    for (int s = 0; s < INPUT_MAX_JOYS; s++) {
        if (!in(k, s))
            continue;
        for (int k = 0; k < 2; k++) {   /* the stick pushed furthest, past the dead zone */
            int x = pads[s].axis[k * 2], y = pads[s].axis[k * 2 + 1];
            double r = hypot(x, y), m = r > stick_dead ? (r - stick_dead) / (32767 - stick_dead) * 32767 / r : 0;
            if (r * m > best[k]) {
                best[k] = r * m;
                st->axis[k * 2] = clamp16(x * m);
                st->axis[k * 2 + 1] = clamp16(y * m);
            }
        }
        for (int t = PAD_LT; t <= PAD_RT; t++)
            if (pads[s].axis[t] > st->axis[t])
                st->axis[t] = pads[s].axis[t];
        for (int b = 0; b < PAD_BUTTONS; b++)
            st->button[b] |= pads[s].button[b];
    }
    pthread_mutex_unlock(&pad_lock);
}

static int is_player(int p, int s) { return pads[s].connected && player_of_locked(s) == p; }

/* a player's controller(s), dead zone applied: for a player the game has no joystick for (a
   port reading player 2 itself), or a port's own menus */
int pad_player_state(int player, PadState *st)
{
    gather(st, is_player, player);
    int any = 0;
    pthread_mutex_lock(&pad_lock);
    for (int s = 0; s < INPUT_MAX_JOYS; s++)
        any |= is_player(player, s);
    pthread_mutex_unlock(&pad_lock);
    return any;
}

static void send_state(int joy, const PadState *in);

static void send_joy(int j)
{
    if (game_joy[j] < 0)
        return;
    PadState st;
    gather(&st, member, j);
    send_state(game_joy[j], &st);
}

static int only(int k, int s) { return s == k && pads[s].connected; }

static void send_state(int joy, const PadState *in)
{
    PadState st = *in;
    if (port_pad_send(joy, &st))
        return;
    input_joy_axis(joy, INPUT_AXIS_X, st.axis[PAD_LX]);
    input_joy_axis(joy, INPUT_AXIS_Y, st.axis[PAD_LY]);
    input_joy_axis(joy, INPUT_AXIS_RX, st.axis[PAD_RX]);
    input_joy_axis(joy, INPUT_AXIS_RY, st.axis[PAD_RY]);
    input_joy_axis(joy, INPUT_AXIS_Z, clamp16(st.axis[PAD_LT] - st.axis[PAD_RT]));
    for (int b = 0; b < PAD_BUTTONS; b++)
        if (joy_button[b] >= 0)
            input_joy_button(joy, joy_button[b], st.button[b]);
    int up = st.button[PAD_DUP], dn = st.button[PAD_DDOWN], l = st.button[PAD_DLEFT], r = st.button[PAD_DRIGHT];
    int v = INPUT_POV_CENTERED;
    if (up && r) v = 4500; else if (r && dn) v = 13500;
    else if (dn && l) v = 22500; else if (l && up) v = 31500;
    else if (up) v = 0; else if (r) v = 9000; else if (dn) v = 18000; else if (l) v = 27000;
    input_joy_pov(joy, 0, v);
}

static void send_all(void)
{
    if (PER_CONTROLLER) {
        for (int s = 0; s < INPUT_MAX_JOYS; s++) {
            pthread_mutex_lock(&pad_lock);
            int joy = pads[s].connected ? pads[s].joy : -1;
            pthread_mutex_unlock(&pad_lock);
            if (joy < 0)
                continue;
            PadState st;
            gather(&st, only, s);
            send_state(joy, &st);
        }
        return;
    }
    for (int j = 0; j < GAME_JOYS; j++)
        send_joy(j);
}

void pad_players(int n)
{
    n = n < 1 ? 1 : n > PAD_MAX_PLAYERS ? PAD_MAX_PLAYERS : n;
    LOG(LOG_INPUT, "players: %d (frame %lu)", n, frame_count);
    pthread_mutex_lock(&pad_lock);
    nplayers = n;
    memset(player_id, 0, sizeof player_id);
    /* the controllers connected now, in the order they were connected: the first stays
       player 1, the next take players 2..n */
    int order[INPUT_MAX_JOYS], k = 0;
    for (int s = 0; s < INPUT_MAX_JOYS; s++)
        if (pads[s].connected)
            order[k++] = s;
    for (int a = 1; a < k; a++)
        for (int b = a; b > 0 && pads[order[b]].seq < pads[order[b - 1]].seq; b--) {
            int t = order[b]; order[b] = order[b - 1]; order[b - 1] = t;
        }
    for (int i = 1; i < k; i++)
        claim_locked(order[i]);
    pthread_mutex_unlock(&pad_lock);
    send_all();
}

void pad_two_players(int on) { pad_players(on ? 2 : 1); }
int pad_player_count(void) { return nplayers; }   /* 1 outside split screen */

/* ------------------------------------------------------------------ controllers come and go */
int pad_attach(const char *id, const char *name)
{
    game_joys_ensure();
    int slot = -1;
    pthread_mutex_lock(&pad_lock);
    for (int s = 0; s < INPUT_MAX_JOYS && slot < 0; s++)
        if (!pads[s].connected)
            slot = s;
    if (slot >= 0) {
        Pad *p = &pads[slot];
        memset(p, 0, sizeof *p);
        p->joy = -1;
        p->connected = 1;
        p->seq = next_seq++;
        snprintf(p->id, sizeof p->id, "%s", id && *id ? id : "controller");
        /* two identical controllers with no serial number: the second is "#2" (and so on) */
        for (int n = 2, clash = 1; clash; n++) {
            clash = 0;
            for (int s = 0; s < INPUT_MAX_JOYS; s++)
                if (s != slot && pads[s].connected && !strcmp(pads[s].id, p->id)) {
                    snprintf(p->id, sizeof p->id, "%s#%d", id && *id ? id : "controller", n);
                    clash = 1;
                    break;
                }
        }
        claim_locked(slot);             /* split screen: a waiting player takes a new controller */
    }
    pthread_mutex_unlock(&pad_lock);
    LOG(LOG_INPUT, "controller \"%s\" connected (%d)", name ? name : "?", slot);
    if (slot < 0)
        return -1;
    if (MERGED_LATE && game_joy[0] < 0) {   /* the first controller: the merged joystick appears */
        int joy = input_joy_attach("controllers", name ? name : "Controller", PORT_PAD_AXES, PORT_PAD_BUTTONS, 1);
        if (joy >= 0)
            input_joy_set_ff(joy, ifc_rumble_wanted());
        game_joy[0] = joy;
    }
    if (PER_CONTROLLER) {               /* its own joystick, named after it */
        int joy = input_joy_attach(id && *id ? id : "controller", name ? name : "Controller",
                                   PORT_PAD_AXES, PORT_PAD_BUTTONS, 1);
        if (joy >= 0)
            input_joy_set_ff(joy, ifc_rumble_wanted());   /* force feedback plays as rumble */
        pthread_mutex_lock(&pad_lock);
        pads[slot].joy = joy;
        pthread_mutex_unlock(&pad_lock);
    }
    input_used(1);
    active_slot = slot;
    send_all();
    return slot;
}

int pad_prompts(void) { return last_controller && pad_count() > 0; }

void pad_detach(int slot)
{
    if (slot < 0 || slot >= INPUT_MAX_JOYS)
        return;
    pthread_mutex_lock(&pad_lock);
    int joy = pads[slot].connected ? pads[slot].joy : -1;
    memset(&pads[slot], 0, sizeof pads[slot]);     /* (its player, if it had one, waits for it) */
    pads[slot].joy = -1;
    pthread_mutex_unlock(&pad_lock);
    if (PER_CONTROLLER && joy >= 0)
        input_joy_disconnect(joy);      /* (the game sees it unplugged) */
    if (MERGED_LATE && game_joy[0] >= 0 && !pad_count()) {
        input_joy_disconnect(game_joy[0]);      /* the last one gone: the merged joystick too */
        game_joy[0] = -1;
    }
    if (active_slot == slot)
        active_slot = -1;
    send_all();                         /* (what the unplugged one held lets go) */
    if (!pad_count())
        input_used(0);
}

void pad_axis(int slot, int axis, int value)
{
    if (slot < 0 || slot >= INPUT_MAX_JOYS || axis < 0 || axis >= PAD_AXES)
        return;
    pthread_mutex_lock(&pad_lock);
    pads[slot].axis[axis] = value;
    pthread_mutex_unlock(&pad_lock);
    if (abs(value) > 12000) {
        input_used(1);
        active_slot = slot;
    }
    send_all();
}

void pad_button(int slot, int button, int down)
{
    if (slot < 0 || slot >= INPUT_MAX_JOYS || button < 0 || button >= PAD_BUTTONS)
        return;
    if (down) {
        input_used(1);
        active_slot = slot;
#if defined(PORT_FILM_SKIP_ANY) && PORT_FILM_SKIP_ANY
        if (!pads[slot].button[button])
            video_skip_press();       /* (a film: skipped when the game next looks) */
#endif
    }
    pthread_mutex_lock(&pad_lock);
    pads[slot].button[button] = down != 0;
    pthread_mutex_unlock(&pad_lock);
    send_all();
}

int pad_held(int button)
{
    int r = 0;
    pthread_mutex_lock(&pad_lock);
    for (int s = 0; s < INPUT_MAX_JOYS; s++)
        r |= pads[s].connected && pads[s].button[button];
    pthread_mutex_unlock(&pad_lock);
    return r;
}

int pad_axis_value(int axis)
{
    int r = 0;
    pthread_mutex_lock(&pad_lock);
    for (int s = 0; s < INPUT_MAX_JOYS; s++)
        if (pads[s].connected && abs(pads[s].axis[axis]) > abs(r))
            r = pads[s].axis[axis];
    pthread_mutex_unlock(&pad_lock);
    return r;
}

int pad_count(void)
{
    int n = 0;
    pthread_mutex_lock(&pad_lock);
    for (int s = 0; s < INPUT_MAX_JOYS; s++)
        n += pads[s].connected;
    pthread_mutex_unlock(&pad_lock);
    return n;
}

/* at exit: every controller's motors stopped */
__attribute__((weak)) void ifc_exit(void)   /* (a port's own may replace it: 1NSANE's ifc21.c) */
{
    for (int j = 0; j < GAME_JOYS; j++)
        if (game_joy[j] >= 0)
            input_joy_rumble(game_joy[j], 0, 0, 0);
    for (int s = 0; s < INPUT_MAX_JOYS; s++)
        if (pads[s].connected && pads[s].joy >= 0)
            input_joy_rumble(pads[s].joy, 0, 0, 0);
}

/* ------------------------------------------------------------------ <PORT_ENV>_PAD */
/* kind: 0 a button, 1 an axis, 2 plugging (index 0 unplug, 1 the controller, 2 another one),
   3 another controller joining (index: which, 1..3) */
typedef struct { unsigned long at, release; int kind, index, value, hold, state, who, relative, film; } PadPress;
static PadPress *script;
static int nscript, script_slot[PAD_MAX_PLAYERS] = {-1, -1, -1, -1};
static const char *const plug_names[3] = {"UNPLUG", "PLUG", "PLUG2"};
static const struct { const char *name; int button; } button_names[] = {
    {"A", PAD_A}, {"B", PAD_B}, {"X", PAD_X}, {"Y", PAD_Y}, {"BACK", PAD_BACK}, {"VIEW", PAD_BACK},
    {"SELECT", PAD_BACK}, {"GUIDE", PAD_GUIDE}, {"START", PAD_START}, {"MENU", PAD_START},
    {"LS", PAD_LSTICK}, {"L3", PAD_LSTICK}, {"RS", PAD_RSTICK}, {"R3", PAD_RSTICK},
    {"LB", PAD_LB}, {"RB", PAD_RB}, {"UP", PAD_DUP}, {"DOWN", PAD_DDOWN}, {"LEFT", PAD_DLEFT},
    {"RIGHT", PAD_DRIGHT}, {"MISC", PAD_MISC1},
};
static const char *const axis_names[PAD_AXES] = {"LX", "LY", "RX", "RY", "LT", "RT"};

void pad_options(void)
{
    const char *dz = option_str("controller_deadzone", NULL);
    double v = dz ? atof(dz) : 6;
    stick_dead = (v < 0 ? 0 : v > 90 ? 90 : v) / 100 * 32767;
}

static int script_attach(int who)
{
    static const char *const ids[PAD_MAX_PLAYERS] = {"script-pad", "script-pad-2", "script-pad-3", "script-pad-4"};
    static const char *const names[PAD_MAX_PLAYERS] = {"Scripted controller", "Second scripted controller",
                                                       "Third scripted controller", "Fourth scripted controller"};
    if (script_slot[who] < 0)
        script_slot[who] = pad_attach(ids[who], names[who]);
    return script_slot[who];
}

void pad_init(void)
{
    pad_options();
    const char *env = port_getenv("PAD");
#ifndef PORT_PAD_ALWAYS
#define PORT_PAD_ALWAYS 0   /* (port.h 1: the game's joystick is there in --test runs without a script too) */
#endif
    if (!deterministic || (env && *env) || PORT_PAD_ALWAYS)   /* (keyboard test runs stay as the original, for
                                               lockstep; an empty value is no script: LEGO Island 2's hash runs) */
        game_joys_ensure();
    if (!env || !*env)
        return;
    char *copy = strdup(env), *save = NULL;
    for (char *tok = strtok_r(copy, ", \t\n", &save); tok; tok = strtok_r(NULL, ", \t\n", &save)) {
        int relative = tok[0] == '+', film = tok[0] == '~';
        char *colon = strchr(tok, ':'), *end;
        unsigned long at = colon ? strtoul(tok + (relative || film), &end, 10) : 0;
        if (!colon || end != colon) {
            LOG(LOG_ALWAYS, "PAD: \"%s\" is not frame:BUTTON or frame:AXIS=value", tok);
            continue;
        }
        char *name = colon + 1, *star = strchr(name, '*'), *eq = strchr(name, '=');
        int hold = 3;
        if (star) {
            *star = 0;
            hold = atoi(star + 1) > 0 ? atoi(star + 1) : 1;
        }
        PadPress p = {at, 0, 0, -1, 1, hold, 0, 0, relative, film};
        if (!strncasecmp(name, "JOIN", 4)) {             /* JOIN, JOIN3, JOIN4 */
            p.kind = 3;
            p.index = name[4] >= '2' && name[4] <= '4' ? name[4] - '1' : 1;
        } else {
            if ((name[0] == 'P' || name[0] == 'p') && name[1] >= '1' && name[1] <= '4' && name[2] == '.')
                name++;
            if (name[0] >= '1' && name[0] <= '4' && name[1] == '.') {
                p.who = name[0] - '1';
                name += 2;
            }
            for (int k = 0; k < 3 && !eq; k++)
                if (!strcasecmp(name, plug_names[k])) {
                    p.kind = 2;
                    p.index = k;
                }
            if (p.kind == 2) {
            } else if (eq) {
                *eq = 0;
                p.kind = 1;
                p.value = atoi(eq + 1);
                for (int a = 0; a < PAD_AXES; a++)
                    if (!strcasecmp(name, axis_names[a]))
                        p.index = a;
            } else {
                for (size_t k = 0; k < sizeof button_names / sizeof *button_names; k++)
                    if (!strcasecmp(name, button_names[k].name))
                        p.index = button_names[k].button;
            }
        }
        if (p.index < 0) {
            LOG(LOG_ALWAYS, "PAD: unknown %s \"%s\"", eq ? "axis" : "button", name);
            continue;
        }
        script = realloc(script, (nscript + 1) * sizeof *script);
        script[nscript++] = p;
    }
    free(copy);
    int first = -1;                     /* the first plugging event: if it plugs in, none at start-up */
    for (int k = 0; k < nscript; k++)
        if (script[k].kind == 2 && (first < 0 || script[k].at < script[first].at))
            first = k;
    if (first >= 0 && script[first].index) {
        LOG(LOG_ALWAYS, "PAD: %d scripted inputs, the controller not connected yet", nscript);
        return;
    }
    script_attach(0);
    LOG(LOG_ALWAYS, "PAD: controller connected (%d), %d scripted inputs", script_slot[0], nscript);
}

static void script_plug(unsigned long frame, int which)
{
    if (script_slot[0] >= 0) {
        LOG(LOG_ALWAYS, "PAD: frame %lu: controller %d unplugged", frame, script_slot[0]);
        pad_detach(script_slot[0]);
        script_slot[0] = -1;
    }
    if (which) {
        script_slot[0] = which == 1 ? pad_attach("script-pad", "Scripted controller")
                                    : pad_attach("script-pad-other", "Another scripted controller");
        LOG(LOG_ALWAYS, "PAD: frame %lu: %s connected as controller %d", frame,
            which == 1 ? "the controller" : "another controller", script_slot[0]);
    }
}

__attribute__((weak)) long input_script_mark_frame(void);

static void script_run(unsigned long frame, int film)
{
    long mark = input_script_mark_frame ? input_script_mark_frame() : -1;
    for (int k = 0; k < nscript; k++) {
        PadPress *s = &script[k];
        int down = -1;
        if (s->film != film) {
            /* a frame presented while a film's press is held: the film ended (skipped by that very
               press, say) before the press's release tick came, so it is let go now */
            if (!film && s->film && s->state == 1) {
                s->state = 2;
                down = 0;
            } else
                continue;
        }
        int due = s->relative ? mark >= 0 && frame >= (unsigned long)mark + s->at : frame >= s->at;
        if (down >= 0)
            ;
        else if (s->state == 0 && due) {
            s->state = 1;
            s->release = frame + (unsigned long)s->hold;
            down = 1;
        } else if (s->state == 1 && frame >= s->release) {
            s->state = 2;
            down = 0;
        }
        if (down < 0)
            continue;
        if (s->kind == 3) {
            if (down && script_slot[s->index] < 0)
                LOG(LOG_ALWAYS, "PAD: frame %lu: controller %d joined (%d)", frame, s->index + 1, script_attach(s->index));
            continue;
        }
        if (s->kind == 2) {
            if (down)
                script_plug(frame, s->index);
            continue;
        }
        int slot = s->who ? script_attach(s->who) : script_slot[0];
        if (s->kind == 1) {
            LOG(LOG_ALWAYS, "PAD: frame %lu: %d.%s = %d", frame, s->who + 1, axis_names[s->index], down ? s->value : 0);
            pad_axis(slot, s->index, down ? s->value : 0);
        } else {
            const char *n = "?";
            for (size_t b = 0; b < sizeof button_names / sizeof *button_names; b++)
                if (button_names[b].button == s->index) {
                    n = button_names[b].name;
                    break;
                }
            LOG(LOG_ALWAYS, "PAD: frame %lu: %d.%s %s", frame, s->who + 1, n, down ? "down" : "up");
            pad_button(slot, s->index, down);
        }
    }
}

void pad_frame(unsigned long frame)
{
    script_run(frame, 0);
    port_pad_frame(frame);
}

/* A port's film player, once per wait while a film plays (no frames are presented then): the
   scripted controller's "~ticks:" inputs count these instead of frames */
void pad_film_tick(void)
{
    static unsigned long ticks;
    script_run(++ticks, 1);
}

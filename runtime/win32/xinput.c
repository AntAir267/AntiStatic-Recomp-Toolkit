/* xinput.c: XInput 1.3 (the program imports ordinals 2, 3, 4 = XInputGetState, XInputSetState,
   XInputGetCapabilities [verified: xinput1_3.dll's export table]) on the core's controllers
   (pad.h), by the rules in GAME-GOALS: user 0 is every connected controller merged and is always
   there, so a controller plugged in at any time works; users 1..3 are players 2..4's
   controllers once split screen has started (pad_players), absent otherwise.
   From nitronic-native (2026-11-13). NOT RUN WITH A CONTROLLER YET: checked only as far as Nitronic
   Rush's start-up calls. Weak: a port's own copy wins. XInput 1.4 and 9.1.0 by name too. */
#include "runtime.h"
#include "pad.h"
#include "input.h"

#define ERROR_DEVICE_NOT_CONNECTED 1167u

static int user_state(uint32_t user, PadState *st)
{
    if (user > 3)
        return 0;
    int connected = pad_player_state((int)user, st);
    if (user == 0 && !connected)
        memset(st, 0, sizeof *st);          /* player 1's pad is always present, at rest */
    return user == 0 || connected;
}

static int16_t stick(int v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

/* XINPUT_GAMEPAD at g: wButtons, bLeftTrigger, bRightTrigger, sThumbLX, LY, RX, RY (Y up) */
static void put_gamepad(uint32_t g, const PadState *st)
{
    static const struct { int pad; uint16_t bit; } map[] = {
        {PAD_DUP, 0x0001}, {PAD_DDOWN, 0x0002}, {PAD_DLEFT, 0x0004}, {PAD_DRIGHT, 0x0008},
        {PAD_START, 0x0010}, {PAD_BACK, 0x0020}, {PAD_LSTICK, 0x0040}, {PAD_RSTICK, 0x0080},
        {PAD_LB, 0x0100}, {PAD_RB, 0x0200}, {PAD_A, 0x1000}, {PAD_B, 0x2000}, {PAD_X, 0x4000}, {PAD_Y, 0x8000},
    };
    uint16_t b = 0;
    for (unsigned i = 0; i < sizeof map / sizeof *map; i++)
        if (st->button[map[i].pad])
            b |= map[i].bit;
    W16(g, b);
    W8(g + 2, (uint8_t)(st->axis[PAD_LT] >> 7));
    W8(g + 3, (uint8_t)(st->axis[PAD_RT] >> 7));
    W16(g + 4, (uint16_t)stick(st->axis[PAD_LX]));
    W16(g + 6, (uint16_t)stick(-st->axis[PAD_LY]));
    W16(g + 8, (uint16_t)stick(st->axis[PAD_RX]));
    W16(g + 10, (uint16_t)stick(-st->axis[PAD_RY]));
}

/* XInputGetState(user, XINPUT_STATE*): dwPacketNumber changes when the state does */
API_WEAK(XINPUT1_3, 2, 2)
{
    static PadState last[4];
    static uint32_t packet[4];
    uint32_t user = ARG(0), out = ARG(1);
    PadState st;
    if (!user_state(user, &st))
        return ERROR_DEVICE_NOT_CONNECTED;
    if (memcmp(&st, &last[user], sizeof st)) {
        last[user] = st;
        packet[user]++;
    }
    W32(out, packet[user]);
    put_gamepad(out + 4, &st);
    return 0;
}

/* XInputSetState(user, XINPUT_VIBRATION* {left = low-frequency motor, right = high}): the motors
   stay at a speed until it is set again [guess: input_joy_rumble's duration restarts on each
   call, so a long one stands in for "until changed"] */
API_WEAK(XINPUT1_3, 3, 2)
{
    uint32_t user = ARG(0), v = ARG(1);
    PadState st;
    if (!user_state(user, &st))
        return ERROR_DEVICE_NOT_CONNECTED;
    uint16_t lo = R16(v), hi = R16(v + 2);
    LOG(LOG_INPUT, "XInputSetState(%u): low %u high %u", user, lo, hi);
    int joy = pad_game_joy((int)user);     /* (the player's game joystick: it shakes that player's controllers) */
    if (joy >= 0)
        input_joy_rumble(joy, lo, hi, lo || hi ? 60000 : 0);
    return 0;
}

/* XInputGetCapabilities(user, flags, XINPUT_CAPABILITIES*): a gamepad with every control and
   both motors */
API_WEAK(XINPUT1_3, 4, 3)
{
    uint32_t user = ARG(0), out = ARG(2);
    PadState st;
    if (!user_state(user, &st))
        return ERROR_DEVICE_NOT_CONNECTED;
    W8(out, 1);                             /* XINPUT_DEVTYPE_GAMEPAD */
    W8(out + 1, 1);                         /* XINPUT_DEVSUBTYPE_GAMEPAD */
    W16(out + 2, 0);
    W16(out + 4, 0xf3ff); W8(out + 6, 0xff); W8(out + 7, 0xff);
    W16(out + 8, 0xffff); W16(out + 10, 0xffff); W16(out + 12, 0xffff); W16(out + 14, 0xffff);
    W16(out + 16, 0xffff); W16(out + 18, 0xffff);
    return 0;
}

/* the same functions by name, and XInput 1.4 / 9.1.0 (same ordinals and layouts) */
#define XI_ALIAS(dll, name, ord) void w_XINPUT1_3_##ord(CPU *c); __attribute__((weak)) void w_##dll##_##name(CPU *c) { w_XINPUT1_3_##ord(c); }
XI_ALIAS(XINPUT1_3, XInputGetState, 2) XI_ALIAS(XINPUT1_3, XInputSetState, 3) XI_ALIAS(XINPUT1_3, XInputGetCapabilities, 4)
XI_ALIAS(XINPUT1_4, XInputGetState, 2) XI_ALIAS(XINPUT1_4, XInputSetState, 3) XI_ALIAS(XINPUT1_4, XInputGetCapabilities, 4)
XI_ALIAS(XINPUT1_4, 2, 2) XI_ALIAS(XINPUT1_4, 3, 3) XI_ALIAS(XINPUT1_4, 4, 4)
XI_ALIAS(XINPUT9_1_0, XInputGetState, 2) XI_ALIAS(XINPUT9_1_0, XInputSetState, 3) XI_ALIAS(XINPUT9_1_0, XInputGetCapabilities, 4)

// Game pads on Windows: SDL3's gamepad support (Xbox, PlayStation, Switch, and the many others SDL knows), loaded
// at run time from SDL3.dll beside the exe (build_windows.py builds it; HOTDVR.exe can't link SDL itself: its own
// host_sdl.c is the SDL the game sees). Without SDL3.dll, XInput's Xbox pads. Every pad connected is read, each a
// device of its own (flat.h's players: alone, each is player 1's; Start on another pad than player 1's brings in
// player 2):
//  - a sight moved by either stick (or the pad's gyro, the GAME tab's Gyro aim: DualSense, DualShock 4, Switch Pro),
//    the right trigger or R1 shooting, the left trigger, L1 or X / Square reloading, A / Cross or Start for coins and
//    Start, Back / Select / Share for the menu;
//  - in the menu: the stick or the D-pad moves, A selects, B goes back, the bumpers switch tabs.
// The game's own crosshair shows where each pad's sight is (player 1 red, player 2 blue).
//
// FLAT_TEST_PAD=n (tests): n virtual SDL gamepads (1..4). FLAT_TEST_PAD_SCRIPT=steps drives them (else, with one
// pad: the right stick pushed right for half a second at 3 s, the right trigger at 5 s, A at 6 s). A step is
// WHO@SECONDS:WHAT, comma-separated; WHO is a pad's number (1..n) or K (the keyboard and mouse); a pad's WHAT is A,
// START, RT, LT, BACK (pressed for 0.2 s), RX+ RX- RY+ RY- LX+ LX- LY+ LY- (that stick pushed for 0.3 s; RX+40: 40%
// of the way), OFF or ON
// (unplugged, plugged in again); the keyboard's is SPACE (Space pressed), or X;Y (the mouse to that point of the
// picture, 0..1 each, and its left button clicked). Each pad's sight is logged a second after each step.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "flat/flat.h"
#include "platform/platform.h"
#include "quest_options.h"

#define LOGI(...) plat_log(PLAT_LOG_INFO, __VA_ARGS__)

// ---- the part of SDL3's API used here (SDL3/SDL_gamepad.h's values), by name from SDL3.dll
typedef uint32_t SDLJoystickID;
typedef struct SDLGamepad SDLGamepad;
enum { SDL_INIT_GAMEPAD_ = 0x00002000u, SDL_INIT_JOYSTICK_ = 0x00000200u };
enum { AX_LEFTX, AX_LEFTY, AX_RIGHTX, AX_RIGHTY, AX_LTRIGGER, AX_RTRIGGER };
enum { B_SOUTH, B_EAST, B_WEST, B_NORTH, B_BACK, B_GUIDE, B_START, B_LSTICK, B_RSTICK, B_LSHOULDER, B_RSHOULDER,
       B_UP, B_DOWN, B_LEFT, B_RIGHT, B_MISC1 };
enum { SENSOR_GYRO = 2 };
typedef struct {
    uint32_t version;
    uint16_t type, padding, vendor_id, product_id, naxes, nbuttons, nballs, nhats, ntouchpads, nsensors, padding2[2];
    uint32_t button_mask, axis_mask;
    const char *name;
    const void *touchpads, *sensors;
    void *userdata, *Update, *SetPlayerIndex, *Rumble, *RumbleTriggers, *SetLED, *SendEffect, *SetSensorsEnabled, *Cleanup;
} VirtualDesc;   // SDL_VirtualJoystickDesc (SDL 3.2+)

static struct {
    int tried, on;
    bool (*Init)(uint32_t);
    bool (*SetHint)(const char *, const char *);
    void (*UpdateGamepads)(void);
    SDLJoystickID *(*GetGamepads)(int *);
    SDLGamepad *(*OpenGamepad)(SDLJoystickID);
    void (*CloseGamepad)(SDLGamepad *);
    bool (*GamepadConnected)(SDLGamepad *);
    const char *(*GetGamepadName)(SDLGamepad *);
    int16_t (*GetGamepadAxis)(SDLGamepad *, int);
    bool (*GetGamepadButton)(SDLGamepad *, int);
    bool (*GamepadHasSensor)(SDLGamepad *, int);
    bool (*SetGamepadSensorEnabled)(SDLGamepad *, int, bool);
    bool (*GetGamepadSensorData)(SDLGamepad *, int, float *, int);
    void (*free_)(void *);
    // tests: virtual pads
    SDLJoystickID (*AttachVirtualJoystick)(const VirtualDesc *);
    bool (*DetachVirtualJoystick)(SDLJoystickID);
    void *(*OpenJoystick)(SDLJoystickID);
    void (*CloseJoystick)(void *);
    bool (*SetJoystickVirtualAxis)(void *, int, int16_t);
    bool (*SetJoystickVirtualButton)(void *, int, bool);
} S;

static int load_sdl(void) {
    HMODULE m = LoadLibraryW(L"SDL3.dll");
    if (!m) return 0;
#define GET(field, name) if (!(*(FARPROC *)&S.field = GetProcAddress(m, name))) return 0;
    GET(Init, "SDL_Init") GET(SetHint, "SDL_SetHint") GET(UpdateGamepads, "SDL_UpdateGamepads") GET(GetGamepads, "SDL_GetGamepads")
    GET(OpenGamepad, "SDL_OpenGamepad") GET(CloseGamepad, "SDL_CloseGamepad") GET(GamepadConnected, "SDL_GamepadConnected")
    GET(GetGamepadName, "SDL_GetGamepadName") GET(GetGamepadAxis, "SDL_GetGamepadAxis") GET(GetGamepadButton, "SDL_GetGamepadButton")
    GET(GamepadHasSensor, "SDL_GamepadHasSensor") GET(SetGamepadSensorEnabled, "SDL_SetGamepadSensorEnabled")
    GET(GetGamepadSensorData, "SDL_GetGamepadSensorData") GET(free_, "SDL_free")
    GET(AttachVirtualJoystick, "SDL_AttachVirtualJoystick") GET(DetachVirtualJoystick, "SDL_DetachVirtualJoystick")
    GET(OpenJoystick, "SDL_OpenJoystick") GET(CloseJoystick, "SDL_CloseJoystick")
    GET(SetJoystickVirtualAxis, "SDL_SetJoystickVirtualAxis") GET(SetJoystickVirtualButton, "SDL_SetJoystickVirtualButton")
#undef GET
    S.SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");   // (read every frame by the app, focused or not)
    if (!S.Init(SDL_INIT_GAMEPAD_ | SDL_INIT_JOYSTICK_)) return 0;
    return 1;
}

// ---- XInput, without SDL3.dll
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XGamepad;
typedef struct { DWORD dwPacketNumber; XGamepad Gamepad; } XState;
typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD, XState *);
static PFN_XInputGetState xinput;

// one frame of a pad, in SDL's terms (sticks -1..1, y up; triggers 0..1)
typedef struct { float lx, ly, rx, ry, lt, rt; int b[16]; } Pad;

// the pads connected: SDL's (gp) or XInput's (its slot)
#define MAX_PADS 8
static struct Conn {
    int used;
    SDLGamepad *gp;
    SDLJoystickID jid;
    int xslot;
    int dev;                     // flat.h's device id
    int gyro;
    int prev[16], fire_was, reload_was, active;
    float sx, sy;                // its sight, window pixels (-1: not yet: from the middle)
    double menu_repeat;
} C[MAX_PADS];

static struct Conn *conn_add(void) {
    for (int i = 0; i < MAX_PADS; i++)
        if (!C[i].used) {
            memset(&C[i], 0, sizeof C[i]);
            C[i].used = 1;
            C[i].xslot = -1;
            C[i].sx = -1;
            C[i].dev = flat_pad_attach();
            return &C[i];
        }
    return NULL;
}
static void conn_gone(struct Conn *c) {
    flat_pad_detach(c->dev);
    if (c->gp) S.CloseGamepad(c->gp);
    c->used = 0;
}

static void scan_sdl(double now) {
    static double next;
    S.UpdateGamepads();
    for (int i = 0; i < MAX_PADS; i++)
        if (C[i].used && C[i].gp && !S.GamepadConnected(C[i].gp)) { LOGI("pad: gone"); conn_gone(&C[i]); }
    if (now < next) return;
    next = now + 0.5;
    int n = 0;
    SDLJoystickID *ids = S.GetGamepads(&n);
    for (int k = 0; ids && k < n; k++) {
        int have = 0;
        for (int i = 0; i < MAX_PADS; i++) if (C[i].used && C[i].gp && C[i].jid == ids[k]) have = 1;
        if (have) continue;
        SDLGamepad *gp = S.OpenGamepad(ids[k]);
        if (!gp) continue;
        struct Conn *c = conn_add();
        if (!c) { S.CloseGamepad(gp); break; }
        c->gp = gp; c->jid = ids[k];
        c->gyro = S.GamepadHasSensor(gp, SENSOR_GYRO) && S.SetGamepadSensorEnabled(gp, SENSOR_GYRO, true);
        LOGI("pad: %s%s", S.GetGamepadName(gp) ? S.GetGamepadName(gp) : "a game pad", c->gyro ? " (gyro)" : "");
    }
    if (ids) S.free_(ids);
}
static void read_sdl(struct Conn *c, Pad *p) {
    SDLGamepad *pad = c->gp;
    p->lx = S.GetGamepadAxis(pad, AX_LEFTX) / 32767.0f;
    p->ly = -S.GetGamepadAxis(pad, AX_LEFTY) / 32767.0f;
    p->rx = S.GetGamepadAxis(pad, AX_RIGHTX) / 32767.0f;
    p->ry = -S.GetGamepadAxis(pad, AX_RIGHTY) / 32767.0f;
    p->lt = S.GetGamepadAxis(pad, AX_LTRIGGER) / 32767.0f;
    p->rt = S.GetGamepadAxis(pad, AX_RTRIGGER) / 32767.0f;
    for (int i = 0; i < 16; i++) p->b[i] = S.GetGamepadButton(pad, i);
}

static void scan_xinput(double now) {
    static double next;
    if (!xinput) {
        if (now < next) return;
        next = now + 1e9;   // (tried once)
        HMODULE m = LoadLibraryW(L"xinput1_4.dll");
        if (!m) m = LoadLibraryW(L"xinput9_1_0.dll");
        if (m) xinput = (PFN_XInputGetState)GetProcAddress(m, "XInputGetState");
        if (!xinput) return;
        next = 0;
    }
    if (now < next) return;
    next = now + 1;   // (polling an empty slot is slow: once a second)
    for (int s = 0; s < 4; s++) {
        int have = 0;
        for (int i = 0; i < MAX_PADS; i++) if (C[i].used && C[i].xslot == s) have = 1;
        XState st;
        if (have || xinput((DWORD)s, &st) != ERROR_SUCCESS) continue;
        struct Conn *c = conn_add();
        if (!c) break;
        c->xslot = s;
        LOGI("pad: XInput pad %d", s);
    }
}
static int read_xinput(struct Conn *c, Pad *p) {
    XState st;
    if (xinput((DWORD)c->xslot, &st) != ERROR_SUCCESS) { LOGI("pad: XInput pad %d gone", c->xslot); conn_gone(c); return 0; }
    const XGamepad *g = &st.Gamepad;
    p->lx = g->sThumbLX / 32767.0f; p->ly = g->sThumbLY / 32767.0f;
    p->rx = g->sThumbRX / 32767.0f; p->ry = g->sThumbRY / 32767.0f;
    p->lt = g->bLeftTrigger / 255.0f; p->rt = g->bRightTrigger / 255.0f;
    const WORD w = g->wButtons;
    p->b[B_SOUTH] = !!(w & 0x1000); p->b[B_EAST] = !!(w & 0x2000); p->b[B_WEST] = !!(w & 0x4000); p->b[B_NORTH] = !!(w & 0x8000);
    p->b[B_BACK] = !!(w & 0x0020); p->b[B_START] = !!(w & 0x0010); p->b[B_LSHOULDER] = !!(w & 0x0100); p->b[B_RSHOULDER] = !!(w & 0x0200);
    p->b[B_UP] = !!(w & 0x0001); p->b[B_DOWN] = !!(w & 0x0002); p->b[B_LEFT] = !!(w & 0x0004); p->b[B_RIGHT] = !!(w & 0x0008);
    return 1;
}

// ---- tests: scripted virtual pads (and the keyboard and mouse)
static struct {
    int n;
    SDLJoystickID id[4];
    void *joy[4];
    double start;
    struct Step { int who; double at; char what[16]; } step[96];
    int nsteps;
    int legacy;   // FLAT_TEST_PAD=1 without a script: the old one
} T = { .n = -1 };
static void test_attach(int k) {
    VirtualDesc d;
    memset(&d, 0, sizeof d);
    d.version = sizeof d;
    d.type = 1;   // SDL_JOYSTICK_TYPE_GAMEPAD: a standard mapping
    d.naxes = 6;
    d.nbuttons = 15;
    static char name[4][16];
    snprintf(name[k], sizeof name[k], "test pad %d", k + 1);
    d.name = name[k];
    d.button_mask = (1u << 15) - 1;   // SOUTH .. DPAD_RIGHT
    d.axis_mask = 0x3f;               // both sticks, both triggers
    T.id[k] = S.AttachVirtualJoystick(&d);
    T.joy[k] = T.id[k] ? S.OpenJoystick(T.id[k]) : NULL;
    LOGI("test: virtual pad %d %s", k + 1, T.joy[k] ? "attached" : "failed");
}
static void test_detach(int k) {
    if (!T.joy[k]) return;
    S.CloseJoystick(T.joy[k]);
    S.DetachVirtualJoystick(T.id[k]);
    T.joy[k] = NULL;
    LOGI("test: virtual pad %d unplugged", k + 1);
}
static const char *const stick_names[] = { "LX", "LY", "RX", "RY" };
static void test_pad(double now) {
    if (T.n == -1) {
        const char *e = getenv("FLAT_TEST_PAD"), *sc = getenv("FLAT_TEST_PAD_SCRIPT");
        T.n = e ? atoi(e) : 0;
        if (T.n > 4) T.n = 4;
        if (T.n <= 0 || !S.on) { T.n = 0; return; }
        for (int k = 0; k < T.n; k++) test_attach(k);
        T.start = now;
        T.legacy = !(sc && *sc);
        for (const char *p = sc; p && *p && T.nsteps < 96;) {
            struct Step *s = &T.step[T.nsteps];
            char who[4] = "";
            int used = 0;
            if (sscanf(p, "%3[^@]@%lf:%15[^,]%n", who, &s->at, s->what, &used) >= 3) {
                s->who = who[0] == 'K' || who[0] == 'k' ? 0 : atoi(who);
                T.nsteps++;
            }
            p = strchr(p, ',');
            if (p) p++;
        }
        LOGI("test: %d virtual pads, %d steps", T.n, T.nsteps);
        return;
    }
    if (!T.n) return;
    const double t = now - T.start;
    if (T.legacy && T.joy[0]) {   // 3 s: right stick right for 0.5 s; 5 s: the right trigger; 6 s: A (coins, Start)
        S.SetJoystickVirtualAxis(T.joy[0], AX_RIGHTX, t >= 3 && t < 3.5 ? 32767 : 0);
        S.SetJoystickVirtualAxis(T.joy[0], AX_RTRIGGER, t >= 5 && t < 5.2 ? 32767 : -32768);
        S.SetJoystickVirtualAxis(T.joy[0], AX_LTRIGGER, -32768);
        S.SetJoystickVirtualButton(T.joy[0], B_SOUTH, t >= 6 && t < 6.2);
        static int told[2];
        if (t >= 2.9 && !told[0]) { told[0] = 1; LOGI("test: sight before the stick at %.0f,%.0f", C[0].sx, C[0].sy); }
        if (t >= 3.8 && !told[1]) { told[1] = 1; LOGI("test: sight after the stick at %.0f,%.0f", C[0].sx, C[0].sy); }
        return;
    }
    // each pad's state from the steps whose time covers now
    for (int k = 0; k < T.n; k++) {
        if (!T.joy[k]) continue;
        int16_t ax[6] = { 0, 0, 0, 0, -32768, -32768 };
        int b[15] = { 0 };
        for (int i = 0; i < T.nsteps; i++) {
            const struct Step *s = &T.step[i];
            if (s->who != k + 1) continue;
            const double d = t - s->at;
            if (d < 0) continue;
            if (d < 0.2) {
                if (!strcmp(s->what, "A")) b[B_SOUTH] = 1;
                else if (!strcmp(s->what, "START")) b[B_START] = 1;
                else if (!strcmp(s->what, "BACK")) b[B_BACK] = 1;
                else if (!strcmp(s->what, "RT")) ax[AX_RTRIGGER] = 32767;
                else if (!strcmp(s->what, "LT")) ax[AX_LTRIGGER] = 32767;
            }
            if (d < 0.3)
                for (int a = 0; a < 4; a++)
                    if (!strncmp(s->what, stick_names[a], 2) && (s->what[2] == '+' || s->what[2] == '-')) {
                        const int pct = s->what[3] ? atoi(s->what + 3) : 100;   // (RX+40: pushed 40%)
                        ax[a] = (int16_t)((s->what[2] == '+' ? 32767 : -32767) * pct / 100 * (a == AX_LEFTY || a == AX_RIGHTY ? -1 : 1));
                    }
        }
        for (int a = 0; a < 6; a++) S.SetJoystickVirtualAxis(T.joy[k], a, ax[a]);
        for (int i = 0; i < 15; i++) S.SetJoystickVirtualButton(T.joy[k], i, b[i] != 0);
    }
    // the steps that happen once: unplugging, the keyboard and mouse, the log a second after
    static int done[96], logged[96];
    for (int i = 0; i < T.nsteps; i++) {
        const struct Step *s = &T.step[i];
        if (!done[i] && t >= s->at) {
            done[i] = 1;
            if (s->who) LOGI("test: step %d@%.1f:%s", s->who, s->at, s->what);
            else LOGI("test: step K@%.1f:%s", s->at, s->what);
            if (s->who >= 1 && s->who <= T.n) {
                if (!strcmp(s->what, "OFF")) test_detach(s->who - 1);
                else if (!strcmp(s->what, "ON") && !T.joy[s->who - 1]) test_attach(s->who - 1);
            } else if (!s->who) {
                if (!strcmp(s->what, "SPACE")) { flat_dev_start(FLAT_DEV_KBM, 1); flat_dev_start(FLAT_DEV_KBM, 0); }
                else {
                    float fx, fy;
                    int vx, vy, vw, vh;
                    flat_view_rect(&vx, &vy, &vw, &vh);
                    if (sscanf(s->what, "%f;%f", &fx, &fy) == 2 && vw > 0) {
                        flat_dev_pointer(FLAT_DEV_KBM, vx + (int)(fx * vw), vy + (int)(fy * vh));
                        flat_dev_trigger(FLAT_DEV_KBM, 1);
                        flat_dev_trigger(FLAT_DEV_KBM, 0);
                    }
                }
            }
        }
        if (!logged[i] && t >= s->at + 1) {
            logged[i] = 1;
            char line[256];
            int n = 0;
            for (int c = 0; c < MAX_PADS; c++)
                if (C[c].used)
                    n += snprintf(line + n, sizeof line - (size_t)n, " [pad %d: player %d, sight %.0f,%.0f]", C[c].dev - FLAT_DEV_PAD + 1,
                                  flat_dev_player(C[c].dev) + 1, C[c].sx, C[c].sy);
            LOGI("test: after %.1f s:%s; player 2 %s", s->at, line, flat_player2_in() ? "in" : "out");
        }
    }
}

// ---- every frame
static float dz(float v) { const float d = 0.22f; if (fabsf(v) < d) return 0; const float g = (fabsf(v) - d) / (1 - d); return (v < 0 ? -g : g) * g; }

static void pad_frame(struct Conn *c, const Pad *p, double now, double dt) {
    int pressed[16];
    for (int i = 0; i < 16; i++) { pressed[i] = p->b[i] && !c->prev[i]; c->prev[i] = p->b[i]; }
    const float mx = fabsf(dz(p->rx)) > fabsf(dz(p->lx)) ? dz(p->rx) : dz(p->lx), my = fabsf(dz(p->ry)) > fabsf(dz(p->ly)) ? dz(p->ry) : dz(p->ly);
    if (flat_menu_is_open()) {
        if (pressed[B_SOUTH]) flat_menu_key(FLAT_KEY_ENTER);
        if (pressed[B_EAST] || pressed[B_BACK]) flat_menu_key(FLAT_KEY_BACK);
        if (pressed[B_RSHOULDER]) flat_menu_key(FLAT_KEY_TAB);
        if (pressed[B_LSHOULDER]) flat_menu_key(FLAT_KEY_TAB_BACK);
        int dir = -1;
        if (p->b[B_UP] || my > 0.5f) dir = FLAT_KEY_UP;
        else if (p->b[B_DOWN] || my < -0.5f) dir = FLAT_KEY_DOWN;
        else if (p->b[B_LEFT] || mx < -0.5f) dir = FLAT_KEY_LEFT;
        else if (p->b[B_RIGHT] || mx > 0.5f) dir = FLAT_KEY_RIGHT;
        if (dir < 0) c->menu_repeat = 0;
        else if (now >= c->menu_repeat) { flat_menu_key(dir); c->menu_repeat = now + (c->menu_repeat ? 0.12 : 0.35); }
        if (c->fire_was) flat_dev_trigger(c->dev, 0);
        if (c->reload_was) flat_dev_reload(c->dev, 0);
        c->fire_was = c->reload_was = 0;
        return;
    }
    if (pressed[B_BACK]) { flat_menu_set_open(1); return; }
    if (pressed[B_SOUTH] || pressed[B_START]) { flat_dev_start(c->dev, 1); flat_dev_start(c->dev, 0); }
    int vx, vy, vw, vh;
    flat_view_rect(&vx, &vy, &vw, &vh);
    if (vw <= 0 || vh <= 0) return;
    if (c->sx < 0) { c->sx = vx + vw * 0.5f; c->sy = vy + vh * 0.5f; }
    // the sight: a sixth of the picture a second at a light push, two at a full one; and the pad's gyro (Gyro aim)
    if (mx || my) {
        c->active = 1;
        c->sx += mx * (float)vw * 2.0f * (float)dt;
        c->sy -= my * (float)vw * 2.0f * (float)dt;
    }
    if (c->gyro && c->gp && flat_opt_get(QOPT_GYRO)) {
        float g[3] = { 0, 0, 0 };   // rad/s: x pitch (up +), y yaw (left +), z roll
        if (S.GetGamepadSensorData(c->gp, SENSOR_GYRO, g, 3) && (fabsf(g[0]) > 0.01f || fabsf(g[1]) > 0.01f)) {
            const float k = (float)vw / 0.7f * (float)flat_opt_get(QOPT_GYRO_SPEED) / 100.0f;
            c->sx -= g[1] * k * (float)dt;
            c->sy -= g[0] * k * (float)dt;
            c->active = 1;
        }
    }
    // (a little past the picture's edges: aiming off it reloads, as on the cabinet)
    if (c->sx < vx - vw * 0.05f) c->sx = vx - vw * 0.05f;
    if (c->sx > vx + vw * 1.05f) c->sx = vx + vw * 1.05f;
    if (c->sy < vy - vh * 0.05f) c->sy = vy - vh * 0.05f;
    if (c->sy > vy + vh * 1.05f) c->sy = vy + vh * 1.05f;
    if (c->active) flat_dev_pointer(c->dev, (int)c->sx, (int)c->sy);
    const int fire = p->rt > 0.4f || p->b[B_RSHOULDER], reload = p->lt > 0.4f || p->b[B_LSHOULDER] || p->b[B_WEST];
    if (fire != c->fire_was) { c->active = 1; flat_dev_pointer(c->dev, (int)c->sx, (int)c->sy); flat_dev_trigger(c->dev, fire); c->fire_was = fire; }
    if (reload != c->reload_was) { flat_dev_reload(c->dev, reload); c->reload_was = reload; }
}

void pad_poll(double now, double dt) {
    if (!S.tried) { S.tried = 1; S.on = load_sdl(); LOGI("pad: %s", S.on ? "SDL3 gamepads" : "XInput (no SDL3.dll)"); }
    test_pad(now);
    if (S.on) scan_sdl(now); else scan_xinput(now);
    for (int i = 0; i < MAX_PADS; i++) {
        struct Conn *c = &C[i];
        if (!c->used) continue;
        Pad p;
        memset(&p, 0, sizeof p);
        if (c->gp) read_sdl(c, &p);
        else if (!read_xinput(c, &p)) continue;
        pad_frame(c, &p, now, dt);
    }
}

// the mouse aims again: the sights of the pads that drive the mouse's player start in the middle next time
void pad_mouse_moved(void) {
    for (int i = 0; i < MAX_PADS; i++)
        if (C[i].used && flat_dev_player(C[i].dev) == flat_dev_player(FLAT_DEV_KBM)) { C[i].active = 0; C[i].sx = -1; }
}

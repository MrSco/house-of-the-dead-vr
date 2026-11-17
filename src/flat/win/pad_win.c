// Game pads on Windows: SDL3's gamepad support (Xbox, PlayStation, Switch, and the many others SDL knows), loaded
// at run time from SDL3.dll beside the exe (build_windows.py builds it; HOTDVR.exe can't link SDL itself: its own
// host_sdl.c is the SDL the game sees). Without SDL3.dll, XInput's Xbox pads.
//  - a sight moved by either stick (or the pad's gyro, the GAME tab's Gyro aim: DualSense, DualShock 4, Switch Pro),
//    the right trigger or R1 shooting, the left trigger, L1 or X / Square reloading, A / Cross or Start for coins and
//    Start, Back / Select / Share for the menu;
//  - in the menu: the stick or the D-pad moves, A selects, B goes back, the bumpers switch tabs.
// The game's own crosshair shows where the sight is.
//
// FLAT_TEST_PAD=1 (tests): a virtual SDL gamepad driven by script: the right stick pushed right for half a second,
// then a pull of the right trigger, then A.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdbool.h>
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
    // tests: a virtual pad
    SDLJoystickID (*AttachVirtualJoystick)(const VirtualDesc *);
    void *(*OpenJoystick)(SDLJoystickID);
    bool (*SetJoystickVirtualAxis)(void *, int, int16_t);
    bool (*SetJoystickVirtualButton)(void *, int, bool);
} S;
static SDLGamepad *pad;
static int pad_gyro;

static int load_sdl(void) {
    HMODULE m = LoadLibraryW(L"SDL3.dll");
    if (!m) return 0;
#define GET(field, name) if (!(*(FARPROC *)&S.field = GetProcAddress(m, name))) return 0;
    GET(Init, "SDL_Init") GET(SetHint, "SDL_SetHint") GET(UpdateGamepads, "SDL_UpdateGamepads") GET(GetGamepads, "SDL_GetGamepads")
    GET(OpenGamepad, "SDL_OpenGamepad") GET(CloseGamepad, "SDL_CloseGamepad") GET(GamepadConnected, "SDL_GamepadConnected")
    GET(GetGamepadName, "SDL_GetGamepadName") GET(GetGamepadAxis, "SDL_GetGamepadAxis") GET(GetGamepadButton, "SDL_GetGamepadButton")
    GET(GamepadHasSensor, "SDL_GamepadHasSensor") GET(SetGamepadSensorEnabled, "SDL_SetGamepadSensorEnabled")
    GET(GetGamepadSensorData, "SDL_GetGamepadSensorData") GET(free_, "SDL_free")
    GET(AttachVirtualJoystick, "SDL_AttachVirtualJoystick") GET(OpenJoystick, "SDL_OpenJoystick")
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
static int xslot = -1;
static double xnext_scan;

// one frame of a pad, in SDL's terms (sticks -1..1, y up; triggers 0..1)
typedef struct { float lx, ly, rx, ry, lt, rt; int b[16]; } Pad;

static int read_sdl(Pad *p, double now) {
    S.UpdateGamepads();
    if (pad && !S.GamepadConnected(pad)) { S.CloseGamepad(pad); pad = NULL; LOGI("pad: gone"); }
    if (!pad) {
        static double next;
        if (now < next) return 0;
        next = now + 0.5;
        int n = 0;
        SDLJoystickID *ids = S.GetGamepads(&n);
        if (ids && n > 0) pad = S.OpenGamepad(ids[0]);
        if (ids) S.free_(ids);
        if (!pad) return 0;
        pad_gyro = S.GamepadHasSensor(pad, SENSOR_GYRO) && S.SetGamepadSensorEnabled(pad, SENSOR_GYRO, true);
        LOGI("pad: %s%s", S.GetGamepadName(pad) ? S.GetGamepadName(pad) : "a game pad", pad_gyro ? " (gyro)" : "");
    }
    p->lx = S.GetGamepadAxis(pad, AX_LEFTX) / 32767.0f;
    p->ly = -S.GetGamepadAxis(pad, AX_LEFTY) / 32767.0f;
    p->rx = S.GetGamepadAxis(pad, AX_RIGHTX) / 32767.0f;
    p->ry = -S.GetGamepadAxis(pad, AX_RIGHTY) / 32767.0f;
    p->lt = S.GetGamepadAxis(pad, AX_LTRIGGER) / 32767.0f;
    p->rt = S.GetGamepadAxis(pad, AX_RTRIGGER) / 32767.0f;
    for (int i = 0; i < 16; i++) p->b[i] = S.GetGamepadButton(pad, i);
    return 1;
}
static int read_xinput(Pad *p, double now) {
    if (!xinput) {
        HMODULE m = LoadLibraryW(L"xinput1_4.dll");
        if (!m) m = LoadLibraryW(L"xinput9_1_0.dll");
        if (m) xinput = (PFN_XInputGetState)GetProcAddress(m, "XInputGetState");
        if (!xinput) return 0;
    }
    XState st;
    if (xslot < 0) {
        if (now < xnext_scan) return 0;
        xnext_scan = now + 1;   // (polling an empty slot is slow: once a second)
        for (int i = 0; i < 4 && xslot < 0; i++) if (xinput((DWORD)i, &st) == ERROR_SUCCESS) xslot = i;
        if (xslot < 0) return 0;
        LOGI("pad: XInput pad %d", xslot);
    }
    if (xinput((DWORD)xslot, &st) != ERROR_SUCCESS) { xslot = -1; return 0; }
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

static float sx = -1, sy = -1;   // the sight, window pixels

// ---- tests: a scripted virtual pad
static void *vjoy;
static double vstart;
static void test_pad(double now) {
    static int state = -1;
    if (state == -1) {
        const char *e = getenv("FLAT_TEST_PAD");
        state = e && atoi(e) ? 0 : -2;
    }
    if (state == -2 || !S.on) return;
    if (state == 0) {
        VirtualDesc d;
        memset(&d, 0, sizeof d);
        d.version = sizeof d;
        d.type = 1;   // SDL_JOYSTICK_TYPE_GAMEPAD: a standard mapping
        d.naxes = 6;
        d.nbuttons = 15;
        d.name = "test pad";
        d.button_mask = (1u << 15) - 1;   // SOUTH .. DPAD_RIGHT
        d.axis_mask = 0x3f;               // both sticks, both triggers
        const SDLJoystickID id = S.AttachVirtualJoystick(&d);
        vjoy = id ? S.OpenJoystick(id) : NULL;
        LOGI("test: virtual pad %s", vjoy ? "attached" : "failed");
        vstart = now;
        state = vjoy ? 1 : -2;
        return;
    }
    const double t = now - vstart;
    // 3 s: right stick right for 0.5 s; 5 s: the right trigger; 6 s: A (coins, Start)
    S.SetJoystickVirtualAxis(vjoy, AX_RIGHTX, t >= 3 && t < 3.5 ? 32767 : 0);
    S.SetJoystickVirtualAxis(vjoy, AX_RTRIGGER, t >= 5 && t < 5.2 ? 32767 : -32768);
    S.SetJoystickVirtualAxis(vjoy, AX_LTRIGGER, -32768);
    S.SetJoystickVirtualButton(vjoy, B_SOUTH, t >= 6 && t < 6.2);
    static int told[2];
    if (t >= 2.9 && !told[0]) { told[0] = 1; LOGI("test: sight before the stick at %.0f,%.0f", sx, sy); }
    if (t >= 3.8 && !told[1]) { told[1] = 1; LOGI("test: sight after the stick at %.0f,%.0f", sx, sy); }
    if (t > 8) state = 2;   // (stays plugged in, still)
}

// ---- every frame
static int active, fire_was, reload_was, prev[16];
static double menu_repeat;

static float dz(float v) { const float d = 0.22f; if (fabsf(v) < d) return 0; const float g = (fabsf(v) - d) / (1 - d); return (v < 0 ? -g : g) * g; }

void pad_poll(double now, double dt) {
    if (!S.tried) { S.tried = 1; S.on = load_sdl(); LOGI("pad: %s", S.on ? "SDL3 gamepads" : "XInput (no SDL3.dll)"); }
    test_pad(now);
    Pad p;
    memset(&p, 0, sizeof p);
    if (!(S.on ? read_sdl(&p, now) : read_xinput(&p, now))) return;
    int pressed[16];
    for (int i = 0; i < 16; i++) { pressed[i] = p.b[i] && !prev[i]; prev[i] = p.b[i]; }
    const float mx = fabsf(dz(p.rx)) > fabsf(dz(p.lx)) ? dz(p.rx) : dz(p.lx), my = fabsf(dz(p.ry)) > fabsf(dz(p.ly)) ? dz(p.ry) : dz(p.ly);
    if (flat_menu_is_open()) {
        if (pressed[B_SOUTH]) flat_menu_key(FLAT_KEY_ENTER);
        if (pressed[B_EAST] || pressed[B_BACK]) flat_menu_key(FLAT_KEY_BACK);
        if (pressed[B_RSHOULDER]) flat_menu_key(FLAT_KEY_TAB);
        if (pressed[B_LSHOULDER]) flat_menu_key(FLAT_KEY_TAB_BACK);
        int dir = -1;
        if (p.b[B_UP] || my > 0.5f) dir = FLAT_KEY_UP;
        else if (p.b[B_DOWN] || my < -0.5f) dir = FLAT_KEY_DOWN;
        else if (p.b[B_LEFT] || mx < -0.5f) dir = FLAT_KEY_LEFT;
        else if (p.b[B_RIGHT] || mx > 0.5f) dir = FLAT_KEY_RIGHT;
        if (dir < 0) menu_repeat = 0;
        else if (now >= menu_repeat) { flat_menu_key(dir); menu_repeat = now + (menu_repeat ? 0.12 : 0.35); }
        fire_was = reload_was = 0;
        return;
    }
    if (pressed[B_BACK]) { flat_menu_set_open(1); return; }
    if (pressed[B_SOUTH] || pressed[B_START]) { flat_start(1); flat_start(0); }
    int vx, vy, vw, vh;
    flat_view_rect(&vx, &vy, &vw, &vh);
    if (vw <= 0 || vh <= 0) return;
    if (sx < 0) { sx = vx + vw * 0.5f; sy = vy + vh * 0.5f; }
    // the sight: a sixth of the picture a second at a light push, two at a full one; and the pad's gyro (Gyro aim)
    if (mx || my) {
        active = 1;
        sx += mx * (float)vw * 2.0f * (float)dt;
        sy -= my * (float)vw * 2.0f * (float)dt;
    }
    if (pad_gyro && S.on && flat_opt_get(QOPT_GYRO)) {
        float g[3] = { 0, 0, 0 };   // rad/s: x pitch (up +), y yaw (left +), z roll
        if (S.GetGamepadSensorData(pad, SENSOR_GYRO, g, 3) && (fabsf(g[0]) > 0.01f || fabsf(g[1]) > 0.01f)) {
            const float k = (float)vw / 0.7f * (float)flat_opt_get(QOPT_GYRO_SPEED) / 100.0f;
            sx -= g[1] * k * (float)dt;
            sy -= g[0] * k * (float)dt;
            active = 1;
        }
    }
    // (a little past the picture's edges: aiming off it reloads, as on the cabinet)
    if (sx < vx - vw * 0.05f) sx = vx - vw * 0.05f;
    if (sx > vx + vw * 1.05f) sx = vx + vw * 1.05f;
    if (sy < vy - vh * 0.05f) sy = vy - vh * 0.05f;
    if (sy > vy + vh * 1.05f) sy = vy + vh * 1.05f;
    if (active) flat_pointer((int)sx, (int)sy);
    const int fire = p.rt > 0.4f || p.b[B_RSHOULDER], reload = p.lt > 0.4f || p.b[B_LSHOULDER] || p.b[B_WEST];
    if (fire != fire_was) { active = 1; flat_pointer((int)sx, (int)sy); flat_trigger(fire); fire_was = fire; }
    if (reload != reload_was) { flat_reload(reload); reload_was = reload; }
}

void pad_mouse_moved(void) { active = 0; sx = -1; }   // the mouse aims again (the sight starts in the middle next time)

// Game pads on Windows (XInput: Xbox pads, and the many that act as one): a sight moved by either stick, the right
// trigger shooting, X or the left trigger reloading, A or Start for coins and Start, Back for the menu; in the menu
// the stick or the D-pad moves, A selects, B goes back, the bumpers switch tabs. The game's own crosshair shows
// where the sight is (the PICTURE tab's Crosshair).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include "flat/flat.h"
#include "platform/platform.h"

typedef struct {
    WORD wButtons;
    BYTE bLeftTrigger, bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} PadGamepad;
typedef struct { DWORD dwPacketNumber; PadGamepad Gamepad; } PadState;
typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD, PadState *);
enum { P_UP = 0x0001, P_DOWN = 0x0002, P_LEFT = 0x0004, P_RIGHT = 0x0008, P_START = 0x0010, P_BACK = 0x0020,
       P_LB = 0x0100, P_RB = 0x0200, P_A = 0x1000, P_B = 0x2000, P_X = 0x4000 };

static PFN_XInputGetState get_state;
static int loaded, active;
static float sx = -1, sy = -1;              // the sight, in window pixels
static WORD prev_buttons;
static int prev_fire, prev_reload;
static double next_scan, menu_repeat;

static float axis(SHORT v) {
    const float f = (float)v / 32767.0f, dead = 0.24f;
    if (fabsf(f) < dead) return 0;
    const float g = (fabsf(f) - dead) / (1 - dead);
    return (f < 0 ? -1.0f : 1.0f) * g * g;   // fine near the middle, fast at the edge
}

void pad_poll(double now, double dt) {
    if (!loaded) {
        loaded = 1;
        HMODULE m = LoadLibraryW(L"xinput1_4.dll");
        if (!m) m = LoadLibraryW(L"xinput9_1_0.dll");
        if (m) get_state = (PFN_XInputGetState)GetProcAddress(m, "XInputGetState");
    }
    if (!get_state) return;
    // the first connected pad (polling an empty slot is slow: those are looked at once a second)
    PadState st;
    static int slot = -1;
    if (slot < 0 && now < next_scan) return;
    if (slot < 0) {
        next_scan = now + 1;
        for (int i = 0; i < 4 && slot < 0; i++) if (get_state((DWORD)i, &st) == ERROR_SUCCESS) slot = i;
        if (slot < 0) return;
        plat_log(PLAT_LOG_INFO, "pad: XInput pad %d", slot);
    }
    if (get_state((DWORD)slot, &st) != ERROR_SUCCESS) { slot = -1; return; }
    const PadGamepad *g = &st.Gamepad;
    const WORD b = g->wButtons, pressed = (WORD)(b & ~prev_buttons);
    prev_buttons = b;
    float lx = axis(g->sThumbLX), ly = axis(g->sThumbLY), rx = axis(g->sThumbRX), ry = axis(g->sThumbRY);
    const float mx = fabsf(rx) > fabsf(lx) ? rx : lx, my = fabsf(ry) > fabsf(ly) ? ry : ly;
    if (flat_menu_is_open()) {
        if (pressed & P_A) flat_menu_key(FLAT_KEY_ENTER);
        if (pressed & (P_B | P_BACK)) flat_menu_key(FLAT_KEY_BACK);
        if (pressed & P_RB) flat_menu_key(FLAT_KEY_TAB);
        if (pressed & P_LB) flat_menu_key(FLAT_KEY_TAB_BACK);
        // the D-pad, or the stick (repeating while held)
        int dir = -1;
        if ((b & P_UP) || my > 0.5f) dir = FLAT_KEY_UP;
        else if ((b & P_DOWN) || my < -0.5f) dir = FLAT_KEY_DOWN;
        else if ((b & P_LEFT) || mx < -0.5f) dir = FLAT_KEY_LEFT;
        else if ((b & P_RIGHT) || mx > 0.5f) dir = FLAT_KEY_RIGHT;
        if (dir < 0) menu_repeat = 0;
        else if (now >= menu_repeat) { flat_menu_key(dir); menu_repeat = now + (menu_repeat ? 0.12 : 0.35); }
        prev_fire = prev_reload = 0;
        return;
    }
    if (pressed & P_BACK) { flat_menu_set_open(1); return; }
    if (pressed & (P_A | P_START)) { flat_start(1); flat_start(0); }
    // the sight: a sixth of the picture's width a second at a light push, two at a full one
    int vx, vy, vw, vh;
    flat_view_rect(&vx, &vy, &vw, &vh);
    if (vw <= 0 || vh <= 0) return;
    if (sx < 0) { sx = vx + vw * 0.5f; sy = vy + vh * 0.5f; }
    if (mx || my) {
        active = 1;
        const float speed = (float)vw * 2.0f * (float)dt;
        sx += mx * speed;
        sy -= my * speed;
        // (a little past the picture's edges: aiming off it reloads, as on the cabinet)
        if (sx < vx - vw * 0.05f) sx = vx - vw * 0.05f;
        if (sx > vx + vw * 1.05f) sx = vx + vw * 1.05f;
        if (sy < vy - vh * 0.05f) sy = vy - vh * 0.05f;
        if (sy > vy + vh * 1.05f) sy = vy + vh * 1.05f;
    }
    if (active) flat_pointer((int)sx, (int)sy);
    const int fire = g->bRightTrigger > 100, reload = (b & P_X) || g->bLeftTrigger > 100;
    if (fire != prev_fire) { active = 1; flat_pointer((int)sx, (int)sy); flat_trigger(fire); }
    if (reload != prev_reload) flat_reload(reload);
    prev_fire = fire;
    prev_reload = reload;
}

void pad_mouse_moved(void) { active = 0; sx = -1; }   // the mouse aims again (the sight starts in the middle next time)

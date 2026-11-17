// quest_menu.c -- the VR menu (see quest_menu.h). Adapted from the Time Crisis 2 VR project's quest/quest_menu.c (MIT).
//
// Immediate mode: one layout function describes the whole menu. It runs once with this frame's input (clicks, stick
// steps, drags) to apply actions, once more to settle the hit boxes, focus order and help line for the new state, and once
// more to draw, only when a hash of everything shown has changed. The layout is fixed in pixels; nothing scrolls.
#include "quest_menu.h"
#include "quest_canvas.h"
#include "quest_options.h"
#include "ui_font_data.h"
#include "online.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- palette ----
static const QColor PANEL = { 12, 19, 29, 245 }, EDGE = { 54, 183, 207, 255 }, LINE = { 31, 51, 71, 255 }, BODY = { 17, 27, 40, 255 },
    TEXT = { 238, 243, 246, 255 }, MUTED = { 143, 166, 181, 255 }, ACCENT = { 255, 202, 110, 255 }, INK = { 26, 18, 6, 255 },
    GOOD = { 140, 235, 150, 255 }, WARN = { 255, 150, 120, 255 }, HOVER = { 26, 43, 61, 255 }, ITEM = { 14, 24, 36, 255 },
    WELL = { 10, 17, 26, 255 }, RAIL = { 36, 56, 75, 255 }, ZERO = { 76, 98, 116, 255 }, GOLD = { 36, 29, 15, 255 },
    GOLDLINE = { 107, 84, 38, 255 }, DANGER = { 90, 43, 34, 255 }, KBD = { 26, 38, 53, 255 }, KBDLINE = { 46, 66, 87, 255 },
    MODAL = { 16, 26, 38, 255 }, SHADE = { 4, 7, 11, 184 }, WARNINK = { 42, 13, 7, 255 }, DIM = { 91, 115, 133, 255 },
    WARNLINE = { 107, 51, 38, 255 }, GOODINK = { 11, 36, 16, 255 }, WARNBG = { 58, 30, 24, 255 };

// ---- layout (panel pixels) ----
enum { BODY_Y = 135, BODY_H = 428, CX0 = 29, CX1 = 1091, CW = CX1 - CX0, CY0 = BODY_Y + 16, FOOT_Y = BODY_Y + BODY_H, COLW = 520, COL2 = CX0 + COLW + 22 };

// ---- ids: kind << 8 | argument ----
enum { K_TAB = 1, K_FOOT, K_ROW, K_SEG, K_STEP, K_TRACK, K_RESET, K_TABS, K_MBTN, K_ON, K_GAME, K_KEY, K_VOICE, K_VOL, K_VOLROW };
#define ID(k, a) ((k) << 8 | (a))
#define KIND(id) ((id) >> 8)
#define ARG(id) ((id) & 255)
enum { M_NONE, M_QUIT, M_HOST, M_HOSTLAN, M_QUICK, M_LEAVE, M_CODE, M_NAME, M_IP, M_SERVER, M_CONTROLS };
enum { S_MAIN, S_BROWSE, S_LAN };
enum { ON_HOST, ON_CODE, ON_BROWSE, ON_QUICK, ON_LAN, ON_NAME, ON_MUTE, ON_LEAVE, ON_BACK, ON_REFRESH, ON_HOSTLAN, ON_IPEDIT, ON_JOINIP, ON_PREV, ON_NEXT, ON_SERVER };
enum { F_BUTTON, F_SEG, F_SLIDER, F_TABS };   // what left / right does on a focused row
enum { PASS_ACT, PASS_LAYOUT, PASS_DRAW };

// ---- the settings rows ----
enum { T_SEG, T_SLIDER };
typedef struct RowDef {
    int opt; const char *label, *sub, *help; int type; const char *opts[4]; int nopts, step; const char *unit; bool sign;
} RowDef;
static const RowDef GUN_ROWS[] = {
    { QOPT_AIM, "Aim guide", "What shows where you point", "LASER: a beam and the spot it lands on. TRACER: a streak on each shot. DOT: only the spot. OFF: arcade-pure.",
      T_SEG, { "LASER", "TRACER", "DOT", "OFF" }, 4, 1, "", false },
    { QOPT_CROSSHAIR, "Screen crosshair", "The cabinet view's sight on the game's screen",
      "ON: the cabinet's screen shows where you aim. OFF: no sight, as on the real cabinet.", T_SEG, { "ON", "OFF" }, 2, 1, "", false },
    { QOPT_GUN_MODEL, "Gun model", "What you hold",
      "CABINET: the arcade cabinet's own gun, on its cord. PISTOL: a handgun whose slide kicks and racks and whose magazine drops out.",
      T_SEG, { "CABINET", "PISTOL" }, 2, 1, "", false },
    { QOPT_PITCH, "Gun angle", "Lower it if shots land high", "Tilts the barrel about your hand. + raises it.", T_SLIDER, { 0 }, 0, 1, "\xC2\xB0", true },
    { QOPT_GUN_SIZE, "Gun size", "Smaller or bigger in your hand", "Scales the gun about your hand, so your hand fits its grip.", T_SLIDER, { 0 }, 0, 5, "%", false },
    { QOPT_RUMBLE, "Rumble", "A kick in your hand on each shot", "The controller vibrates when you shoot and reload.", T_SEG, { "ON", "OFF" }, 2, 1, "", false },
};
static const RowDef HANDS_ROWS[] = {
    { QOPT_HAND, "Gun hand", "One gun, as on the cabinet, or one in each hand",
      "RIGHT or LEFT: one gun; your other hand holds a magazine to reload with. BOTH: a gun in each hand, the one you fire aims.",
      T_SEG, { "RIGHT", "LEFT", "BOTH" }, 3, 1, "", false },
    { QOPT_GESTURE, "Reload gesture", "Reload by moving, not with buttons",
      "MAGAZINE: your free hand's grip gives a magazine, push it into the gun's grip, rack the slide. Two guns: knock the grips. POINT DOWN: aim at the floor. On, it is the only way to reload.",
      T_SEG, { "OFF", "MAGAZINE", "POINT DOWN" }, 3, 1, "", false },
    { QOPT_GRIP, "Grip reloads", "A firm squeeze of the grip reloads",
      "Squeezing the gun hand's grip all the way reloads once, as do B, Y and firing off screen. Only with Reload gesture OFF: with a gesture on, it does nothing.",
      T_SEG, { "ON", "OFF" }, 2, 1, "", false },
    { QOPT_CAB_RELOAD, "Cabinet reload", "How you reload in the cabinet view",
      "GESTURE: as Reload gesture says. OFF SCREEN: in the cabinet view you reload as on the real cabinet, by shooting off the screen.",
      T_SEG, { "GESTURE", "OFF SCREEN" }, 2, 1, "", false },
    { QOPT_GUN_Y, "Gun height", "Up or down in your hand", "+ raises the gun relative to the controller.", T_SLIDER, { 0 }, 0, 5, " mm", true },
    { QOPT_GUN_Z, "Gun depth", "+ pulls it back towards you", "Slides the gun forward or back along the controller.", T_SLIDER, { 0 }, 0, 5, " mm", true },
};
#define ROOM_ROW \
    { QOPT_ROOM, "Cabinet room", "What surrounds the cabinet", \
      "In the cabinet view: DARK, your own room through the headset's cameras (PASSTHROUGH), or the first stage's courtyard.", \
      T_SEG, { "DARK", "PASSTHROUGH", "COURTYARD" }, 3, 1, "", false },

static const RowDef VIEW_ROWS[] = {
    { QOPT_VIEW, "View", "Stand in the game, or play its cabinet", "IMMERSIVE: you stand in the game's world. CABINET: the game on an arcade screen. Holding your gun hand's stick click switches too.",
      T_SEG, { "IMMERSIVE", "CABINET" }, 2, 1, "", false },
    { QOPT_WORLD, "World size", "100% is life size", "How big the game's world feels around you.", T_SLIDER, { 0 }, 0, 5, "%", false },
    { QOPT_HUD, "HUD distance", "Score, lives and the game's menus", "How far ahead the game's 2D layer floats: its score, lives, text and menus.",
      T_SLIDER, { 0 }, 0, 10, " cm", false },
    ROOM_ROW
};
// In the cabinet view the VIEW tab places the cabinet (World size and HUD distance are the immersive view's)
static const RowDef VIEW_CAB_ROWS[] = {
    { QOPT_VIEW, "View", "Stand in the game, or play its cabinet", "IMMERSIVE: you stand in the game's world. CABINET: the game on an arcade screen. Holding your gun hand's stick click switches too.",
      T_SEG, { "IMMERSIVE", "CABINET" }, 2, 1, "", false },
    { QOPT_CAB_SIZE, "Cabinet size", "Bigger cabinet, bigger screen", "How big the cabinet is. Or hold both grips: spread or bring your hands together to size it, move them to carry it, twist to turn it.",
      T_SLIDER, { 0 }, 0, 5, "%", false },
    { QOPT_CAB_DIST, "Cabinet distance", "+ moves it away from you", "How far the cabinet stands from you. Its reset also brings back one carried aside or turned with both grips.",
      T_SLIDER, { 0 }, 0, 5, " cm", true },
    { QOPT_CAB_HEIGHT, "Cabinet height", "Lower it if you play seated", "Raises the cabinet off the floor or sinks it into it.",
      T_SLIDER, { 0 }, 0, 5, " cm", true },
    ROOM_ROW
};

static const RowDef COMFORT_ROWS[] = {
    { QOPT_HORIZON, "Horizon", "Keep the world level when the game's camera tilts",
      "LEVEL: the world stays level; look down or up yourself. NO ROLL: no sideways tilt. GAME: as the game's camera.",
      T_SEG, { "GAME", "NO ROLL", "LEVEL" }, 3, 1, "", false },
    { QOPT_VIGNETTE, "Turn vignette", "Darkens the edges while the camera turns", "When the game's camera swings round, the edges of your view darken. HIGH narrows it more.",
      T_SEG, { "OFF", "LOW", "HIGH" }, 3, 1, "", false },
};
static const RowDef GRAPHICS_ROWS[] = {
    { QOPT_RESOLUTION, "Resolution", "", "Pixels per eye, from the headset's recommended size. Higher is sharper and costs GPU time. Applies the next time the app starts.",
      T_SEG, { "80%", "100%", "125%", "150%" }, 4, 1, "", false },
    { QOPT_HZ, "Refresh rate", "120 Hz shows each game frame twice", "How often the headset redraws. The game runs at the arcade's 57.5 frames a second; 120 Hz is the smoothest.",
      T_SEG, { "72 HZ", "90 HZ", "120 HZ" }, 3, 1, "", false },
};
// the flat apps' PICTURE tab (in place of GRAPHICS)
static const RowDef PICTURE_ROWS[] = {
    { QOPT_PICTURE, "Picture", "How the game is drawn",
      "HD: the game's 3D drawn sharp at your screen's resolution. ORIGINAL: at the arcade's own 496 x 384, its pixels scaled up.",
      T_SEG, { "HD", "ORIGINAL" }, 2, 1, "", false },
    { QOPT_CROSSHAIR, "Crosshair", "The game's sight where you aim",
      "ON: the game shows where you aim, as its two-player sights do. OFF: none, as on the real cabinet (a light gun needs none).",
      T_SEG, { "ON", "OFF" }, 2, 1, "", false },
    { QOPT_FULLSCREEN, "Display", "A window, or the whole screen", "Alt+Enter or F11 switches too.",
      T_SEG, { "WINDOW", "FULL SCREEN" }, 2, 1, "", false },
};
static const RowDef SYSTEM_ROWS[] = {
    { QOPT_VOLUME, "Game volume", "Everything the game plays", "All of the game's sound: music, voices and guns. Voice chat isn't in it.",
      T_SLIDER, { 0 }, 0, 5, "%", false },
    { QOPT_MUSIC, "Music", "The soundtrack", "The game's music (its CD tracks). The game's own Options page has a music level too; both apply.",
      T_SLIDER, { 0 }, 0, 5, "%", false },
    { QOPT_SFX, "Effects", "Guns, zombies and voices", "Everything but the music: shots, hits, the zombies and the game's voices. The game's Options page has its own level too.",
      T_SLIDER, { 0 }, 0, 5, "%", false },
    { QOPT_VOICE, "Voice chat", "Your partner online", "How loud your partner is. Over 100% boosts a quiet microphone. Also on the ONLINE tab.",
      T_SLIDER, { 0 }, 0, 10, "%", false },
    { QOPT_CRASH_REPORTS, "Crash reports", "Sent at the next start after a crash",
      "ON: after a crash, the next start sends its report (crash point, end of port.log, settings, headset model; nothing personal). OFF: kept.",
      T_SEG, { "ON", "OFF" }, 2, 1, "", false },
};

// ---- state ----
static int tab, modal;
static online_view V;                                    // the ONLINE tab: what online.c says, and the browser's list
static online_game games[40]; static int game_count;
static int screen, page;
static char kbuf[64];
static const char *kerror;   // the lobby server editor: why SAVE was refused (online_set_server), or NULL
static bool klower = true;   // the lobby server editor's letters: lowercase (ABC switches)
static int hover, pressed, focus;
static bool stick_mode;
static bool have_pointer; static float ptr_x, ptr_y;   // the laser on the panel, smoothed
static double repeat_at;
static int stick_dx, stick_dy; static double stick_next;
static const QMenuHost *H;
static uint32_t drawn_hash = 1;
// this pass
static int pass;
static QCanvas cv;
static int ev_click, ev_adjust, ev_adjust_dir, ev_drag; static float ev_drag_x;
typedef struct Hit { int id, x, y, w, h; } Hit;
static Hit hits[160]; static int nhits;
typedef struct Focus { int id, row, cx, kind; } Focus;
static Focus foci[100]; static int nfoci, frow;
static char help[200];

void qmenu_open(int t) {
    if (t >= 0 && t < QMENU_TAB_COUNT) tab = t;
    modal = M_NONE; pressed = 0; focus = ID(K_FOOT, 0);
}
bool qmenu_pointer(float *px, float *py) { if (!have_pointer) return false; *px = ptr_x; *py = ptr_y; return true; }
bool qmenu_modal(void) { return modal != M_NONE || screen != S_MAIN; }

// ---- layout primitives ----
static bool drawing(void) { return pass == PASS_DRAW; }
static bool is_hover(int id) { return !stick_mode && hover == id; }
static bool pointer_in(float x, float y, float w, float h) { return !stick_mode && have_pointer && hover && ptr_x >= x && ptr_x < x + w && ptr_y >= y && ptr_y < y + h; }
static bool is_focus(int id) { return stick_mode && focus == id; }
static void hit(int id, int x, int y, int w, int h) { if (nhits < (int)(sizeof hits / sizeof *hits)) hits[nhits++] = (Hit){ id, x, y, w, h }; }
static void focusable(int id, int kind, int cx) { if (nfoci < (int)(sizeof foci / sizeof *foci)) foci[nfoci++] = (Focus){ id, frow, cx, kind }; }
static void next_row(void) { frow++; }
static bool clicked(int id) { return pass == PASS_ACT && ev_click == id; }
static void say_help(int id, const char *text) { if ((is_hover(id) || is_focus(id)) && text) snprintf(help, sizeof help, "%s", text); }
static void rrect(float x, float y, float w, float h, float r, QColor k) { if (drawing()) qc_rrect(&cv, x, y, w, h, r, k); }
static void rline(float x, float y, float w, float h, float r, float t, QColor k) { if (drawing()) qc_rrect_line(&cv, x, y, w, h, r, t, k); }
static int text(int font, float x, float y, const char *s, QColor k) { return drawing() ? qc_text(&cv, font, x, y, s, k, 0) : qc_text_width(font, s, 0); }
static void text_track(int font, float x, float y, const char *s, QColor k, int tracking) { if (drawing()) qc_text(&cv, font, x, y, s, k, tracking); }
static void text_fit(int font, float x, float y, float w, const char *s, QColor k, int align) { if (drawing()) qc_text_fit(&cv, font, x, y, w, s, k, align); }
static int width(int font, const char *s) { return qc_text_width(font, s, 0); }
static float mid(int font, float y, float h) { return y + h / 2 + qc_font_size(font) * .36f; }   // the baseline centring capitals
static int para(int font, float x, float y, float w, float line_h, const char *s, QColor k, int max_lines) {
    char line[200]; int lines = 0;
    while (*s && lines < max_lines) {
        int n = 0, last_space = -1;
        while (s[n]) {
            if (s[n] == ' ') last_space = n;
            char t[200]; int m = n + 1 < (int)sizeof t - 1 ? n + 1 : (int)sizeof t - 1; memcpy(t, s, m); t[m] = 0;
            if (width(font, t) > w && n > 0) { if (last_space > 0) n = last_space; break; }
            n++;
        }
        int m = n < (int)sizeof line - 1 ? n : (int)sizeof line - 1; memcpy(line, s, m); line[m] = 0;
        if (lines == max_lines - 1 && s[n]) text_fit(font, x, y + lines * line_h, w, s, k, 0); else text(font, x, y + lines * line_h, line, k);
        lines++; s += n; while (*s == ' ') s++;
    }
    return lines;
}

// ---- icons ----
enum { I_PLAY, I_LEFT, I_RIGHT, I_RECENTER, I_RESET, I_LIST, I_CROSS, I_HOUSE, I_KEYS, I_BOLT, I_SWAP, I_EJECT, I_CLOUD };
static void icon(int kind, float cx, float cy, float s, QColor k) {
    if (!drawing()) return;
    const float h = s / 2, t = s * .13f < 2 ? 2 : s * .13f;
    switch (kind) {
    case I_PLAY: { float p[] = { cx - h * .7f, cy - h * .85f, cx + h * .85f, cy, cx - h * .7f, cy + h * .85f }; qc_poly(&cv, p, 3, k); break; }
    case I_LEFT: { float p[] = { cx + h * .5f, cy - h * .6f, cx - h * .55f, cy, cx + h * .5f, cy + h * .6f }; qc_poly(&cv, p, 3, k); break; }
    case I_RIGHT: { float p[] = { cx - h * .5f, cy - h * .6f, cx + h * .55f, cy, cx - h * .5f, cy + h * .6f }; qc_poly(&cv, p, 3, k); break; }
    case I_RESET: {   // an anticlockwise arrow
        float r = h * .72f; qc_arc(&cv, cx, cy, r, -2.2f, 3.0f, t, k);
        float ax = cx + cosf(-2.2f) * r, ay = cy + sinf(-2.2f) * r;
        float p[] = { ax - h * .42f, ay - h * .18f, ax + h * .22f, ay - h * .5f, ax + h * .18f, ay + h * .26f }; qc_poly(&cv, p, 3, k); break; }
    case I_RECENTER:
        qc_arc(&cv, cx, cy, h * .62f, 0, 6.2832f, t * .8f, k);
        qc_line(&cv, cx - h, cy, cx - h * .35f, cy, t * .8f, k); qc_line(&cv, cx + h * .35f, cy, cx + h, cy, t * .8f, k);
        qc_line(&cv, cx, cy - h, cx, cy - h * .35f, t * .8f, k); qc_line(&cv, cx, cy + h * .35f, cx, cy + h, t * .8f, k);
        qc_circle(&cv, cx, cy, t * .9f, k); break;
    case I_LIST: for (int i = 0; i < 3; i++) qc_line(&cv, cx - h * .8f, cy - h * .55f + i * h * .55f, cx + h * .8f, cy - h * .55f + i * h * .55f, t, k); break;
    case I_CROSS: qc_line(&cv, cx - h * .6f, cy - h * .6f, cx + h * .6f, cy + h * .6f, t, k); qc_line(&cv, cx + h * .6f, cy - h * .6f, cx - h * .6f, cy + h * .6f, t, k); break;
    case I_HOUSE: { float p[] = { cx - h * .9f, cy - h * .05f, cx, cy - h * .85f, cx + h * .9f, cy - h * .05f }; qc_poly(&cv, p, 3, k);
        qc_rrect(&cv, cx - h * .6f, cy - h * .1f, h * 1.2f, h * .95f, 1, k); qc_rrect(&cv, cx - h * .18f, cy + h * .3f, h * .36f, h * .55f, 1, WELL); break; }
    case I_KEYS: qc_rrect_line(&cv, cx - h, cy - h * .6f, s, h * 1.2f, h * .2f, t * .8f, k);
        for (int r = 0; r < 2; r++) for (int i = 0; i < 4; i++) qc_rrect(&cv, cx - h * .7f + i * h * .42f - h * .1f, cy - h * .32f + r * h * .36f, h * .22f, h * .2f, 1, k);
        qc_rrect(&cv, cx - h * .45f, cy + h * .32f, h * .9f, h * .14f, 1, k); break;
    case I_BOLT: { float p[] = { cx + h * .2f, cy - h, cx - h * .6f, cy + h * .12f, cx - h * .02f, cy + h * .12f, cx - h * .2f, cy + h, cx + h * .6f, cy - h * .15f, cx + h * .02f, cy - h * .15f };
        qc_poly(&cv, p, 6, k); break; }
    case I_SWAP: qc_line(&cv, cx - h * .8f, cy - h * .35f, cx + h * .8f, cy - h * .35f, t, k); qc_line(&cv, cx - h * .8f, cy + h * .35f, cx + h * .8f, cy + h * .35f, t, k);
        { float a[] = { cx + h * .9f, cy - h * .35f, cx + h * .4f, cy - h * .7f, cx + h * .4f, cy }; qc_poly(&cv, a, 3, k);
          float b[] = { cx - h * .9f, cy + h * .35f, cx - h * .4f, cy, cx - h * .4f, cy + h * .7f }; qc_poly(&cv, b, 3, k); } break;
    case I_EJECT: { float p[] = { cx - h * .8f, cy + h * .1f, cx, cy - h * .75f, cx + h * .8f, cy + h * .1f }; qc_poly(&cv, p, 3, k); qc_rrect(&cv, cx - h * .8f, cy + h * .38f, h * 1.6f, h * .3f, 1, k); break; }
    case I_CLOUD: qc_circle(&cv, cx - h * .35f, cy + h * .1f, h * .42f, k); qc_circle(&cv, cx + h * .1f, cy - h * .12f, h * .55f, k); qc_circle(&cv, cx + h * .5f, cy + h * .15f, h * .38f, k);
        qc_rrect(&cv, cx - h * .75f, cy + h * .1f, h * 1.6f, h * .42f, h * .2f, k); break;
    }
}

// ---- widgets ----
// a button; style 0 ghost, 1 filled accent, 2 small ghost
static bool button(int id, float x, float y, float w, float h, const char *label, int font, int style, const char *tip) {
    hit(id, (int)x, (int)y, (int)w, (int)h); say_help(id, tip);
    const bool hot = is_hover(id) || is_focus(id);
    if (style == 1) { rrect(x, y, w, h, 12, ACCENT); if (hot) rline(x - 4, y - 4, w + 8, h + 8, 15, 3, TEXT); }
    else { rrect(x, y, w, h, style == 2 ? 10 : 12, hot ? HOVER : WELL); rline(x, y, w, h, style == 2 ? 10 : 12, 2, hot ? ACCENT : LINE); }
    if (drawing()) qc_text_fit(&cv, font, x + 8, mid(font, y, h), w - 16, label, style == 1 ? INK : hot ? ACCENT : TEXT, 1);
    return clicked(id);
}
static bool step_button(int id, float x, float y, float w, float h, bool left) {
    hit(id, (int)x, (int)y, (int)w, (int)h);
    const bool hot = is_hover(id) || pressed == id;
    rrect(x, y, w, h, 14, WELL); rline(x, y, w, h, 14, 2, hot ? ACCENT : LINE);
    icon(left ? I_LEFT : I_RIGHT, x + w / 2, y + h / 2, h * .5f, hot ? ACCENT : TEXT);
    return clicked(id);
}
// an item: icon, title, subtitle; style 0 normal, 1 gold, 2 danger
static bool item(int id, float x, float y, float w, float h, int ic, const char *title, const char *sub, const char *tip, int style) {
    hit(id, (int)x, (int)y, (int)w, (int)h); focusable(id, F_BUTTON, (int)(x + w / 2)); say_help(id, tip);
    const bool hot = is_hover(id) || is_focus(id);
    rrect(x, y, w, h, 14, hot ? HOVER : style == 1 ? GOLD : ITEM); rline(x, y, w, h, 14, 2, hot ? ACCENT : style == 1 ? GOLDLINE : style == 2 ? DANGER : LINE);
    QColor ink = style == 1 ? ACCENT : TEXT, ic_ink = style == 1 ? ACCENT : style == 2 ? WARN : EDGE;
    icon(ic, x + 16 + 17, y + h / 2, 24, ic_ink);
    if (h < 60) text_fit(UI_B22, x + 64, mid(UI_B22, y, h), w - 80, title, ink, 0);
    else { text_fit(UI_B22, x + 64, y + h / 2 - 3, w - 80, title, ink, 0); text_fit(UI_M15, x + 64, y + h / 2 + 18, w - 80, sub, MUTED, 0); }
    return clicked(id);
}
// a box with a small spaced title; returns the y below the title
static float box(float x, float y, float w, float h, const char *title, QColor title_ink, QColor border) {
    rrect(x, y, w, h, 16, WELL); rline(x, y, w, h, 16, 2, border);
    text_track(UI_M15, x + 20, y + 14 + 16, title, title_ink, 2);
    return y + 14 + 24;
}
static void kv(float x, float y, float w, const char *label, const char *value, QColor ink) {
    text(UI_M20, x, mid(UI_M20, y, 38), label, MUTED);
    if (value && drawing()) qc_text_fit(&cv, UI_B20, x + w * .38f, mid(UI_B20, y, 38), w * .62f, value, ink, 2);
}
static void meter(float right, float y, int level, QColor on) {
    for (int i = 0; i < 8; i++) rrect(right - 8 * 11 + i * 11, y + 10, 8, 18, 2, i < level ? on : RAIL);
}
static void kbd(float *x, float y, const char *key, const char *what) {
    if (!drawing()) { *x += width(UI_B16, key) + 16 + 6 + width(UI_M17, what) + 14; return; }
    const int kw = width(UI_B16, key) + 16;
    qc_rrect(&cv, *x, y - 19, kw, 26, 7, KBD); qc_rrect_line(&cv, *x, y - 19, kw, 26, 7, 1, KBDLINE); qc_text(&cv, UI_B16, *x + 8, y, key, TEXT, 0);
    *x += kw + 6; *x += qc_text(&cv, UI_M17, *x, y, what, MUTED, 0) + 14;
}
static float kbd_width(const char *const *k, int n) { float x = 0; const int keep = pass; pass = PASS_LAYOUT; for (int i = 0; i < n; i++) kbd(&x, 0, k[i * 2], k[i * 2 + 1]); pass = keep; return x - 14; }

// ---- the header, tabs and footer ----
static const char *const KEYS_LASER[] = { "TRIGGER", "select", "B", "back", "GRIPS", "tabs" };
static const char *const KEYS_STICK[] = { "STICK", "move", "A", "select", "B", "back", "GRIPS", "tabs" };
static const char *const KEYS_MOUSE[] = { "CLICK", "select", "ESC", "back", "TAB", "tabs" };
static const char *const KEYS_TOUCH[] = { "TAP", "select", "BACK", "back", "TAB", "tabs" };
static const char *const KEYS_ARROWS[] = { "ARROWS", "move", "ENTER", "select", "ESC", "back", "TAB", "tabs" };
// the tabs a build shows: the flat apps have no VR settings
static bool tab_shown(int t) { return !H || !H->flat || t == QMENU_TAB_GRAPHICS || t == QMENU_TAB_SYSTEM || t == QMENU_TAB_ONLINE; }
static int next_tab(int t, int d) {
    for (int i = 0; i < QMENU_TAB_COUNT; i++) { t = (t + d + QMENU_TAB_COUNT) % QMENU_TAB_COUNT; if (tab_shown(t)) break; }
    return t;
}
static bool online_now(void);
static void header(void) {
    text_track(UI_B27, 30, 46, online_now() ? "MENU \xC2\xB7 ONLINE, THE GAME GOES ON" : "PAUSED", TEXT, 2);
    text_track(UI_M15, 30, 66, H->flat ? "THE HOUSE OF THE DEAD" : "THE HOUSE OF THE DEAD \xC2\xB7 VR", MUTED, 3);
    const float right = 1092;
    if (!H->saved) {
        const char *t = "SAVING FAILED - settings not kept"; const int w = width(UI_B16, t) + 28;
        rrect(right - w, 26, w, 30, 15, WARN); text(UI_B16, right - w + 14, 47, t, WARNINK);
    } else {
        const char *const *k = H->flat ? (stick_mode ? KEYS_ARROWS : H->touch ? KEYS_TOUCH : KEYS_MOUSE) : stick_mode ? KEYS_STICK : KEYS_LASER;
        const int n = stick_mode ? 4 : H->touch ? 2 : 3;
        float x = right - kbd_width(k, n); for (int i = 0; i < n; i++) kbd(&x, 47, k[i * 2], k[i * 2 + 1]);
    }
}
static void tabs(void) {
    static const char *const names[QMENU_TAB_COUNT] = { "GUN", "HANDS", "VIEW", "COMFORT", "GRAPHICS", "SYSTEM", "ONLINE" };
    rrect(3, BODY_Y - 2, QMENU_W - 6, 2, 0, LINE);
    if (drawing()) qc_clip(&cv, 0, 0, QMENU_W, BODY_Y);
    float x = 27;
    focusable(ID(K_TABS, 0), F_TABS, QMENU_W / 2);
    for (int i = 0; i < QMENU_TAB_COUNT; i++) {
        if (!tab_shown(i)) continue;
        const char *name = H->flat && i == QMENU_TAB_GRAPHICS ? "PICTURE" : names[i];
        const int id = ID(K_TAB, i), w = width(UI_B24, name) + 4 + 40;
        hit(id, (int)x, 78, w, 55);
        const bool active = tab == i, hot = is_hover(id) || (is_focus(ID(K_TABS, 0)) && active);
        if (active) { rrect(x, 78, w, 80, 14, BODY); rline(x, 78, w, 80, 14, 2, LINE); rrect(x + 2, BODY_Y - 2, w - 4, 2, 0, BODY); }
        else if (hot) rrect(x, 78, w, 80, 14, HOVER);
        if (is_focus(ID(K_TABS, 0)) && active) rline(x + 1, 79, w - 2, 80, 13, 3, ACCENT);
        text_track(UI_B24, x + 20, mid(UI_B24, 78, 55), name, active ? ACCENT : hot ? TEXT : MUTED, 1);
        // a dot: a change on that tab waits for the next start; online: playing together (green) or not yet
        if (i == QMENU_TAB_GRAPHICS && !H->flat && H->get(QOPT_RESOLUTION) != H->boot_resolution && drawing())
            qc_circle(&cv, x + 20 + qc_text_width(UI_B24, names[i], 1) + 9, mid(UI_B24, 78, 55) - 9, 5, ACCENT);
        if (i == QMENU_TAB_ONLINE && online_now() && drawing())
            qc_circle(&cv, x + 20 + qc_text_width(UI_B24, name, 1) + 9, mid(UI_B24, 78, 55) - 9, 5, V.state == OV_LINKED ? GOOD : ACCENT);
        if (clicked(id)) tab = i;
        x += w + 8;
    }
    if (drawing()) qc_unclip(&cv);
    next_row();
}
static void footer(void) {
    rrect(3, FOOT_Y, QMENU_W - 6, 2, 0, LINE);
    if (drawing()) qc_text_fit(&cv, UI_M17, 27, FOOT_Y + 30, CW, help, MUTED, 0);
    const char *labels[5] = { "RESUME", "RECENTER", "CONTROLS", "GAME MENU", "QUIT" };
    const char *tips[5] = { H->flat ? "Back to the game. Esc does the same." : "Back to the game. The menu button does the same.",
                            "Puts the game where you are now, facing the way you look.",
                            "Every button and gesture: the controllers, your hands and this menu.",
                            "The game's own pause screen (Esc): Continue, or Exit to its title screen.",
                            "Closes the app. Your settings and the game's saves are kept." };
    const int icons[5] = { I_PLAY, I_RECENTER, I_KEYS, I_LIST, I_CROSS };
    tips[2] = H->flat ? "The mouse, the keys and this menu." : tips[2];
    const int nb = H->flat ? 4 : 5;
    const float y = FOOT_Y + 46, h = QMENU_H - 3 - 16 - y, unit = (CW + 4 - 12 * (nb - 1)) / (nb + 0.1f);
    float x = 27;
    for (int i = 0; i < 5; i++) {
        if (H->flat && i == 1) continue;   // (RECENTER: nothing to recenter on a screen)
        const int id = ID(K_FOOT, i); const float w = i == 0 ? unit * 1.1f : unit;
        hit(id, (int)x, (int)y, (int)w, (int)h); focusable(id, F_BUTTON, (int)(x + w / 2)); say_help(id, tips[i]);
        const bool hot = is_hover(id) || is_focus(id), gold = i == 0, danger = i == 4;
        rrect(x, y, w, h, 14, hot ? HOVER : gold ? GOLD : ITEM); rline(x, y, w, h, 14, 2, hot ? ACCENT : gold ? GOLDLINE : danger ? DANGER : LINE);
        if (is_focus(id)) rline(x - 1, y - 1, w + 2, h + 2, 15, 3, ACCENT);
        const int tw = width(UI_B22, labels[i]); const float tx = x + (w - tw - 36) / 2;
        icon(icons[i], tx + 12, y + h / 2, 24, gold ? ACCENT : danger ? WARN : EDGE);
        text(UI_B22, tx + 36, mid(UI_B22, y, h), labels[i], gold ? ACCENT : TEXT);
        if (clicked(id)) {
            if (i == 0) H->action(QACT_RESUME);
            else if (i == 1) H->action(QACT_RECENTER);
            else if (i == 2) { modal = M_CONTROLS; focus = ID(K_MBTN, 1); }
            else if (i == 3) H->action(QACT_GAME_MENU);
            else { modal = M_QUIT; focus = ID(K_MBTN, 1); }
        }
        x += w + 12;
    }
    next_row();
}

// ---- settings tabs ----
static void value_text(const RowDef *r, int v, char *out, size_t n) { snprintf(out, n, r->sign && v > 0 ? "+%d%s" : "%d%s", v, r->unit); }
// A row another setting makes meaningless: why (its line), or NULL while it applies
static const char *row_off(int opt) {
    const bool cab_offscreen = H->get(QOPT_VIEW) == QVIEW_CABINET && H->get(QOPT_CAB_RELOAD) == QCABRELOAD_OFFSCREEN;
    if (opt == QOPT_GRIP && H->get(QOPT_GESTURE) != QGESTURE_OFF && !cab_offscreen) return "Off while Reload gesture is on";
    if (opt == QOPT_GESTURE && cab_offscreen) return "Off in the cabinet view: Cabinet reload is OFF SCREEN";
    return NULL;
}
static void settings_row(const RowDef *r, float y, float h) {
    const int row = ID(K_ROW, r->opt);
    const QOptDef *def = &qopt_defs[r->opt];
    int v = H->get(r->opt);
    // disabled: shown dimmed with why, still focusable (the stick passes through, the help shows), but nothing
    // changes it (clicks, the stick and drags), and its value stays for when it applies again
    const char *off = row_off(r->opt);
    const int saved_click = ev_click, saved_adjust = ev_adjust, saved_drag = ev_drag;
    hit(row, CX0, (int)y, CW, (int)h); focusable(row, r->type == T_SLIDER ? F_SLIDER : F_SEG, CX0 + CW / 2);
    say_help(row, r->help);
    const bool hot = pointer_in(CX0, y, CW, h) || is_focus(row);
    if (hot && !off) rrect(CX0, y, CW, h, 14, HOVER);
    if (is_focus(row)) rline(CX0, y, CW, h, 14, 2, off ? LINE : ACCENT);
    if (off) ev_click = ev_adjust = ev_drag = 0;
    // the label, its line and a chip
    char sub[160]; snprintf(sub, sizeof sub, "%s", off ? off : r->sub); QColor sub_ink = MUTED;
    if (r->opt == QOPT_RESOLUTION) snprintf(sub, sizeof sub, "Per eye \xC2\xB7 running %u \xC3\x97 %u", H->active_w, H->active_h);
    if (r->opt == QOPT_HZ && H->hz_now) snprintf(sub, sizeof sub, "The game runs at 60 \xC2\xB7 now %d Hz", H->hz_now);
    const float ly = y + (h - 54) / 2;
    const int lw = text(UI_B27, CX0 + 18, ly + 25, r->label, off ? MUTED : TEXT);
    if (r->opt == QOPT_RESOLUTION && v != H->boot_resolution) {
        const int cw = width(UI_B14, "NEXT START") + 20; rrect(CX0 + 18 + lw + 10, ly + 6, cw, 22, 11, ACCENT); text(UI_B14, CX0 + 18 + lw + 20, ly + 22, "NEXT START", INK);
    }
    // the control, right-aligned before the reset
    const float right = CX1 - 18 - 44 - 10;
    float widths[4], total = 8 + (r->nopts - 1) * 4;
    for (int i = 0; i < r->nopts; i++) { widths[i] = width(UI_B20, r->opts[i]) + 36; if (widths[i] < 112) widths[i] = 112; total += widths[i]; }
    const float control = r->type == T_SLIDER ? 550 : total;
    text_fit(UI_M15, CX0 + 18, ly + 52, right - control - 20 - CX0 - 18, sub, sub_ink, 0);
    if (r->type == T_SEG) {
        // the options in the order shown; Rumble, Grip reloads and Crash reports list ON first though 1 is on
        float x = right - total; const float sy = y + (h - 50) / 2;
        rrect(x, sy, total, 50, 14, WELL); rline(x, sy, total, 50, 14, 2, LINE);
        x += 4;
        const bool flip = r->opt == QOPT_RUMBLE || r->opt == QOPT_GRIP || r->opt == QOPT_CRASH_REPORTS || r->opt == QOPT_CROSSHAIR;
        for (int i = 0; i < r->nopts; i++) {
            const int val = flip ? r->nopts - 1 - i : i;
            const int id = ID(K_SEG, r->opt * 8 + i);
            hit(id, (int)x, (int)sy + 4, (int)widths[i], 42);
            const bool on = val == v, h2 = is_hover(id) && !off;
            if (on && off) rline(x, sy + 4, widths[i], 42, 10, 2, LINE);   // (the kept value, dimmed)
            else if (on) rrect(x, sy + 4, widths[i], 42, 10, ACCENT); else if (h2) rrect(x, sy + 4, widths[i], 42, 10, HOVER);
            text_fit(UI_B20, x, mid(UI_B20, sy + 4, 42), widths[i], r->opts[i], on && !off ? INK : h2 ? TEXT : MUTED, 1);
            if (clicked(id)) H->set(r->opt, val);
            x += widths[i] + 4;
        }
        const int n = r->nopts, d = flip ? -ev_adjust_dir : ev_adjust_dir;
        if (pass == PASS_ACT && ev_adjust == row) H->set(r->opt, (v + d + n) % n);
        if (stick_mode && clicked(row)) H->set(r->opt, (v + 1) % n);
    } else {
        const float sy = y + (h - 52) / 2; float x = right - 550;
        const int minus = ID(K_STEP, r->opt * 2), plus = ID(K_STEP, r->opt * 2 + 1), track = ID(K_TRACK, r->opt);
        if (step_button(minus, x, sy, 52, 52, true)) H->set(r->opt, v - r->step);
        const float TW = 284;   // the track; the value takes the rest of the 550
        hit(track, (int)x + 62, (int)sy, (int)TW, 52);
        const float p = (float)(v - def->min) / (def->max - def->min), z = (float)(def->def - def->min) / (def->max - def->min);
        rrect(x + 62, sy + 22, TW, 8, 4, RAIL);
        rrect(x + 62 + TW * fminf(p, z), sy + 22, TW * fabsf(p - z), 8, 4, EDGE);
        rrect(x + 62 + TW * z - 1, sy + 14, 2, 24, 1, ZERO);
        if (drawing()) { qc_circle(&cv, x + 62 + TW * p, sy + 26, 16, off ? LINE : EDGE); qc_circle(&cv, x + 62 + TW * p, sy + 26, 12, off ? MUTED : (is_hover(track) || pressed == track) ? ACCENT : TEXT); }
        if (pass == PASS_ACT && ev_drag == track) {
            float f = (ev_drag_x - (x + 62)) / TW; if (f < 0) f = 0; if (f > 1) f = 1;
            int nv = (int)lroundf((def->min + f * (def->max - def->min)) / r->step) * r->step; H->set(r->opt, nv);
        }
        if (step_button(plus, x + 62 + TW + 10, sy, 52, 52, false)) H->set(r->opt, v + r->step);
        char val[24]; value_text(r, v, val, sizeof val); text_fit(UI_B27, x + 62 + TW + 70, mid(UI_B27, sy, 52), 550 - (62 + TW + 70), val, TEXT, 2);
        if (pass == PASS_ACT && ev_adjust == row) H->set(r->opt, v + ev_adjust_dir * r->step);
    }
    // reset to the default (Cabinet distance's also the cabinet's side and turn, which only grabbing sets)
    const float bx = CX1 - 18 - 44, by = y + (h - 44) / 2;
    const bool placed = r->opt == QOPT_CAB_DIST && (H->get(QOPT_CAB_SIDE) || H->get(QOPT_CAB_TURN));
    if ((v != def->def || placed) && !off) {
        const int id = ID(K_RESET, r->opt); hit(id, (int)bx, (int)by, 44, 44);
        const bool h2 = is_hover(id); rline(bx, by, 44, 44, 12, 2, h2 ? ACCENT : LINE); icon(I_RESET, bx + 22, by + 22, 22, h2 ? ACCENT : MUTED);
        if (clicked(id)) {
            H->set(r->opt, def->def);
            if (r->opt == QOPT_CAB_DIST) { H->set(QOPT_CAB_SIDE, 0); H->set(QOPT_CAB_TURN, 0); }
        }
    }
    ev_click = saved_click; ev_adjust = saved_adjust; ev_drag = saved_drag;
    next_row();
}
// the rows' spacing for a tab of n rows (slots: rows and anything under them, as SYSTEM's button); six fit closer
static float row_step(int slots) { return slots > 5 ? 68 : 78; }
static void settings_in(const RowDef *rows, int n, int slots) {
    float y = CY0;
    const float step = row_step(slots);
    for (int i = 0; i < n; i++) { settings_row(&rows[i], y, step - 4); y += step; }
}
static void settings(const RowDef *rows, int n) { settings_in(rows, n, n); }


// ---- the ONLINE tab (src/net/online.c; the Time Crisis 2 VR project's ONLINE tab, for this game) ----
static bool online_now(void) { return V.state != OV_OFFLINE && V.state != OV_FINDING && V.state != OV_FAILED; }
static void on_item(int act, float x, float *y, int ic, const char *title, const char *sub, const char *tip, int style) {
    const int id = ID(K_ON, act);
    if (item(id, x, *y, COLW, 68, ic, title, sub, tip, style)) {
        switch (act) {
        case ON_HOST: modal = M_HOST; focus = ID(K_MBTN, 1); break;
        case ON_CODE: modal = M_CODE; kbuf[0] = 0; focus = ID(K_KEY, 0); break;
        case ON_BROWSE: screen = S_BROWSE; page = 0; online_browse(true); break;
        case ON_QUICK: modal = M_QUICK; focus = ID(K_MBTN, 1); break;
        case ON_LAN: screen = S_LAN; online_browse(true); break;
        case ON_LEAVE: modal = M_LEAVE; focus = ID(K_MBTN, 1); break;
        case ON_HOSTLAN: modal = M_HOSTLAN; focus = ID(K_MBTN, 1); break;
        }
    }
    next_row(); *y += 76;
}
// a row inside a box: its label, then a choice of n at the right; returns the option picked this frame, or -1
static int box_choice(int kind, float x, float cy, const char *label, const char *const *opts, int n, int sel, const char *tip) {
    text(UI_M20, x + 20, mid(UI_M20, cy, 38), label, MUTED);
    const int row = ID(kind, 9); hit(row, (int)x, (int)cy, (int)COLW, 38); focusable(row, F_SEG, (int)(x + COLW / 2)); say_help(row, tip);
    float w[4], total = 6;
    for (int i = 0; i < n; i++) { w[i] = width(UI_B16, opts[i]) + 28; if (w[i] < 72) w[i] = 72; total += w[i]; }
    const float sx = x + COLW - 20 - total; rrect(sx, cy + 3, total, 32, 10, WELL); rline(sx, cy + 3, total, 32, 10, 2, LINE);
    int picked = -1; float ox = sx + 3;
    for (int i = 0; i < n; i++) {
        const int id = ID(kind, i); hit(id, (int)ox, (int)cy + 6, (int)w[i], 26);
        if (i == sel) rrect(ox, cy + 6, w[i], 26, 8, ACCENT); else if (is_hover(id)) rrect(ox, cy + 6, w[i], 26, 8, HOVER);
        text_fit(UI_B16, ox, mid(UI_B16, cy + 6, 26), w[i], opts[i], i == sel ? INK : TEXT, 1);
        if (clicked(id)) picked = i;
        ox += w[i];
    }
    if (is_focus(row)) rline(x + 8, cy, COLW - 16, 38, 10, 2, ACCENT);
    if (pass == PASS_ACT && ev_adjust == row) picked = (sel + ev_adjust_dir + n) % n;
    if (pass == PASS_ACT && stick_mode && ev_click == row) picked = (sel + 1) % n;
    next_row();
    return picked;
}
static float voice_box(float x, float y, const char *title) {
    const bool on = V.voice_mode != 0, partner = V.state == OV_LINKED || V.state == OV_CATCHING_UP;
    const int rows = on ? (partner ? 4 : 3) : 1, notes = (on && V.mic_denied) || !on ? 1 : 0;
    const float h = 52 + rows * 38 + notes * 23;
    float cy = box(x, y, COLW, h, title, MUTED, LINE);
    {
        static const char *const modes[] = { "OPEN MIC", "PUSH TO TALK", "OFF" };
        const int mode = V.voice_mode == 0 ? 2 : V.voice_mode == 2 ? 1 : 0;
        const int m = box_choice(K_VOICE, x, cy, "Voice chat", modes, 3, mode,
            "OPEN MIC sends when you speak. PUSH TO TALK sends while you hold the stick click of the hand without the gun (a gun in each: the left). Echo and noise are filtered out.");
        if (m >= 0 && m != mode) online_set_voice_mode(m == 2 ? 0 : m == 1 ? 2 : 1);
    }
    cy += 38;
    if (on) {
        text(UI_M20, x + 20, mid(UI_M20, cy, 38), "You", MUTED);
        const float r = x + COLW - 20;
        if (V.mic_denied) text_fit(UI_B20, r - 200, mid(UI_B20, cy, 38), 200, "NO MIC", WARN, 2);
        else if (V.voice_mode == 2) {
            const char *hold = V.talking ? "TALKING" : H->get(QOPT_HAND) == QHAND_LEFT ? "hold RIGHT STICK" : "hold LEFT STICK";
            text_fit(V.talking ? UI_B20 : UI_M17, r - 108 - 230, mid(V.talking ? UI_B20 : UI_M17, cy, 38), 220, hold, V.talking ? GOOD : MUTED, 2);
            meter(r, cy, V.talking ? V.level : 0, GOOD);
        } else {
            const int id = ID(K_ON, ON_MUTE);
            if (button(id, r - 96, cy + 4, 96, 30, V.muted ? "UNMUTE" : "MUTE", UI_B16, V.muted ? 1 : 2, "Mute or unmute your microphone."))
                online_toggle_mute();
            focusable(id, F_BUTTON, (int)(r - 48)); next_row();
            if (V.muted) text_fit(UI_B20, r - 300, mid(UI_B20, cy, 38), 190, "MUTED", WARN, 2); else meter(r - 108, cy, V.level, GOOD);
        }
        cy += 38;
        if (partner) {
            text(UI_M20, x + 20, mid(UI_M20, cy, 38), "Partner", MUTED);
            meter(x + COLW - 20, cy, V.partner_level, GOOD);
            cy += 38;
        }
        const int row = ID(K_VOLROW, 0);
        text(UI_M20, x + 20, mid(UI_M20, cy, 38), "Partner volume", MUTED);
        hit(row, (int)x, (int)cy, (int)COLW - 170, 38); focusable(row, F_SLIDER, (int)(x + COLW / 2)); say_help(row, "How loud your partner is. Over 100% boosts a quiet microphone.");
        if (is_focus(row)) rline(x + 8, cy, COLW - 16, 38, 10, 2, ACCENT);
        const float sx = x + COLW - 20 - 152;
        if (step_button(ID(K_VOL, 0), sx, cy + 1, 40, 36, true)) H->set(QOPT_VOICE, V.volume - 10);
        char vol[12]; snprintf(vol, sizeof vol, "%d%%", V.volume); text_fit(UI_B20, sx + 40, mid(UI_B20, cy, 38), 72, vol, TEXT, 1);
        if (step_button(ID(K_VOL, 1), sx + 112, cy + 1, 40, 36, false)) H->set(QOPT_VOICE, V.volume + 10);
        if (pass == PASS_ACT && ev_adjust == row) H->set(QOPT_VOICE, V.volume + ev_adjust_dir * 10);
        next_row();
        cy += 38;
        if (V.mic_denied) para(UI_M15, x + 20, cy + 16, COLW - 40, 21, "Allow the microphone: Quest Settings > Apps > Permissions", WARN, 1);
    } else para(UI_M15, x + 20, cy + 16, COLW - 40, 21, "Voice chat is off: you neither hear nor send.", MUTED, 1);
    return y + h + 8;
}
static void online_offline(void) {
    float y = CY0;
    on_item(ON_HOST, CX0, &y, I_HOUSE, "HOST A ROOM", "A code to share \xC2\xB7 you play P1",
            "The app restarts into an online game (about 10 s) and the game starts from its title. Friends join with your code at any time.", 0);
    on_item(ON_CODE, CX0, &y, I_KEYS, "JOIN WITH CODE", "Your friend's 4-letter code \xC2\xB7 you play P2",
            "A keyboard for the code. Then the app restarts and catches up with your friend's game.", 0);
    on_item(ON_BROWSE, CX0, &y, I_LIST, "BROWSE GAMES", "Open rooms and games on your network", "Lists open rooms on the lobby server and games hosted on your network.", 0);
    on_item(ON_QUICK, CX0, &y, I_BOLT, "QUICK MATCH", "Join anyone who is waiting", "Joins the first open room on the lobby server.", 0);
    on_item(ON_LAN, CX0, &y, I_SWAP, "LOCAL NETWORK\xE2\x80\xA6", "Host here, or join by IP", "Host or join on your own network, without the lobby server.", 0);
    // right column: you, voice
    frow = 1;   // beside the left column's rows: up / down walks each column
    float ry = CY0;
    const bool msg = V.message[0] && (V.state == OV_FINDING || V.state == OV_FAILED);
    const float h = 52 + 2 * 38 + (V.state == OV_FINDING || msg ? 38 : 0);
    float cy = box(COL2, ry, COLW, h, "YOU", MUTED, LINE);
    text(UI_M20, COL2 + 20, mid(UI_M20, cy, 38), "Name", MUTED);
    {
        const int id = ID(K_ON, ON_NAME); const float bx = COL2 + COLW - 20 - 96;
        if (drawing()) qc_text_fit(&cv, UI_B20, bx - 260, mid(UI_B20, cy, 38), 250, V.name, TEXT, 2);
        if (button(id, bx, cy + 4, 96, 30, "CHANGE", UI_B16, 2, "Your name in the lobby and on your partner's menu.")) { modal = M_NAME; snprintf(kbuf, sizeof kbuf, "%s", V.name); focus = ID(K_KEY, 0); }
        focusable(id, F_BUTTON, (int)bx + 48); next_row();
    }
    text(UI_M20, COL2 + 20, mid(UI_M20, cy + 38, 38), "Server", MUTED);
    {
        const int id = ID(K_ON, ON_SERVER); const float bx = COL2 + COLW - 20 - 96;
        if (drawing()) qc_text_fit(&cv, UI_M17, COL2 + 100, mid(UI_M17, cy + 38, 38), COLW - 226, V.server, TEXT, 2);
        if (button(id, bx, cy + 42, 96, 30, "CHANGE", UI_B16, 2, "The lobby server. Both players must use the same one.")) {
            modal = M_SERVER; snprintf(kbuf, sizeof kbuf, "%s", V.server); kerror = NULL; klower = true; focus = ID(K_KEY, 0);
        }
        focusable(id, F_BUTTON, (int)bx + 48); next_row();
    }
    if (V.state == OV_FINDING) kv(COL2 + 20, cy + 76, COLW - 40, "Joining", "looking for the room\xE2\x80\xA6", ACCENT);
    else if (msg) kv(COL2 + 20, cy + 76, COLW - 40, "Last try", V.message, WARN);
    ry += h + 8;
    voice_box(COL2, ry, "VOICE CHAT");
}
static void online_browse_page(void) {
    if (button(ID(K_ON, ON_BACK), CX0, CY0 + 8, 104, 34, "BACK", UI_B16, 2, "Back to the ONLINE menu. B does the same.")) { screen = S_MAIN; online_browse(false); }
    focusable(ID(K_ON, ON_BACK), F_BUTTON, CX0 + 52);
    int rooms = 0;
    for (int i = 0; i < game_count; i++) rooms += !games[i].lan;
    char title[160];
    if (V.browse_answered) snprintf(title, sizeof title, "%s \xC2\xB7 %d rooms \xC2\xB7 %d on your network", V.server_name[0] ? V.server_name : V.server, rooms, game_count - rooms);
    else snprintf(title, sizeof title, V.browse_busy ? "Asking %s\xE2\x80\xA6" : "%s does not answer \xC2\xB7 %d on your network", V.server, game_count - rooms);
    const int pages = game_count > 0 ? (game_count + 4) / 5 : 1;
    if (page >= pages) page = pages - 1;
    if (page < 0) page = 0;
    text_fit(UI_M17, CX0 + 118, CY0 + 31, CW - 118 - 310, title, MUTED, 0);
    char pg[16]; snprintf(pg, sizeof pg, "%d / %d", page + 1, pages);
    const float px = CX1 - 140 - 12 - 150;
    if (step_button(ID(K_ON, ON_PREV), px, CY0 + 6, 40, 36, true)) page = (page + pages - 1) % pages;
    text_fit(UI_B20, px + 40, CY0 + 31, 70, pg, TEXT, 1);
    if (step_button(ID(K_ON, ON_NEXT), px + 110, CY0 + 6, 40, 36, false)) page = (page + 1) % pages;
    focusable(ID(K_ON, ON_NEXT), F_SLIDER, (int)px + 75);
    say_help(ID(K_ON, ON_NEXT), "Pages of five. Stick left / right turns the page.");
    if (pass == PASS_ACT && ev_adjust == ID(K_ON, ON_NEXT)) page = (page + ev_adjust_dir + pages) % pages;
    if (button(ID(K_ON, ON_REFRESH), CX1 - 140, CY0 + 8, 140, 34, "REFRESH", UI_B16, 2, "Asks the server again.")) online_refresh();
    focusable(ID(K_ON, ON_REFRESH), F_BUTTON, CX1 - 70);
    next_row();
    float y = CY0 + 58;
    if (!game_count) { para(UI_M20, CX0 + 20, y + 40, CW - 40, 26, V.browse_busy || V.discovering ? "Looking for games\xE2\x80\xA6" : "No games yet - host one!", MUTED, 1); return; }
    for (int i = page * 5; i < game_count && i < page * 5 + 5; i++) {
        const online_game *g = &games[i]; const int id = ID(K_GAME, i); const bool ok = g->status == OG_JOIN;
        hit(id, CX0, (int)y, CW, 54);
        if (ok) { focusable(id, F_BUTTON, CX0 + CW / 2); say_help(id, "Join this game as P2: the app restarts and catches up with it."); }
        else say_help(id, g->status == OG_OLD ? "That room runs another version of the game." : "That room is full.");
        const bool hot = ok && (is_hover(id) || is_focus(id));
        rrect(CX0, y, CW, 54, 12, hot ? HOVER : ITEM); rline(CX0, y, CW, 54, 12, 2, hot ? ACCENT : LINE);
        const QColor ink = ok ? TEXT : DIM;
        icon(g->lan ? I_HOUSE : I_CLOUD, CX0 + 30, y + 27, 20, ok ? EDGE : DIM);
        const int nw = text(UI_B22, CX0 + 56, mid(UI_B22, y, 54), g->name, ink);
        if (g->detail[0]) text_fit(UI_M15, CX0 + 56 + nw + 12, mid(UI_M15, y, 54), CW - 56 - nw - 12 - 230, g->detail, MUTED, 0);
        static const char *const st[] = { "JOIN", "PLAYING", "FULL", "OLD" };
        const QColor bg = ok ? GOOD : g->status == OG_OLD ? WARNBG : RAIL, fg = ok ? GOODINK : g->status == OG_OLD ? WARN : MUTED;
        rrect(CX1 - 16 - 110, y + 14, 110, 26, 13, bg); text_fit(UI_B16, CX1 - 16 - 110, mid(UI_B16, y + 14, 26), 110, st[g->status], fg, 1);
        if (ok && clicked(id)) { online_join_game(i); screen = S_MAIN; online_browse(false); }
        if (ok) next_row();
        y += 60;
    }
}
static void online_lan_page(void) {
    if (button(ID(K_ON, ON_BACK), CX0, CY0 + 8, 104, 34, "BACK", UI_B16, 2, "Back to the ONLINE menu. B does the same.")) { screen = S_MAIN; online_browse(false); }
    focusable(ID(K_ON, ON_BACK), F_BUTTON, CX0 + 52);
    text(UI_B20, CX0 + 118, CY0 + 32, "LOCAL NETWORK", TEXT);
    text(UI_M17, CX0 + 118 + width(UI_B20, "LOCAL NETWORK") + 10, CY0 + 32, "\xC2\xB7 no lobby server needed", MUTED);
    next_row();
    float y = CY0 + 58;
    on_item(ON_HOSTLAN, CX0, &y, I_HOUSE, "HOST ON THIS NETWORK", "Friends on your Wi-Fi or VPN join you",
            "The app restarts into an online game hosted on this headset; others on your network find it here.", 0);
    int lan[3], n = 0;
    for (int i = 0; i < game_count && n < 3; i++) if (games[i].lan) lan[n++] = i;
    const float bh = 52 + (n ? n * 48 + (n - 1) * 6 : 0) + 23;
    float cy = box(CX0, y, COLW, bh, "FOUND ON YOUR NETWORK", MUTED, LINE);
    for (int k = 0; k < n; k++) {
        const online_game *g = &games[lan[k]]; const int id = ID(K_GAME, lan[k]); const float ry = cy + k * 54;
        hit(id, CX0 + 18, (int)ry, COLW - 36, 48); focusable(id, F_BUTTON, CX0 + COLW / 2); say_help(id, "Join this game as P2: the app restarts and catches up with it.");
        const bool hot = is_hover(id) || is_focus(id);
        rrect(CX0 + 18, ry, COLW - 36, 48, 12, hot ? HOVER : ITEM); rline(CX0 + 18, ry, COLW - 36, 48, 12, 2, hot ? ACCENT : LINE);
        icon(I_HOUSE, CX0 + 44, ry + 24, 18, EDGE);
        const int nw = text(UI_B22, CX0 + 66, mid(UI_B22, ry, 48), g->name, TEXT);
        text_fit(UI_M15, CX0 + 66 + nw + 10, mid(UI_M15, ry, 48), COLW - 36 - 66 - nw - 10 - 110, g->detail, MUTED, 0);
        rrect(CX0 + COLW - 18 - 16 - 80, ry + 12, 80, 24, 12, GOOD); text_fit(UI_B16, CX0 + COLW - 18 - 16 - 80, mid(UI_B16, ry + 12, 24), 80, "JOIN", GOODINK, 1);
        if (clicked(id)) { online_join_game(lan[k]); screen = S_MAIN; online_browse(false); }
        next_row();
    }
    para(UI_M15, CX0 + 20, cy + (n ? n * 54 : 0) + 14, COLW - 40, 21, V.discovering ? "Searching\xE2\x80\xA6" : n ? "" : "Nobody found yet. Searching again every few seconds.", MUTED, 1);
    frow = 1;
    const float h = 52 + 52 + 46 + 54;
    float iy = box(COL2, CY0 + 58, COLW, h, "JOIN BY IP", MUTED, LINE);
    {
        char a[40]; snprintf(a, sizeof a, "%s", V.join_ip[0] ? V.join_ip : "-");
        const int w = width(UI_B44, a), pw = width(UI_M20, ":27750");
        text(UI_B44, COL2 + (COLW - w - pw - 6) / 2, iy + 42, a, TEXT); text(UI_M20, COL2 + (COLW - w - pw - 6) / 2 + w + 6, iy + 42, ":27750", MUTED);
    }
    para(UI_M15, COL2 + 20, iy + 52 + 18, COLW - 40, 21, "For a friend over Tailscale / ZeroTier, or one who doesn't show up on the left.", MUTED, 2);
    const float by = iy + 52 + 46 + 10;
    if (button(ID(K_ON, ON_IPEDIT), COL2 + 20, by, 200, 44, "EDIT ADDRESS", UI_B20, 0, "A number pad for the address.")) { modal = M_IP; snprintf(kbuf, sizeof kbuf, "%s", V.join_ip); focus = ID(K_KEY, 0); }
    focusable(ID(K_ON, ON_IPEDIT), F_BUTTON, COL2 + 120);
    if (button(ID(K_ON, ON_JOINIP), COL2 + 232, by, 110, 44, "JOIN", UI_B20, 1, "Connects to that address as P2: the app restarts and catches up with that game.")) {
        if (V.join_ip[0]) { online_join_ip(); screen = S_MAIN; online_browse(false); }
    }
    focusable(ID(K_ON, ON_JOINIP), F_BUTTON, COL2 + 287);
    next_row();
}
static void players_box(float x, float y) {
    float cy = box(x, y, COLW, 52 + 2 * 38, "PLAYERS", MUTED, LINE);
    const bool p2 = V.partner[0] && (V.host ? V.state == OV_LINKED : true);
    for (int i = 0; i < 2; i++) {
        char who[48];
        const bool me = V.host ? i == 0 : i == 1;
        snprintf(who, sizeof who, "%s%s", me ? V.name : V.partner, me ? " (you)" : "");
        text(UI_M20, x + 20, mid(UI_M20, cy + i * 38, 38), i ? "P2" : "P1", MUTED);
        if (me || p2) text_fit(UI_B20, x + 120, mid(UI_B20, cy + i * 38, 38), COLW - 140, who, TEXT, 2);
        else text_fit(UI_M20, x + 120, mid(UI_M20, cy + i * 38, 38), COLW - 140, V.host ? "waiting for a partner\xE2\x80\xA6" : "connecting\xE2\x80\xA6", MUTED, 2);
    }
}
static void online_session_page(void) {
    float y = CY0;
    if (V.host && !V.lan) {
        const bool open = V.state != OV_CONNECTING;
        float cy = box(CX0, y, COLW, 52 + 84 + 46, !open ? "OPENING YOUR ROOM\xE2\x80\xA6" : V.state == OV_LINKED ? "ROOM \xC2\xB7 PLAYING TOGETHER" : "ROOM OPEN \xC2\xB7 YOUR CODE",
                       open ? GOOD : ACCENT, LINE);
        { const int w = qc_text_width(UI_B76, V.code, 22); text_track(UI_B76, CX0 + (COLW - w) / 2, cy + 72, V.code, ACCENT, 22); }
        para(UI_M15, CX0 + 20, cy + 84 + 18, COLW - 40, 21, "Your friend: ONLINE tab, JOIN WITH CODE, or find you in BROWSE GAMES. They can join at any time.", MUTED, 2);
        players_box(CX0, y + 52 + 84 + 46 + 8);
    } else if (V.host) {
        const int rows = V.address_count > 0 ? V.address_count : 1;
        float cy = box(CX0, y, COLW, 52 + rows * 38 + 46, "HOSTING ON THIS NETWORK \xC2\xB7 PORT 27750", GOOD, LINE);
        for (int i = 0; i < rows; i++) kv(CX0 + 20, cy + i * 38, COLW - 40, "Address", V.address_count ? V.address[i] : "no network", TEXT);
        para(UI_M15, CX0 + 20, cy + rows * 38 + 18, COLW - 40, 21, "Friends on your Wi-Fi find you in LOCAL NETWORK. Over a VPN they use JOIN BY IP.", MUTED, 2);
        players_box(CX0, y + 52 + rows * 38 + 46 + 8);
    } else {
        const char *t = V.state == OV_CONNECTING ? "CONNECTING TO THE HOST\xE2\x80\xA6" : V.state == OV_CATCHING_UP ? "CATCHING UP WITH THE HOST'S GAME" :
                        V.state == OV_PARTNER_GONE ? "THE HOST IS NOT ANSWERING" : V.desync ? "OUT OF STEP WITH THE HOST" : "PLAYING TOGETHER";
        const bool bad = V.state == OV_PARTNER_GONE || V.desync;
        float cy = box(CX0, y, COLW, 52 + 4 * 38, t, bad ? WARN : V.state == OV_LINKED ? GOOD : ACCENT, bad ? WARNLINE : LINE);
        kv(CX0 + 20, cy, COLW - 40, "You", "P2", GOOD);
        kv(CX0 + 20, cy + 38, COLW - 40, "Host", V.partner[0] ? V.partner : "-", TEXT);
        char ping[48];
        if (V.state == OV_CATCHING_UP) snprintf(ping, sizeof ping, "%d%% of their game run", V.catch_up_pct);
        else if (V.ping_ms >= 0) snprintf(ping, sizeof ping, "%d ms to the server", V.ping_ms);
        else snprintf(ping, sizeof ping, "-");
        kv(CX0 + 20, cy + 76, COLW - 40, V.state == OV_CATCHING_UP ? "Caught up" : "Ping", ping, TEXT);
        kv(CX0 + 20, cy + 114, COLW - 40, V.lan ? "Network" : "Room", V.lan ? "local" : V.code[0] ? V.code : "-", ACCENT);
        float ny = box(CX0, y + 52 + 4 * 38 + 8, COLW, 52 + 46, "ONE GAME, TWO PLAYERS", MUTED, LINE);
        para(UI_M15, CX0 + 20, ny + 16, COLW - 40, 21, bad ? "Leave and join again: the app restarts and catches up with the host's game from its start." :
             "The host's game runs on both headsets: your shots go to it and come back a moment later.", MUTED, 2);
    }
    frow = 0; next_row();
    float ry = voice_box(COL2, CY0, "VOICE CHAT");
    on_item(ON_LEAVE, COL2, &ry, I_EJECT, V.host ? (V.lan ? "STOP HOSTING" : "CLOSE ROOM") : "LEAVE ONLINE GAME",
            V.host ? "Back to playing alone" : "Your partner plays on", "The app restarts and you play alone again.", 2);
}
static void online_tab(void) {
    if (online_now()) { if (screen != S_MAIN) { screen = S_MAIN; online_browse(false); } online_session_page(); return; }
    if (screen == S_BROWSE) online_browse_page(); else if (screen == S_LAN) online_lan_page(); else online_offline();
}

// ---- dialogs ----
static void modal_frame(float w, float h, float *x, float *y) {
    if (drawing()) qc_rrect(&cv, 3, 3, QMENU_W - 6, QMENU_H - 6, 20, SHADE);
    *x = (QMENU_W - w) / 2; *y = (QMENU_H - h) / 2;
    rrect(*x, *y, w, h, 20, MODAL); rline(*x, *y, w, h, 20, 3, ACCENT);
}
static void modal_buttons(float x, float y, float w, const char *ok) {
    const float bw = 180;
    if (button(ID(K_MBTN, 0), x + w - 32 - bw * 2 - 14, y, bw, 52, "CANCEL", UI_B22, 0, "Back to the menu. B does the same.")) modal = M_NONE;
    focusable(ID(K_MBTN, 0), F_BUTTON, (int)(x + w - 32 - bw * 1.5f - 14));
    if (button(ID(K_MBTN, 1), x + w - 32 - bw, y, bw, 52, ok, UI_B22, 1, NULL)) {
        const int m = modal;
        modal = M_NONE;
        if (m == M_QUIT) H->action(QACT_QUIT);
        else if (m == M_HOST) online_host_room();
        else if (m == M_HOSTLAN) online_host_lan();
        else if (m == M_QUICK) online_quick();
        else if (m == M_LEAVE) online_leave();
        else if (m == M_CODE) { if (strlen(kbuf) == 4) online_join_code(kbuf); }
        else if (m == M_NAME) online_set_name(kbuf);
        else if (m == M_IP) online_set_join_ip(kbuf);
        else if (m == M_SERVER) { kerror = online_set_server(kbuf); if (kerror) modal = M_SERVER; }
    }
    focusable(ID(K_MBTN, 1), F_BUTTON, (int)(x + w - 32 - bw / 2));
    next_row();
}
static void confirm(void) {
    const float w = 660; float x, y;
    const char *title = "QUIT?", *body = "The app closes. The game in progress is lost.", *note = "Your settings, high scores and the game's options are kept.", *ok = "QUIT";
    if (modal == M_HOST || modal == M_HOSTLAN) {
        title = modal == M_HOST ? "HOST A ROOM?" : "HOST ON THIS NETWORK?";
        body = "The app restarts into an online game (about 10 s): the game starts from its title, and you play P1.";
        note = modal == M_HOST ? "Your room's code shows on this tab. Friends join with it at any time." : "Friends on your network find your game in LOCAL NETWORK.";
        ok = "HOST";
    } else if (modal == M_QUICK) {
        title = "QUICK MATCH?";
        body = "Joins the first open room on the lobby server: the app restarts and catches up with that game. You play P2.";
        note = "The game in progress here is lost.";
        ok = "JOIN";
    } else if (modal == M_LEAVE) {
        title = V.host ? "STOP PLAYING ONLINE?" : "LEAVE THE ONLINE GAME?";
        body = "The app restarts and you play alone again, from the game's title.";
        note = V.host ? "Your partner's game ends with yours." : "Your partner plays on. You can join again with the code.";
        ok = "LEAVE";
    }
    const float h = 26 + 40 + 12 + 2 * 28 + 2 * 24 + 8 + 20 + 52 + 26;
    modal_frame(w, h, &x, &y);
    text(UI_B32, x + 32, y + 26 + 30, title, ACCENT);
    float ty = y + 26 + 40 + 12 + 20;
    ty += para(UI_M20, x + 32, ty, w - 64, 28, body, TEXT, 2) * 28;
    para(UI_M17, x + 32, ty + 8, w - 64, 24, note, MUTED, 2);
    modal_buttons(x, y + h - 26 - 52, w, ok);
}
// The controls, as they are now: the gun hand, the reload gesture and the views' reloading set what each one does.
static void controls_row(float x, float *y, float w, const char *what, const char *does) {
    text(UI_B20, x, *y + 20, what, TEXT);
    para(UI_M17, x, *y + 42, w, 21, does, MUTED, 1);
    *y += 50;
}
static void controls_page_flat(void) {
    const float w = 1060, h = 560; float x, y; modal_frame(w, h, &x, &y);
    text(UI_B32, x + 32, y + 26 + 30, "CONTROLS", ACCENT);
    const float lx = x + 32, rx = x + 600, lw = 530, rw = w - 600 - 32;
    float ly = y + 96, ry = y + 96;
    if (H->touch) {
        text(UI_B20, lx, ly, "TOUCH", ACCENT); ly += 12;
        controls_row(lx, &ly, lw, "Tap the picture", "Shoot there. Off the picture: reload");
        controls_row(lx, &ly, lw, "A second finger, RELOAD", "Reload");
        controls_row(lx, &ly, lw, "START", "Coins and Start; skip a scene");
        controls_row(lx, &ly, lw, "MENU, Back", "This menu (the game pauses, not online)");
        text(UI_B20, rx, ry, "GAME PADS", ACCENT); ry += 12;
        controls_row(rx, &ry, rw, "Either stick", "Move the sight");
        controls_row(rx, &ry, rw, "R2 / R1", "Shoot");
        controls_row(rx, &ry, rw, "L2 / L1 / X", "Reload");
        controls_row(rx, &ry, rw, "A / Start", "Coins and Start");
        controls_row(rx, &ry, rw, "Select / Back", "This menu");
        const float bw = 180;
        if (button(ID(K_MBTN, 1), x + w - 32 - bw, y + h - 26 - 52, bw, 52, "CLOSE", UI_B22, 1, NULL)) modal = M_NONE;
        focusable(ID(K_MBTN, 1), F_BUTTON, (int)(x + w - 32 - bw / 2));
        next_row();
        return;
    }
    text(UI_B20, lx, ly, "MOUSE AND KEYS", ACCENT); ly += 12;
    controls_row(lx, &ly, lw, "Mouse", "Aim (a light gun that is a mouse to Windows works the same)");
    controls_row(lx, &ly, lw, "Left button", "Shoot. Off the picture: reload");
    controls_row(lx, &ly, lw, "Right button", "Reload");
    controls_row(lx, &ly, lw, "Space / Enter", "Coins and Start; skip a scene");
    controls_row(lx, &ly, lw, "Esc", "This menu (the game pauses, not online)");
    controls_row(lx, &ly, lw, "Alt+Enter / F11", "Full screen, or a window");
    controls_row(lx, &ly, lw, "T", "Online, PUSH TO TALK: talk while held");
    text(UI_B20, rx, ry, "IN THIS MENU", ACCENT); ry += 12;
    controls_row(rx, &ry, rw, "Click", "Choose what the pointer is on");
    controls_row(rx, &ry, rw, "Arrows, Enter, Esc", "Move, choose, back");
    controls_row(rx, &ry, rw, "Tab", "The next tab (Shift: the one before)");
    const float bw = 180;
    if (button(ID(K_MBTN, 1), x + w - 32 - bw, y + h - 26 - 52, bw, 52, "CLOSE", UI_B22, 1, NULL)) modal = M_NONE;
    focusable(ID(K_MBTN, 1), F_BUTTON, (int)(x + w - 32 - bw / 2));
    next_row();
}
static void controls_page(void) {
    if (H->flat) { controls_page_flat(); return; }
    const float w = 1060, h = 690; float x, y; modal_frame(w, h, &x, &y);
    text(UI_B32, x + 32, y + 26 + 30, "CONTROLS", ACCENT);
    const int gun = H->get(QOPT_HAND), gest = H->get(QOPT_GESTURE);
    const char *gh = gun == QHAND_LEFT ? "Left" : "Right", *oh = gun == QHAND_LEFT ? "right" : "left";
    char b[3][96];
    const float lx = x + 32, rx = x + 600, lw = 530, rw = w - 600 - 32;
    float ly = y + 96, ry = y + 96;
    text(UI_B20, lx, ly, "CONTROLLERS", ACCENT); ly += 12;
    controls_row(lx, &ly, lw, "Trigger", "Shoot. Off the screen: reload (the cabinet view, or Reload gesture OFF)");
    if (gest == QGESTURE_MAGAZINE && gun != QHAND_BOTH)
        controls_row(lx, &ly, lw, "Reload: the magazine", "Other hand's grip: a magazine. Into the gun's grip, then pull the slide");
    else if (gest == QGESTURE_DOWN) controls_row(lx, &ly, lw, "Reload: point down", "Aim the gun at the floor");
    else if (gest == QGESTURE_MAGAZINE) controls_row(lx, &ly, lw, "Reload: two guns", "Knock the grips together");
    else controls_row(lx, &ly, lw, "Reload", "Shoot off the screen, B / Y, or a firm grip squeeze (Grip reloads ON)");
    controls_row(lx, &ly, lw, "B / Y", gest == QGESTURE_MAGAZINE ? "Drop the magazine (then a new one in, and the slide)" : "Reload (Reload gesture OFF)");
    controls_row(lx, &ly, lw, "A / X", "Coins and Start; skip a scene");
    controls_row(lx, &ly, lw, "Menu button (left)", "This menu (the game pauses, not online)");
    snprintf(b[0], sizeof b[0], "%s stick click, held", gun == QHAND_BOTH ? "Right" : gh);
    controls_row(lx, &ly, lw, b[0], "Switch: in the game / the cabinet");
    snprintf(b[1], sizeof b[1], "%s stick click", gun == QHAND_BOTH ? "Left" : oh[0] == 'l' ? "Left" : "Right");
    controls_row(lx, &ly, lw, b[1], "Online, PUSH TO TALK: talk while held");
    if (gun != QHAND_BOTH) controls_row(lx, &ly, lw, "Other hand's grip, at the gun", "Hold the gun in both hands");
    controls_row(lx, &ly, lw, "Both grips, hands apart", "The cabinet view: move, turn and size the cabinet");
    text(UI_B20, rx, ry, "YOUR HANDS (no controllers)", ACCENT); ry += 12;
    controls_row(rx, &ry, rw, "Point", "Aim");
    controls_row(rx, &ry, rw, "Index finger pinch", "Shoot. Off the screen: reload");
    snprintf(b[2], sizeof b[2], "%s hand's middle finger pinch", gun == QHAND_BOTH ? "Left" : oh[0] == 'l' ? "Left" : "Right");
    controls_row(rx, &ry, rw, b[2], "Coins and Start; skip a scene");
    controls_row(rx, &ry, rw, "Left palm to you, pinch", "This menu");
    ry += 12;
    text(UI_B20, rx, ry, "IN THIS MENU", ACCENT); ry += 12;
    controls_row(rx, &ry, rw, "Trigger / pinch", "Choose what the laser points at");
    controls_row(rx, &ry, rw, "Sticks, A / X, B / Y", "Move, choose, back");
    controls_row(rx, &ry, rw, "Grips", "The tab before / after");
    const float bw = 180;
    if (button(ID(K_MBTN, 1), x + w - 32 - bw, y + h - 26 - 52, bw, 52, "CLOSE", UI_B22, 1, NULL)) modal = M_NONE;
    focusable(ID(K_MBTN, 1), F_BUTTON, (int)(x + w - 32 - bw / 2));
    next_row();
}
static void keyboard(void) {
    static const char code_keys[] = "ACDEFGHJKLMNPQRTUVWXY34679", name_keys[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_", ip_keys[] = "123456789.0",
        server_keys[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.:", server_lower_keys[] = "abcdefghijklmnopqrstuvwxyz0123456789-.:";
    const bool server_edit = modal == M_SERVER;
    const char *keys = modal == M_CODE ? code_keys : modal == M_NAME ? name_keys : server_edit ? (klower ? server_lower_keys : server_keys) : ip_keys;
    const int nkeys = (int)strlen(keys), cols = modal == M_IP ? 3 : 9, len = modal == M_CODE ? 4 : modal == M_NAME ? 12 : server_edit ? 63 : 15;
    const int cells = nkeys + (modal == M_IP ? 1 : 2), rows = (cells + cols - 1) / cols;
    const float w = modal == M_CODE ? 720 : modal == M_NAME || server_edit ? 860 : 560, keyw = modal == M_IP ? 132 : (w - 64 - (cols - 1) * 8) / cols;
    const float slot_h = modal == M_CODE ? 86 : modal == M_NAME ? 62 : 72;
    const float h = 26 + 40 + 10 + slot_h + 16 + rows * 62 + 18 + 52 + 26 + (server_edit ? 24 : 0);
    float x, y; modal_frame(w, h, &x, &y);
    text(UI_B32, x + 32, y + 26 + 30, modal == M_CODE ? "JOIN WITH CODE" : modal == M_NAME ? "YOUR NAME" : server_edit ? "LOBBY SERVER" : "JOIN BY IP", ACCENT);
    const float sy = y + 26 + 40 + 10; const int n = (int)strlen(kbuf);
    if (server_edit) {   // (two lines of 32)
        rrect(x + 32, sy, w - 64, slot_h, 14, WELL); rline(x + 32, sy, w - 64, slot_h, 14, 3, ACCENT);
        char first[33]; snprintf(first, sizeof first, "%.32s", kbuf);
        text(UI_B24, x + 48, sy + 29, first, TEXT);
        text(UI_B24, x + 48, sy + 59, n > 32 ? kbuf + 32 : "", TEXT);
    } else if (modal == M_IP) {
        rrect(x + 32, sy, w - 64, slot_h, 14, WELL); rline(x + 32, sy, w - 64, slot_h, 14, 3, ACCENT);
        const int aw = width(UI_B44, kbuf), pw = width(UI_M20, ":27750");
        text(UI_B44, x + (w - aw - pw - 8) / 2, sy + 52, kbuf, TEXT); text(UI_M20, x + (w - aw - pw - 8) / 2 + aw + 8, sy + 52, ":27750", MUTED);
    } else {
        const float sw = modal == M_CODE ? 72 : 46, gap = modal == M_CODE ? 10 : 8, total = len * sw + (len - 1) * gap;
        for (int i = 0; i < len; i++) {
            const float bx = x + (w - total) / 2 + i * (sw + gap); const bool cur = i == n;
            rrect(bx, sy, sw, slot_h, 14, WELL); rline(bx, sy, sw, slot_h, 14, 3, cur ? ACCENT : LINE);
            if (i < n) { char c[2] = { kbuf[i], 0 }; const int f = modal == M_CODE ? UI_B44 : UI_B27; text_fit(f, bx, mid(f, sy, slot_h), sw, c, TEXT, 1); }
        }
    }
    const float ky = sy + slot_h + 16, kx = modal == M_IP ? x + (w - 3 * keyw - 16) / 2 : x + 32;
    for (int i = 0; i < cells; i++) {
        const bool del = i >= nkeys;
        if (modal != M_IP && i == nkeys + 1) break;   // DEL spans two cells
        const int col = i % cols, row = i / cols; const float bx = kx + col * (keyw + 8), by = ky + row * 62, bw = del && modal != M_IP ? keyw * 2 + 8 : keyw;
        const int id = ID(K_KEY, i);
        if (col == 0 && i) next_row();
        hit(id, (int)bx, (int)by, (int)bw, 54); focusable(id, F_BUTTON, (int)(bx + bw / 2));
        const bool hot = is_hover(id) || is_focus(id);
        rrect(bx, by, bw, 54, 12, hot ? HOVER : WELL); rline(bx, by, bw, 54, 12, 2, hot ? ACCENT : LINE);
        char label[8];
        if (del) snprintf(label, sizeof label, "DEL"); else { label[0] = keys[i]; label[1] = 0; }
        text_fit(del ? UI_B20 : UI_B24, bx, mid(del ? UI_B20 : UI_B24, by, 54), bw, label, hot ? ACCENT : TEXT, 1);
        if (clicked(id)) {
            kerror = NULL;
            if (del) { if (n > 0) kbuf[n - 1] = 0; }
            else if (n < len) { kbuf[n] = keys[i]; kbuf[n + 1] = 0; }
        }
    }
    if (server_edit) {   // beside DEL: the letters' case
        const float bx = kx + 5 * (keyw + 8), by = ky + (rows - 1) * 62, bw = keyw * 2 + 8;
        if (button(ID(K_MBTN, 4), bx, by, bw, 54, klower ? "ABC" : "abc", UI_B24, 0, "Switch between lowercase and uppercase letters.")) klower = !klower;
        focusable(ID(K_MBTN, 4), F_BUTTON, (int)(bx + bw / 2));
    }
    next_row();
    if (server_edit) {
        text_fit(UI_M17, x + 32, y + h - 26 - 52 - 10, w - 64, kerror ? kerror : "Hostname or IPv4, optional :port. Both players use the same server.",
                 kerror ? WARN : MUTED, 0);
        if (button(ID(K_MBTN, 2), x + 32, y + h - 26 - 52, 180, 52, "DEFAULT", UI_B22, 0, "The community server's address; SAVE applies it.")) {
            snprintf(kbuf, sizeof kbuf, "%s", online_default_server()); kerror = NULL;
        }
        focusable(ID(K_MBTN, 2), F_BUTTON, (int)x + 122);
        if (button(ID(K_MBTN, 3), x + 32 + 194, y + h - 26 - 52, 160, 52, "CLEAR", UI_B22, 0, "Clears the address, to enter a new one.")) { kbuf[0] = 0; kerror = NULL; }
        focusable(ID(K_MBTN, 3), F_BUTTON, (int)x + 306);
    }
    modal_buttons(x, y + h - 26 - 52, w, modal == M_CODE ? "JOIN" : "SAVE");
}

// ---- the whole menu ----
// The panel's background, frame and body never change: drawn once, then copied under every redraw (inside the limit).
static uint8_t base_px[QMENU_W * QMENU_H * 4]; static bool base_ready;
static void restore_base(void) {
    if (!base_ready) {
        QCanvas b; qc_init(&b, base_px, QMENU_W, QMENU_H);
        qc_fill(&b, qrgba(54, 183, 207, 0));
        qc_rrect(&b, 0, 0, QMENU_W, QMENU_H, 22, PANEL);
        qc_rrect(&b, 3, BODY_Y, QMENU_W - 6, BODY_H, 0, BODY);
        qc_rrect_line(&b, 0, 0, QMENU_W, QMENU_H, 22, 3, EDGE);
        base_ready = true;
    }
    for (int y = cv.ly0; y < cv.ly1; y++) memcpy(cv.px + ((size_t)y * QMENU_W + cv.lx0) * 4, base_px + ((size_t)y * QMENU_W + cv.lx0) * 4, (size_t)(cv.lx1 - cv.lx0) * 4);
}
static void layout(int which) {
    pass = which; nhits = 0; nfoci = 0; frow = 0;
    if (H->flat)
        snprintf(help, sizeof help, "%s", stick_mode ? "Arrows: move and change \xC2\xB7 Enter: select \xC2\xB7 Esc: back \xC2\xB7 Tab: tabs"
                                          : H->touch ? "Tap anything." : "Point at anything and click.");
    else
        snprintf(help, sizeof help, "%s", stick_mode ? "Stick: move and change \xC2\xB7 A: select \xC2\xB7 B: back \xC2\xB7 grips: tabs" : "Point at anything and pull the trigger.");
    if (drawing()) restore_base();
    const bool m = modal != M_NONE;
    const int saved_click = ev_click, saved_adjust = ev_adjust, saved_drag = ev_drag;
    if (m) { ev_click = ev_adjust = ev_drag = 0; }   // the page under a dialog is inert
    header(); tabs();
    if (drawing()) qc_clip(&cv, 3, BODY_Y, QMENU_W - 6, BODY_H);
    if (tab == QMENU_TAB_GUN) settings(GUN_ROWS, sizeof GUN_ROWS / sizeof *GUN_ROWS);
    else if (tab == QMENU_TAB_HANDS) settings(HANDS_ROWS, sizeof HANDS_ROWS / sizeof *HANDS_ROWS);
    else if (tab == QMENU_TAB_VIEW) {
        if (H->get(QOPT_VIEW) == QVIEW_CABINET) settings(VIEW_CAB_ROWS, sizeof VIEW_CAB_ROWS / sizeof *VIEW_CAB_ROWS);
        else settings(VIEW_ROWS, sizeof VIEW_ROWS / sizeof *VIEW_ROWS);
    }
    else if (tab == QMENU_TAB_COMFORT) settings(COMFORT_ROWS, sizeof COMFORT_ROWS / sizeof *COMFORT_ROWS);
    // (Display, the last: a window's; a touch screen has none)
    else if (tab == QMENU_TAB_GRAPHICS && H->flat) settings(PICTURE_ROWS, sizeof PICTURE_ROWS / sizeof *PICTURE_ROWS - (H->touch ? 1 : 0));
    else if (tab == QMENU_TAB_GRAPHICS) settings(GRAPHICS_ROWS, sizeof GRAPHICS_ROWS / sizeof *GRAPHICS_ROWS);
    else if (tab == QMENU_TAB_SYSTEM) {
        // (crash reports: the Quest's only, its last row)
        const int n = sizeof SYSTEM_ROWS / sizeof *SYSTEM_ROWS - (H->flat ? 1 : 0);
        settings_in(SYSTEM_ROWS, n, n + 1);   // (its ROM button below them: a sixth slot)
        const float by = CY0 + row_step(n + 1) * n + 6;
        // the game's own ROM set: another one (a different revision, a fresh copy)
        if (button(ID(K_ROW, QOPT_COUNT), CX0, by, 300, 48, "CHANGE ROM SET", UI_B20, 0,
                   "Choose your MAME ROM set of the game (hotd.zip or hotd.7z, Revision A). The app restarts with it."))
            H->action(QACT_ARCADE_IMPORT);
        next_row();
    }
    else online_tab();
    if (drawing()) qc_unclip(&cv);
    frow = 1000; footer();
    if (m) {
        ev_click = saved_click; ev_adjust = saved_adjust; ev_drag = saved_drag;
        nhits = 0; nfoci = 0; frow = 0;   // only the dialog takes input
        if (modal == M_CONTROLS) controls_page();
        else if (modal == M_CODE || modal == M_NAME || modal == M_IP || modal == M_SERVER) keyboard(); else confirm();
    }
}

// ---- input ----
static const Focus *find_focus(int id) { for (int i = 0; i < nfoci; i++) if (foci[i].id == id) return &foci[i]; return NULL; }
static void move_focus(int dx, int dy) {
    const Focus *f = find_focus(focus);
    if (!f) { focus = nfoci ? (modal ? foci[nfoci - 1].id : ID(K_FOOT, 0)) : 0; return; }
    if (dy) {   // the next row up or down that has something, nearest column
        int best = -1, best_row = 0;
        for (int i = 0; i < nfoci; i++) {
            const int r = foci[i].row;
            if (dy < 0 ? r >= f->row : r <= f->row) continue;
            const bool closer = best < 0 || (dy < 0 ? r > best_row : r < best_row) || (r == best_row && abs(foci[i].cx - f->cx) < abs(foci[best].cx - f->cx));
            if (closer) { best = i; best_row = r; }
        }
        if (best >= 0) focus = foci[best].id;
        return;
    }
    if (f->kind == F_TABS) { tab = next_tab(tab, dx); return; }
    if (f->kind == F_SEG || f->kind == F_SLIDER) { ev_adjust = focus; ev_adjust_dir = dx; return; }
    int best = -1;   // a button: its neighbour in the same row
    for (int i = 0; i < nfoci; i++) {
        if (foci[i].row != f->row || foci[i].id == focus) continue;
        if (dx > 0 ? foci[i].cx <= f->cx : foci[i].cx >= f->cx) continue;
        if (best < 0 || abs(foci[i].cx - f->cx) < abs(foci[best].cx - f->cx)) best = i;
    }
    if (best >= 0) focus = foci[best].id;
}
static int pick(float x, float y) {
    for (int i = nhits - 1; i >= 0; i--) if (x >= hits[i].x && x < hits[i].x + hits[i].w && y >= hits[i].y && y < hits[i].y + hits[i].h) return hits[i].id;
    return 0;
}
static const Hit *find_hit(int id) { for (int i = nhits - 1; i >= 0; i--) if (hits[i].id == id) return &hits[i]; return NULL; }
static bool inside_box(const Hit *in, const Hit *box) { return in->x >= box->x && in->y >= box->y && in->x + in->w <= box->x + box->w && in->y + in->h <= box->y + box->h; }
// what the smoothed pointer is on; at an edge the current one holds for 6 more pixels, so a trembling hand doesn't flicker
static int sticky_pick(void) {
    const int next = pick(ptr_x, ptr_y);
    if (!hover || next == hover) return next;
    const Hit *now = find_hit(hover), *cand = next ? find_hit(next) : NULL;
    if (!now || (cand && inside_box(cand, now))) return next;
    const float m = 6;
    if (ptr_x >= now->x - m && ptr_x < now->x + now->w + m && ptr_y >= now->y - m && ptr_y < now->y + now->h + m) return hover;
    return next;
}
static int axis_step(float v, float other, int *held, double now, double *next) {
    int d = fabsf(v) >= .65f && fabsf(v) >= fabsf(other) ? (v > 0 ? 1 : -1) : 0;
    if (fabsf(v) < .25f || fabsf(v) < fabsf(other)) *held = 0;
    if (!d) return 0;
    if (d != *held) { *held = d; *next = now + .4; return d; }
    if (now >= *next) { *next = now + .1; return d; }
    return 0;
}
// what the ONLINE tab shows, fresh every quarter second
static void online_refresh_view(double now) {
    static double next;
    if (now < next && now > next - 1) return;
    next = now + .25;
    online_get_view(&V);
    game_count = tab == QMENU_TAB_ONLINE && screen != S_MAIN ? online_games(games, 40) : 0;
}
void qmenu_update(const QMenuHost *host, const QMenuInput *in) {
    H = host;
    online_refresh_view(in->now);
    ev_click = ev_adjust = ev_drag = 0; ev_adjust_dir = 0;
    // the laser
    static float last_px = -1, last_py = -1; static double last_now;
    if (in->pointer) {
        if (fabsf(in->px - last_px) > 3 || fabsf(in->py - last_py) > 3 || in->press) stick_mode = false;
        last_px = in->px; last_py = in->py;
        // a hand is never still: follow the laser a little behind (~35 ms), jump on big moves
        const double dt = in->now - last_now;
        if (!have_pointer || fabsf(in->px - ptr_x) > 80 || fabsf(in->py - ptr_y) > 80 || dt <= 0 || dt > .25) { ptr_x = in->px; ptr_y = in->py; }
        else { const float a = (float)(1 - exp(-dt / .035)); ptr_x += (in->px - ptr_x) * a; ptr_y += (in->py - ptr_y) * a; }
        have_pointer = true;
        hover = sticky_pick();
    } else { hover = 0; have_pointer = false; last_px = last_py = -1; }
    last_now = in->now;
    if (in->press && !stick_mode) {
        pressed = hover; repeat_at = in->now + .4;
        if (KIND(pressed) == K_STEP) ev_click = pressed;
        if (KIND(pressed) == K_TRACK) { ev_drag = pressed; ev_drag_x = ptr_x; }
    }
    if (in->held && pressed && !in->press) {
        if (KIND(pressed) == K_TRACK && in->pointer) { ev_drag = pressed; ev_drag_x = ptr_x; }
        else if (KIND(pressed) == K_STEP && hover == pressed && in->now >= repeat_at) { ev_click = pressed; repeat_at = in->now + .09; }
    }
    if (in->release) {
        if (pressed && pressed == hover && KIND(pressed) != K_STEP && KIND(pressed) != K_TRACK) ev_click = pressed;
        pressed = 0;
    }
    if (!in->held && !in->release) pressed = 0;
    // the sticks, A / B, grips
    int dx = axis_step(in->stick_x, in->stick_y, &stick_dx, in->now, &stick_next);
    int dy = axis_step(in->stick_y, in->stick_x, &stick_dy, in->now, &stick_next);
    if (dx || dy || in->select || in->tab) { if (!stick_mode && !find_focus(focus)) focus = modal ? ID(K_MBTN, 1) : ID(K_FOOT, 0); stick_mode = true; hover = 0; }
    if (!tab_shown(tab)) tab = next_tab(tab, 1);
    if (in->tab && !modal) { tab = next_tab(tab, in->tab); if (KIND(focus) != K_FOOT) focus = ID(K_TABS, 0); }
    if (dy) move_focus(0, -dy);   // stick up = the row above
    else if (dx) move_focus(dx, 0);
    if (in->select && stick_mode) ev_click = focus;
    if (in->back) {
        if (modal) { modal = M_NONE; focus = ID(K_FOOT, 0); }
        else if (tab == QMENU_TAB_ONLINE && screen != S_MAIN) { screen = S_MAIN; online_browse(false); focus = ID(K_TABS, 0); }
        else { focus = ID(K_TABS, 0); stick_mode = true; }
    }
    const int was_modal = modal;
    layout(PASS_ACT);
    if (modal != was_modal && !modal) focus = ID(K_FOOT, 0);
    ev_click = ev_adjust = ev_drag = 0;
    layout(PASS_LAYOUT);
    if (tab != QMENU_TAB_ONLINE && screen != S_MAIN) { screen = S_MAIN; online_browse(false); }   // left the browser
    if (!find_focus(focus) && nfoci) focus = modal ? ID(K_MBTN, 1) : ID(K_FOOT, 0);
    if (in->pointer) hover = sticky_pick();
}

// ---- drawing ----
static uint32_t mix(uint32_t h, const void *p, size_t n) { const uint8_t *b = p; for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u; return h; }
// the boxes a pointer or highlight change touches: the thing itself, grown for its outline, and any box it sits in
static void dirty_add(int id, int *x0, int *y0, int *x1, int *y1) {
    if (!id) return;
    Hit box; const Hit *h = find_hit(id);
    if (id == ID(K_TABS, 0)) { box = (Hit){ id, 0, 60, QMENU_W, BODY_Y - 60 + 4 }; h = &box; }
    if (!h) return;
    Hit grown = { id, h->x - 8, h->y - 8, h->w + 16, h->h + 16 };
    for (int i = 0; i < nhits; i++) if (hits[i].id != id && inside_box(h, &hits[i]) && hits[i].w < QMENU_W - 40) {
        const Hit *o = &hits[i];
        if (o->x - 8 < grown.x) { grown.w += grown.x - (o->x - 8); grown.x = o->x - 8; }
        if (o->y - 8 < grown.y) { grown.h += grown.y - (o->y - 8); grown.y = o->y - 8; }
        if (o->x + o->w + 8 > grown.x + grown.w) grown.w = o->x + o->w + 8 - grown.x;
        if (o->y + o->h + 8 > grown.y + grown.h) grown.h = o->y + o->h + 8 - grown.y;
    }
    if (grown.x < *x0) *x0 = grown.x;
    if (grown.y < *y0) *y0 = grown.y;
    if (grown.x + grown.w > *x1) *x1 = grown.x + grown.w;
    if (grown.y + grown.h > *y1) *y1 = grown.y + grown.h;
}
bool qmenu_render(const QMenuHost *host, uint8_t *pixels, double now, int dirty[4]) {
    (void)now;
    H = host;
    // what is shown, minus the pointer / highlight
    struct { int tab, modal, stick, opt[QOPT_COUNT], boot_r, hz; unsigned aw, ah; bool saved, lower; int screen, page, ngames; char kbuf[64]; } s;
    memset(&s, 0, sizeof s);
    s.tab = tab; s.modal = modal; s.stick = stick_mode;
    s.screen = screen; s.page = page; s.ngames = game_count; snprintf(s.kbuf, sizeof s.kbuf, "%s", kbuf); s.lower = modal == M_SERVER && klower;
    for (int i = 0; i < QOPT_COUNT; i++) s.opt[i] = H->get(i);
    s.boot_r = H->boot_resolution; s.hz = H->hz_now; s.aw = H->active_w; s.ah = H->active_h; s.saved = H->saved;
    uint32_t h = mix(2166136261u, &s, sizeof s);
    if (modal == M_SERVER && kerror) h = mix(h, kerror, strlen(kerror));
    if (tab == QMENU_TAB_ONLINE || online_now()) { h = mix(h, &V, sizeof V); h = mix(h, games, (size_t)game_count * sizeof *games); }
    const int point[3] = { stick_mode ? 0 : hover, pressed, stick_mode ? focus : 0 };
    uint32_t p = mix(2166136261u, point, sizeof point); p = mix(p, help, strlen(help));
    static uint32_t drawn_point; static int drawn_ids[3];
    if (h == drawn_hash && p == drawn_point) return false;
    qc_init(&cv, pixels, QMENU_W, QMENU_H);
    int x0 = QMENU_W, y0 = QMENU_H, x1 = 0, y1 = 0;
    if (h == drawn_hash) {
        // only the pointer or the highlight moved: the old and new boxes, and the help line
        for (int i = 0; i < 3; i++) { dirty_add(drawn_ids[i], &x0, &y0, &x1, &y1); dirty_add(point[i], &x0, &y0, &x1, &y1); }
        if (x0 > 3) x0 = 3;                     // full width: rows and the help line span it
        if (x1 < QMENU_W - 3) x1 = QMENU_W - 3;
        if (FOOT_Y + 4 < y0) y0 = FOOT_Y + 4;
        if (FOOT_Y + 40 > y1) y1 = FOOT_Y + 40;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > QMENU_W) x1 = QMENU_W;
        if (y1 > QMENU_H) y1 = QMENU_H;
        if ((y1 - y0) * (x1 - x0) > QMENU_W * QMENU_H * 3 / 5) { x0 = y0 = 0; x1 = QMENU_W; y1 = QMENU_H; }
        else {
            // the help line and the boxes are apart: two strips are cheaper than everything between them
            qc_limit(&cv, 0, FOOT_Y + 4, QMENU_W, 36); layout(PASS_DRAW);
            if (y0 < FOOT_Y + 4) { const int top = y0, bottom = y1 < FOOT_Y + 4 ? y1 : FOOT_Y + 4; qc_limit(&cv, x0, top, x1 - x0, bottom - top); layout(PASS_DRAW); }
            if (y1 > FOOT_Y + 40) { qc_limit(&cv, x0, FOOT_Y + 40, x1 - x0, y1 - (FOOT_Y + 40)); layout(PASS_DRAW); }
        }
    } else { x0 = y0 = 0; x1 = QMENU_W; y1 = QMENU_H; }
    if (x0 == 0 && y0 == 0 && x1 == QMENU_W && y1 == QMENU_H) layout(PASS_DRAW);
    drawn_hash = h; drawn_point = p; memcpy(drawn_ids, point, sizeof point);
    if (dirty) { dirty[0] = x0; dirty[1] = y0; dirty[2] = x1 - x0; dirty[3] = y1 - y0; }
    return true;
}

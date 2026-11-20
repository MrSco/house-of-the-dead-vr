// The VR menu: tabs (GUN, HANDS, VIEW, COMFORT, GRAPHICS, SYSTEM, ONLINE) over a footer (RESUME, RECENTER, RESTART, QUIT), driven by the gun's
// laser and trigger, or by the sticks, A and B, the grips switching tabs. Adapted from the Time Crisis 2 VR project's
// pause menu (quest/quest_menu.c, MIT). It draws on the CPU into a QMENU_W x QMENU_H RGBA texture (quest_canvas.c), only
// when something on it changed; quest_xr.c shows it on a panel in front of you.
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { QMENU_W = 1120, QMENU_H = 728 };
enum { QACT_RESUME, QACT_RECENTER, QACT_RESTART, QACT_QUIT, QACT_ARCADE_IMPORT };
// QACT_RESTART: the game back to its start, as switched on (arcade.cpp arcade_restart), settings and scores kept
typedef struct QMenuHost {
    int (*get)(int option);              // quest_options.h
    void (*set)(int option, int value);
    void (*action)(int action);
    int boot_resolution;                 // what the app started with: the resolution applies at the next start
    unsigned active_w, active_h;         // the running per-eye size
    bool saved;                          // the settings file was written
    int hz_now;                          // the display's refresh rate now (0: unknown)
    bool flat;                           // the flat apps (src/flat): a PICTURE tab, SYSTEM and ONLINE; mouse and keys
    bool touch;                          // ...on a touch screen (no window to size; taps, Back)
    bool tv;                             // ...on Apple TV (the Siri Remote: swipes move the focus, a click selects)
    bool reports;                        // ...that sends crash reports (the SYSTEM tab's row)
    // the app's updates from the project's GitHub releases (the SYSTEM tab; quest_xr.c: Updater.java's status line,
    // "state \t offered \t - \t message \t test builds 0/1 \t installed"; NULL: no updater in this app)
    const char *update;
    void (*update_cmd)(const char *cmd); // "check", "update", "testbuilds:0" / "testbuilds:1"
} QMenuHost;

typedef struct QMenuInput {
    bool pointer;                        // the laser is on the panel, at panel pixel px, py
    float px, py;
    bool press, release, held;           // the pointing hand's trigger
    float stick_x, stick_y;              // either thumbstick, the larger deflection (up is +)
    bool select, back;                   // A or X; B or Y
    int tab;                             // -1 left grip, +1 right grip
    double now;                          // seconds
} QMenuInput;

enum { QMENU_TAB_GUN, QMENU_TAB_HANDS, QMENU_TAB_VIEW, QMENU_TAB_COMFORT, QMENU_TAB_GRAPHICS, QMENU_TAB_SYSTEM, QMENU_TAB_ONLINE, QMENU_TAB_COUNT };
void qmenu_open(int tab);                // focus on RESUME; -1 keeps the last tab
void qmenu_update(const QMenuHost *host, const QMenuInput *in);
// Draws into pixels (QMENU_W * QMENU_H * 4) if anything changed since the last call; returns true when it did, with the
// box it changed in dirty (x, y, w, h).
bool qmenu_render(const QMenuHost *host, uint8_t *pixels, double now, int dirty[4]);
bool qmenu_pointer(float *px, float *py);   // the steadied laser point on the panel (menu pixels); false: none
bool qmenu_modal(void);                     // a dialog or the ONLINE browser is up (back closes it, not the menu)

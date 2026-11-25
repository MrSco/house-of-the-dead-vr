// The flat app (docs/platforms.md, "Flat mode"): the game on a screen, as the VR app's cabinet view shows it on the
// cabinet's monitor. Shared by every flat build (Windows now; Android phones and iOS later); each OS's front end
// (win/main_win.c) owns the window, the GL context and the input devices, and calls these.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- the app (flat_app.c)
// Once at start, before the window shows: files_dir is where the ROM set (arcade/), the saves, the settings and
// port.log live (made when missing). 0 when the game cannot start at all.
int flat_app_start(const char *files_dir);
// before flat_app_start: option o (quest_options.h) starts at v where options.cfg doesn't have it yet (the player's
// own choice, once saved, stays). Apple TV HD: Picture Original (src/flat/tvos)
void flat_app_default(int o, int v);
// Every frame of the front end, before drawing: minimized = nothing of the window shows (the game pauses, unless
// online, where pausing would stall the partner).
void flat_app_frame(int minimized);
int flat_app_restart_requested(void);   // online play begins and ends with the app opening again
void flat_app_quit(void);               // the window is closing: the partner hears at once, the sound stops

// ---- the picture (flat.c), with the front end's GL context current
enum { FLAT_PICTURE_HD, FLAT_PICTURE_ORIGINAL };
int flat_gl_init(void);                 // 0 when the GL is not enough (logged)
void flat_set_picture(int mode);        // HD: the 3D at the window's resolution; ORIGINAL: at the arcade's own
// HD's most: the picture drawn at most this many lines (0: the window's) and samples (0: 4), then scaled to the window.
// Apple TV: a 4K TV's whole height with 4x MSAA is more than its GPU fills at 60 Hz (src/flat/tvos)
void flat_picture_limit(int lines, int samples);
void flat_redraw(void);                 // draw the picture again (a setting changed while the game is paused)
// draw a frame into the current framebuffer (w x h pixels; 0 is the window's)
void flat_draw(int w, int h, unsigned framebuffer);

// ---- input, in window pixels (y down)
int flat_pointer_in_view(int x, int y); // over the game's picture (the front end hides the system pointer there)
void flat_view_rect(int *x, int *y, int *w, int *h);   // where the picture is in the window (pixels, y down)

// ---- the players (flat.c): the game's two guns, as on the cabinet. Each input comes from a device: the keyboard
// and mouse (one device: a light gun that is a mouse too), touch, the Siri Remote, each pad. Alone, every device is
// player 1's (the last one moved aims). Player 2 joins with Start on another device while player 1 plays: that
// device is player 2's gun from then on (flat_dev_start says exactly when). Not online: player 2 is the partner.
enum { FLAT_DEV_KBM = 1, FLAT_DEV_TOUCH, FLAT_DEV_REMOTE, FLAT_DEV_PAD = 16 };   // pads: flat_pad_attach's
void flat_dev_pointer(int dev, int x, int y);   // the device's aim (a pad's sight: sent every frame is fine)
void flat_dev_trigger(int dev, int down);       // shoot (the left mouse button, a tap, a pad's trigger)
void flat_dev_reload(int dev, int down);        // reload (the right mouse button; shooting off the picture too)
void flat_dev_start(int dev, int down);         // coins and Start, skipping (Space, Enter, a pad's A or Start)
int flat_dev_player(int dev);                   // the player (0, 1) the device drives now
int flat_player2_in(void);                      // a player 2 here (a device holds the seat)
void flat_players_frame(void);                  // (flat_app_frame)
// a pad connected: its device id (one that went comes back with its id: player 2's stays player 2's); gone: its
// trigger and reload let go
int flat_pad_attach(void);
void flat_pad_detach(int dev);

// ---- rumble (flat.c): each player's shots, reloads and hits (quest.h host_player_events) on the device that player
// used last (shot, reloaded or pressed Start with: a pad's motors, a phone's for touch). Only the players here (online:
// this one, never the partner), never while the game is paused or the menu is open (a running one stops), the GAME
// tab's Rumble. FLAT_TEST_RUMBLE=1 (tests) logs each request.
// Trigger feedback (the GAME tab's, muted by Rumble OFF too) on that player's pad: a DualSense's right trigger (R2
// shoots) resists then snaps like a gun's while that player plays, shoots once through the snap and buzzes on a
// shot; its left (L2 reloads) clicks lightly and buzzes on a reload; an Xbox pad's trigger motors buzz on a shot
// (right) and a reload (left). Loose again when that player isn't playing, paused, in the menu,
// at the quit, or when the pad is no longer that player's.
enum { FLAT_RUMBLE_SHOT, FLAT_RUMBLE_RELOAD, FLAT_RUMBLE_HIT };
void flat_rumble_frame(int paused);     // (flat_app_frame)
void flat_rumble_stop(void);            // every rumble and trigger effect off now (the quit, the app leaving the screen)
int flat_player_device(int p);          // the device player p used last (0: none yet)
// what each front end provides: device dev's rumble for event `what` (FLAT_RUMBLE_*), its low (heavy) and high
// (light) frequency motors 0..1 for ms; ms 0: stop it. Never blocking. A device with nothing to vibrate ignores it.
void flat_os_rumble(int dev, int what, float low, float high, int ms);
// ...and its triggers (right: R2 / RT, else L2 / LT): OFF no effect (a DualSense's trigger loose); WEAPON a
// DualSense's gun feel (resistance to a break point, then a snap: the right trigger); CLICK a light click (the left
// trigger, which reloads); BUZZ amp 0..1 for ms (an Xbox pad's trigger motor, a DualSense trigger's vibration from
// that trigger's break on: flat.c sends its feel or OFF after it). A pad without either ignores it.
enum { FLAT_TRIG_OFF, FLAT_TRIG_WEAPON, FLAT_TRIG_CLICK, FLAT_TRIG_BUZZ };
void flat_os_trigger(int dev, int right, int mode, float amp, int ms);
// Each feel's shape, 0 at rest to 1 pulled full (a DualSense's zones are tenths): resistance from START, a give at
// BREAK, and the pull counts at FIRE, a tenth past the give, so a shot goes only once the finger is through the snap
// as a real trigger's does (GoldenEye VR, 2026-11-23: it went at 40%, ahead of the snap). It lets go below BREAK, so
// the next shot is a short pull back, not a whole new one. A trigger without a feel counts at FLAT_TRIG_PRESS.
#define FLAT_TRIG_PRESS 0.4f
#define FLAT_GUN_START 0.3f
#define FLAT_GUN_BREAK 0.6f
#define FLAT_GUN_FIRE 0.7f
#define FLAT_GUN_STRENGTH 0.85f
#define FLAT_CLICK_START 0.2f
#define FLAT_CLICK_BREAK 0.3f
#define FLAT_CLICK_FIRE 0.4f
#define FLAT_CLICK_STRENGTH 0.3f
// where device dev's trigger (right: R2) counts as pulled, 0..1, for a pad whose triggers take the feels (a
// DualSense; pass felt 0 for any other): down, the trigger counts as held now (its let-go point is asked for)
float flat_trigger_at(int dev, int right, int felt, int down);

// ---- the menu (flat_menu.c): the VR app's menu (quest_menu.c) as a panel over the game, which pauses (not online)
void flat_menu_set_open(int open);
int flat_menu_is_open(void);
enum { FLAT_KEY_UP, FLAT_KEY_DOWN, FLAT_KEY_LEFT, FLAT_KEY_RIGHT, FLAT_KEY_ENTER, FLAT_KEY_BACK, FLAT_KEY_TAB, FLAT_KEY_TAB_BACK };
void flat_menu_key(int key);            // a key pressed while it is open (FLAT_KEY_BACK: Esc, which also closes it)
void flat_menu_mouse(int x, int y, int button_down);   // window pixels
void flat_menu_draw(int w, int h);      // (flat_draw calls it)
void flat_menu_tv(int on);              // Apple TV's layout (src/flat/tvos): the remote's wording, its GAME rows, no QUIT

// ---- gyro aim (flat_app.c; the GAME tab's Gyro aim, touch screens): the device's turn rates about the screen's own
// axes (radians a second, right-handed: up = turning left, right = tilting the top towards you) move the sight
int flat_gyro_on(void);                 // a tap then shoots at the sight, not where it lands
void flat_gyro(float rate_right, float rate_up, double dt);

// ---- the settings (flat_app.c: options.cfg, the menu's)
int flat_opt_get(int option);
int flat_opt_default(int option);       // the default this app gives it (flat_app_default, else qopt_defs')
void flat_opt_set(int option, int value);

// ---- what each front end provides
void flat_os_fullscreen(int on);
void flat_os_import_rom(void);          // choose the ROM set; it is copied in and the app starts again with it
void flat_os_quit(void);
int flat_os_touch(void);                // a touch screen (the menu's layout)
// crash reports (Windows: platform/windows/crash_win.c), at start before the game thread; elsewhere none yet
void flat_os_crash_reports(const char *files, const int *opt);
int flat_os_has_reports(void);
#ifdef _WIN32
void flat_os_pc_switch(void);           // HOTDVR.exe: start again in the VR headset (and keep to it)
#endif

// GL helpers for the panel (flat.c)
unsigned flat_gl_program(const char *vs, const char *fs);
void flat_gl_rect(unsigned program, int mvp_uniform, float x, float y, float w, float h, int win_w, int win_h);
void flat_gl_rect_uv(unsigned program, int mvp_uniform, float x, float y, float w, float h, int win_w, int win_h,
                     float u0, float v0, float u1, float v1);

// ---- touch screens (flat_touch.c): a tap on the picture shoots there; buttons beside it
enum { FLAT_BTN_NONE, FLAT_BTN_START, FLAT_BTN_RELOAD, FLAT_BTN_MENU };
void flat_touch_controls(int on);       // show them (the touch front ends)
int flat_touch_button(int x, int y);    // the button at a window point, or FLAT_BTN_NONE
void flat_touch_pressed(int button, int down);   // drawn pressed while held
void flat_touch_draw(int w, int h);     // (flat_draw calls it)

#ifdef __cplusplus
}
#endif

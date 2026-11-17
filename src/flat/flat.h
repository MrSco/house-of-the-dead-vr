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
// Every frame of the front end, before drawing: minimized = nothing of the window shows (the game pauses, unless
// online, where pausing would stall the partner).
void flat_app_frame(int minimized);
int flat_app_restart_requested(void);   // online play begins and ends with the app opening again
void flat_app_quit(void);               // the window is closing: the partner hears at once, the sound stops

// ---- the picture (flat.c), with the front end's GL context current
enum { FLAT_PICTURE_HD, FLAT_PICTURE_ORIGINAL };
int flat_gl_init(void);                 // 0 when the GL is not enough (logged)
void flat_set_picture(int mode);        // HD: the 3D at the window's resolution; ORIGINAL: at the arcade's own
// draw a frame into the current framebuffer (w x h pixels; 0 is the window's)
void flat_draw(int w, int h, unsigned framebuffer);

// ---- input, in window pixels (y down)
void flat_pointer(int x, int y);        // the gun's aim
int flat_pointer_in_view(int x, int y); // over the game's picture (the front end hides the system pointer there)
void flat_view_rect(int *x, int *y, int *w, int *h);   // where the picture is in the window (pixels, y down)
void flat_trigger(int down);            // shoot (the left mouse button, a tap, a pad's trigger)
void flat_reload(int down);             // reload (the right mouse button; shooting off the picture also reloads)
void flat_start(int down);              // coins and Start, skipping (Space, Enter, a pad's Start)

// ---- the menu (flat_menu.c): the VR app's menu (quest_menu.c) as a panel over the game, which pauses (not online)
void flat_menu_set_open(int open);
int flat_menu_is_open(void);
enum { FLAT_KEY_UP, FLAT_KEY_DOWN, FLAT_KEY_LEFT, FLAT_KEY_RIGHT, FLAT_KEY_ENTER, FLAT_KEY_BACK, FLAT_KEY_TAB, FLAT_KEY_TAB_BACK };
void flat_menu_key(int key);            // a key pressed while it is open (FLAT_KEY_BACK: Esc, which also closes it)
void flat_menu_mouse(int x, int y, int button_down);   // window pixels
void flat_menu_draw(int w, int h);      // (flat_draw calls it)

// ---- the settings (flat_app.c: options.cfg, the menu's)
int flat_opt_get(int option);
void flat_opt_set(int option, int value);

// ---- what each front end provides
void flat_os_fullscreen(int on);
void flat_os_import_rom(void);          // choose the ROM set; it is copied in and the app starts again with it
void flat_os_quit(void);
int flat_os_touch(void);                // a touch screen (the menu's layout)

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

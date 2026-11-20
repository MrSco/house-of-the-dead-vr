// The VR shell (docs/vr.md, docs/platforms.md "src/xr"): OpenXR + OpenGL (ES) 3, shared by the Quest app (its host:
// src/quest/quest_main.c, a NativeActivity with EGL and GL ES), PCVR (src/xr/xr_win.c: HOTDVR.exe with WGL and
// desktop GL 3.3 on any OpenXR runtime: SteamVR, Meta's (Link), Virtual Desktop's) and, without OpenXR (XR_OPENXR 0),
// the Vision Pro (src/visionos/xr_vos.m: Compositor Services, ARKit and ANGLE's GL ES 3, one pass per eye; its hands
// and Sense controllers come in an XrhInput each frame). xr.h is the seam between the shell and its host.
//
// The recompiled arcade game runs on its own thread (SDL_main in arcade.cpp, against host_sdl.c) at the
// arcade's 57.52 frames a second, publishing each frame's world in 3D (m2_vr.h, drawn by m2_gl.c; d3d_vr.h's VrScene
// for the guns' aim); this thread runs the headset at its display rate and draws, every display frame, the newest
// game scene for both eyes:
//  - immersive (the default): you stand where the game's camera is, the scene around you in stereo at its real
//    size (QUEST_SCALE metres per game unit, 0.11 by default) with your head tracked on top of the game's camera
//    moves; the HUD floats where the game's screen would be;
//  - cabinet (the gun hand's thumbstick click held switches): a small arcade room with the game on a cabinet's screen, drawn by the GPU
//    at twice the game's resolution.
// Moving your head never touches the game, so it stays smooth even when a game frame is late.
//
// Controllers are light guns: the trigger fires. In the immersive view the gun's ray is cast into the scene and the
// point it hits is where the game is told you aimed (so you hit what the laser touches, from wherever you hold the
// gun); on the cabinet it is where the ray meets the screen. Firing away from the game's picture reloads, as on the
// cabinet; so do B, Y, a firm squeeze of the grip and the reload gestures (the HANDS tab: the magazine in your free
// hand touched to the gun's grip, or aiming at the floor). Every reload is one short press of the game's reload, so
// a grip held while playing never keeps the gun off the screen. A / X skip scenes (Space). The left menu button
// opens the VR menu (quest_menu.c: settings, RECENTER, the game's own Esc menu, QUIT), which pauses the game. The gun
// is in your right hand, your left, or both (the hand that fired last aims). Hand tracking: the hand's aim ray aims,
// an index pinch fires and clicks in the menu, the off hand's middle-finger pinch is A / X (coins, Start, skip), the left
// palm's menu gesture opens the menu (XR_FB_hand_tracking_aim,
// with the simple controller profile as well). On a PC a thumbstick's double click opens the menu too (SteamVR keeps
// the left menu button for its dashboard; Index controllers have none).
//
// Files: the files folder (Quest: the app's external files dir, /sdcard/Android/data/<package>/files; PC: "files"
// beside HOTDVR.exe) holds arcade/ (your ROM set), save/, port.log, vr-options.cfg (the menu's settings) and env.txt
// (KEY=VALUE lines, the PORT_* switches of the other builds plus QUEST_HZ, QUEST_RES_SCALE, QUEST_SCALE, which
// override the settings). Vision Pro: the app's Documents (the Files app shows it).
#ifndef XR_OPENXR
#define XR_OPENXR 1          // (as xr.h has it)
#endif
#if !XR_OPENXR
#include <EGL/egl.h>         // (ANGLE's)
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <stdint.h>
#define XR_GLES 1
#define XR_GLSL "#version 300 es\n"
#elif defined(__ANDROID__)
#include <android/log.h>
#include <android/native_activity.h>
#include <jni.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <unistd.h>
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#define XR_GLES 1
#define XR_GLSL "#version 300 es\n"
#else
#include "platform/gl.h"
#include <unknwn.h>          // (IUnknown, for openxr_platform.h: WIN32_LEAN_AND_MEAN leaves it out)
#undef near                 // (windows.h's, for 16-bit pointers: names here)
#undef far
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#define XR_GLES 0
#define XR_GLSL PLAT_GLSL_VERSION
#endif
#if XR_OPENXR
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#endif
#include <SDL.h>
#include "xr.h"
#include "quest.h"
#include "d3d_vr.h"
#include "quest_gun.h"
#include "quest_hand.h"
#include "quest_canvas.h"
#include "quest_cabinet.h"
#include "ui_font_data.h"
#include "quest_menu.h"
#include "quest_options.h"
#include "online.h"
#include "voice.h"
#include "netplay.h"
#include "m2_vr.h"            // the arcade game's polygons (src/core/m2_gl.c draws them)
#include "platform/platform.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !XR_OPENXR
// what the shell takes from OpenXR's types: times in nanoseconds, poses, fields of view; the controllers' actions and
// hands (get_bool, get_float, get_stick read them from the host's XrhInput)
typedef int64_t XrTime;
typedef struct { struct { float x, y, z, w; } orientation; struct { float x, y, z; } position; } XrPosef;
typedef struct { float angleLeft, angleRight, angleUp, angleDown; } XrFovf;
typedef int XrAction, XrPath;
#define XR_NULL_PATH (-1)
enum { A_FIRE, A_RELOAD, A_BACK, A_SKIP, A_MENU, A_MODE, A_STICK, A_TRIG, A_THUMB };
#elif XR_GLES
typedef XrSwapchainImageOpenGLESKHR XrSwapchainImageGL;
#define XR_TYPE_SWAPCHAIN_IMAGE_GL XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR
#define XR_GL_ENABLE_EXTENSION_NAME XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME
#else
typedef XrSwapchainImageOpenGLKHR XrSwapchainImageGL;
#define XR_TYPE_SWAPCHAIN_IMAGE_GL XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR
#define XR_GL_ENABLE_EXTENSION_NAME XR_KHR_OPENGL_ENABLE_EXTENSION_NAME
#endif

#if defined(__ANDROID__) && XR_OPENXR
#define TAG "hotdvr"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#else
#define LOGI(...) plat_log(PLAT_LOG_INFO, __VA_ARGS__)
#define LOGE(...) plat_log(PLAT_LOG_ERROR, __VA_ARGS__)
#endif

// The screen: 4:3, 2 m wide, 2.4 m ahead, centred at eye height where the app started (LOCAL space; the system's
// recenter moves it in front of you again).
// The cabinet view's screen (metres, LOCAL): the plain cabinet's, or the cabinet model's (place_cabinet)
static float scr_w = 2.0f, scr_h = 1.5f, scr_y = -0.05f, scr_z = -2.4f;
#define SCR_W scr_w
#define SCR_H scr_h
#define SCR_Y scr_y
#define SCR_Z scr_z
#define CAB_SCREEN_AHEAD 0.75f   // the model's screen this far ahead of where you started (a player at its guns)
#define FRAME_MAX (1280 * 1024)
// the menu's panel: this wide, this far ahead of your eyes when it opens
#define MENU_W_M 1.3f
#define MENU_H_M (MENU_W_M * QMENU_H / QMENU_W)
#define MENU_DIST 1.4f
// the magazine in the free hand (the MAGAZINE reload gesture), in that hand's aim space: held in the fist the way
// the gun's grip is (the hands model holds that grip at rest), its top out above the index finger at MAG_TOP_Y
#define MAG_Z 0.03f
#define MAG_TOP_Y (-0.03f)
#define MAG_BOTTOM_Y (-0.135f)

static struct {
    char files[512];
    volatile int exit_requested, game_done, game_failed, game_tid;
    int sc_w, sc_h;                      // the eyes' pictures
    XrAction a_fire, a_reload, a_back, a_skip, a_menu, a_mode, a_stick; XrPath hand[2];
#if XR_OPENXR
    // OpenXR
    XrInstance inst; XrSystemId sys; XrSession sess; XrSessionState state; int running;
    XrSpace local, stage;
    XrViewConfigurationView vcv[2];
    XrSwapchain sc[2]; uint32_t sc_count[2]; XrSwapchainImageGL *sc_img[2];
    XrActionSet aset; XrAction a_aim, a_haptic; XrSpace aim[2];
    XrSpace viewsp;
    PFN_xrEnumerateDisplayRefreshRatesFB enum_rates; PFN_xrRequestDisplayRefreshRateFB req_rate;
#if XR_GLES
    PFN_xrSetAndroidApplicationThreadKHR set_thread;
#endif
    PFN_xrPerfSettingsSetPerformanceLevelEXT perf_level;
    int steamvr;                         // the runtime is SteamVR (its quirks: xr_win.c, the menu's double click)
#else
    // the host's frames (xr_host_frame)
    const XrhInput *in;                  // this display frame's input
    int focused;                         // the app shows and takes input
    int closing;                         // a last page is up (xr_close_with): a pull or the menu closes the app
#endif
    // GL
    GLuint fbo, depth_rb, prog_col, prog_tex, screen_tex; int tex_w, tex_h, ms;
    GLint u_col_mvp, u_tex_mvp, u_tex_size;
#if XR_GLES
    PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC fb_tex_ms; PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC rb_ms;
#else
    // desktop GL: the eyes and the cabinet's picture drawn multisampled into renderbuffers, then resolved (blit) into
    // the swapchain's image or cab_tex through resolve_fbo
    GLuint ms_color, cab_ms_color, resolve_fbo;
    GLenum sc_fmt;                       // the swapchains' format (an MSAA resolve needs the same on both sides)
#endif
    uint32_t *frame;
    // the game's scene (d3d_vr.h)
    int cabinet;                         // 0 immersive, 1 the arcade room
    int screen_scene;                    // immersive, a scene the arcade shows round nothing (the game-over's): its
                                         // picture on a screen ahead in the dark, you still in the immersive view
    float scale;                         // metres per game unit
    const VrScene *scene; uint32_t scene_serial;
    GLuint cab_fbo, cab_tex, cab_depth;
    GLint u_tex_alpha;
    int cab_dirty, frame_new;
    // input
    int active, fire[2], fire_btn[2], skip, menu, mode_btn;
    XrTime mode_t;                       // the right stick's click went down then (the view switch: VIEW_HOLD_NS)
    volatile int restart_requested;     // online.c: the app opens again (into or out of an online game)
    int need_files, need_fire, import_started;           // no ROM set: the setup page, not the game
    int cab_model;                                       // the cabinet model is loaded (quest_cabinet.c)
    // reloading: the grips (on / just squeezed), B and Y, the gestures' re-arming, the game's reload press (one
    // short press at a time: down until rl_until, none again before rl_last + RELOAD_GAP)
    int grip_on[2], grip_edge[2], back_on[2], mag_armed, down_armed[2], rl_down;
    XrTime rl_until, rl_last;
    // the MAGAZINE reload with one gun: a magazine in the free hand while its grip is squeezed (mag_hold; mag_spent:
    // it went in, the hand is empty until the grip is let go), in the gun (mag_in) until the slide is racked: the free
    // hand takes the slide (rack) where it was at rack_z0 (gun space) and pulls it back (rack_pull 0..1); let go, it
    // springs forward from rack_from (rack_end: when)
    int mag_hold, mag_spent, mag_in, rack, slide_near;
    // the magazine in the gun (mag_seated: the gun starts with one, B / Y drops it, a new one goes in only once it
    // is out); the dropped one falls from drop_m (the gun's pose then) for a while after drop_t
    int mag_seated, ammo_seen; XrTime drop_t; float drop_m[16];
    float rack_z0, rack_pull, rack_from;
    XrTime rack_end;
    // TRACER: each hand's last shot, muzzle to where it hit
    float tracer[2][6];
    int aim_ok[2], on_screen[2], hit3d[2];
    float aim_m[2][16], hit_t[2];
    float floor_y;
    // the guns: the model loaded, the last shot (display time, ns) and the trigger of each hand, the barrel's angle
    int gun_model; XrTime now, shot_t[2]; float gun_pitch;
    int hands_model; XrAction a_trig, a_thumb; float trig[2]; int thumb[2];
    float shot_grip;
    XrTime pend_t[2]; int pend_ammo[2]; float pend_tr[2][6];  // a pull waiting for the game to fire (shot_confirm)
    int support, sup_near; float sup_rel[16];           // TWO HANDS: the free hand holds the gun; its pose in the gun's space then                                    // QUEST_SHOT_LEFT: the left grip, else -1   // the hands: trigger pulled, thumb resting
    // the settings (quest_options.h); the size of the world and the 2D layer's distance they give
    int opt[QOPT_COUNT], boot_res, opt_saved, hz_now;
    float base_scale, panel_m;
    // RECENTER: everything is drawn in a world placed in LOCAL space by home (LOCAL <- world)
    float home[16], home_inv[16], head_m[16];
    int head_ok;
    // the menu: open, its panel's pose in the world (x right, y up, z towards you), the lasers on it
    int menu_open, menu_ready, menu_on[2], mfire[2], fire_hold[2], sel_btn, back_btn;
    float menu_m[16], menu_t[2];
    GLuint prog_menu, menu_tex; GLint u_menu_mvp;
    // comfort: the vignette's program and strength now
    GLuint prog_vig; GLint u_vig, u_vig_k; float vig;
} S = { .active = 1, .floor_y = -1.6f, .scale = 0.11f, .panel_m = 1.5f,
#if !XR_OPENXR
        .hand = { 0, 1 }, .a_fire = A_FIRE, .a_reload = A_RELOAD, .a_back = A_BACK, .a_skip = A_SKIP, .a_menu = A_MENU,
        .a_mode = A_MODE, .a_stick = A_STICK, .a_trig = A_TRIG, .a_thumb = A_THUMB,
#endif
};

// hand h holds a gun (the HANDS tab)
static int is_gun(int h) {
    int m = S.opt[QOPT_HAND];
    return m == QHAND_BOTH || h == (m == QHAND_LEFT ? 0 : 1);
}
// the hand without a gun (-1: a gun in each), and whether it reloads by magazine and slide
static int free_hand(void) { int m = S.opt[QOPT_HAND]; return m == QHAND_BOTH ? -1 : m == QHAND_LEFT ? 1 : 0; }
// Your bare hands are playing (hand tracking; ht_update): no grips to squeeze, so you reload as on the cabinet, by
// shooting off the screen, whatever the HANDS tab says (the settings stay: the controllers picked up, they apply again)
static int using_hands;
// the reload gesture that applies: Reload gesture's, or none in the cabinet view when Cabinet reload is OFF SCREEN,
// or while your hands play
static int gesture(void) {
    if (using_hands) return QGESTURE_OFF;
    return S.cabinet && S.opt[QOPT_CAB_RELOAD] == QCABRELOAD_OFFSCREEN ? QGESTURE_OFF : S.opt[QOPT_GESTURE];
}
static int mag_reload(void) { return gesture() == QGESTURE_MAGAZINE && free_hand() >= 0; }
// how far back the slide is drawn by hand (0..1): held, or springing forward after
static float slide_by_hand(void) {
    if (S.rack) return S.rack_pull;
    float t = S.rack_end ? (float)(S.now - S.rack_end) / 1e9f : 1;
    return t < 0.06f ? S.rack_from * (1 - t / 0.06f) : 0;
}
#if XR_OPENXR
static int xr_ok(XrResult r, const char *what) {
    if (XR_SUCCEEDED(r)) return 1;
    char s[XR_MAX_RESULT_STRING_SIZE] = "";
    if (S.inst) xrResultToString(S.inst, r, s);
    LOGE("%s: %d %s", what, (int)r, s);
    return 0;
}
#define XR(call) xr_ok((call), #call)
// the headset shows this app and gives it the input
static int focused(void) { return S.state == XR_SESSION_STATE_FOCUSED; }
#else
static int focused(void) { return S.focused; }
#endif

// ---------------------------------------------------------------- files and settings
#if XR_GLES   // (HOTDVR.exe: flat_app.c's, linked beside it)
const char *quest_files_dir(void) { return S.files; }
#endif
static void apply_options(void);
static void request_restart(void) { S.restart_requested = 1; }
int xr_restart_requested(void) { return S.restart_requested; }
static void mkdirs(const char *p) {
    char b[600];
    snprintf(b, sizeof b, "%s", p);
    for (char *s = b + 1; *s; s++) if (*s == '/') { *s = 0; plat_mkdir(b); *s = '/'; }
    plat_mkdir(b);
}
// The host made the files folder and went into it (port.log, PORT_SHOT pictures land there)
void xr_setup(const char *files) {
    snprintf(S.files, sizeof S.files, "%s", files);
    char p[600];
#if XR_OPENXR
    snprintf(p, sizeof p, "%s/game", S.files); mkdirs(p);
#else
    snprintf(p, sizeof p, "%s/arcade", S.files); mkdirs(p);   // (Vision Pro: the Files app shows where the set goes)
#endif
    snprintf(p, sizeof p, "%s/save", S.files); mkdirs(p);
    // env.txt: KEY=VALUE per line
    FILE *f = fopen("env.txt", "r");
    static char line[8192];                 // (PORT_INPUT scripts are long)
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        plat_setenv(line, eq + 1);
        LOGI("env %s=%s", line, eq + 1);
    }
    if (f) fclose(f);
    LOGI("files: %s", S.files);
    quest_overlay_key = D3D_VR_KEY;         // the game's 2D layers: this colour is see-through (arcade_quest.cpp)
    const char *sc = getenv("QUEST_SCALE");
    if (sc && atof(sc) > 0.01 && atof(sc) < 10) S.scale = (float)atof(sc);
    S.base_scale = S.scale;
    // the settings (the menu's), and the world where you started
    char op[600];
    snprintf(op, sizeof op, "%s/vr-options.cfg", S.files);
    qoptions_load(op, S.opt);
    S.opt_saved = 1;
    S.shot_grip = -1;
    S.mag_seated = 1; S.ammo_seen = -1;
    // a settings file the app can't write (copied in over adb: the shell user's) is replaced by its own copy now,
    // so the menu's changes save
    if (plat_exists(op) && !plat_writable(op)) S.opt_saved = qoptions_save(op, S.opt);
    S.boot_res = S.opt[QOPT_RESOLUTION];
    memset(S.home, 0, sizeof S.home); S.home[0] = S.home[5] = S.home[10] = S.home[15] = 1;
    memcpy(S.home_inv, S.home, sizeof S.home);
    apply_options();
    // a crash in the last run: its report, before the game starts a new port.log (the SYSTEM tab's Crash reports)
    xr_os_crash_reports(S.files, S.opt);
    // online co-op (src/net/online.c): this run may be the restart into an online game (online-session.cfg), whose
    // game starts from fresh saves (game_main); voice chat beside it
    voice_init();
    online_restart = request_restart;
    if (online_boot(S.files)) { xr_os_online_started(); online_start(); apply_options(); }   // (the partner's crosshair)
}
// ---------------------------------------------------------------- your ROM set
// The app holds none of the game: it plays your own MAME ROM set (hotd, Revision A), which the host copies into
// files/arcade (hotd.zip or hotd.7z: the Quest's ImportActivity, the PC's Open dialog).
static int game_files_ok(char *why, size_t wn) {
    char zip[700];
    snprintf(zip, sizeof zip, "%s/arcade/hotd.zip", S.files);
    if (plat_exists(zip)) return 1;
    snprintf(zip, sizeof zip, "%s/arcade/hotd.7z", S.files);
    if (plat_exists(zip)) return 1;
    snprintf(why, wn, "No ROM set yet.");
    return 0;
}
static void show_setup(const char *why) {
    char msg[1600];
    snprintf(msg, sizeof msg,
             "LOAD YOUR ROM SET\n\n"
             "This app holds none of The House of the Dead: it plays the 1996 arcade game from your own MAME ROM set.\n\n"
#if XR_OPENXR
             "1. Copy hotd.zip or hotd.7z (MAME's set, Revision A) to the headset (into Download, say).\n\n"
             "2. Pull the trigger here and choose it. It is copied into the app and the game starts. You can then "
             "delete the original.\n\n"
             "%s\n\nMenu button: quit.", why);
#else   // (Vision Pro: the document picker, in a window beside the space)
             "1. Put hotd.zip or hotd.7z (MAME's set, Revision A) in the Files app (iCloud Drive, say).\n\n"
             "2. Pinch here (or pull a trigger) and choose it in the window that opens. It is copied into the app and "
             "the game starts.\n\n"
             "%s\n\nTo leave: press the Digital Crown.", why);
#endif
    quest_show_message(msg);
}
// ---------------------------------------------------------------- the game thread
static int game_main(void *arg) {
    (void)arg;
#if defined(__ANDROID__)
    S.game_tid = gettid();
#endif
    char game[600], save[600];
    snprintf(game, sizeof game, "%s/arcade", S.files);      // hotd.zip (arcade.cpp)
    snprintf(save, sizeof save, "%s/save", S.files);
    // Online play (and QUEST_FRESH_SAVE=1, the determinism tests): every player's game starts from the same files, so
    // it saves into a folder of its own that starts empty each time (the game makes its default settings and scores)
    {
        const char *e = getenv("QUEST_FRESH_SAVE");
        if ((e && atoi(e)) || online_session()) {
            snprintf(save, sizeof save, "%s/online-save", S.files);
            plat_rmtree(save);
            mkdirs(save);
            LOGI("fresh save folder %s", save);
        }
    }
    char *argv[] = { "hotdvr", game, save, NULL };
    int r = SDL_main(3, argv);
    LOGI("game returned %d", r);
    if (r) {
        char msg[1600];
        snprintf(msg, sizeof msg,
                 "The game did not start (exit code %d).\n\n"
                 "Your ROM set belongs in\n%s\n\n"
                 "(hotd.zip or hotd.7z, MAME's set, Revision A: the SYSTEM tab's Change ROM set loads it).\n\n"
                 "port.log in %s has the details.\n\nPress the menu button to quit.", r, game, S.files);
        quest_show_message(msg);
        S.game_failed = 1;
    } else {
        S.game_done = 1;
    }
    return 0;
}
static void start_game(void) {
    // (recompiled code nests deeply on the host stack)
    if (!plat_thread_start_stack(game_main, NULL, (size_t)64 << 20)) { LOGE("cannot start the game thread"); S.exit_requested = 1; }
}
void xr_start(void) {
    char why[200];
    if (game_files_ok(why, sizeof why)) start_game();
    else { S.need_files = 1; S.need_fire = 1; LOGI("no game files: %s", why); show_setup(why); }
}
int xr_should_exit(void) { return S.exit_requested || S.game_done; }
void xr_menu_toggle(void);

// ---------------------------------------------------------------- matrices (column-major, as GL wants them)
static void mat_mul(float *o, const float *a, const float *b) {
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int k = 0; k < 4; k++)
            r[c * 4 + k] = a[k] * b[c * 4] + a[4 + k] * b[c * 4 + 1] + a[8 + k] * b[c * 4 + 2] + a[12 + k] * b[c * 4 + 3];
    memcpy(o, r, sizeof r);
}
static void mat_pose(float *m, XrPosef p) {
    float x = p.orientation.x, y = p.orientation.y, z = p.orientation.z, w = p.orientation.w;
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y + z * w);     m[2] = 2 * (x * z - y * w);      m[3] = 0;
    m[4] = 2 * (x * y - z * w);     m[5] = 1 - 2 * (x * x + z * z); m[6] = 2 * (y * z + x * w);      m[7] = 0;
    m[8] = 2 * (x * z + y * w);     m[9] = 2 * (y * z - x * w);     m[10] = 1 - 2 * (x * x + y * y); m[11] = 0;
    m[12] = p.position.x; m[13] = p.position.y; m[14] = p.position.z; m[15] = 1;
}
static void mat_view(float *m, XrPosef p) {         // inverse of a rigid pose
    float r[16];
    mat_pose(r, p);
    m[0] = r[0]; m[1] = r[4]; m[2] = r[8];  m[3] = 0;
    m[4] = r[1]; m[5] = r[5]; m[6] = r[9];  m[7] = 0;
    m[8] = r[2]; m[9] = r[6]; m[10] = r[10]; m[11] = 0;
    m[12] = -(m[0] * r[12] + m[4] * r[13] + m[8] * r[14]);
    m[13] = -(m[1] * r[12] + m[5] * r[13] + m[9] * r[14]);
    m[14] = -(m[2] * r[12] + m[6] * r[13] + m[10] * r[14]);
    m[15] = 1;
}
static void mat_proj(float *m, XrFovf f, float n, float fa) {
    float l = tanf(f.angleLeft), r = tanf(f.angleRight), d = tanf(f.angleDown), u = tanf(f.angleUp);
    float w = r - l, h = u - d;
    memset(m, 0, 16 * sizeof(float));
    m[0] = 2 / w; m[8] = (r + l) / w;
    m[5] = 2 / h; m[9] = (u + d) / h;
    m[10] = -(fa + n) / (fa - n); m[14] = -2 * fa * n / (fa - n);
    m[11] = -1;
}
typedef struct { float x, y, z; } V3;
static V3 xform(const float *m, float x, float y, float z) {
    return (V3){ m[0] * x + m[4] * y + m[8] * z + m[12], m[1] * x + m[5] * y + m[9] * z + m[13], m[2] * x + m[6] * y + m[10] * z + m[14] };
}

// ---------------------------------------------------------------- scene geometry (rebuilt every frame on the CPU)
typedef struct { float x, y, z, r, g, b, a; } Vtx;
static Vtx tris[4096], lines[512], beams[256];
static int ntris, nlines, nbeams;
static void put(Vtx *arr, int *n, int cap, V3 p, const float *c, float shade) {
    if (*n < cap) arr[(*n)++] = (Vtx){ p.x, p.y, p.z, c[0] * shade, c[1] * shade, c[2] * shade, c[3] };
}
static void quad(Vtx *arr, int *n, int cap, V3 a, V3 b, V3 c, V3 d, const float *col, float shade) {
    put(arr, n, cap, a, col, shade); put(arr, n, cap, b, col, shade); put(arr, n, cap, c, col, shade);
    put(arr, n, cap, a, col, shade); put(arr, n, cap, c, col, shade); put(arr, n, cap, d, col, shade);
}
// an axis-aligned box in the space of m (NULL: world), shaded per face so its shape reads without lighting
static void box(const float *m, float x0, float y0, float z0, float x1, float y1, float z1, const float *col) {
    static const float id[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    if (!m) m = id;
    V3 p[8];
    for (int i = 0; i < 8; i++) p[i] = xform(m, i & 1 ? x1 : x0, i & 2 ? y1 : y0, i & 4 ? z1 : z0);
    quad(tris, &ntris, 4096, p[4], p[5], p[7], p[6], col, 1.0f);    // front (+z)
    quad(tris, &ntris, 4096, p[1], p[0], p[2], p[3], col, 0.55f);   // back
    quad(tris, &ntris, 4096, p[0], p[4], p[6], p[2], col, 0.75f);   // left
    quad(tris, &ntris, 4096, p[5], p[1], p[3], p[7], col, 0.75f);   // right
    quad(tris, &ntris, 4096, p[2], p[6], p[7], p[3], col, 1.25f);   // top
    quad(tris, &ntris, 4096, p[0], p[1], p[5], p[4], col, 0.4f);    // bottom
}
// a thin beam from o along d, as two crossed strips (visible from any side)
static void beam(V3 o, V3 d, float len, float width, const float *col) {
    V3 up = fabsf(d.y) < 0.9f ? (V3){ 0, 1, 0 } : (V3){ 1, 0, 0 };
    V3 u = { d.y * up.z - d.z * up.y, d.z * up.x - d.x * up.z, d.x * up.y - d.y * up.x };
    float ul = sqrtf(u.x * u.x + u.y * u.y + u.z * u.z);
    u.x /= ul; u.y /= ul; u.z /= ul;
    V3 v = { d.y * u.z - d.z * u.y, d.z * u.x - d.x * u.z, d.x * u.y - d.y * u.x };
    V3 e = { o.x + d.x * len, o.y + d.y * len, o.z + d.z * len };
    float h = width / 2;
    for (int k = 0; k < 2; k++) {
        V3 s = k ? v : u;
        quad(beams, &nbeams, 256, (V3){ o.x - s.x * h, o.y - s.y * h, o.z - s.z * h }, (V3){ o.x + s.x * h, o.y + s.y * h, o.z + s.z * h },
             (V3){ e.x + s.x * h, e.y + s.y * h, e.z + s.z * h }, (V3){ e.x - s.x * h, e.y - s.y * h, e.z - s.z * h }, col, 1);
    }
}
// The cabinet model (cabinet view): on the floor, its screen CAB_SCREEN_AHEAD ahead; the game's picture on its screen
static float cab_m[16];
// The cabinet's placement (cabinet view): world <- the cabinet's frame (its floor point under the screen, x right, y up,
// z towards you), a turn about y and a move; cab_m adds its size. The screen (scr_*) is in that frame. Its size, side,
// height, distance and turn are the VIEW tab's (or a two-grip grab's, live: CG).
static float cab_pl[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
static struct {
    int on;                       // grabbing
    float size, side, height, dist, turn;   // live (metres, radians), the options' while not grabbing
    V3 mid0, org0; float span0, ang0, size0, turn0;   // the grab's start (rebased when the stick moves it)
    XrTime grip_t[2];             // when each grip went down
} CG;
// The cabinet's default place (the options are relative to it: 100%, 0 cm, 0 degrees): where the player put it
#define CAB_BASE_SIZE 1.16f
#define CAB_BASE_DIST 0.32f
#define CAB_BASE_HEIGHT (-0.27f)
#define CAB_BASE_SIDE 0.05f
#define CAB_BASE_TURN 2.0f
static float cab_front_z(float floor_y, float size, float *zf) {
    // the cabinet's front stays where the 100% cabinet's stands (you at its guns): bigger ones reach further back
    float sc[5];
    qcab_screen(sc);
    const float front = qcab_front();
    *zf = -CAB_SCREEN_AHEAD - sc[2] + front;
    (void)floor_y;
    return *zf - size * front;
}
static void place_cabinet(float floor_y) {
    if (!S.cab_model) return;
    if (!CG.on) {
        CG.size = CAB_BASE_SIZE * S.opt[QOPT_CAB_SIZE] / 100.0f;
        CG.side = CAB_BASE_SIDE + S.opt[QOPT_CAB_SIDE] / 100.0f;
        CG.height = CAB_BASE_HEIGHT + S.opt[QOPT_CAB_HEIGHT] / 100.0f;
        CG.dist = CAB_BASE_DIST + S.opt[QOPT_CAB_DIST] / 100.0f;
        CG.turn = (CAB_BASE_TURN + S.opt[QOPT_CAB_TURN]) * 3.14159265f / 180.0f;
    }
    float sc[5], zf;
    qcab_screen(sc);
    const float s = CG.size, z = cab_front_z(floor_y, s, &zf) - CG.dist, c = cosf(CG.turn), sn = sinf(CG.turn);
    const float pl[16] = { c, 0, -sn, 0,  0, 1, 0, 0,  sn, 0, c, 0,  CG.side, floor_y + CG.height, z, 1 };
    memcpy(cab_pl, pl, sizeof pl);
    const float sm[16] = { s, 0, 0, 0,  0, s, 0, 0,  0, 0, s, 0,  0, 0, 0, 1 };
    mat_mul(cab_m, cab_pl, sm);
    scr_w = 2 * sc[3] * s; scr_h = 2 * sc[4] * s;
    scr_y = sc[1] * s; scr_z = sc[2] * s;
}
// A gun's cord: from the bottom of its grip to the cabinet, hanging with the slack a 1.6 m cord has (two crossed
// strips a segment, like the beams)
static void cord(V3 a, V3 b, const float *col) {
    const float len = 1.6f, w = 0.007f;
    float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z, dist = sqrtf(dx * dx + dy * dy + dz * dz);
    float sag = dist < len ? 0.55f * sqrtf(len * len - dist * dist) / 2 : 0.01f;
    if (sag > 0.6f) sag = 0.6f;
    V3 prev = a;
    for (int i = 1; i <= 20; i++) {
        float t = i / 20.0f;
        V3 p = { a.x + dx * t, a.y + dy * t - sag * 4 * t * (1 - t), a.z + dz * t };
        V3 d = { p.x - prev.x, p.y - prev.y, p.z - prev.z };
        float l = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
        if (l > 1e-5f) {
            d.x /= l; d.y /= l; d.z /= l;
            V3 up = fabsf(d.y) < 0.9f ? (V3){ 0, 1, 0 } : (V3){ 1, 0, 0 };
            V3 u = { d.y * up.z - d.z * up.y, d.z * up.x - d.x * up.z, d.x * up.y - d.y * up.x };
            float ul = sqrtf(u.x * u.x + u.y * u.y + u.z * u.z);
            u.x /= ul; u.y /= ul; u.z /= ul;
            V3 v = { d.y * u.z - d.z * u.y, d.z * u.x - d.x * u.z, d.x * u.y - d.y * u.x };
            for (int k = 0; k < 2; k++) {
                V3 q = k ? v : u;
                float h = w / 2;
                quad(tris, &ntris, 4096, (V3){ prev.x - q.x * h, prev.y - q.y * h, prev.z - q.z * h },
                     (V3){ prev.x + q.x * h, prev.y + q.y * h, prev.z + q.z * h },
                     (V3){ p.x + q.x * h, p.y + q.y * h, p.z + q.z * h }, (V3){ p.x - q.x * h, p.y - q.y * h, p.z - q.z * h }, col, k ? 0.8f : 1.0f);
            }
        }
        prev = p;
    }
}
// The cabinet view's surroundings (QOPT_ROOM): passthrough only when the headset has it, the courtyard only once made
static int pt_available(void);
static int cabinet_room(void) {
    int r = S.opt[QOPT_ROOM];
    if (r == QROOM_PASSTHROUGH && !pt_available()) return QROOM_DARK;
    if (r == QROOM_COURTYARD && !m2_gl_backdrop_ready()) return QROOM_DARK;
    return r;
}
static int gun_cabinet;   // (load_gun: the cabinet's gun is in your hand)
static void load_reload_lines(void);   // (the reload gesture's lines: below)
// The gun's tilt in your hand at Gun angle 0 (degrees, + raises the barrel): the angle that feels right held as a
// pistol is (players set -15 on the old 0)
#define GUN_PITCH_BASE (-15.0f)
// from the muzzle o to the aim ray's point t ahead of the aim pose m: the direction (d) and the length
static float muzzle_beam(const float *m, V3 o, float t, V3 *d) {
    V3 e = xform(m, 0, 0, -t);
    V3 v = { e.x - o.x, e.y - o.y, e.z - o.z };
    float l = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    if (l > 1e-4f) { d->x = v.x / l; d->y = v.y / l; d->z = v.z / l; }
    return l;
}
static void build_scene(void) {
    ntris = nlines = nbeams = 0;
    const float floor_y = S.floor_y - S.home[13];        // the floor in the world (home only turns about y)
    place_cabinet(floor_y);
    // floor grid, 8 x 8 m around the start position
    static const float grid[4] = { 0.035f, 0.05f, 0.08f, 1 };
    for (int i = -8; i <= 8 && S.cabinet && cabinet_room() == QROOM_DARK; i++) {
        float t = i * 0.5f;
        put(lines, &nlines, 512, (V3){ t, floor_y, -4 }, grid, 1); put(lines, &nlines, 512, (V3){ t, floor_y, 4 }, grid, 1);
        put(lines, &nlines, 512, (V3){ -4, floor_y, t }, grid, 1); put(lines, &nlines, 512, (V3){ 4, floor_y, t }, grid, 1);
    }
    // a plain cabinet around the screen: body down to the floor, a bezel, a hood on top
    static const float body[4] = { 0.06f, 0.06f, 0.07f, 1 }, bezel[4] = { 0.012f, 0.012f, 0.014f, 1 };
    float top = SCR_Y + SCR_H / 2, bot = SCR_Y - SCR_H / 2;
    if (S.cabinet && S.cab_model) {   // the cabinet model: the cabinet gun's cord, from its port on the control panel
        for (int h = 0; h < 2; h++) {
            if (!S.aim_ok[h] || !is_gun(h) || !gun_cabinet) continue;
            float gb[3], an[3];
            qgun_butt(gb);
            // your side's port: player 1 left, player 2 right (as on the cabinet); two guns: each hand its side
            qcab_anchor(S.opt[QOPT_HAND] == QHAND_BOTH ? (h == 0 ? 0 : 1) : (net_local == 1 ? 1 : 0), an);
            static const float black[4] = { 0.02f, 0.02f, 0.022f, 1 };   // (a coloured one read as a laser)
            cord(xform(S.aim_m[h], gb[0], gb[1], gb[2]), xform(cab_m, an[0], an[1], an[2]), black);
        }
    } else if (S.cabinet) {
        box(NULL, -SCR_W / 2 - 0.16f, floor_y, SCR_Z - 0.8f, SCR_W / 2 + 0.16f, top + 0.35f, SCR_Z - 0.02f, body);
        box(NULL, -SCR_W / 2 - 0.06f, bot - 0.06f, SCR_Z - 0.02f, SCR_W / 2 + 0.06f, top + 0.06f, SCR_Z - 0.004f, bezel);
        box(NULL, -SCR_W / 2 - 0.16f, top + 0.2f, SCR_Z - 0.02f, SCR_W / 2 + 0.16f, top + 0.35f, SCR_Z + 0.25f, body);
    }
    // guns and sights
    static const float gun_on[4] = { 0.55f, 0.06f, 0.04f, 1 }, gun_off[4] = { 0.18f, 0.18f, 0.2f, 1 };
    static const float sight[4] = { 1.0f, 0.12f, 0.06f, 0.55f }, sight_off[4] = { 0.5f, 0.5f, 0.55f, 0.25f };
    for (int h = 0; h < 2; h++) {
        if (!S.aim_ok[h]) continue;
        const float *m = S.aim_m[h];
        int act = h == S.active;
        if (!is_gun(h) && !S.menu_open) {
            // the free hand: the magazine while its grip is squeezed (MAGAZINE), else a small marker at its fingers,
            // lit where it can take the slide
            if (mag_reload() && S.mag_hold) {
                static const float mag[4] = { 0.035f, 0.035f, 0.04f, 1 }, brass[4] = { 0.55f, 0.38f, 0.12f, 1 };
                box(m, -0.012f, MAG_BOTTOM_Y, MAG_Z - 0.017f, 0.012f, MAG_TOP_Y - 0.012f, MAG_Z + 0.017f, mag);
                box(m, -0.007f, MAG_TOP_Y - 0.012f, MAG_Z - 0.014f, 0.007f, MAG_TOP_Y, MAG_Z + 0.01f, brass);
            } else if (mag_reload() && !S.hands_model) {    // (with the hands: the hand itself lights)
                static const float idle[4] = { 0.09f, 0.09f, 0.1f, 1 }, near[4] = { 0.7f, 0.42f, 0.08f, 1 }, held[4] = { 1, 0.6f, 0.12f, 1 };
                const float *c = S.rack ? held : S.slide_near ? near : idle;
                box(m, -0.012f, -0.012f, -0.012f, 0.012f, 0.012f, 0.012f, c);
            }
            continue;
        }
        if (is_gun(h) && mag_reload() && S.mag_seated && !gun_cabinet) {   // a magazine in the gun: its base plate
            static const float plate[4] = { 0.02f, 0.02f, 0.024f, 1 }, rim[4] = { 0.55f, 0.38f, 0.12f, 1 };
            float gb[3];
            qgun_grip_base(gb);
            box(m, gb[0] - 0.0135f, gb[1] - 0.011f, gb[2] - 0.027f, gb[0] + 0.0135f, gb[1] + 0.001f, gb[2] + 0.027f, plate);
            box(m, gb[0] - 0.014f, gb[1] - 0.013f, gb[2] - 0.028f, gb[0] + 0.014f, gb[1] - 0.009f, gb[2] + 0.028f, rim);
        }
        if (is_gun(h) && mag_reload() && S.drop_t) {      // the dropped magazine, falling out of the grip
            float t = (float)(S.now - S.drop_t) / 1e9f;
            if (t > 1.2f) S.drop_t = 0;
            else {
                static const float mag[4] = { 0.035f, 0.035f, 0.04f, 1 }, plate[4] = { 0.02f, 0.02f, 0.024f, 1 };
                float fm[16], gb[3];
                memcpy(fm, S.drop_m, sizeof fm);
                fm[13] -= 4.9f * t * t;                      // gravity
                qgun_grip_base(gb);
                float y0 = gb[1] - 0.1f;                     // its top at the grip's base after the first 10 cm
                box(fm, gb[0] - 0.012f, y0 - 0.1f, gb[2] - 0.017f, gb[0] + 0.012f, y0, gb[2] + 0.017f, mag);
                if (!gun_cabinet)   // (the cabinet gun's: its grip's base, qgun_draw_cap in draw_guns)
                    box(fm, gb[0] - 0.0135f, y0 - 0.111f, gb[2] - 0.027f, gb[0] + 0.0135f, y0 - 0.099f, gb[2] + 0.027f, plate);
            }
        }
        if (!S.gun_model && is_gun(h)) {
            box(m, -0.014f, -0.022f, 0.0f, 0.014f, 0.012f, 0.15f, act ? gun_on : gun_off);     // barrel, muzzle at the aim point
            box(m, -0.012f, -0.11f, 0.09f, 0.012f, -0.02f, 0.13f, act ? gun_on : gun_off);     // grip
        }
        float mz = qgun_muzzle_z();         // the sight starts at the muzzle and ends where the aim ray lands
        V3 o = xform(m, 0, qgun_muzzle_y(), -mz), d = { -m[8], -m[9], -m[10] };
        if (S.menu_open) {                  // the menu: always a laser, to the panel
            float len = muzzle_beam(m, o, (S.menu_on[h] ? S.menu_t[h] : 2.0f), &d);
            beam(o, d, len > 0.01f ? len : 0.01f, act ? 0.003f : 0.002f, act ? sight : sight_off);
            continue;
        }
        float len = muzzle_beam(m, o, S.cabinet ? (S.on_screen[h] ? S.hit_t[h] : 4.0f) : (S.hit3d[h] ? S.hit_t[h] : 6.0f), &d);
        if (len < 0.01f) len = 0.01f;
        if (host_player_ammo(net_local) == 0) continue;      // empty: no laser, dot or tracer until the reload
        if (S.opt[QOPT_AIM] == QAIM_LASER) beam(o, d, len, act ? 0.003f : 0.002f, act ? sight : sight_off);
        if (S.opt[QOPT_AIM] == QAIM_TRACER && S.shot_t[h]) {
            // a streak from the muzzle to where the shot went, fading over 65 ms
            float age = (float)(S.now - S.shot_t[h]) / 1e6f;
            if (age >= 0 && age < 65) {
                const float *t = S.tracer[h];
                V3 to = { t[0], t[1], t[2] }, td = { t[3] - t[0], t[4] - t[1], t[5] - t[2] };
                float tl = sqrtf(td.x * td.x + td.y * td.y + td.z * td.z);
                if (tl > 0.01f) {
                    td.x /= tl; td.y /= tl; td.z /= tl;
                    const float col[4] = { 1, 0.32f, 0.025f, 1 - age / 65 };
                    beam(to, td, tl, 0.005f, col);
                }
            }
        }
        if (!S.cabinet && S.hit3d[h] && act && (S.opt[QOPT_AIM] == QAIM_LASER || S.opt[QOPT_AIM] == QAIM_DOT)) {
            // a small marker where the shot lands, sized to the distance
            float r = 0.004f + 0.006f * len;
            static const float dot[4] = { 1.0f, 0.25f, 0.1f, 1 };
            V3 e = { o.x + d.x * (len - r), o.y + d.y * (len - r), o.z + d.z * (len - r) };
            box(NULL, e.x - r, e.y - r, e.z - r, e.x + r, e.y + r, e.z + r, dot);
        }
    }
}

// ---------------------------------------------------------------- GL
static GLuint shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char b[1024]; glGetShaderInfoLog(s, sizeof b, NULL, b); LOGE("shader: %s", b); }
    return s;
}
static GLuint program(const char *vs, const char *fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, shader(GL_VERTEX_SHADER, vs));
    glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char b[1024]; glGetProgramInfoLog(p, sizeof b, NULL, b); LOGE("program: %s", b); }
    return p;
}
static const char *VS_COL =
    XR_GLSL "uniform mat4 mvp;\nlayout(location=0) in vec3 pos;\nlayout(location=1) in vec4 col;\nout vec4 v_col;\n"
    "void main() { gl_Position = mvp * vec4(pos, 1.0); v_col = col; }\n";
static const char *FS_COL =
    XR_GLSL "precision mediump float;\nin vec4 v_col;\nout vec4 o;\nvoid main() { o = v_col; }\n";
static const char *VS_TEX =
    XR_GLSL "uniform mat4 mvp;\nlayout(location=0) in vec3 pos;\nlayout(location=1) in vec2 uv;\nout vec2 v_uv;\n"
    "void main() { gl_Position = mvp * vec4(pos, 1.0); v_uv = uv; }\n";
// "sharp bilinear": crisp game pixels when magnified, smooth when minified, no shimmer at their edges
static const char *FS_TEX =
    XR_GLSL "precision highp float;\nuniform sampler2D tex;\nuniform vec2 texsize;\nuniform int alpha;\nin vec2 v_uv;\nout vec4 o;\n"
    "void main() {\n"
    "  vec2 px = v_uv * texsize;\n"
    "  vec2 seam = floor(px + 0.5);\n"
    "  vec2 w = max(fwidth(px), vec2(1e-4));\n"
    "  px = seam + clamp((px - seam) / w, -0.5, 0.5);\n"
    "  vec4 c = texture(tex, px / texsize);\n"
    "  o = vec4(c.rgb, alpha != 0 ? c.a : 1.0);\n"
    "}\n";

// the turn vignette: clear in the middle, dark towards the edges, the clear part shrinking as vig grows
static const char *VS_VIG =
    XR_GLSL "layout(location=0) in vec2 pos;\nout vec2 p;\nvoid main() { gl_Position = vec4(pos, 0.0, 1.0); p = pos; }\n";
// (by each pixel's direction from the eye, through that eye's projection (k: m0, m8, m5, m9): the eyes' views are
// not centred alike, so a ring in each picture's middle was two rings, out of step; this is one, round where you look)
static const char *FS_VIG =
    XR_GLSL "precision highp float;\nuniform float vig;\nuniform vec4 k;\nin vec2 p;\nout vec4 o;\n"
    "void main() {\n"
    "  vec2 t = vec2((p.x + k.y) / k.x, (p.y + k.w) / k.z);\n"   // the ray's tangents (x / -z, y / -z)
    "  float ang = degrees(atan(length(t)));\n"
    "  float inner = mix(62.0, 22.0, vig);\n"                       // open: past the view; closed: a 22 degree window
    "  float a = smoothstep(inner, inner + 14.0, ang);\n"
    "  o = vec4(0.0, 0.0, 0.0, a * min(1.0, vig * 3.0));\n"
    "}\n";
// the menu's panel: its texture as it is (mipmapped: steady text from any distance)
static const char *FS_MENU =
    XR_GLSL "precision mediump float;\nuniform sampler2D tex;\nin vec2 v_uv;\nout vec4 o;\n"
    "void main() { o = texture(tex, v_uv); }\n";
static void game_gl_init(void);
static void load_gun(void);
// A depth (and stencil) renderbuffer of w x h at the eyes' sample count
static void depth_storage(int w, int h) {
#if XR_GLES
    if (S.ms > 1) S.rb_ms(GL_RENDERBUFFER, S.ms, GL_DEPTH24_STENCIL8, w, h);
#else
    if (S.ms > 1) glRenderbufferStorageMultisample(GL_RENDERBUFFER, S.ms, GL_DEPTH24_STENCIL8, w, h);
#endif
    else glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
}
// what a framebuffer's depth and stencil held is not needed after it was drawn (a tiler need not write it out)
static void discard_depth(void) {
#if XR_GLES
    const GLenum discard[2] = { GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT };
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 2, discard);
#endif
}
#if !XR_GLES
// desktop GL: a multisampled colour renderbuffer for an MSAA framebuffer, resolved into a texture by resolve()
static GLuint ms_color(int w, int h, GLenum fmt) {
    GLuint rb = 0;
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    if (S.ms > 1) glRenderbufferStorageMultisample(GL_RENDERBUFFER, S.ms, fmt, w, h);
    else glRenderbufferStorage(GL_RENDERBUFFER, fmt, w, h);
    return rb;
}
// a framebuffer (MSAA) resolved into tex (w x h), which then holds the picture
static void resolve(GLuint from_fbo, GLuint tex, int w, int h) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, from_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, S.resolve_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
#endif
static int gl_init(void) {
    LOGI("GL: %s / %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    S.ms = 1;
#if XR_GLES
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    if (ext && strstr(ext, "GL_EXT_multisampled_render_to_texture")) {
        S.fb_tex_ms = (PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC)eglGetProcAddress("glFramebufferTexture2DMultisampleEXT");
        S.rb_ms = (PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC)eglGetProcAddress("glRenderbufferStorageMultisampleEXT");
        if (S.fb_tex_ms && S.rb_ms) S.ms = 4;
    }
#else
    {   // 4x MSAA (fewer if the driver has fewer); the swapchains are sRGB: GL encodes into them only with this on
        GLint mx = 0;
        glGetIntegerv(GL_MAX_SAMPLES, &mx);
        S.ms = mx >= 4 ? 4 : mx >= 2 ? 2 : 1;
        glEnable(GL_FRAMEBUFFER_SRGB);
        glGenFramebuffers(1, &S.resolve_fbo);
    }
#endif
    S.prog_col = program(VS_COL, FS_COL);
    S.u_col_mvp = glGetUniformLocation(S.prog_col, "mvp");
    S.prog_tex = program(VS_TEX, FS_TEX);
    S.u_tex_mvp = glGetUniformLocation(S.prog_tex, "mvp");
    S.u_tex_size = glGetUniformLocation(S.prog_tex, "texsize");
    S.u_tex_alpha = glGetUniformLocation(S.prog_tex, "alpha");
    glUseProgram(S.prog_tex);
    glUniform1i(glGetUniformLocation(S.prog_tex, "tex"), 0);
    glGenFramebuffers(1, &S.fbo);
    glGenRenderbuffers(1, &S.depth_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, S.depth_rb);
    depth_storage(S.sc_w, S.sc_h);
#if !XR_GLES
    S.ms_color = ms_color(S.sc_w, S.sc_h, S.sc_fmt);
    glBindFramebuffer(GL_FRAMEBUFFER, S.fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, S.ms_color);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, S.depth_rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) LOGE("eye framebuffer incomplete");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
#endif
    glGenTextures(1, &S.screen_tex);
    glBindTexture(GL_TEXTURE_2D, S.screen_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    S.frame = malloc(FRAME_MAX * 4);
    game_gl_init();
    load_gun();
    S.cab_model = qcab_load("cabinet.tccab");
    S.hands_model = qhand_load("hands.tchand");
    load_reload_lines();
    S.prog_vig = program(VS_VIG, FS_VIG);
    S.u_vig = glGetUniformLocation(S.prog_vig, "vig");
    S.u_vig_k = glGetUniformLocation(S.prog_vig, "k");
    S.prog_menu = program(VS_TEX, FS_MENU);
    S.u_menu_mvp = glGetUniformLocation(S.prog_menu, "mvp");
    glUseProgram(S.prog_menu);
    glUniform1i(glGetUniformLocation(S.prog_menu, "tex"), 0);
    glGenTextures(1, &S.menu_tex);
    apply_options();       // the texture filtering, now that the samplers exist
    LOGI("eye buffers %dx%d, %dx MSAA", S.sc_w, S.sc_h, S.ms);
    return 1;
}

// the game's newest frame -> the screen texture (sRGB: filtered in linear light like everything else)
static void upload_frame(void) {
    int w, h;
    if (!qframe_take(S.frame, FRAME_MAX, &w, &h)) return;
    S.frame_new = 1;
    glBindTexture(GL_TEXTURE_2D, S.screen_tex);
    if (w != S.tex_w || h != S.tex_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, S.frame);
        S.tex_w = w; S.tex_h = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, S.frame);
    }
}

static void draw_col(const float *mvp, const Vtx *v, int n, GLenum mode);

// ---------------------------------------------------------------- the game's 3D scene on the GPU (d3d_vr.h)
// Game camera space: x right, y up, z forward, game units. In VR it sits where the LOCAL space has its origin (your
// eyes when the app started or you recentered): game -> LOCAL = scale * (x, y, -z).
#define CAB_W 1280
#define CAB_H 960
// a colour byte (sRGB-ish) in linear light
static float lin(uint32_t c8) {   // (the exact sRGB curve, as the eye buffers': m2_gl.c to_linear)
    float c = (float)(c8 & 255) / 255.0f;
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

// The gun model (Gun model): the arcade cabinet's own in the player's colour (player 1's red on the left, player 2's
// blue on the right, as the operator's manual has it; the cabinet's model, CC-BY: arcade-gun.json), or the pistol. Again when the setting or your player (online) changes.
static int gun_slot = -1, gun_kind = -1, gun_size = -1, gun_cabinet;   // gun_cabinet: the cabinet's gun is in your hand
// Each model's fit in the hand (qgun_fit, at Gun size 100%): the hands were posed on a grip; this brings each gun's
// grip into the palm and its trigger in front of the trigger finger. QUEST_GUN_FIT=scale,dx,dy,dz (env.txt, metres)
// overrides it, for fitting by eye.
static const float gun_fits[2][4] = {
    // the cabinet's gun at its own size (players liked it so; 82%, 5 mm left, 3 mm up, 2 mm ahead fits the hands'
    // grip exactly, by previews of hands.tchand on arcade-gun.tcgun, but reads as a toy)
    [QGUN_CABINET] = { 1.0f, 0.0f, 0.0f, 0.0f },
    [QGUN_PISTOL] = { 1.0f, 0.0f, 0.0f, 0.0f },             // (the gun the hands were posed on)
};
static void gun_fit(int cabinet, int size_pct) {
    float f[4];
    memcpy(f, gun_fits[cabinet ? QGUN_CABINET : QGUN_PISTOL], sizeof f);
    const char *e = getenv("QUEST_GUN_FIT");
    if (e) sscanf(e, "%f,%f,%f,%f", &f[0], &f[1], &f[2], &f[3]);
    qgun_fit(f[0] * size_pct / 100.0f, f[1], f[2], f[3]);
}
static void load_gun(void) {
    gun_slot = net_local;
    gun_kind = S.opt[QOPT_GUN_MODEL];
    gun_size = S.opt[QOPT_GUN_SIZE];
    gun_cabinet = 0;
    if (gun_kind == QGUN_CABINET) {
        S.gun_model = qgun_load(net_local == 1 ? "arcade-gun-blue.tcgun" : "arcade-gun.tcgun");
        if (S.gun_model) gun_cabinet = 1;
    }
    if (!gun_cabinet) S.gun_model = qgun_load("player-gun.tcgun");
    gun_fit(gun_cabinet, gun_size);
}
static void game_gl_init(void) {
    m2_gl_init();
    // the cabinet's picture: the game's own view, drawn at CAB_W x CAB_H
    glGenTextures(1, &S.cab_tex);
    glBindTexture(GL_TEXTURE_2D, S.cab_tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_SRGB8_ALPHA8, CAB_W, CAB_H);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenRenderbuffers(1, &S.cab_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, S.cab_depth);
    depth_storage(CAB_W, CAB_H);
    glGenFramebuffers(1, &S.cab_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, S.cab_fbo);
#if XR_GLES
    if (S.ms > 1) S.fb_tex_ms(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, S.cab_tex, 0, S.ms);
    else glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, S.cab_tex, 0);
#else
    S.cab_ms_color = ms_color(CAB_W, CAB_H, GL_SRGB8_ALPHA8);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, S.cab_ms_color);
#endif
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, S.cab_depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) LOGE("cabinet framebuffer incomplete");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// the newest scene (and its world for m2_gl.c)
static void game_sync(void) {
    const VrScene *sc = d3d_vr_scene();
    if (sc && sc->serial != S.scene_serial) {
        S.scene = sc;
        S.scene_serial = sc->serial;
        m2_gl_sync();
        {   // a scene the arcade game shows round nothing (the game-over scene: you fall, the camera at your body):
            // immersive, on a screen ahead in the dark while it lasts (arcade_quest.cpp arcade_screen_scene); the
            // view stays immersive (it went to the cabinet's room: a jarring cut)
            extern volatile int arcade_screen_scene, arcade_view_cabinet;
            const int cab = S.opt[QOPT_VIEW] == QVIEW_CABINET, scr = !cab && arcade_screen_scene;
            if (cab != S.cabinet) { S.cabinet = cab; S.cab_dirty = 1; }
            S.screen_scene = scr;
            arcade_view_cabinet = cab || scr;
        }
        S.cab_dirty = 1;
    }
}

// draw the scene with mvp = (clip <- game camera space): the arcade's shading (m2_gl.c); the scene's own triangles
// (d3d_vr.h) are only for the guns' aim
static void draw_game(const float *mvp) { m2_gl_draw(mvp); }

// the game's own projection (its 640 x 480 picture) as a GL matrix: clip <- game camera space
static void game_proj(float *m, const VrScene *sc) {
    float fx = sc ? sc->fx : 640, fy = sc ? sc->fy : -620, w = sc ? (float)sc->w : 640, h = sc ? (float)sc->h : 480;
    float n = 1.0f, f = 4000.0f;
    memset(m, 0, 16 * sizeof(float));
    m[0] = fx / (w * 0.5f);
    m[5] = -fy / (h * 0.5f);
    m[10] = (f + n) / (f - n); m[14] = -2 * f * n / (f - n);
    m[11] = 1;
}
// game camera space -> LOCAL: scale * (x, y, -z)
static void game_to_local(float *m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = S.scale; m[5] = S.scale; m[10] = -S.scale; m[15] = 1;
}
// Horizon (comfort): a turn of the game camera's space that brings the world's up to its y (LEVEL), or only takes out
// the sideways tilt (NO ROLL); the camera's heading stays. Column-major 3x3 in a 4x4.
static void level_mat(float *m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1;
    const VrScene *sc = S.scene;
    if (S.cabinet || !sc || !sc->up_ok || S.opt[QOPT_HORIZON] == QHORIZON_GAME) return;
    float ux = sc->up[0], uy = sc->up[1], uz = sc->up[2];
    if (S.opt[QOPT_HORIZON] == QHORIZON_NOROLL) {       // about z: x' = c x - s y, y' = s x + c y, with u.x -> 0
        float l = sqrtf(ux * ux + uy * uy);
        if (l < 1e-3f) return;
        float c = uy / l, s = ux / l;
        m[0] = c; m[1] = s; m[4] = -s; m[5] = c;
        return;
    }
    // LEVEL: the shortest turn taking u to (0, 1, 0): about u x y = (-uz, 0, ux), by acos(uy)
    float ax = -uz, az = ux, sn = sqrtf(ax * ax + az * az), c = uy;
    if (sn < 1e-5f) return;
    ax /= sn; az /= sn;
    float t = 1 - c;
    float r[9] = { t * ax * ax + c, sn * az, t * ax * az,          // columns: R e_x, R e_y, R e_z (Rodrigues, axis (ax, 0, az))
                   -sn * az, c, sn * ax,
                   t * ax * az, -sn * ax, t * az * az + c };
    m[0] = r[0]; m[1] = r[1]; m[2] = r[2];
    m[4] = r[3]; m[5] = r[4]; m[6] = r[5];
    m[8] = r[6]; m[9] = r[7]; m[10] = r[8];
}

// a textured quad (positions xyz + uv per corner) with the overlay program
static void tex_quad(const float *mvp, const float *q, GLuint tex, float tw, float th, int alpha) {
    glUseProgram(S.prog_tex);
    glUniformMatrix4fv(S.u_tex_mvp, 1, GL_FALSE, mvp);
    glUniform2f(S.u_tex_size, tw, th);
    glUniform1i(S.u_tex_alpha, alpha);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), q);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), q + 3);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}
// the game's screen's corners at depth zp in game units (left, right, top, bottom), as the game projects
static void screen_rect(float zp, float *l, float *r, float *t, float *b) {
    const VrScene *sc = S.scene;
    float fx = sc ? sc->fx : 640, fy = sc ? sc->fy : -620, cx = sc ? sc->cx : 320, cy = sc ? sc->cy : 240;
    float w = sc ? (float)sc->w : 640, h = sc ? (float)sc->h : 480;
    *l = -cx * zp / fx, *r = (w - cx) * zp / fx, *t = -cy * zp / fy, *b = (h - cy) * zp / fy;
}
// the 2D overlay (HUD, menus) where the game's screen is: the picture's corners at depth zp in game units
static void draw_overlay(const float *mvp, float zp) {
    if (!S.tex_w) return;
    float l, r, t, b;
    screen_rect(zp, &l, &r, &t, &b);
    const float q[] = { l, t, zp, 0, 0,  r, t, zp, 1, 0,  r, b, zp, 1, 1,
                        l, t, zp, 0, 0,  r, b, zp, 1, 1,  l, b, zp, 0, 1 };
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    tex_quad(mvp, q, S.screen_tex, (float)S.tex_w, (float)S.tex_h, 1);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}
// the game's full-screen fade, over everything
static void draw_fade(void) {
    const VrScene *sc = S.scene;
    if (!sc || !sc->fade || !(sc->fade >> 24)) return;
    uint32_t c = sc->fade;
    float col[4] = { lin(c >> 16), lin(c >> 8), lin(c), (float)(c >> 24) / 255.0f };
    static const float id[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    Vtx q[6];
    const float xy[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
    for (int i = 0; i < 6; i++) q[i] = (Vtx){ xy[i][0], xy[i][1], 0, col[0], col[1], col[2], col[3] };
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    draw_col(id, q, 6, GL_TRIANGLES);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}
// the cabinet's picture: the game's view as the game projects it, the overlay on top
static void render_cabinet_view(void) {
    if (!S.cab_dirty && !S.frame_new) return;
    S.cab_dirty = S.frame_new = 0;
    const VrScene *sc = S.scene;
    glBindFramebuffer(GL_FRAMEBUFFER, S.cab_fbo);
    glViewport(0, 0, CAB_W, CAB_H);
    uint32_t c = sc ? sc->clear : 0;
    glClearColor(lin(c >> 16), lin(c >> 8), lin(c), 1);
    glClearDepthf(1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    float p[16];
    game_proj(p, sc);
    m2_gl_near(1.0f);                       // (game_proj's near plane)
    draw_game(p);
    if (S.tex_w) {
        static const float id[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        const float q[] = { -1, 1, 0, 0, 0,  1, 1, 0, 1, 0,  1, -1, 0, 1, 1,  -1, 1, 0, 0, 0,  1, -1, 0, 1, 1,  -1, -1, 0, 0, 1 };
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        tex_quad(id, q, S.screen_tex, (float)S.tex_w, (float)S.tex_h, 1);
        glDisable(GL_BLEND);
    }
    draw_fade();
    discard_depth();
#if !XR_GLES
    resolve(S.cab_fbo, S.cab_tex, CAB_W, CAB_H);
#endif
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// Where the gun's ray (LOCAL space) first meets the scene, as the game's screen position. The game then casts its
// own ray from its camera through that pixel, which passes through the same point: you hit what the laser
// touches. Returns 1 with the pixel when the point is in the game's picture; *dist gets the distance in metres.
static int scene_aim(const float *m, float *sx, float *sy, float *dist) {
    const VrScene *sc = S.scene;
    if (!sc) return 0;
    float k = 1.0f / S.scale;
    V3 o = { m[12] * k, m[13] * k, -m[14] * k }, d = { -m[8], -m[9], m[10] };   // LOCAL -> game: (x, y, -z) / scale
    // The 2D layer first: where the ray crosses its panel, when something is drawn there (menus, title screens, the
    // HUD) or the frame has no 3D at all. The game draws its cursor there, so the cursor sits on the laser.
    float zp = S.panel_m * k;
    if (d.z > 1e-4f) {
        float t = (zp - o.z) / d.z;
        if (t > 0) {
            float px = sc->cx + sc->fx * (o.x + d.x * t) / zp, py = sc->cy + sc->fy * (o.y + d.y * t) / zp;
            if (px >= 0 && px < sc->w && py >= 0 && py < sc->h) {
                int opaque = 0;
                if (S.tex_w && S.tex_h) {
                    int fx = (int)(px * S.tex_w / sc->w), fy = (int)(py * S.tex_h / sc->h);
                    opaque = (S.frame[(size_t)fy * S.tex_w + fx] >> 24) != 0;
                }
                if (opaque || !sc->nb) { *sx = px; *sy = py; *dist = t * S.scale; return 1; }
            }
        }
    }
    {   // the world is drawn turned by the Horizon setting: the ray back into the game camera's own space
        float lv[16];
        level_mat(lv);
        V3 a = o, b = d;          // lv is a rotation: its inverse is its transpose
        o = (V3){ lv[0] * a.x + lv[1] * a.y + lv[2] * a.z, lv[4] * a.x + lv[5] * a.y + lv[6] * a.z, lv[8] * a.x + lv[9] * a.y + lv[10] * a.z };
        d = (V3){ lv[0] * b.x + lv[1] * b.y + lv[2] * b.z, lv[4] * b.x + lv[5] * b.y + lv[6] * b.z, lv[8] * b.x + lv[9] * b.y + lv[10] * b.z };
    }
    float best = 1e30f;
    for (uint32_t i = 0; i < sc->nb; i++) {
        const VrBatch *b = &sc->b[i];
        if (!b->zwrite || (b->blend && b->dst == 2)) continue;        // effects: not solid
        for (uint32_t j = b->first; j + 2 < b->first + b->count + 0 && j < b->first + b->count; j += 3) {
            const VrVert *A = &sc->v[j], *B = &sc->v[j + 1], *C = &sc->v[j + 2];
            VrVert gtv[3];
            if (b->group) {      // an earlier frame's slice: where it is now
                const float *g = sc->groupm[b->group - 1];
                const VrVert *in[3] = { A, B, C };
                for (int q = 0; q < 3; q++) {
                    gtv[q] = *in[q];
                    gtv[q].x = g[0] * in[q]->x + g[1] * in[q]->y + g[2] * in[q]->z + g[9];
                    gtv[q].y = g[3] * in[q]->x + g[4] * in[q]->y + g[5] * in[q]->z + g[10];
                    gtv[q].z = g[6] * in[q]->x + g[7] * in[q]->y + g[8] * in[q]->z + g[11];
                }
                A = &gtv[0]; B = &gtv[1]; C = &gtv[2];
            }
            V3 e1 = { B->x - A->x, B->y - A->y, B->z - A->z }, e2 = { C->x - A->x, C->y - A->y, C->z - A->z };
            V3 p = { d.y * e2.z - d.z * e2.y, d.z * e2.x - d.x * e2.z, d.x * e2.y - d.y * e2.x };
            float det = e1.x * p.x + e1.y * p.y + e1.z * p.z;
            if (fabsf(det) < 1e-9f) continue;
            float inv = 1.0f / det;
            V3 tv = { o.x - A->x, o.y - A->y, o.z - A->z };
            float u = (tv.x * p.x + tv.y * p.y + tv.z * p.z) * inv;
            if (u < 0 || u > 1) continue;
            V3 q = { tv.y * e1.z - tv.z * e1.y, tv.z * e1.x - tv.x * e1.z, tv.x * e1.y - tv.y * e1.x };
            float v = (d.x * q.x + d.y * q.y + d.z * q.z) * inv;
            if (v < 0 || u + v > 1) continue;
            float t = (e2.x * q.x + e2.y * q.y + e2.z * q.z) * inv;
            if (t > 0.05f && t < best) best = t;
        }
    }
    int hit = best < 1e29f;
    float t = hit ? best : 3000.0f;
    V3 h = { o.x + d.x * t, o.y + d.y * t, o.z + d.z * t };
    *dist = hit ? t * S.scale : 0;
    if (h.z < 1.0f) return 0;                                            // behind the game's camera
    *sx = sc->cx + sc->fx * h.x / h.z;
    *sy = sc->cy + sc->fy * h.y / h.z;
    return *sx >= 0 && *sx < sc->w && *sy >= 0 && *sy < sc->h;
}

static void draw_col(const float *mvp, const Vtx *v, int n, GLenum mode) {
    if (!n) return;
    glUseProgram(S.prog_col);
    glUniformMatrix4fv(S.u_col_mvp, 1, GL_FALSE, mvp);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), &v->x);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx), &v->r);
    glDrawArrays(mode, 0, n);
}
static void draw_dim(float a) {
    static const float id[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    Vtx q[6];
    const float xy[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
    for (int i = 0; i < 6; i++) q[i] = (Vtx){ xy[i][0], xy[i][1], 0, 0, 0, 0, a };
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    draw_col(id, q, 6, GL_TRIANGLES);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}
// Turn vignette (comfort): while the game camera turns or moves fast, the edges of the view darken (by the
// direction from the eye: proj, this eye's projection)
static void draw_vignette(const float *proj) {
    if (S.vig < 0.01f || S.cabinet || S.screen_scene) return;
    static const float q[12] = { -1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1 };
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(S.prog_vig);
    glUniform1f(S.u_vig, S.vig);
    glUniform4f(S.u_vig_k, proj[0], proj[8], proj[5], proj[9]);
    glDisableVertexAttribArray(1);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), q);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}
static void update_vignette(float dt) {
    const VrScene *sc = S.scene;
    float target = 0;
    if (sc && !S.menu_open && S.opt[QOPT_VIGNETTE] != QVIGNETTE_OFF) {
        // degrees a second the world turns around you (the heading only when the horizon is kept level), and metres a
        // second it moves past you: the game walks you at about 0.7 m/s indoors, swoops at 8 to 12, turns at up to 200
        // degrees a second; LOW eases in later and closes less than HIGH
        const int high = S.opt[QOPT_VIGNETTE] == QVIGNETTE_HIGH;
        float rate = (S.opt[QOPT_HORIZON] == QHORIZON_LEVEL ? sc->turn : sc->turn_all) * 60.0f * 57.2958f;
        float speed = sc->move * S.scale * 60.0f;
        float t_turn = high ? (rate - 15.0f) / 60.0f : (rate - 30.0f) / 90.0f;
        float t_move = high ? (speed - 1.0f) / 3.0f : (speed - 2.0f) / 6.0f;
        target = t_turn > t_move ? t_turn : t_move;
        target = target < 0 ? 0 : target > 1 ? 1 : target;
        target *= high ? 1.0f : 0.65f;
    }
    float tau = target > S.vig ? 0.08f : 0.35f;          // closes quickly, opens gently
    S.vig += (target - S.vig) * (1 - expf(-dt / tau));
}
static QMenuHost menu_host(void);
// the menu's panel: redrawn on the CPU only when it changed, then only the box that changed goes up
static void draw_menu(const float *pv) {
    static uint8_t *px;
    if (!px) px = calloc((size_t)QMENU_W * QMENU_H, 4);
    if (!px) return;
    QMenuHost host = menu_host();
    int box[4] = { 0, 0, QMENU_W, QMENU_H };
    int drew = qmenu_render(&host, px, (double)S.now / 1e9, box);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, S.menu_tex);
    if (!S.menu_ready) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, QMENU_W, QMENU_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenerateMipmap(GL_TEXTURE_2D);
        S.menu_ready = 1;
    } else if (drew && box[2] > 0 && box[3] > 0) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, QMENU_W);
        glTexSubImage2D(GL_TEXTURE_2D, 0, box[0], box[1], box[2], box[3], GL_RGBA, GL_UNSIGNED_BYTE, px + ((size_t)box[1] * QMENU_W + box[0]) * 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    const float *m = S.menu_m, hw = MENU_W_M / 2, hh = MENU_H_M / 2;
    V3 c[4];
    const float cx[4] = { -hw, hw, hw, -hw }, cy[4] = { hh, hh, -hh, -hh };
    for (int i = 0; i < 4; i++) c[i] = xform(m, cx[i], cy[i], 0);
    const float q[] = { c[0].x, c[0].y, c[0].z, 0, 0,  c[1].x, c[1].y, c[1].z, 1, 0,  c[2].x, c[2].y, c[2].z, 1, 1,
                        c[0].x, c[0].y, c[0].z, 0, 0,  c[2].x, c[2].y, c[2].z, 1, 1,  c[3].x, c[3].y, c[3].z, 0, 1 };
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(S.prog_menu);
    glUniformMatrix4fv(S.u_menu_mvp, 1, GL_FALSE, pv);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), q);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), q + 3);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisable(GL_BLEND);
}
// a texture on a quad: centred at c, half extents along r and u (unit vectors), over everything
static void draw_tex_quad(const float *pv, GLuint tex, V3 c, V3 r, V3 u, float hw, float hh) {
    V3 q4[4];
    const float sx[4] = { -1, 1, 1, -1 }, sy[4] = { 1, 1, -1, -1 };
    for (int i = 0; i < 4; i++)
        q4[i] = (V3){ c.x + r.x * hw * sx[i] + u.x * hh * sy[i], c.y + r.y * hw * sx[i] + u.y * hh * sy[i], c.z + r.z * hw * sx[i] + u.z * hh * sy[i] };
    const float q[] = { q4[0].x, q4[0].y, q4[0].z, 0, 0,  q4[1].x, q4[1].y, q4[1].z, 1, 0,  q4[2].x, q4[2].y, q4[2].z, 1, 1,
                        q4[0].x, q4[0].y, q4[0].z, 0, 0,  q4[2].x, q4[2].y, q4[2].z, 1, 1,  q4[3].x, q4[3].y, q4[3].z, 0, 1 };
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUseProgram(S.prog_menu);
    glUniformMatrix4fv(S.u_menu_mvp, 1, GL_FALSE, pv);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), q);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), q + 3);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}
// Online: what is happening (a partner joined or left, waiting for the host, catching up with its game), on a label
// low in front of you
#define OLAB_W 720
#define OLAB_H 72
static void draw_online_label(const float *pv) {
    if (!online_session() || !S.head_ok || S.menu_open) return;   // the menu's ONLINE tab says it all
    static char shown[100]; static GLuint tex; static double next_view; static online_view v;
    double now = (double)S.now / 1e9;
    if (now >= next_view) { online_get_view(&v); next_view = now + 0.5; }
    char text[100] = "";
    if (!online_toast(text, sizeof text, NULL)) {
        if (v.state == OV_CATCHING_UP) snprintf(text, sizeof text, "CATCHING UP WITH THE HOST'S GAME... %d%%", v.catch_up_pct);
        else if (!v.host && v.state == OV_CONNECTING) snprintf(text, sizeof text, "CONNECTING TO THE HOST...");
        else if (!v.host && v.state == OV_PARTNER_GONE) snprintf(text, sizeof text, "THE HOST IS NOT ANSWERING - MENU: ONLINE TAB TO LEAVE");
        else if (v.desync) snprintf(text, sizeof text, "OUT OF STEP WITH THE HOST - LEAVE AND JOIN AGAIN");
    }
    if (!text[0]) return;
    if (strcmp(text, shown) || !tex) {
        snprintf(shown, sizeof shown, "%s", text);
        static uint8_t px[OLAB_W * OLAB_H * 4];
        QCanvas c;
        qc_init(&c, px, OLAB_W, OLAB_H);
        qc_fill(&c, qrgba(0, 0, 0, 0));
        qc_rrect(&c, 2, 2, OLAB_W - 4, OLAB_H - 4, 16, qrgba(10, 14, 22, 215));
        qc_rrect_line(&c, 2, 2, OLAB_W - 4, OLAB_H - 4, 16, 3, qrgb(54, 183, 207));
        qc_text_fit(&c, UI_B32, 14, OLAB_H / 2 + qc_font_ascent(UI_B32) / 2 - 2, OLAB_W - 28, text, qrgb(235, 240, 245), 1);
        if (!tex) glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, OLAB_W, OLAB_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    // 1.3 m ahead of the head, a little below the eyes, upright and turned to you
    const float *h = S.head_m;
    float fx = -h[8], fz = -h[10], l = sqrtf(fx * fx + fz * fz);
    if (l < 1e-3f) return;
    fx /= l; fz /= l;
    V3 c = { h[12] + fx * 1.3f, h[13] - 0.32f, h[14] + fz * 1.3f };
    V3 r = { -fz, 0, fx }, u = { 0, 1, 0 };
    draw_tex_quad(pv, tex, c, r, u, 0.36f, 0.36f * OLAB_H / OLAB_W);
}
// The MAGAZINE reload's next step, on a small label over the gun turned to you: an empty gun's magazine to drop (B or
// Y), a new one to put in, the slide to rack (the gun itself shows its magazine's base plate under the grip)
#define HINT_W 448
#define HINT_H 64
static void draw_rack_hint(const float *pv) {
    int f = free_hand(), g = 1 - f;
    if (!mag_reload() || S.menu_open || !S.aim_ok[g] || !S.head_ok) return;
    int a = host_player_ammo(net_local), k;
    if (a < 0) return;
    if (!S.mag_seated) k = 1;                        // out: a new one
    else if (S.mag_in) k = 2;                        // a new one in: the slide
    else if (a == 0 && host_player_playing(net_local)) k = 0;   // empty, in play: out with the old one
    else return;
    static GLuint texs[3];
    GLuint tex = texs[k];
    if (!tex) {
        static uint8_t px[HINT_W * HINT_H * 4];
        char what[40];
        if (k == 0) snprintf(what, sizeof what, "DROP THE MAG: %s", g == 1 ? "B" : "Y");
        else snprintf(what, sizeof what, "%s", k == 1 ? "INSERT A MAGAZINE" : "RACK THE SLIDE");
        QCanvas c;
        qc_init(&c, px, HINT_W, HINT_H);
        qc_fill(&c, qrgba(0, 0, 0, 0));
        qc_rrect(&c, 2, 2, HINT_W - 4, HINT_H - 4, 14, qrgba(12, 10, 10, 200));
        qc_rrect_line(&c, 2, 2, HINT_W - 4, HINT_H - 4, 14, 3, qrgb(255, 140, 30));
        qc_text_fit(&c, UI_B32, 8, HINT_H / 2 + qc_font_ascent(UI_B32) / 2 - 2, HINT_W - 16, what, qrgb(255, 170, 60), 1);
        glGenTextures(1, &tex);
        texs[k] = tex;
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, HINT_W, HINT_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    // 14 cm wide, 7 cm over the slide's rear, facing the head
    V3 c = xform(S.aim_m[g], 0, 0.07f, 0.02f);
    V3 d = { S.head_m[12] - c.x, S.head_m[13] - c.y, S.head_m[14] - c.z };
    float dl = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    if (dl < 1e-3f) return;
    d.x /= dl; d.y /= dl; d.z /= dl;
    V3 r = { d.z, 0, -d.x };                             // (0,1,0) x d
    float rl = sqrtf(r.x * r.x + r.z * r.z);
    if (rl < 1e-3f) return;
    r.x /= rl; r.z /= rl;
    V3 u = { d.y * r.z - d.z * r.y, d.z * r.x - d.x * r.z, d.x * r.y - d.y * r.x };
    const float hw = 0.07f, hh = hw * HINT_H / HINT_W;   // (14 cm wide: the text as large as at 320 px over 10 cm)
    V3 q4[4];
    const float sx[4] = { -1, 1, 1, -1 }, sy[4] = { 1, 1, -1, -1 };
    for (int i = 0; i < 4; i++)
        q4[i] = (V3){ c.x + r.x * hw * sx[i] + u.x * hh * sy[i], c.y + r.y * hw * sx[i] + u.y * hh * sy[i], c.z + r.z * hw * sx[i] + u.z * hh * sy[i] };
    const float q[] = { q4[0].x, q4[0].y, q4[0].z, 0, 0,  q4[1].x, q4[1].y, q4[1].z, 1, 0,  q4[2].x, q4[2].y, q4[2].z, 1, 1,
                        q4[0].x, q4[0].y, q4[0].z, 0, 0,  q4[2].x, q4[2].y, q4[2].z, 1, 1,  q4[3].x, q4[3].y, q4[3].z, 0, 1 };
    glDisable(GL_DEPTH_TEST);                          // over the gun and the hands, always readable
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUseProgram(S.prog_menu);
    glUniformMatrix4fv(S.u_menu_mvp, 1, GL_FALSE, pv);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), q);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), q + 3);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}
// The hands (quest_hand.c): on each gun, holding its grip, the trigger finger following the trigger; the free hand
// closing with the grip and the trigger as your fingers do on the controller, round the magazine while it holds one,
// round the slide while it racks, and lit while it is near enough to take the slide.
// ---------------------------------------------------------------- online: your partner's guns and hands
// Your guns and hands as drawn this frame (ME: draw_guns and draw_hands fill it) go to your partner (pose_send), in
// the frame you stand in (stand_frame); theirs come back (pose_recv) and are drawn beside you in yours, player 1 on
// the left and player 2 on the right as at the cabinet. Either view: each player's cabinet is their own, but the
// guns and hands are where they are at it.
#define PARTNER_SIDE 0.62f      // metres between the two players
#define POSE_VER 1
#define POSE_HAND 23            // a hand: flags, position (3 x int16, mm), turn (4 x int16), 5 curls, slide, trigger, flash
#define POSE_LEN (2 + 2 * POSE_HAND)
static struct {
    int hand[2], gun[2];        // drawn this frame: the hand model; a gun
    float m[2][16], curl[2][5], slide[2], trig[2], flash[2];
} ME;
static struct {
    int on, loaded, gun_ok;     // drawing them; their gun model loaded (kind * 2 + slot, -1 none), and it loaded
    int kind;                   // 1 the cabinet's gun, 0 the pistol
    int hand[2], gun[2], seen[2];
    float pos[2][3], q[2][4];   // smoothed (their stand frame)
    float m[2][16], curl[2][5], slide[2], trig[2], flash[2];
} PR = { .loaded = -1 };
static int partner_slot(void) { return net_local == 1 ? 0 : 1; }
static void rigid_inv(float *o, const float *m) {   // (o may be m)
    float r[16] = { m[0], m[4], m[8], 0,  m[1], m[5], m[9], 0,  m[2], m[6], m[10], 0,  0, 0, 0, 1 };
    r[12] = -(r[0] * m[12] + r[4] * m[13] + r[8] * m[14]);
    r[13] = -(r[1] * m[12] + r[5] * m[13] + r[9] * m[14]);
    r[14] = -(r[2] * m[12] + r[6] * m[13] + r[10] * m[14]);
    memcpy(o, r, sizeof r);
}
static void mat_quat(const float *m, float *q) {   // a rigid matrix's turn (x, y, z, w)
    const float tr = m[0] + m[5] + m[10];
    if (tr > 0) { float s = sqrtf(tr + 1) * 2; q[3] = 0.25f * s; q[0] = (m[6] - m[9]) / s; q[1] = (m[8] - m[2]) / s; q[2] = (m[1] - m[4]) / s; }
    else if (m[0] > m[5] && m[0] > m[10]) { float s = sqrtf(1 + m[0] - m[5] - m[10]) * 2; q[3] = (m[6] - m[9]) / s; q[0] = 0.25f * s; q[1] = (m[4] + m[1]) / s; q[2] = (m[8] + m[2]) / s; }
    else if (m[5] > m[10]) { float s = sqrtf(1 + m[5] - m[0] - m[10]) * 2; q[3] = (m[8] - m[2]) / s; q[0] = (m[4] + m[1]) / s; q[1] = 0.25f * s; q[2] = (m[9] + m[6]) / s; }
    else { float s = sqrtf(1 + m[10] - m[0] - m[5]) * 2; q[3] = (m[1] - m[4]) / s; q[0] = (m[8] + m[2]) / s; q[1] = (m[9] + m[6]) / s; q[2] = 0.25f * s; }
}
// The frame you stand in (world <- it): the floor where you stand, facing the screen. Immersive: under where you
// recentered; the cabinet view: where the cabinet's default place has you, moved and turned as the cabinet is.
static void stand_frame(float *m) {
    const float floor_y = S.floor_y - S.home[13];
    const float t[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, floor_y, 0, 1 };
    if (!S.cabinet || !S.cab_model) { memcpy(m, t, sizeof t); return; }
    float zf;   // the default placement (place_cabinet's with the options at 0, on a floor at 0), undone after cab_pl
    const float z = cab_front_z(0, CAB_BASE_SIZE, &zf) - CAB_BASE_DIST, a = CAB_BASE_TURN * 3.14159265f / 180.0f;
    const float c = cosf(a), sn = sinf(a);
    const float d[16] = { c, 0, -sn, 0,  0, 1, 0, 0,  sn, 0, c, 0,  CAB_BASE_SIDE, CAB_BASE_HEIGHT, z, 1 };
    float di[16];
    rigid_inv(di, d);
    mat_mul(m, cab_pl, di);
}
static void put16(uint8_t *p, float v) {
    long k = lroundf(v);
    if (k < -32767) k = -32767;
    if (k > 32767) k = 32767;
    p[0] = (uint8_t)k; p[1] = (uint8_t)((uint16_t)k >> 8);
}
static float get16(const uint8_t *p) { return (float)(int16_t)(p[0] | p[1] << 8); }
static uint8_t unit8(float x) { return (uint8_t)(fminf(1, fmaxf(0, x)) * 255 + 0.5f); }
static void pose_send(void) {
    if (!online_session()) return;
    float st[16], sti[16];
    stand_frame(st);
    rigid_inv(sti, st);
    uint8_t b[POSE_LEN];
    memset(b, 0, sizeof b);
    b[0] = POSE_VER; b[1] = (uint8_t)(gun_cabinet ? 1 : 0);
    for (int h = 0; h < 2; h++) {
        uint8_t *o = b + 2 + h * POSE_HAND;
        o[0] = (uint8_t)((ME.hand[h] ? 1 : 0) | (ME.gun[h] ? 2 : 0));
        if (!o[0]) continue;
        float r[16], q[4];
        mat_mul(r, sti, ME.m[h]);
        mat_quat(r, q);
        for (int k = 0; k < 3; k++) put16(o + 1 + 2 * k, r[12 + k] * 1000);
        for (int k = 0; k < 4; k++) put16(o + 7 + 2 * k, q[k] * 32767);
        for (int k = 0; k < 5; k++) o[15 + k] = unit8(ME.curl[h][k]);
        o[20] = unit8(ME.slide[h]); o[21] = unit8(ME.trig[h]); o[22] = unit8(ME.flash[h]);
    }
    online_pose_out(b, sizeof b);
}
static void pose_recv(float dt) {
    uint8_t b[ONLINE_POSE_MAX];
    uint32_t age = 0;
    const int n = online_session() ? online_pose_in(b, &age) : 0;
    PR.on = n >= POSE_LEN && b[0] == POSE_VER && age < 700;      // (silent that long: gone, or not in a game)
    if (!PR.on) { PR.seen[0] = PR.seen[1] = 0; return; }
    PR.kind = b[1] ? 1 : 0;
    float st[16], base[16];
    stand_frame(st);
    const float off[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  partner_slot() == 1 ? PARTNER_SIDE : -PARTNER_SIDE, 0, 0, 1 };
    mat_mul(base, st, off);
    const float k = dt > 0 ? 1 - expf(-dt * 30) : 1;   // smoothed towards each datagram's (they come at ~60 Hz)
    for (int h = 0; h < 2; h++) {
        const uint8_t *o = b + 2 + h * POSE_HAND;
        PR.hand[h] = o[0] & 1; PR.gun[h] = (o[0] & 2) != 0;
        if (!(o[0] & 3)) { PR.seen[h] = 0; continue; }
        float p[3], q[4], l = 0;
        for (int j = 0; j < 3; j++) p[j] = get16(o + 1 + 2 * j) / 1000;
        for (int j = 0; j < 4; j++) { q[j] = get16(o + 7 + 2 * j) / 32767; l += q[j] * q[j]; }
        if (l < 1e-6f) { PR.seen[h] = 0; continue; }
        const float dx = p[0] - PR.pos[h][0], dy = p[1] - PR.pos[h][1], dz = p[2] - PR.pos[h][2];
        if (!PR.seen[h] || dx * dx + dy * dy + dz * dz > 0.25f) {   // new, or a jump: there at once
            memcpy(PR.pos[h], p, sizeof p); memcpy(PR.q[h], q, sizeof q); PR.seen[h] = 1;
        } else {
            float dot = 0;
            for (int j = 0; j < 4; j++) dot += q[j] * PR.q[h][j];
            const float sg = dot < 0 ? -1.0f : 1.0f;
            for (int j = 0; j < 3; j++) PR.pos[h][j] += (p[j] - PR.pos[h][j]) * k;
            for (int j = 0; j < 4; j++) PR.q[h][j] += (sg * q[j] - PR.q[h][j]) * k;
        }
        l = 0;
        for (int j = 0; j < 4; j++) l += PR.q[h][j] * PR.q[h][j];
        l = 1 / sqrtf(l);
        for (int j = 0; j < 4; j++) PR.q[h][j] *= l;
        XrPosef ps = { { PR.q[h][0], PR.q[h][1], PR.q[h][2], PR.q[h][3] }, { PR.pos[h][0], PR.pos[h][1], PR.pos[h][2] } };
        float rel[16];
        mat_pose(rel, ps);
        mat_mul(PR.m[h], base, rel);
        for (int j = 0; j < 5; j++) PR.curl[h][j] = o[15 + j] / 255.0f;
        PR.slide[h] = o[20] / 255.0f; PR.trig[h] = o[21] / 255.0f; PR.flash[h] = o[22] / 255.0f;
    }
}
// their gun model: the cabinet's in their colour, or the pistol (model slot 1)
static void partner_gun_load(void) {
    if (!PR.on) return;
    const int want = PR.kind * 2 + partner_slot();
    if (want == PR.loaded) return;
    PR.loaded = want;
    qgun_use(1);
    PR.gun_ok = qgun_load(!PR.kind ? "player-gun.tcgun" : partner_slot() == 1 ? "arcade-gun-blue.tcgun" : "arcade-gun.tcgun");
    gun_fit(PR.kind, 100);
    qgun_use(0);
}
static void draw_partner(const float *proj, const float *view) {
    if (!PR.on) return;
    static const float lit[3] = { 1, 1, 1 };
    for (int h = 0; h < 2; h++) {
        if (!PR.seen[h]) continue;
        if (PR.gun[h] && PR.gun_ok) {
            qgun_use(1);
            qgun_draw(view, proj, PR.m[h], PR.slide[h], PR.trig[h], PR.flash[h], lit, 1);
            qgun_use(0);
        }
        if (PR.hand[h] && S.hands_model) qhand_draw(view, proj, PR.m[h], h, PR.curl[h], lit);
    }
}

static float get_float(XrAction a, XrPath sub);
static void draw_hands(const float *proj, const float *view) {
    if (!S.hands_model) return;
    for (int h = 0; h < 2; h++) {
        if (!S.aim_ok[h]) continue;
        float c[5];
        static const float lit[3] = { 1, 1, 1 }, near[3] = { 1.35f, 1.05f, 0.75f };
        const float *tint = lit;
        if (is_gun(h)) {
            if (!S.gun_model) continue;
            c[QHAND_THUMB] = 0.9f; c[QHAND_INDEX] = 0.42f + 0.18f * S.trig[h];
            c[QHAND_MIDDLE] = 0.85f; c[QHAND_RING] = 0.88f; c[QHAND_PINKY] = 0.9f;
        } else if (S.support) {                          // on the gun, round the gun hand
            float sm[16], s2 = h == 0 ? 1.0f : -1.0f;    // (placed for the left hand; mirrored for the right)
            const float ax = -25 * 3.14159265f / 180, ay = -10 * s2 * 3.14159265f / 180;
            const float ca = cosf(ax), sa = sinf(ax), cb = cosf(ay), sb = sinf(ay);
            // turned about x, then about y, and moved: tools/vr/hands_import.py's previews put the hand there
            const float t[16] = { cb, 0, -sb, 0,  sb * sa, ca, cb * sa, 0,  sb * ca, -sa, cb * ca, 0,
                                  -0.02f * s2, -0.025f, -0.03f, 1 };
            mat_mul(sm, S.aim_m[1 - h], t);
            c[QHAND_THUMB] = 0.8f; c[QHAND_INDEX] = 0.7f; c[QHAND_MIDDLE] = 0.72f; c[QHAND_RING] = 0.75f; c[QHAND_PINKY] = 0.8f;
            qhand_draw(view, proj, sm, h, c, lit);
            ME.hand[h] = 1; memcpy(ME.m[h], sm, sizeof sm); memcpy(ME.curl[h], c, sizeof c);
            continue;
        } else if (mag_reload() && S.mag_hold) {         // round the magazine
            c[QHAND_THUMB] = 0.85f; c[QHAND_INDEX] = 0.8f; c[QHAND_MIDDLE] = 0.85f; c[QHAND_RING] = 0.88f; c[QHAND_PINKY] = 0.9f;
        } else if (mag_reload() && S.rack) {             // pinching the slide
            c[QHAND_THUMB] = 0.8f; c[QHAND_INDEX] = 0.75f; c[QHAND_MIDDLE] = 0.8f; c[QHAND_RING] = 0.85f; c[QHAND_PINKY] = 0.9f;
        } else {
            float g = S.shot_grip >= 0 ? fminf(S.shot_grip, 1) : get_float(S.a_reload, S.hand[h]);
            c[QHAND_THUMB] = S.thumb[h] || S.shot_grip >= 0 ? 0.55f + 0.35f * g : 0.12f;
            c[QHAND_INDEX] = 0.1f + 0.75f * S.trig[h];
            c[QHAND_MIDDLE] = c[QHAND_RING] = c[QHAND_PINKY] = 0.12f + 0.76f * g;
            if ((mag_reload() && S.slide_near) || S.sup_near) tint = near;
        }
        qhand_draw(view, proj, S.aim_m[h], h, c, tint);
        ME.hand[h] = 1; memcpy(ME.m[h], S.aim_m[h], sizeof ME.m[h]); memcpy(ME.curl[h], c, sizeof c);
    }
}
// The cabinet gun's magazines' base: its grip's base (quest_gun.c cuts it off the gun), contoured as the grip is:
// completing the grip while one is in, under the one in your free hand, under the dropped one
static void draw_mag_caps(const float *proj, const float *view) {
    float top[3];
    if (!gun_cabinet || !mag_reload() || !qgun_cap_top(top)) return;
    float m[16];
    for (int h = 0; h < 2; h++) {
        if (!S.aim_ok[h] || !is_gun(h)) continue;
        if (S.mag_seated) qgun_draw_cap(view, proj, S.aim_m[h]);   // where the gun's grip was cut: it completes it
        if (S.drop_t) {        // falling (build_scene's magazine: its bottom 20 cm under the grip's base)
            float t = (float)(S.now - S.drop_t) / 1e9f, gb[3], fm[16];
            if (t <= 1.2f) {
                memcpy(fm, S.drop_m, sizeof fm);
                fm[13] -= 4.9f * t * t;
                qgun_grip_base(gb);
                const float d[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, (gb[1] - 0.2f) - top[1], 0, 1 };
                mat_mul(m, fm, d);
                qgun_draw_cap(view, proj, m);
            }
        }
    }
    const int f = free_hand();
    if (f >= 0 && S.aim_ok[f] && S.mag_hold && !S.menu_open) {   // in your free hand: under its magazine
        const float d[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  -top[0], MAG_BOTTOM_Y - top[1], MAG_Z - top[2], 1 };
        mat_mul(m, S.aim_m[f], d);
        qgun_draw_cap(view, proj, m);
    }
}
static void draw_guns(const float *proj, const float *view) {
    ME.hand[0] = ME.hand[1] = ME.gun[0] = ME.gun[1] = 0;
    for (int h = 0; h < 2 && S.gun_model; h++) {
        if (!S.aim_ok[h] || !is_gun(h)) continue;
        float age = S.shot_t[h] ? (float)(S.now - S.shot_t[h]) / 1e6f : -1;
        static const float lit[3] = { 1, 1, 1 }, dim[3] = { 0.55f, 0.55f, 0.6f };   // the gun that aims, the other
        float slide = fmaxf(qgun_recoil(age), mag_reload() ? slide_by_hand() : 0);
        if (host_player_ammo(net_local) == 0 && qgun_has_slide()) slide = 1;   // empty: the slide locked back
        // no magazine reload (another gesture, OFF SCREEN in the cabinet, a gun in each hand): the grip uncut
        qgun_draw(view, proj, S.aim_m[h], slide, S.fire[h] ? 1.0f : 0.0f, qgun_flash(age), h == S.active ? lit : dim,
                  !mag_reload());
        ME.gun[h] = 1; memcpy(ME.m[h], S.aim_m[h], sizeof ME.m[h]);
        ME.slide[h] = slide; ME.trig[h] = S.fire[h] ? 1.0f : 0.0f; ME.flash[h] = qgun_flash(age);
    }
    draw_mag_caps(proj, view);
    draw_hands(proj, view);
    draw_partner(proj, view);
}
// The arcade game's world is drawn with a near plane of its own, WORLD_NEAR (the eyes' 5 cm spends most of the depth
// buffer on what is at your face: far off, the mansion's windows fought their walls), and the depth is cleared
// after it for what is near you (the guns, the hands, the cabinet, the HUD). World geometry nearer than WORLD_NEAR
// is cut: zombies at your face come no closer than that.
#define WORLD_NEAR 0.2f
static void world_pv(float *out, const float *proj, const float *view) {
    float p[16];
    const float n = WORLD_NEAR, f = 1000.0f;
    memcpy(p, proj, sizeof p);
    p[10] = -(f + n) / (f - n); p[14] = -2 * f * n / (f - n);   // (mat_proj's two near-plane terms)
    mat_mul(out, p, view);
}
static void draw_scene(const float *proj, const float *view, const float *pv) {
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    if (!S.cabinet) {
        // immersive: the game's world around you, its 2D overlay where its screen would be, 1.5 m ahead
        float g2l[16], m[16], lv[16];
        game_to_local(g2l);
        mat_mul(m, pv, g2l);
        level_mat(lv);
        if (S.screen_scene) {   // the game-over scene: the arcade's picture (the overlay in it) where the overlay goes
            float l, r, t, b;
            const float zp = S.panel_m / S.scale;
            screen_rect(zp, &l, &r, &t, &b);
            const float q[] = { l, t, zp, 0, 1,  r, t, zp, 1, 1,  r, b, zp, 1, 0,
                                l, t, zp, 0, 1,  r, b, zp, 1, 0,  l, b, zp, 0, 0 };   // (upside down for GL)
            draw_col(pv, tris, ntris, GL_TRIANGLES);
            if (S.scene) tex_quad(m, q, S.cab_tex, CAB_W, CAB_H, 0);
        } else {
            {   // the world: its own near plane, then the depth cleared for what is near you
                float pvw[16], mw[16], mlw[16];
                world_pv(pvw, proj, view);
                mat_mul(mw, pvw, g2l);
                mat_mul(mlw, mw, lv);
                m2_gl_near(WORLD_NEAR / S.scale);
                draw_game(mlw);
                glClear(GL_DEPTH_BUFFER_BIT);
            }
            draw_col(pv, tris, ntris, GL_TRIANGLES);
            draw_overlay(m, S.panel_m / S.scale);
        }
    } else {
        if (cabinet_room() == QROOM_COURTYARD) {   // the first stage's courtyard round the cabinet, level, at eye height
            float g2l[16], m[16];
            float pvw[16];
            world_pv(pvw, proj, view);
            game_to_local(g2l);
            mat_mul(m, pvw, g2l);
            m2_gl_near(WORLD_NEAR / S.scale);
            m2_gl_draw_backdrop(m);
            glClear(GL_DEPTH_BUFFER_BIT);
        }
        draw_col(pv, lines, nlines, GL_LINES);
        if (S.cab_model) qcab_draw(view, proj, cab_m);
        draw_col(pv, tris, ntris, GL_TRIANGLES);
    }
    // the screen (in the cabinet's frame)
    float l = -SCR_W / 2, r = SCR_W / 2, t = SCR_Y + SCR_H / 2, b = SCR_Y - SCR_H / 2;
    float pvc[16];
    mat_mul(pvc, pv, cab_pl);
    if (S.cabinet && S.scene) {
        // the GPU's picture of the game (rendered upside down for GL: v grows upwards)
        const float q[] = { l, t, SCR_Z, 0, 1,  r, t, SCR_Z, 1, 1,  r, b, SCR_Z, 1, 0,
                            l, t, SCR_Z, 0, 1,  r, b, SCR_Z, 1, 0,  l, b, SCR_Z, 0, 0 };
        tex_quad(pvc, q, S.cab_tex, CAB_W, CAB_H, 0);
    } else if (S.cabinet && S.tex_w) {
        const float q[] = { l, t, SCR_Z, 0, 0,  r, t, SCR_Z, 1, 0,  r, b, SCR_Z, 1, 1,
                            l, t, SCR_Z, 0, 0,  r, b, SCR_Z, 1, 1,  l, b, SCR_Z, 0, 1 };
        glUseProgram(S.prog_tex);
        glUniformMatrix4fv(S.u_tex_mvp, 1, GL_FALSE, pvc);
        glUniform2f(S.u_tex_size, (float)S.tex_w, (float)S.tex_h);
        glUniform1i(S.u_tex_alpha, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, S.screen_tex);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), q);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), q + 3);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    if (S.menu_open) {
        // the menu: the game dimmed behind it, the panel, then the guns and lasers in front of it
        if (!S.cabinet && !S.screen_scene) draw_fade();
        draw_dim(0.5f);
        glClear(GL_DEPTH_BUFFER_BIT);
        draw_menu(pv);
    }
    draw_guns(proj, view);
    // sights: translucent, drawn last without writing depth
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    draw_col(pv, beams, nbeams, GL_TRIANGLES);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    draw_rack_hint(pv);
    draw_online_label(pv);
    if (!S.cabinet && !S.menu_open) { if (!S.screen_scene) draw_fade(); draw_vignette(proj); }   // (the screen has its fade)
}
static void facing_pose(float *m, float up, float ahead);
// No game to show (its files missing, or it didn't start): the message page (quest_show_message) on a panel ahead of
// where you looked when it came, and your guns
static void draw_message(const float *proj, const float *view, const float *pv) {
    static int placed; static float m[16];
    if (!placed && S.head_ok) { facing_pose(m, -0.05f, 1.5f); placed = 1; }
    if (placed && S.tex_w) {
        V3 c = { m[12], m[13], m[14] }, r = { m[0], m[1], m[2] }, u = { m[4], m[5], m[6] };
        draw_tex_quad(pv, S.screen_tex, c, r, u, 0.8f, 0.8f * S.tex_h / S.tex_w);
    }
    draw_guns(proj, view);
}
static float arcade_eye_x[2];   // the eyes' offsets from the middle of the head (metres, left negative)
#if !XR_OPENXR
// Passthrough (the cabinet room's PASSTHROUGH): not on Vision Pro yet (it would be the space's mixed immersion)
static struct { int running; } PT;
static int pt_available(void) { return 0; }
static void pt_update(void) {}
#else
// Passthrough (XR_FB_passthrough): your room round the cabinet (Cabinet room: PASSTHROUGH). A reconstruction layer
// under the eyes' layer, which is then blended by its alpha (clear: 0).
static struct {
    int ext, ok, running;
    XrPassthroughFB pt;
    XrPassthroughLayerFB layer;
    PFN_xrCreatePassthroughFB create;
    PFN_xrPassthroughStartFB start;
    PFN_xrPassthroughPauseFB pause;
    PFN_xrCreatePassthroughLayerFB create_layer;
    PFN_xrPassthroughLayerResumeFB layer_resume;
    PFN_xrPassthroughLayerPauseFB layer_pause;
} PT;
static int pt_available(void) { return PT.ok; }
static void pt_init(void) {
    if (!PT.ext) return;
    xrGetInstanceProcAddr(S.inst, "xrCreatePassthroughFB", (PFN_xrVoidFunction *)&PT.create);
    xrGetInstanceProcAddr(S.inst, "xrPassthroughStartFB", (PFN_xrVoidFunction *)&PT.start);
    xrGetInstanceProcAddr(S.inst, "xrPassthroughPauseFB", (PFN_xrVoidFunction *)&PT.pause);
    xrGetInstanceProcAddr(S.inst, "xrCreatePassthroughLayerFB", (PFN_xrVoidFunction *)&PT.create_layer);
    xrGetInstanceProcAddr(S.inst, "xrPassthroughLayerResumeFB", (PFN_xrVoidFunction *)&PT.layer_resume);
    xrGetInstanceProcAddr(S.inst, "xrPassthroughLayerPauseFB", (PFN_xrVoidFunction *)&PT.layer_pause);
    if (!PT.create || !PT.start || !PT.pause || !PT.create_layer || !PT.layer_resume || !PT.layer_pause) return;
    XrPassthroughCreateInfoFB ci = { XR_TYPE_PASSTHROUGH_CREATE_INFO_FB };
    if (!XR(PT.create(S.sess, &ci, &PT.pt))) return;
    XrPassthroughLayerCreateInfoFB li = { XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB };
    li.passthrough = PT.pt;
    li.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
    if (!XR(PT.create_layer(S.sess, &li, &PT.layer))) return;
    PT.ok = 1;
    LOGI("passthrough: available");
}
// on while the cabinet view's room is PASSTHROUGH (and the game is showing)
static void pt_update(void) {
    if (!PT.ok) return;
    const int want = S.cabinet && !S.need_files && !S.game_failed && cabinet_room() == QROOM_PASSTHROUGH;
    if (want == PT.running) return;
    if (want) {
        if (XR(PT.start(PT.pt)) && XR(PT.layer_resume(PT.layer))) PT.running = 1;
    } else {
        XR(PT.layer_pause(PT.layer));
        XR(PT.pause(PT.pt));
        PT.running = 0;
    }
    LOGI("passthrough: %s", PT.running ? "on" : "off");
}
#endif
#if !XR_GLES
// XR_SHOT=seconds[,seconds...] (env.txt, tests): the left eye's whole picture that long after the first frame, to
// xr_shot.ppm (then xr_shot2.ppm, ...) in the files folder
static void shot(GLuint tex) {
    static int n = -1; static double at[8]; static uint64_t t0;
    if (n < 0) {
        n = 0; t0 = plat_now_ns();
        const char *e = getenv("XR_SHOT");
        while (e && *e && n < 8) { at[n++] = atof(e); e = strchr(e, ','); if (e) e++; }
    }
    static int done;
    if (done >= n || (double)(plat_now_ns() - t0) / 1e9 < at[done]) return;
    uint8_t *px = malloc((size_t)S.sc_w * S.sc_h * 4);
    if (!px) return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, S.resolve_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glReadPixels(0, 0, S.sc_w, S.sc_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    char name[64];
    if (done) snprintf(name, sizeof name, "xr_shot%d.ppm", done + 1); else snprintf(name, sizeof name, "xr_shot.ppm");
    FILE *f = fopen(name, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", S.sc_w, S.sc_h);
        for (int y = S.sc_h - 1; y >= 0; y--)
            for (int x = 0; x < S.sc_w; x++) fwrite(px + ((size_t)y * S.sc_w + x) * 4, 1, 3, f);
        fclose(f);
        LOGI("XR_SHOT: %s (%d x %d, the left eye)", name, S.sc_w, S.sc_h);
    }
    free(px);
    done++;
}
// The PC's window shows the left eye (xr_os_mirror): its middle at the window's shape
static void mirror(GLuint tex) {
    int ww, wh;
    if (!xr_os_mirror(&ww, &wh) || ww <= 0 || wh <= 0) return;
    int sw = S.sc_w * 3 / 4, sh = S.sc_h * 3 / 4;            // (the eye's middle: its edges are the lens's)
    if (sw * wh > sh * ww) sw = sh * ww / wh; else sh = sw * wh / ww;
    const int sx = (S.sc_w - sw) / 2, sy = (S.sc_h - sh) / 2;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, S.resolve_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_FRAMEBUFFER_SRGB);                         // (the window is not sRGB: the bytes as they are)
    glBlitFramebuffer(sx, sy, sx + sw, sy + sh, 0, 0, ww, wh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glEnable(GL_FRAMEBUFFER_SRGB);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
#endif
// one eye (fov; v: eye <- LOCAL) into the colour texture tex (S.sc_w x S.sc_h, sRGB) through the eyes' framebuffer
static void draw_eye(int e, GLuint tex, XrFovf fov, float *v) {
    glBindFramebuffer(GL_FRAMEBUFFER, S.fbo);
#if XR_GLES
    if (S.ms > 1) S.fb_tex_ms(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0, S.ms);
    else glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, S.depth_rb);
#endif
    glViewport(0, 0, S.sc_w, S.sc_h);
    if (!S.cabinet && S.scene && !S.need_files && !S.game_failed) {
        uint32_t c = S.scene->clear;      // the game's background colour beyond what it draws
        glClearColor(lin(c >> 16), lin(c >> 8), lin(c), 1);
    } else if (PT.running) {
        glClearColor(0, 0, 0, 0);          // your room shows through where nothing is drawn
    } else {
        glClearColor(0.004f, 0.005f, 0.009f, 1);
    }
    glClearDepthf(1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    float p[16], pv[16];
    mat_proj(p, fov, 0.05f, S.cabinet && cabinet_room() != QROOM_COURTYARD ? 100.0f : 1000.0f);
    mat_mul(v, v, S.home);                  // the world, where RECENTER put it
    mat_mul(pv, p, v);
    if (S.need_files || S.game_failed) draw_message(p, v, pv);   // the setup page (or why the game didn't start)
    else draw_scene(p, v, pv);
    if (!S.need_files && !S.game_failed && !S.cabinet && !S.screen_scene) m2_gl_draw_face(p, arcade_eye_x[e]);   // a zombie's hit, on your face
    discard_depth();
#if !XR_GLES
    resolve(S.fbo, tex, S.sc_w, S.sc_h);
    if (e == 0) { shot(tex); mirror(tex); }
#endif
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
#if XR_OPENXR
static void render_eye(int e, const XrView *view) {
    uint32_t idx;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!XR(xrAcquireSwapchainImage(S.sc[e], &ai, &idx))) return;
    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    XR(xrWaitSwapchainImage(S.sc[e], &wi));
    float v[16];
    mat_view(v, view->pose);
    draw_eye(e, S.sc_img[e][idx].image, view->fov, v);
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    XR(xrReleaseSwapchainImage(S.sc[e], &ri));
}
#else
void xr_host_eye(int e, const float *eye, const float tan[4], unsigned tex) {
    const XrFovf fov = { atanf(tan[0]), atanf(tan[1]), atanf(tan[2]), atanf(tan[3]) };
    float v[16];
    rigid_inv(v, eye);
    draw_eye(e, tex, fov, v);
}
#endif

// ---------------------------------------------------------------- input: controllers as light guns
#if XR_OPENXR
static XrPath path(const char *s) { XrPath p = XR_NULL_PATH; xrStringToPath(S.inst, s, &p); return p; }
static void haptic(int h, float amp, int ms) {
    if (!S.opt[QOPT_RUMBLE]) return;
    XrHapticVibration v = { XR_TYPE_HAPTIC_VIBRATION };
    v.amplitude = amp; v.duration = (XrDuration)ms * 1000000; v.frequency = XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo hi = { XR_TYPE_HAPTIC_ACTION_INFO };
    hi.action = S.a_haptic; hi.subactionPath = S.hand[h];
    xrApplyHapticFeedback(S.sess, &hi, (const XrHapticBaseHeader *)&v);
}
static void haptic_stop(int h) {
    XrHapticActionInfo hi = { XR_TYPE_HAPTIC_ACTION_INFO };
    hi.action = S.a_haptic; hi.subactionPath = S.hand[h];
    xrStopHapticFeedback(S.sess, &hi);
}
static int get_bool(XrAction a, XrPath sub) {
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a; gi.subactionPath = sub;
    XrActionStateBoolean st = { XR_TYPE_ACTION_STATE_BOOLEAN };
    return XR_SUCCEEDED(xrGetActionStateBoolean(S.sess, &gi, &st)) && st.isActive && st.currentState;
}
static float get_float(XrAction a, XrPath sub) {
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a; gi.subactionPath = sub;
    XrActionStateFloat st = { XR_TYPE_ACTION_STATE_FLOAT };
    return XR_SUCCEEDED(xrGetActionStateFloat(S.sess, &gi, &st)) && st.isActive ? st.currentState : 0.0f;
}
// Hand tracking (XR_EXT_hand_tracking with XR_FB_hand_tracking_aim): your bare hands as guns. The runtime's aim ray
// from each hand aims, an index pinch fires (and clicks in the menu), the left palm's menu gesture opens the menu.
// Added to the controllers' actions, not instead: whichever is in your hands works.
static struct {
    int ext, ok;
    XrHandTrackerEXT tr[2];
    PFN_xrCreateHandTrackerEXT create;
    PFN_xrLocateHandJointsEXT locate;
    int active[2], valid[2], fire[2], menu;   // this frame: the hand tracked; its aim ray; an index pinch; the menu gesture
    int skip;                            // a middle-finger pinch (the off hand): A / X, coins and Start
    int touch;                           // the controllers' profile is current (an interaction profile change says)
    int ctl_ok[2];                       // the controllers' own aim poses this frame
    float pinch[2];                      // the index pinch's strength (the hand model's trigger finger)
    XrPosef aim[2];                      // the aim ray's pose (LOCAL)
} HT = { .touch = 1 };                   // (no profile change heard yet: a tracked controller counts)
static void ht_init(void) {
    if (!HT.ext) return;
    xrGetInstanceProcAddr(S.inst, "xrCreateHandTrackerEXT", (PFN_xrVoidFunction *)&HT.create);
    xrGetInstanceProcAddr(S.inst, "xrLocateHandJointsEXT", (PFN_xrVoidFunction *)&HT.locate);
    XrSystemHandTrackingPropertiesEXT hp = { XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT };
    XrSystemProperties sp = { XR_TYPE_SYSTEM_PROPERTIES };
    sp.next = &hp;
    if (!HT.create || !HT.locate || !XR_SUCCEEDED(xrGetSystemProperties(S.inst, S.sys, &sp)) || !hp.supportsHandTracking) {
        LOGI("hand tracking: not available");
        return;
    }
    for (int h = 0; h < 2; h++) {
        XrHandTrackerCreateInfoEXT ci = { XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT };
        ci.hand = h ? XR_HAND_RIGHT_EXT : XR_HAND_LEFT_EXT;
        ci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
        if (!XR(HT.create(S.sess, &ci, &HT.tr[h]))) { LOGE("hand tracking: no tracker"); return; }
    }
    HT.ok = 1;
    LOGI("hand tracking: on (aim, pinch, menu gesture)");
}
// this frame's hands (t: the frame's display time)
static void ht_update(XrTime t) {
    HT.menu = HT.skip = 0;
    for (int h = 0; h < 2; h++) {
        HT.active[h] = HT.valid[h] = HT.fire[h] = 0; HT.pinch[h] = 0;
        if (!HT.ok) continue;
        XrHandJointLocationEXT j[XR_HAND_JOINT_COUNT_EXT];
        XrHandTrackingAimStateFB aim = { XR_TYPE_HAND_TRACKING_AIM_STATE_FB };
        XrHandJointLocationsEXT jl = { XR_TYPE_HAND_JOINT_LOCATIONS_EXT };
        jl.next = &aim; jl.jointCount = XR_HAND_JOINT_COUNT_EXT; jl.jointLocations = j;
        XrHandJointsLocateInfoEXT li = { XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT };
        li.baseSpace = S.local; li.time = t;
        if (!XR_SUCCEEDED(HT.locate(HT.tr[h], &li, &jl)) || !jl.isActive) continue;
        HT.active[h] = 1;
        if (!(aim.status & XR_HAND_TRACKING_AIM_VALID_BIT_FB)) continue;
        HT.valid[h] = 1;
        HT.aim[h] = aim.aimPose;
        // (a hand making the system's gesture, palm to your face, neither fires nor aims at anything)
        const int gesture = (aim.status & XR_HAND_TRACKING_AIM_SYSTEM_GESTURE_BIT_FB) != 0;
        HT.fire[h] = !gesture && (aim.status & XR_HAND_TRACKING_AIM_INDEX_PINCHING_BIT_FB);
        HT.pinch[h] = gesture ? 0 : aim.pinchStrengthIndex;
        if (aim.status & XR_HAND_TRACKING_AIM_MENU_PRESSED_BIT_FB) HT.menu = 1;
        // thumb to middle finger on the off hand (the one without a gun; a gun in each: the left), the index not
        // pinching too (a fist pinches every finger): the skip button. Never the gun hand's: it shoots.
        const int off = free_hand() >= 0 ? free_hand() : 0;
        if (h == off && !gesture && (aim.status & XR_HAND_TRACKING_AIM_MIDDLE_PINCHING_BIT_FB) && !HT.fire[h]) HT.skip = 1;
    }
}
static int sync_actions(void) {
    XrActiveActionSet as = { S.aset, XR_NULL_PATH };
    XrActionsSyncInfo si = { XR_TYPE_ACTIONS_SYNC_INFO };
    si.countActiveActionSets = 1; si.activeActionSets = &as;
    return xrSyncActions(S.sess, &si) == XR_SUCCESS;
}
// the head (LOCAL <- head), for the menu and RECENTER: 0 when not tracked
static int head_pose(XrTime t, float *lm) {
    XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if (!S.viewsp || !XR_SUCCEEDED(xrLocateSpace(S.viewsp, S.local, t, &loc)) || (loc.locationFlags & need) != need) return 0;
    mat_pose(lm, loc.pose);
    return 1;
}
// hand h's aim (LOCAL <- aim): the controller's, else a tracked hand's aim ray; 0 when neither
static int aim_pose(int h, XrTime t, float *lm) {
    XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    int ok = XR_SUCCEEDED(xrLocateSpace(S.aim[h], S.local, t, &loc)) && (loc.locationFlags & need) == need;
    HT.ctl_ok[h] = ok && !HT.active[h];
    if (!ok && HT.valid[h]) { loc.pose = HT.aim[h]; ok = 1; }   // a tracked hand's aim ray
    if (ok) mat_pose(lm, loc.pose);
    return ok;
}
static void get_stick(int h, float *x, float *y) {
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = S.a_stick; gi.subactionPath = S.hand[h];
    XrActionStateVector2f st = { XR_TYPE_ACTION_STATE_VECTOR2F };
    *x = *y = 0;
    if (XR_SUCCEEDED(xrGetActionStateVector2f(S.sess, &gi, &st)) && st.isActive) { *x = st.currentState.x; *y = st.currentState.y; }
}
#else
// ---- the host's input (XrhInput): its controllers laid out as Touch's, its hands as the Quest's hand tracking
static void haptic(int h, float amp, int ms) {
    if (!S.opt[QOPT_RUMBLE]) return;
    xr_os_haptic(h, amp, ms);
}
static void haptic_stop(int h) { xr_os_haptic(h, 0, 0); }
// a controller's button (sub: the hand, or XR_NULL_PATH: either)
static int ctl_bool(XrAction a, int h) {
    const XrhController *c = &S.in->ctl[h];
    if (!c->connected) return 0;
    switch (a) {
    case A_FIRE: return c->fire;
    case A_BACK: return c->b;
    case A_SKIP: return c->a;
    case A_MENU: return c->menu;
    case A_MODE: return c->stick_click;
    case A_THUMB: return c->thumb;
    default: return 0;
    }
}
static int get_bool(XrAction a, XrPath sub) {
    if (!S.in) return 0;
    if (sub == XR_NULL_PATH) return ctl_bool(a, 0) || ctl_bool(a, 1);
    return ctl_bool(a, sub);
}
static float get_float(XrAction a, XrPath sub) {
    if (!S.in || sub < 0 || sub > 1 || !S.in->ctl[sub].connected) return 0;
    const XrhController *c = &S.in->ctl[sub];
    return a == A_RELOAD ? c->squeeze : a == A_TRIG ? c->trigger : 0;
}
static void get_stick(int h, float *x, float *y) {
    *x = *y = 0;
    if (S.in && h >= 0 && h < 2 && S.in->ctl[h].connected) { *x = S.in->ctl[h].stick_x; *y = S.in->ctl[h].stick_y; }
}
// Hand tracking (the host's: on Vision Pro ARKit's skeleton, the aim ray and the pinches made in xr_vos.m): your bare
// hands as guns, as the Quest's. Added to the controllers, not instead: whichever is in your hands works.
static struct {
    int ok;
    int active[2], valid[2], fire[2], menu;   // this frame: the hand tracked; its aim ray; an index pinch; the menu gesture
    int skip;                            // a middle-finger pinch (the off hand): A / X, coins and Start
    int touch;                           // controllers are connected
    int ctl_ok[2];                       // the controllers' own aim poses this frame
    float pinch[2];                      // the index pinch's strength (the hand model's trigger finger)
    float aim[2][16];                    // the aim ray's pose (LOCAL)
} HT = { .touch = 1 };
static void ht_update(XrTime t) {
    (void)t;
    HT.menu = HT.skip = 0;
    HT.ok = S.in->hands_ok;
    HT.touch = S.in->ctl[0].connected || S.in->ctl[1].connected;
    for (int h = 0; h < 2; h++) {
        const XrhHand *in = &S.in->hand[h];
        HT.active[h] = HT.valid[h] = HT.fire[h] = 0; HT.pinch[h] = 0;
        if (!in->tracked) continue;
        HT.active[h] = HT.valid[h] = 1;
        memcpy(HT.aim[h], in->aim, sizeof HT.aim[h]);
        // the left palm turned to your face is the menu gesture: that hand neither fires nor aims at anything, and a
        // pinch made so opens the menu (the Quest's menu gesture)
        const int gesture = in->palm_to_face;
        HT.fire[h] = !gesture && in->pinch;
        HT.pinch[h] = gesture ? 0 : in->pinch_strength;
        if (gesture && h == 0 && in->pinch) HT.menu = 1;
        if (gesture) HT.valid[h] = 0;
        // thumb to middle finger on the off hand (the one without a gun; a gun in each: the left), the index not
        // pinching too: the skip button. Never the gun hand's: it shoots.
        const int off = free_hand() >= 0 ? free_hand() : 0;
        if (h == off && !gesture && in->middle_pinch && !HT.fire[h]) HT.skip = 1;
    }
}
static int sync_actions(void) { return 1; }
static int head_pose(XrTime t, float *lm) {
    (void)t;
    if (!S.in->head_ok) return 0;
    memcpy(lm, S.in->head, 16 * sizeof(float));
    return 1;
}
static int aim_pose(int h, XrTime t, float *lm) {
    (void)t;
    const XrhController *c = &S.in->ctl[h];
    int ok = c->connected && c->pose_ok;
    if (ok) memcpy(lm, c->aim, 16 * sizeof(float));
    HT.ctl_ok[h] = ok && !HT.active[h];
    if (!ok && HT.valid[h]) { memcpy(lm, HT.aim[h], 16 * sizeof(float)); ok = 1; }   // a tracked hand's aim ray
    return ok;
}
#endif
// which plays, after the poses: a tracked hand makes it the hands; a controller tracked while no hand is (the
// controllers' profile current) makes it the controllers again. A hand lost from view changes nothing.
static void ht_who_plays(void) {
    const int hands = HT.active[0] || HT.active[1];
    if (hands && !using_hands) { using_hands = 1; LOGI("hands: playing (reload: shoot off the screen)"); }
    else if (!hands && using_hands && HT.touch && (HT.ctl_ok[0] || HT.ctl_ok[1])) { using_hands = 0; LOGI("controllers: playing"); }
}
// hand h's trigger: the controller's, or a tracked hand's index pinch
static int fire_down(int h) { return get_bool(S.a_fire, S.hand[h]) || HT.fire[h]; }
// One press of the game's reload (its right button: the gun off the screen with the trigger pulled), long enough
// for the game to see it, and never two within RELOAD_GAP: a twitching grip or a gesture can't chatter it.
#define RELOAD_PRESS_NS 70000000LL
#define RELOAD_GAP_NS 250000000LL
static void reload_press(int h, int other) {
    if (S.rl_last && S.now - S.rl_last < RELOAD_GAP_NS) return;
    S.rl_last = S.now;
    if (!S.rl_down) quest_mouse_button(SDL_BUTTON_RIGHT, 1);
    S.rl_down = 1; S.rl_until = S.now + RELOAD_PRESS_NS;
    haptic(h, 0.35f, 50);
    if (other >= 0) haptic(other, 0.35f, 50);
}
static void reload_release(int now) {
    if (S.rl_down && (now || S.now >= S.rl_until)) { quest_mouse_button(SDL_BUTTON_RIGHT, 0); S.rl_down = 0; }
}
static void release_all(void) {
    for (int h = 0; h < 2; h++) if (S.fire[h]) { if (S.fire_btn[h]) quest_mouse_button(S.fire_btn[h], 0); S.fire[h] = 0; }
    reload_release(1);
    if (S.skip) { quest_key(SDL_SCANCODE_SPACE, SDLK_SPACE, 0); S.skip = 0; }
    if (S.menu) { quest_key(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE, 0); S.menu = 0; }
}
// The reload gesture's lines for the game's "Shoot outside of the screen!" (said when the trigger is pulled on an
// empty gun): assets voice-drop-the-mag.wav, voice-insert-a-magazine.wav, voice-rack-the-slide.wav (16-bit mono
// 22050 Hz WAVs, the line's rate) to the arcade bridge (arcade_reload_clip), which bakes the one for your step
// (arcade_reload_step) into the line's place
void arcade_reload_clip(int step, const int16_t *pcm, int n);
static void load_reload_lines(void) {
    static const char *const names[3] = { "voice-drop-the-mag.wav", "voice-insert-a-magazine.wav", "voice-rack-the-slide.wav" };
    for (int i = 0; i < 3; i++) {
        qasset *a = qasset_open(names[i]);
        if (!a) { LOGE("reload line: no asset %s", names[i]); continue; }
        size_t n = 0;
        const uint8_t *b = qasset_buffer(a, &n);
        int ok = 0;
        if (b && n > 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4)) {
            int fmt_ok = 0;
            for (size_t o = 12; o + 8 <= n;) {   // the chunks: fmt (PCM, mono, 16-bit, 44.1 kHz), then data
                const uint32_t len = (uint32_t)b[o + 4] | (uint32_t)b[o + 5] << 8 | (uint32_t)b[o + 6] << 16 | (uint32_t)b[o + 7] << 24;
                if (o + 8 + len > n) break;
                const uint8_t *c = b + o + 8;
                if (!memcmp(b + o, "fmt ", 4) && len >= 16)
                    fmt_ok = (c[0] | c[1] << 8) == 1 && (c[2] | c[3] << 8) == 1 && (c[4] | c[5] << 8 | c[6] << 16) == 22050 &&
                             (c[14] | c[15] << 8) == 16;
                if (!memcmp(b + o, "data", 4) && fmt_ok) {
                    int16_t *pcm = malloc(len);
                    if (pcm) {
                        for (uint32_t k = 0; k + 1 < len; k += 2) pcm[k / 2] = (int16_t)(c[k] | c[k + 1] << 8);
                        arcade_reload_clip(i, pcm, (int)(len / 2));
                        free(pcm);
                        ok = 1;
                    }
                    break;
                }
                o += 8 + len + (len & 1);
            }
        }
        qasset_close(a);
        LOGI("reload line %s: %s", names[i], ok ? "loaded" : "not a 16-bit mono 22050 Hz WAV");
    }
}
// ---------------------------------------------------------------- the settings and the VR menu
static void apply_options(void) {
    S.scale = S.base_scale * (float)S.opt[QOPT_WORLD] / 100.0f;
    S.panel_m = (float)S.opt[QOPT_HUD] / 100.0f;
    S.gun_pitch = GUN_PITCH_BASE + (float)S.opt[QOPT_PITCH];
    {
        int cab = S.opt[QOPT_VIEW] == QVIEW_CABINET;
        if (cab != S.cabinet) { S.cabinet = cab; S.cab_dirty = 1; }
        if (cab) S.screen_scene = 0;
        {   // the arcade game's picture keeps its damage marks on a screen (arcade_quest.cpp: the face layer)
            extern volatile int arcade_view_cabinet;
            arcade_view_cabinet = cab || S.screen_scene;
        }
        extern int port_draw_crosshairs;
        // the cabinet shows the game's crosshairs (Screen crosshair); immersive aims in 3D, but online the partner's
        // crosshair shows on the game's 2D layer (gun_crosshair draws only the other player's), so you see where they aim
        port_draw_crosshairs = S.cabinet ? S.opt[QOPT_CROSSHAIR] : online_session();
    }
    if (S.opt[QOPT_HAND] != QHAND_BOTH) S.active = S.opt[QOPT_HAND] == QHAND_LEFT ? 0 : 1;   // the one gun aims
    quest_volume = S.opt[QOPT_VOLUME];
    m2_gl_brightness(S.opt[QOPT_BRIGHTNESS]);   // (the 3D's shadows lifted: GRAPHICS tab)
    {
        extern float port_music_gain, port_sfx_gain;
        port_music_gain = (float)S.opt[QOPT_MUSIC] / 100.0f;
        port_sfx_gain = (float)S.opt[QOPT_SFX] / 100.0f;
        online_set_volume(S.opt[QOPT_VOICE]);   // the partner's voice (online.c keeps it for voice.c)
    }
}
// the display's refresh rate: the setting's, else the fastest up to 90 (QUEST_HZ overrides it at start)
#if !XR_OPENXR
// (the system's: Vision Pro's 90, 96 or 100 Hz; the GRAPHICS tab shows it)
static void request_rate(int first) { (void)first; }
#else
static void request_rate(int first) {
    if (!S.enum_rates || !S.req_rate || !S.sess) return;
    float rates[16], want = 0;
    uint32_t n = 0;
    S.enum_rates(S.sess, 16, &n, rates);
    const char *hz = first ? getenv("QUEST_HZ") : NULL;
    float pref = hz ? (float)atof(hz) : (float)qhz_rates[S.opt[QOPT_HZ]];
    for (uint32_t i = 0; i < n; i++) if (fabsf(rates[i] - pref) < 0.5f) want = rates[i];
    for (uint32_t i = 0; i < n && !want; i++) if (rates[i] <= 90.5f && rates[i] > want) want = rates[i];
    if (want && XR(S.req_rate(S.sess, want))) { S.hz_now = (int)(want + 0.5f); LOGI("display refresh %.0f Hz (of %u rates)", want, n); }
}
#endif
static int opt_get(int o) { return o >= 0 && o < QOPT_COUNT ? S.opt[o] : 0; }
static void opt_set(int o, int v) {
    if (o < 0 || o >= QOPT_COUNT) return;
    if (v < qopt_defs[o].min) v = qopt_defs[o].min;
    if (v > qopt_defs[o].max) v = qopt_defs[o].max;
    if (S.opt[o] == v) return;
    S.opt[o] = v;
    apply_options();
    if (o == QOPT_HZ) request_rate(0);
    char p[600];
    snprintf(p, sizeof p, "%s/vr-options.cfg", S.files);
    S.opt_saved = qoptions_save(p, S.opt);
}
// The game pauses while the menu is open, and while the headset isn't showing this app; not online (a partner's game
// goes on: yours, a guest's, follows it)
static void update_pause(void) { quest_set_paused(!online_session() && (!focused() || S.menu_open)); }
// a pose in the world facing where the head looks (turned about y only), at its position plus (0, up, -ahead)
static void facing_pose(float *m, float up, float ahead) {
    const float *h = S.head_m;
    float fx = -h[8], fz = -h[10], l = sqrtf(fx * fx + fz * fz);
    if (l < 1e-3f) { fx = 0; fz = -1; l = 1; }
    fx /= l; fz /= l;
    memset(m, 0, 16 * sizeof(float));
    m[0] = -fz; m[2] = fx;          // x: right
    m[5] = 1;                       // y: up
    m[8] = -fx; m[10] = -fz;        // z: back, towards you
    m[12] = h[12] + fx * ahead; m[13] = h[13] + up; m[14] = h[14] + fz * ahead; m[15] = 1;
}
// RECENTER: the world's origin where your eyes are, its -z the way you look
static void recenter(void) {
    if (!S.head_ok) return;
    float w[16], local[16];
    facing_pose(w, 0, 0);                   // the head in the world, turned about y only...
    mat_mul(local, S.home, w);              // ...in LOCAL: the new home
    memcpy(S.home, local, sizeof local);
    // its inverse (a rigid pose)
    float *i = S.home_inv, *m = S.home;
    i[0] = m[0]; i[1] = m[4]; i[2] = m[8];  i[3] = 0;
    i[4] = m[1]; i[5] = m[5]; i[6] = m[9];  i[7] = 0;
    i[8] = m[2]; i[9] = m[6]; i[10] = m[10]; i[11] = 0;
    i[12] = -(i[0] * m[12] + i[4] * m[13] + i[8] * m[14]);
    i[13] = -(i[1] * m[12] + i[5] * m[13] + i[9] * m[14]);
    i[14] = -(i[2] * m[12] + i[6] * m[13] + i[10] * m[14]);
    i[15] = 1;
    // the head in the new world: at the origin, facing -z
    memset(S.head_m, 0, sizeof S.head_m); S.head_m[0] = S.head_m[5] = S.head_m[10] = S.head_m[15] = 1;
    S.cab_dirty = 1;
    LOGI("recentered");
}
static void menu_place(void) { facing_pose(S.menu_m, -0.15f, MENU_DIST); }
static void release_all(void);
static void menu_set_open(int open) {
    if (open == S.menu_open) return;
    S.menu_open = open;
    if (open) { release_all(); menu_place(); qmenu_open(-1); S.mfire[0] = S.mfire[1] = 1; haptic_stop(0); haptic_stop(1); }
    else for (int h = 0; h < 2; h++) S.fire_hold[h] = fire_down(h);   // the click that closed it
    update_pause();
    LOGI("menu %s", open ? "open" : "closed");
}
static void menu_action(int a) {
    if (a == QACT_RESUME) menu_set_open(0);
    else if (a == QACT_RECENTER) { recenter(); menu_place(); }
    else if (a == QACT_RESTART) {            // the game back to its start (the game thread, between frames)
        menu_set_open(0);
        arcade_restart();
    } else if (a == QACT_QUIT) S.exit_requested = 1;
    else if (a == QACT_ARCADE_IMPORT) xr_os_import_rom();   // (Quest: ImportActivity ends this run and starts the app again)
#if !XR_GLES
    else if (a == QACT_PC_SWITCH) xr_os_pc_switch();
#endif
}
static QMenuHost menu_host(void) {
    QMenuHost h = { opt_get, opt_set, menu_action, S.boot_res, (unsigned)S.sc_w, (unsigned)S.sc_h, S.opt_saved != 0, S.hz_now };
    if (xr_os_update_line()) h.update = xr_os_update_line(), h.update_cmd = xr_os_update_cmd;   // (the SYSTEM tab's updates)
#if !XR_GLES
    h.pc_switch = 2;                    // HOTDVR.exe: PLAY ON THE SCREEN (the SYSTEM tab)
#endif
    return h;
}
// where hand h's ray meets the panel, in menu pixels
static int menu_hit(int h, float *px, float *py) {
    const float *a = S.aim_m[h], *m = S.menu_m;
    V3 o = { a[12], a[13], a[14] }, d = { -a[8], -a[9], -a[10] };
    V3 n = { m[8], m[9], m[10] }, c = { m[12], m[13], m[14] };
    float dn = d.x * n.x + d.y * n.y + d.z * n.z;
    if (dn > -1e-4f) return 0;
    float t = ((c.x - o.x) * n.x + (c.y - o.y) * n.y + (c.z - o.z) * n.z) / dn;
    if (t <= 0) return 0;
    V3 p = { o.x + d.x * t - c.x, o.y + d.y * t - c.y, o.z + d.z * t - c.z };
    float x = p.x * m[0] + p.y * m[1] + p.z * m[2], y = p.x * m[4] + p.y * m[5] + p.z * m[6];
    *px = (x / MENU_W_M + 0.5f) * QMENU_W; *py = (0.5f - y / MENU_H_M) * QMENU_H;
    S.menu_t[h] = t;
    return *px >= 0 && *px < QMENU_W && *py >= 0 && *py < QMENU_H;
}
static void menu_input(XrTime t) {
    QMenuInput in = { 0 };
    in.now = (double)t / 1e9;
    int fire[2];
    for (int h = 0; h < 2; h++) {
        fire[h] = fire_down(h);
        if (fire[h] && !S.mfire[h]) S.active = h;           // the hand that pulls its trigger points
    }
    float px[2] = { 0, 0 }, py[2] = { 0, 0 };
    for (int h = 0; h < 2; h++) S.menu_on[h] = S.aim_ok[h] && menu_hit(h, &px[h], &py[h]);
    int h = S.active;
    in.pointer = S.menu_on[h]; in.px = px[h]; in.py = py[h];
    in.press = fire[h] && !S.mfire[h]; in.release = !fire[h] && S.mfire[h]; in.held = fire[h];
    if (in.press && in.pointer) haptic(h, 0.15f, 12);
    S.mfire[0] = fire[0]; S.mfire[1] = fire[1];
    int sel = get_bool(S.a_skip, XR_NULL_PATH) || HT.skip, back = get_bool(S.a_back, XR_NULL_PATH);
    in.select = sel && !S.sel_btn; in.back = back && !S.back_btn;
    S.sel_btn = sel; S.back_btn = back;
    for (int k = 0; k < 2; k++) {
        if (S.grip_edge[k]) in.tab = k ? 1 : -1;
        float x, y;
        get_stick(k, &x, &y);
        if (fabsf(x) + fabsf(y) > fabsf(in.stick_x) + fabsf(in.stick_y)) { in.stick_x = x; in.stick_y = y; }
    }
    xr_os_update_poll(in.now);
    QMenuHost host = menu_host();
    qmenu_update(&host, &in);
}

// where the aim ray of hand h meets the screen (u, v in 0..1 from the top left)
static V3 to_local(const float *m, V3 p);
static int screen_hit(int h, float *u, float *v) {
    const float *m = S.aim_m[h], *c = cab_pl;
    V3 o = to_local(c, (V3){ m[12], m[13], m[14] });   // the ray in the cabinet's frame (a rigid placement)
    V3 dw = { -m[8], -m[9], -m[10] };
    V3 d = { c[0] * dw.x + c[1] * dw.y + c[2] * dw.z, c[4] * dw.x + c[5] * dw.y + c[6] * dw.z, c[8] * dw.x + c[9] * dw.y + c[10] * dw.z };
    if (d.z > -1e-4f) return 0;
    float t = (SCR_Z - o.z) / d.z;
    if (t <= 0) return 0;
    *u = (o.x + d.x * t + SCR_W / 2) / SCR_W;
    *v = (SCR_Y + SCR_H / 2 - (o.y + d.y * t)) / SCR_H;
    S.hit_t[h] = t;
    return *u >= 0 && *u < 1 && *v >= 0 && *v < 1;
}
// a point in the world in the space of pose m (a rigid pose: the transpose of its turn)
static V3 to_local(const float *m, V3 p) {
    float x = p.x - m[12], y = p.y - m[13], z = p.z - m[14];
    return (V3){ m[0] * x + m[1] * y + m[2] * z, m[4] * x + m[5] * y + m[6] * z, m[8] * x + m[9] * y + m[10] * z };
}
// A pull on the screen becomes a shot (slide kick, flash, rumble, tracer) when the game's bullet count drops within
// SHOT_WAIT of it; otherwise the game didn't fire (a cut scene, the gun ignored, no bullets) and nothing shows.
#define SHOT_WAIT 200000000     // ns (online, a guest's shots go through the host first: longer)
static void shot_confirm(XrTime t) {
    const XrTime wait = SHOT_WAIT + (XrTime)online_lag_ms() * 1000000;
    int a = host_player_ammo(net_local);
    for (int h = 0; h < 2; h++) {
        if (!S.pend_t[h]) continue;
        if (a >= 0 && S.pend_ammo[h] > 0 && a < S.pend_ammo[h]) {
            S.shot_t[h] = t;
            memcpy(S.tracer[h], S.pend_tr[h], sizeof S.tracer[h]);
            haptic(h, 0.7f, 30);
            S.pend_t[h] = 0;
            for (int k = 0; k < 2; k++) if (S.pend_t[k]) S.pend_ammo[k] = a;    // the other gun's pull: from here
        } else if (t - S.pend_t[h] > wait) S.pend_t[h] = 0;
    }
}
// TWO HANDS (one gun): squeeze the free hand's grip with that controller just under and beside the gun hand's (within
// SUP_R of SUP_X/Y/Z, gun space, x towards the free hand) and it takes hold of the gun round your gun hand. The gun
// then sits where both hands put it, the gun hand's pose and the one the free hand gives it (as it was when it took
// hold) averaged: half the shake of either. Let go of the grip, or pull the hands SUP_BREAK apart, and it lets go.
#define SUP_X 0.03f
#define SUP_Y (-0.10f)
#define SUP_Z 0.02f
#define SUP_R 0.09f
#define SUP_BREAK 0.25f
static void two_hands(void) {
    int f = free_hand(), g = 1 - f;
    S.sup_near = 0;
    if (f < 0 || !S.aim_ok[f] || !S.aim_ok[g] || S.menu_open) { S.support = 0; return; }
    if (S.shot_grip == 2) {      // QUEST_SHOT_LEFT=2 (screenshots): the free hand at the gun, squeezing
        const float t[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  f == 0 ? -SUP_X : SUP_X, SUP_Y, SUP_Z, 1 };
        mat_mul(S.aim_m[f], S.aim_m[g], t);
        S.grip_edge[f] = !S.grip_on[f]; S.grip_on[f] = 1;
    }
    const float *gm = S.aim_m[g], *fm = S.aim_m[f];
    V3 p = to_local(gm, (V3){ fm[12], fm[13], fm[14] });
    float dx = p.x - (f == 0 ? -SUP_X : SUP_X), dy = p.y - SUP_Y, dz = p.z - SUP_Z, d = sqrtf(dx * dx + dy * dy + dz * dz);
    if (S.support) {
        if (!S.grip_on[f] || d > SUP_BREAK) { S.support = 0; haptic(f, 0.2f, 10); LOGI("two hands: let go"); return; }
    } else {
        S.sup_near = d < SUP_R && !S.rack && !S.mag_hold;
        if (!S.grip_edge[f] || !S.sup_near) return;
        S.support = 1; S.sup_near = 0; S.grip_edge[f] = 0; S.mag_spent = 1;   // the squeeze is spent on this
        float gi[16];
        rigid_inv(gi, gm);
        mat_mul(S.sup_rel, gi, fm);
        haptic(f, 0.5f, 25); haptic(g, 0.3f, 15);
        LOGI("two hands");
    }
    float ri[16], g2[16], o[16];
    rigid_inv(ri, S.sup_rel);
    mat_mul(g2, fm, ri);                                 // the gun where the free hand holds it
    V3 ax[3];
    for (int k = 0; k < 3; k++) ax[k] = (V3){ gm[k * 4] + g2[k * 4], gm[k * 4 + 1] + g2[k * 4 + 1], gm[k * 4 + 2] + g2[k * 4 + 2] };
    float zl = sqrtf(ax[2].x * ax[2].x + ax[2].y * ax[2].y + ax[2].z * ax[2].z);
    V3 z = { ax[2].x / zl, ax[2].y / zl, ax[2].z / zl };
    float xd = ax[0].x * z.x + ax[0].y * z.y + ax[0].z * z.z;
    V3 x = { ax[0].x - z.x * xd, ax[0].y - z.y * xd, ax[0].z - z.z * xd };
    float xl = sqrtf(x.x * x.x + x.y * x.y + x.z * x.z); x.x /= xl; x.y /= xl; x.z /= xl;
    V3 y = { z.y * x.z - z.z * x.y, z.z * x.x - z.x * x.z, z.x * x.y - z.y * x.x };
    const float r[16] = { x.x, x.y, x.z, 0,  y.x, y.y, y.z, 0,  z.x, z.y, z.z, 0,
                          (gm[12] + g2[12]) / 2, (gm[13] + g2[13]) / 2, (gm[14] + g2[14]) / 2, 1 };
    memcpy(o, r, sizeof r);
    memcpy(S.aim_m[g], o, sizeof o);
}
// The MAGAZINE reload with one gun (f: the free hand). Squeeze its grip: a magazine appears in it; push its top into
// the bottom of the gun's grip: it goes in (a click in both hands). Squeeze the grip with the hand at the back of
// the slide: it takes the slide; pull back RACK_TRAVEL and the slide is racked: with a magazine in, that is the
// game's reload. Let go early and the slide springs back.
#define SLIDE_GRAB_Y 0.05f      // gun space: where the hand takes the slide (above its rear: the controllers can't meet)
#define SLIDE_GRAB_Z 0.03f
#define SLIDE_GRAB_R 0.09f
#define MAG_IN_R 0.07f          // the magazine's top this near the grip's base goes in
#define RACK_TRAVEL 0.05f       // the hand's pull for a full rack
static void magazine_and_slide(int f) {
    int g = 1 - f;
    if (S.support) { S.mag_hold = 0; S.slide_near = 0; return; }   // holding the gun with both hands
    if (!S.grip_on[f]) S.mag_spent = 0;
    S.mag_hold = S.grip_on[f] && !S.mag_spent && !S.rack;
    S.slide_near = 0;
    if (!S.aim_ok[f] || !S.aim_ok[g]) { if (S.rack) { S.rack = 0; S.rack_from = S.rack_pull; S.rack_end = S.now; } return; }
    const float *gm = S.aim_m[g], *fm = S.aim_m[f];
    V3 hand = to_local(gm, (V3){ fm[12], fm[13], fm[14] });
    float dx = hand.x, dy = hand.y - SLIDE_GRAB_Y, dz = hand.z - SLIDE_GRAB_Z;
    S.slide_near = !S.rack && sqrtf(dx * dx + dy * dy + dz * dz) < SLIDE_GRAB_R;
    if (S.grip_edge[f] && S.slide_near) {           // takes the slide
        S.rack = 1; S.rack_z0 = hand.z; S.rack_pull = 0; S.mag_hold = 0; S.mag_spent = 1;
        haptic(f, 0.25f, 15);
    } else if (S.mag_hold && !S.mag_seated) {       // the magazine at the grip's base goes in (once the old one is out)
        float gb[3]; qgun_grip_base(gb);
        V3 top = to_local(gm, xform(fm, 0, MAG_TOP_Y, MAG_Z));
        float ex = top.x - gb[0], ey = top.y - gb[1], ez = top.z - gb[2];
        if (sqrtf(ex * ex + ey * ey + ez * ez) < MAG_IN_R) {
            S.mag_in = 1; S.mag_seated = 1; S.mag_hold = 0; S.mag_spent = 1;
            haptic(f, 0.6f, 30); haptic(g, 0.6f, 30);
            LOGI("magazine in");
        }
    }
    if (S.rack) {
        if (!S.grip_on[f]) {                         // let go: springs back
            S.rack = 0; S.rack_from = S.rack_pull; S.rack_end = S.now;
            return;
        }
        float p = (hand.z - S.rack_z0) / RACK_TRAVEL;   // + z: back, towards you
        p = p < 0 ? 0 : p > 1 ? 1 : p;
        if ((int)(p * 4) > (int)(S.rack_pull * 4)) haptic(f, 0.12f, 8);    // a little friction on the way
        S.rack_pull = p;
        if (p >= 1) {                                // racked
            S.rack = 0; S.rack_from = 1; S.rack_end = S.now;
            if (S.mag_in) { S.mag_in = 0; reload_press(g, f); LOGI("racked: reload"); }
            else haptic(g, 0.3f, 20);
        }
    }
}
// The cabinet in your hands (cabinet view, the menu shut): both grips squeezed together (within GRAB_PAIR of each
// other, hands apart) take hold of it. It moves with your hands (GRAB_GAIN times as far), turns as they turn about
// each other, and grows or shrinks as they spread or come together, about the point between them; the right stick
// pushes it away or pulls it in (up, down) and sizes it (right, left). Let go: its place is kept (the VIEW tab's
// Cabinet size, distance and height, and its side and turn).
#define GRAB_PAIR 0.35
#define GRAB_GAIN 1.5f
static void cabinet_grab(XrTime t) {
    static XrTime last;
    const float dt = last && t > last ? fminf((float)(t - last) / 1e9f, 0.1f) : 0.0f;
    last = t;
    for (int h = 0; h < 2; h++) if (S.grip_edge[h]) CG.grip_t[h] = t;
    const int can = S.cabinet && S.cab_model && !S.menu_open && S.aim_ok[0] && S.aim_ok[1];
    V3 a = { S.aim_m[0][12], S.aim_m[0][13], S.aim_m[0][14] }, b = { S.aim_m[1][12], S.aim_m[1][13], S.aim_m[1][14] };
    V3 mid = { (a.x + b.x) / 2, (a.y + b.y) / 2, (a.z + b.z) / 2 };
    const float dx = b.x - a.x, dz = b.z - a.z, span = sqrtf(dx * dx + (b.y - a.y) * (b.y - a.y) + dz * dz), ang = atan2f(dx, dz);
    const float floor_y = S.floor_y - S.home[13];
    if (!CG.on) {
        if (!can || !S.grip_on[0] || !S.grip_on[1] || S.support || span < 0.2f) return;
        if (!(S.grip_edge[0] || S.grip_edge[1]) || fabs((double)(CG.grip_t[0] - CG.grip_t[1])) / 1e9 > GRAB_PAIR) return;
        CG.on = 1;
        S.grip_edge[0] = S.grip_edge[1] = 0;          // the squeeze is the grab's (no reload, no magazine)
        S.mag_spent = 1; S.mag_hold = 0;
        haptic(0, 0.3f, 15); haptic(1, 0.3f, 15);
        LOGI("cabinet: grabbed");
    rebase:
        CG.mid0 = mid; CG.span0 = span; CG.ang0 = ang; CG.size0 = CG.size; CG.turn0 = CG.turn;
        {
            float zf;
            CG.org0 = (V3){ CG.side, floor_y + CG.height, cab_front_z(floor_y, CG.size, &zf) - CG.dist };
        }
        return;
    }
    if (!can || !S.grip_on[0] || !S.grip_on[1]) {     // let go: kept
        CG.on = 0;
        opt_set(QOPT_CAB_SIZE, (int)lroundf(CG.size / CAB_BASE_SIZE * 100));
        opt_set(QOPT_CAB_SIDE, (int)lroundf((CG.side - CAB_BASE_SIDE) * 100));
        opt_set(QOPT_CAB_HEIGHT, (int)lroundf((CG.height - CAB_BASE_HEIGHT) * 100));
        opt_set(QOPT_CAB_DIST, (int)lroundf((CG.dist - CAB_BASE_DIST) * 100));
        int deg = (int)lroundf(CG.turn * 180.0f / 3.14159265f - CAB_BASE_TURN);
        while (deg > 180) deg -= 360;
        while (deg < -180) deg += 360;
        opt_set(QOPT_CAB_TURN, deg);
        haptic(0, 0.2f, 10); haptic(1, 0.2f, 10);
        LOGI("cabinet: let go at %d%%, side %d, height %d, distance %d cm, turn %d", S.opt[QOPT_CAB_SIZE],
             S.opt[QOPT_CAB_SIDE], S.opt[QOPT_CAB_HEIGHT], S.opt[QOPT_CAB_DIST], S.opt[QOPT_CAB_TURN]);
        return;
    }
    // hands: about the point between them (moved GRAB_GAIN times as far), turned and sized as they turn and spread
    float k = CG.span0 > 0.05f ? span / CG.span0 : 1;
    float size = CG.size0 * k;
    if (size < 0.5f * CAB_BASE_SIZE) size = 0.5f * CAB_BASE_SIZE;
    if (size > 2.0f * CAB_BASE_SIZE) size = 2.0f * CAB_BASE_SIZE;
    k = size / CG.size0;
    float dturn = ang - CG.ang0;
    while (dturn > 3.14159265f) dturn -= 6.2831853f;
    while (dturn < -3.14159265f) dturn += 6.2831853f;
    const V3 piv = { CG.mid0.x + (mid.x - CG.mid0.x) * GRAB_GAIN, 0, CG.mid0.z + (mid.z - CG.mid0.z) * GRAB_GAIN };
    const float rx = (CG.org0.x - CG.mid0.x) * k, rz = (CG.org0.z - CG.mid0.z) * k, c = cosf(dturn), sn = sinf(dturn);
    const float ox = piv.x + c * rx + sn * rz, oz = piv.z - sn * rx + c * rz;
    const float oy = CG.org0.y + (mid.y - CG.mid0.y) * GRAB_GAIN;
    CG.size = size; CG.turn = CG.turn0 + dturn;
    CG.side = ox; CG.height = oy - floor_y;
    {
        float zf;
        CG.dist = cab_front_z(floor_y, size, &zf) - oz;
    }
    // the right stick: away / nearer, bigger / smaller
    float sx = 0, sy = 0;
    get_stick(1, &sx, &sy);
    if (fabsf(sx) < 0.2f) sx = 0;
    if (fabsf(sy) < 0.2f) sy = 0;
    if ((sx || sy) && dt > 0) {
        CG.dist += sy * 1.2f * dt;
        CG.size *= expf(sx * 0.7f * dt);
        if (CG.size < 0.5f * CAB_BASE_SIZE) CG.size = 0.5f * CAB_BASE_SIZE;
        if (CG.size > 2.0f * CAB_BASE_SIZE) CG.size = 2.0f * CAB_BASE_SIZE;
        goto rebase;
    }
}
// The reload gesture's step, for the game's RELOAD sign (the arcade bridge writes it there instead) and its "Shoot
// outside of the screen!" line (said instead: load_reload_lines): 0 none (the sign stays), 1 / 2 drop the magazine
// with B / Y, 3 put a new one in, 4 rack the slide, 5 aim at the floor
#if XR_GLES
volatile int arcade_reload_step;
#else
extern volatile int arcade_reload_step;   // (HOTDVR.exe: flat_app.c defines it)
#endif
static void reload_step(void) {
    int st = 0, a = host_player_ammo(net_local);
    if (a >= 0 && !S.menu_open && host_player_playing(net_local)) {
        if (mag_reload()) {
            const int g = 1 - free_hand();
            if (!S.mag_seated) st = 3;
            else if (S.mag_in) st = 4;
            else if (a == 0) st = g == 1 ? 1 : 2;
        } else if (gesture() == QGESTURE_DOWN && a == 0) st = 5;
    }
    arcade_reload_step = st;
}
static int test_talk = -1;   // QUEST_TEST_TALK (the frame loop)
static void update_input(XrTime t) {
    S.now = t;
    {   // the head, for the menu and RECENTER
        float lm[16];
        if (head_pose(t, lm)) {
#if !XR_OPENXR
            const int first = !S.head_ok;
#endif
            mat_mul(S.head_m, S.home_inv, lm);
            S.head_ok = 1;
#if !XR_OPENXR
            if (first) recenter();          // the world's origin where your eyes first are, as OpenXR's LOCAL
#endif
        }
    }
    if (!focused()) { release_all(); return; }
    if (!sync_actions()) return;
    ht_update(t);
    int fw, fh;
    qframe_size(&fw, &fh);
    if (!fw) { fw = 640; fh = 480; }
    // the grips: squeezed past 85%, let go below 50% (the runtime's own click point is soft, and a resting
    // hand hovers about it)
    for (int h = 0; h < 2; h++) {     // for the hands' fingers
        S.trig[h] = fmaxf(get_float(S.a_trig, S.hand[h]), HT.pinch[h]);
        S.thumb[h] = get_bool(S.a_thumb, S.hand[h]);
    }
    for (int h = 0; h < 2; h++) {
        float g = get_float(S.a_reload, S.hand[h]);
        int on = S.grip_on[h] ? g > 0.5f : g > 0.85f;
        S.grip_edge[h] = on && !S.grip_on[h];
        S.grip_on[h] = on;
    }
    int px[2], py[2];
    for (int h = 0; h < 2; h++) {
        float lm[16];
        S.aim_ok[h] = aim_pose(h, t, lm);
        S.on_screen[h] = 0;
        static double shot_at = -2;
        static XrTime shot_t0;
        if (shot_at == -2) { const char *e = getenv("QUEST_SHOT_HANDS"); shot_at = e ? atof(e) : -1; shot_t0 = t; }
        if (shot_at >= 0) {
            // QUEST_SHOT_HANDS=seconds (env.txt, screenshots without a player): no hands until then, so PORT_INPUT
            // aims; after it the right gun held still in front of the head, its ray crossing the view 3 m ahead;
            // QUEST_SHOT_LEFT=grip (0..1) also shows the left hand, turned to the view, its grip squeezed that much
            static float left = -2;
            if (left == -2) { const char *e = getenv("QUEST_SHOT_LEFT"); left = e ? (float)atof(e) : -1; S.shot_grip = left; if (left == 3) S.mag_in = S.mag_seated = 1; }
            S.aim_ok[h] = (h == 1 || left >= 0) && S.head_ok && (double)(t - shot_t0) / 1e9 >= shot_at;
            if (!S.aim_ok[h]) continue;
            const float a = h ? 0.06f : -0.9f, c = cosf(a), sn = sinf(a);
            const float r[16] = { c, 0, -sn, 0,  0, 1, 0, 0,  sn, 0, c, 0,  h ? 0.18f : -0.1f, -0.22f, h ? -0.35f : -0.3f, 1 };
            mat_mul(S.aim_m[h], S.head_m, r);
        } else {
            if (!S.aim_ok[h]) continue;
            mat_mul(S.aim_m[h], S.home_inv, lm);          // into the world
        }
        {   // the gun in your hand: moved (controller axes: + y up, + z back) and tilted about there (+ raises the barrel)
            float a = S.gun_pitch * 3.14159265f / 180.0f, c = cosf(a), sn = sinf(a);
            float r[16] = { 1, 0, 0, 0,  0, c, sn, 0,  0, -sn, c, 0,  0, S.opt[QOPT_GUN_Y] * 0.001f, S.opt[QOPT_GUN_Z] * 0.001f, 1 };
            mat_mul(S.aim_m[h], S.aim_m[h], r);
        }
    }
    ht_who_plays();
    two_hands();
    cabinet_grab(t);
    reload_step();
    for (int h = 0; h < 2; h++) {
        if (!S.aim_ok[h] || S.menu_open) continue;
        if (!S.cabinet && S.scene) {
            float sx, sy, dist;
            S.on_screen[h] = scene_aim(S.aim_m[h], &sx, &sy, &dist);
            S.hit3d[h] = dist > 0;
            S.hit_t[h] = dist;
            px[h] = (int)(sx * fw / S.scene->w); py[h] = (int)(sy * fh / S.scene->h);
            continue;
        }
        float u, v;
        S.hit3d[h] = 0;
        S.on_screen[h] = screen_hit(h, &u, &v);
        px[h] = (int)(u * fw); py[h] = (int)(v * fh);
    }
    {   // With a reload gesture on, only the gesture reloads: the game itself takes a shot near its picture's edges
        // as off the screen and reloads (host_gun_live_rect), edges you can't see in the immersive view, so the aim
        // stays inside that box (a shot at the edge lands a step in)
        int x0, y0, x1, y1;
        if (gesture() != QGESTURE_OFF && host_gun_live_rect(&x0, &y0, &x1, &y1))
            for (int h = 0; h < 2; h++) {
                if (!S.on_screen[h]) continue;
                const int ax = x0 * fw / 496, bx = x1 * fw / 496, ay = y0 * fh / 384, by = y1 * fh / 384;
                px[h] = px[h] < ax ? ax : px[h] > bx ? bx : px[h];
                py[h] = py[h] < ay ? ay : py[h] > by ? by : py[h];
            }
    }
    reload_release(0);
    // the menu button opens and closes the VR menu (with the game's files missing: quits)
    if (S.need_files) {                     // the setup page: the trigger chooses your game's folder
        int f = fire_down(0) || fire_down(1);
        if (f && !S.need_fire && !S.import_started) S.import_started = xr_os_import_rom();
        S.need_fire = f;
    }
    int menu = get_bool(S.a_menu, XR_NULL_PATH) || HT.menu;
#if !XR_GLES
    {   // PC: a thumbstick's double click (two within 0.4 s) is the menu button too (SteamVR opens its dashboard on the
        // left menu button; the Index has none). A held click still switches the view, a single one still talks.
        static XrTime last_click[2];
        static int was[2];
        for (int h = 0; h < 2; h++) {
            const int c = get_bool(S.a_mode, S.hand[h]);
            if (c && !was[h]) {
                if (last_click[h] && t - last_click[h] < 400000000LL) { menu = 1; last_click[h] = 0; }
                else last_click[h] = t;
            }
            was[h] = c;
        }
    }
#endif
    if (menu && !S.menu) {
        if (S.game_failed || S.need_files) S.exit_requested = 1;
        else menu_set_open(!S.menu_open);
    }
    S.menu = menu;
    if (S.menu_open) { menu_input(t); return; }
    if (S.opt[QOPT_HAND] != QHAND_BOTH) S.active = S.opt[QOPT_HAND] == QHAND_LEFT ? 0 : 1;
    for (int h = 0; h < 2; h++) {
        int fire = is_gun(h) && fire_down(h), back = get_bool(S.a_back, S.hand[h]);
        if (S.fire_hold[h]) {           // still held from the menu: a shot only once it is let go
            if (!fire) S.fire_hold[h] = 0;
            fire = 0;
        }
        if (fire && !S.fire[h]) S.active = h;                   // the hand that fires takes the crosshair
        if (h == S.active && S.on_screen[h]) quest_pointer(px[h], py[h]);
        if (fire && !S.fire[h]) {
            if (S.on_screen[h] && host_player_ammo(net_local) == 0) {    // empty: the game says "Reload!", the gun only clicks
                S.fire_btn[h] = SDL_BUTTON_LEFT;
                S.shot_t[h] = 0;        // and no streak, kick or flash left over from the last shot
                quest_mouse_button(SDL_BUTTON_LEFT, 1);
                haptic(h, 0.15f, 8);
            } else if (S.on_screen[h]) {
                S.fire_btn[h] = SDL_BUTTON_LEFT;
                {   // the tracer: from the muzzle along the aim to where it hit (or 6 m), shown once the shot is real
                    const float *am = S.aim_m[h];
                    float mz = qgun_muzzle_z(), len = (S.cabinet || S.hit3d[h]) ? S.hit_t[h] : 6.0f;
                    V3 o = xform(am, 0, qgun_muzzle_y(), -mz);
                    float *tr = S.pend_tr[h];
                    tr[0] = o.x; tr[1] = o.y; tr[2] = o.z;
                    tr[3] = am[12] - am[8] * len; tr[4] = am[13] - am[9] * len; tr[5] = am[14] - am[10] * len;
                }
                // the kick, flash, rumble and tracer wait for the game to spend a bullet (shot_confirm): a pull in a
                // cut scene, between scenes or while the game ignores the gun fires nothing
                S.pend_t[h] = t; S.pend_ammo[h] = host_player_ammo(net_local);
                quest_mouse_button(SDL_BUTTON_LEFT, 1);
            } else {                    // off the screen the trigger reloads, as on the cabinet
                S.fire_btn[h] = 0;      // (with a reload gesture on, only the gesture reloads: nothing at all)
                if (gesture() == QGESTURE_OFF) reload_press(h, -1);
            }
        } else if (!fire && S.fire[h] && S.fire_btn[h]) {
            quest_mouse_button(S.fire_btn[h], 0);
        }
        S.fire[h] = fire;
        // B, Y and the grip reload only with no reload gesture on: with one, the gesture is the only way
        int grip = S.grip_edge[h] && S.opt[QOPT_GRIP] && !(mag_reload() && h == free_hand());
        if ((grip || (back && !S.back_on[h])) && gesture() == QGESTURE_OFF) reload_press(h, -1);
        else if (back && !S.back_on[h] && mag_reload() && S.mag_seated && S.aim_ok[1 - free_hand()] &&
                 host_player_playing(net_local)) {   // (only in play: not in menus or cut scenes)
            // B / Y with the MAGAZINE reload: the magazine drops out of the gun (a new one goes in, then the slide)
            int g = 1 - free_hand();
            S.mag_seated = 0; S.mag_in = 0;
            memcpy(S.drop_m, S.aim_m[g], sizeof S.drop_m);
            {   // it starts 10 cm down the grip's axis (out of it), then falls
                S.drop_m[12] -= S.drop_m[4] * 0.1f; S.drop_m[13] -= S.drop_m[5] * 0.1f; S.drop_m[14] -= S.drop_m[6] * 0.1f;
            }
            S.drop_t = t;
            haptic(g, 0.35f, 20);
            LOGI("magazine dropped");
        }
        S.back_on[h] = back;
    }
    shot_confirm(t);
    {   // hit (a life lost): both hands, strong and long. The shots and reloads have their own above (shot_confirm,
        // reload_press: the hand that did it); the hit is the game's (quest.h host_player_events, this player's slot
        // online, never the partner's), as every shell's rumble. Not while the menu is open (the game is paused).
        static HostEventsSeen seen;
        if (host_events_new(&seen, net_local) & 1 << HOST_EV_HIT && !S.menu_open) {
            haptic(0, 1.0f, 400); haptic(1, 1.0f, 400);
            LOGI("rumble: hit, both hands%s", S.opt[QOPT_RUMBLE] ? "" : " (Rumble OFF: none)");
        }
    }
    {
        int a = host_player_ammo(net_local);
        if (a < 0 || (S.ammo_seen >= 0 && a > S.ammo_seen)) { S.mag_seated = 1; S.mag_in = 0; }
        S.ammo_seen = a;
    }
    // the reload gestures
    for (int g = 0; g < 2; g++) {
        if (!is_gun(g) || !S.aim_ok[g]) continue;
        const float *m = S.aim_m[g];
        if (gesture() == QGESTURE_DOWN) {
            // the barrel pointing at the floor (more than 58 degrees down); again once it has come back up past 40
            float dy = -m[9];
            if (S.down_armed[g] && dy < -0.85f) { reload_press(g, -1); S.down_armed[g] = 0; }
            else if (dy > -0.65f) S.down_armed[g] = 1;
        } else if (gesture() == QGESTURE_MAGAZINE && S.opt[QOPT_HAND] == QHAND_BOTH && g == 1) {
            // two guns: knock the grips together; again once 15 cm apart
            if (!S.aim_ok[0]) continue;
            float gb[3]; qgun_grip_base(gb);
            V3 a = xform(m, gb[0], gb[1], gb[2]), b = xform(S.aim_m[0], gb[0], gb[1], gb[2]);
            float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z, d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (S.mag_armed && d < 0.07f) { reload_press(g, 0); S.mag_armed = 0; }
            else if (d > 0.15f) S.mag_armed = 1;
        }
    }
    if (mag_reload()) magazine_and_slide(free_hand());
    // the view: the gun hand's thumbstick click held VIEW_HOLD_NS switches it (a stray click mid-fight doesn't), once a
    // hold; online, push to talk is the other hand's click (a gun in each hand: the right switches, the left talks)
    #define VIEW_HOLD_NS 500000000LL
    const int view_hand = S.opt[QOPT_HAND] == QHAND_LEFT ? 0 : 1;
    if (online_push_to_talk() && !test_talk) online_talk_key(get_bool(S.a_mode, S.hand[1 - view_hand]) && !S.menu_open);
    {
        const int mode = get_bool(S.a_mode, S.hand[view_hand]) && !S.menu_open;
        if (mode && !S.mode_btn) S.mode_t = t;
        if (mode && S.mode_t && t - S.mode_t >= VIEW_HOLD_NS) {
            S.mode_t = 0;                            // (held on: no second switch until let go)
            opt_set(QOPT_VIEW, S.cabinet ? QVIEW_IMMERSIVE : QVIEW_CABINET);
            haptic(view_hand, 0.4f, 25);
            LOGI("view: %s", S.cabinet ? "cabinet" : "immersive");
        }
        if (!mode) S.mode_t = 0;
        S.mode_btn = mode;
    }
    int skip = get_bool(S.a_skip, XR_NULL_PATH) || HT.skip;
    if (skip != S.skip) quest_key(SDL_SCANCODE_SPACE, SDLK_SPACE, skip);
    S.skip = skip;
}

// ---------------------------------------------------------------- each display frame, before the eyes
// The game's newest picture, the input at the frame's display time t (ns), the test switches, the cabinet's picture
// (render: the frame will be shown), the guns, online play's poses, the comfort vignette
static void frame_update(XrTime t, int render) {
    upload_frame();
    game_sync();
    update_input(t);
    {   // QUEST_MENU_AT=seconds[,tab] (env.txt, tests): open the VR menu (on that tab) that long after the first frame
        static double at = -1; static XrTime t0; static int tab = -1;
        if (at < 0) { const char *e = getenv("QUEST_MENU_AT"); at = e ? atof(e) : 0; if (e && strchr(e, ',')) tab = atoi(strchr(e, ',') + 1); t0 = t; }
        if (at > 0 && (double)(t - t0) / 1e9 >= at) { at = 0; menu_set_open(1); if (tab >= 0) qmenu_open(tab); }
    }
    {   // QUEST_TEST_UPDATE=seconds (env.txt, tests): the SYSTEM tab's UPDATE that long after the first frame (the update
        // the launch's check found; none: a check)
        static double at = -1; static XrTime t0;
        if (at < 0) { const char *e = getenv("QUEST_TEST_UPDATE"); at = e ? atof(e) : 0; t0 = t; }
        if (at > 0 && (double)(t - t0) / 1e9 >= at) { at = 0; LOGI("test: update"); xr_os_update_cmd("update"); }
    }
    {   // QUEST_TEST_ONLINE=seconds,host|hostlan|join:CODE|joinip:IP|leave (env.txt, tests): that ONLINE tab action that long after the
        // first frame. env.txt goes first, so the restart the action makes starts as a player would have it.
        static double at = -1; static XrTime t0; static char what[64];
        if (at < 0) { const char *e = getenv("QUEST_TEST_ONLINE"); at = e ? atof(e) : 0; if (e && strchr(e, ',')) snprintf(what, sizeof what, "%s", strchr(e, ',') + 1); t0 = t; }
        if (at > 0 && (double)(t - t0) / 1e9 >= at) {
            at = 0;
            remove("env.txt");
            plat_setenv("QUEST_TEST_ONLINE", "");   // (PC: the restart inherits this process's environment)
            LOGI("test: online %s", what);
            if (!strcmp(what, "host")) online_host_room();
            else if (!strcmp(what, "hostlan")) online_host_lan();
            else if (!strncmp(what, "joinip:", 7)) { online_set_join_ip(what + 7); online_join_ip(); }
            else if (!strcmp(what, "leave")) online_leave();
            else if (!strncmp(what, "join:", 5)) online_join_code(what + 5);
        }
    }
    {   // QUEST_TEST_PICK=seconds[,file] (env.txt, tests): on the setup page, that long after the first frame, the ROM
        // set's import as a pull of the trigger starts it (Quest: ImportActivity; PC: the Open dialog; Vision Pro: the
        // document picker, whose host answers with the file, in the files folder, 3 s later: xr_vos.m)
        static double at = -1; static XrTime t0;
        if (at < 0) { const char *e = getenv("QUEST_TEST_PICK"); at = e ? atof(e) : 0; t0 = t; }
        if (at > 0 && (double)(t - t0) / 1e9 >= at) {
            at = 0;
            if (S.need_files && !S.import_started) { LOGI("test: the ROM set's import"); S.import_started = xr_os_import_rom(); }
        }
    }
    {   // QUEST_TEST_TALK=1 (env.txt, tests): push to talk, held the whole time (voice frames sent without a voice)
        if (test_talk < 0) { test_talk = getenv("QUEST_TEST_TALK") && atoi(getenv("QUEST_TEST_TALK")); if (test_talk) online_set_voice_mode(2); }
        if (test_talk) online_talk_key(true);
    }
    {   // QUEST_TEST_PLAY=1 (env.txt, tests): plays as the flat apps' FLAT_TEST_PLAY does, without a player: coins and
        // Start every few seconds, shots round the screen, every eighth off it (a reload)
        static int play = -1, down, shots; static XrTime t0; static double next_start = 3, next_shot = 4; static uint32_t rng = 12345;
        if (play < 0) { play = getenv("QUEST_TEST_PLAY") && atoi(getenv("QUEST_TEST_PLAY")); t0 = t; }
        const double s = (double)(t - t0) / 1e9;
        if (play && !S.need_files && !S.game_failed) {
            if (s >= next_start) { quest_key(SDL_SCANCODE_SPACE, SDLK_SPACE, 1); quest_key(SDL_SCANCODE_SPACE, SDLK_SPACE, 0); next_start = s + 2.5; }
            if (down) { quest_mouse_button(SDL_BUTTON_LEFT, 0); down = 0; }
            else if (s >= next_shot) {
                rng = rng * 1103515245u + 12345u;
                int x = 40 + (int)((rng >> 8) % 416), y = 40 + (int)((rng >> 18) % 304);
                if (++shots % 8 == 0) x = -20;
                quest_pointer(x, y);
                quest_mouse_button(SDL_BUTTON_LEFT, 1);
                down = 1;
                next_shot = s + 0.3;
            }
        }
    }
    if ((S.cabinet || S.screen_scene) && render) render_cabinet_view();
    if (gun_slot != net_local || gun_kind != S.opt[QOPT_GUN_MODEL]) load_gun();   // the setting, or your player online
    else if (gun_size != S.opt[QOPT_GUN_SIZE]) { gun_size = S.opt[QOPT_GUN_SIZE]; gun_fit(gun_cabinet, gun_size); }
    {   // online: ours out (as drawn last frame), the partner's in
        static XrTime last;
        const float dt = last ? (float)(t - last) / 1e9f : 0.0f;
        last = t;
        pose_send();
        pose_recv(dt > 0 && dt < 0.2f ? dt : 0.0f);
        partner_gun_load();
    }
    pt_update();
    {
        static XrTime last;
        float dt = last ? (float)(t - last) / 1e9f : 0.0f;
        last = t;
        update_vignette(dt > 0 && dt < 0.2f ? dt : 0.0f);
    }
}

#if !XR_OPENXR
// ---------------------------------------------------------------- the host's frames (no OpenXR: Vision Pro)
int xr_host_gl_init(int eye_w, int eye_h) {
    S.sc_w = eye_w; S.sc_h = eye_h;
    return gl_init();
}
void xr_host_frame(const XrhInput *in) {
    S.in = in;
    const XrTime t = (XrTime)(in->time * 1e9);
    if (in->focused != S.focused) {
        S.focused = in->focused;
        update_pause();                     // the game waits while the app is hidden (not online)
        if (!S.focused) release_all();
        LOGI("%s", S.focused ? "focused" : "not focused");
    }
    S.floor_y = in->floor_y;
    if (in->hz) S.hz_now = in->hz;
    frame_update(t, 1);
    if (S.closing) {                        // the last page: a pull (or a pinch) closes the app
        static int was = 1;
        const int f = fire_down(0) || fire_down(1);
        if (f && !was) S.exit_requested = 1;
        was = f;
    }
    arcade_eye_x[0] = -in->ipd / 2; arcade_eye_x[1] = in->ipd / 2;   // (the face layer, m2_gl.c)
    build_scene();
}
void xr_close_with(const char *text) {
    if (S.closing) return;
    S.closing = 1;
    quest_show_message(text);
    S.game_failed = 1;                      // (the message page instead of the game: draw_message)
    LOGI("closing: %s", text);
}
int xr_rom_chosen(int ok) {
    S.import_started = 0;
    if (!ok || !S.need_files) return 0;
    S.need_files = 0;                       // the first set: the game starts now
    quest_clear_message();
    start_game();
    return 1;
}
#else

// ---------------------------------------------------------------- OpenXR setup
static XrAction mk_action(const char *name, const char *label, XrActionType type) {
    XrActionCreateInfo ci = { XR_TYPE_ACTION_CREATE_INFO };
    snprintf(ci.actionName, sizeof ci.actionName, "%s", name);
    snprintf(ci.localizedActionName, sizeof ci.localizedActionName, "%s", label);
    ci.actionType = type;
    ci.countSubactionPaths = 2; ci.subactionPaths = S.hand;
    XrAction a = XR_NULL_HANDLE;
    XR(xrCreateAction(S.aset, &ci, &a));
    return a;
}
static void suggest(const char *profile, const char *const *b, int n) {
    XrActionSuggestedBinding sb[40];
    int k = 0;
    for (int i = 0; i < n && k < 40; i += 2) {
        XrAction a = (XrAction)(uintptr_t)0;
        const char *what = b[i];
        if (!strcmp(what, "aim")) a = S.a_aim; else if (!strcmp(what, "fire")) a = S.a_fire;
        else if (!strcmp(what, "reload")) a = S.a_reload; else if (!strcmp(what, "skip")) a = S.a_skip;
        else if (!strcmp(what, "back")) a = S.a_back; else if (!strcmp(what, "stick")) a = S.a_stick;
        else if (!strcmp(what, "menu")) a = S.a_menu; else if (!strcmp(what, "haptic")) a = S.a_haptic;
        else if (!strcmp(what, "mode")) a = S.a_mode; else if (!strcmp(what, "trig")) a = S.a_trig;
        else if (!strcmp(what, "thumb")) a = S.a_thumb;
        sb[k].action = a; sb[k].binding = path(b[i + 1]); k++;
    }
    XrInteractionProfileSuggestedBinding s = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    s.interactionProfile = path(profile);
    s.countSuggestedBindings = (uint32_t)k; s.suggestedBindings = sb;
    XR(xrSuggestInteractionProfileBindings(S.inst, &s));
}
static void setup_actions(void) {
    S.hand[0] = path("/user/hand/left");
    S.hand[1] = path("/user/hand/right");
    XrActionSetCreateInfo ac = { XR_TYPE_ACTION_SET_CREATE_INFO };
    snprintf(ac.actionSetName, sizeof ac.actionSetName, "gun");
    snprintf(ac.localizedActionSetName, sizeof ac.localizedActionSetName, "Light gun");
    XR(xrCreateActionSet(S.inst, &ac, &S.aset));
    S.a_aim = mk_action("aim", "Aim", XR_ACTION_TYPE_POSE_INPUT);
    S.a_fire = mk_action("fire", "Fire", XR_ACTION_TYPE_BOOLEAN_INPUT);
    S.a_reload = mk_action("reload", "Reload (menu: tabs)", XR_ACTION_TYPE_FLOAT_INPUT);   // the grip, thresholded here
    S.a_back = mk_action("back", "Reload (menu: back)", XR_ACTION_TYPE_BOOLEAN_INPUT);
    S.a_stick = mk_action("stick", "Menu: move", XR_ACTION_TYPE_VECTOR2F_INPUT);
    S.a_skip = mk_action("skip", "Skip scene (menu: select)", XR_ACTION_TYPE_BOOLEAN_INPUT);
    S.a_menu = mk_action("menu", "VR menu", XR_ACTION_TYPE_BOOLEAN_INPUT);
    S.a_mode = mk_action("view", "Immersive / cabinet view", XR_ACTION_TYPE_BOOLEAN_INPUT);
    S.a_haptic = mk_action("haptic", "Recoil", XR_ACTION_TYPE_VIBRATION_OUTPUT);
    S.a_trig = mk_action("finger", "Trigger finger (the hands)", XR_ACTION_TYPE_FLOAT_INPUT);
    S.a_thumb = mk_action("thumb", "Thumb resting (the hands)", XR_ACTION_TYPE_BOOLEAN_INPUT);
    static const char *const touch[] = {
        "aim", "/user/hand/left/input/aim/pose", "aim", "/user/hand/right/input/aim/pose",
        "fire", "/user/hand/left/input/trigger/value", "fire", "/user/hand/right/input/trigger/value",
        "reload", "/user/hand/left/input/squeeze/value", "reload", "/user/hand/right/input/squeeze/value",
        "back", "/user/hand/left/input/y/click", "back", "/user/hand/right/input/b/click",
        "stick", "/user/hand/left/input/thumbstick", "stick", "/user/hand/right/input/thumbstick",
        "skip", "/user/hand/left/input/x/click", "skip", "/user/hand/right/input/a/click",
        "menu", "/user/hand/left/input/menu/click",
        "mode", "/user/hand/left/input/thumbstick/click", "mode", "/user/hand/right/input/thumbstick/click",
        "haptic", "/user/hand/left/output/haptic", "haptic", "/user/hand/right/output/haptic",
        "trig", "/user/hand/left/input/trigger/value", "trig", "/user/hand/right/input/trigger/value",
        "thumb", "/user/hand/left/input/thumbstick/touch", "thumb", "/user/hand/right/input/thumbstick/touch",
        "thumb", "/user/hand/left/input/thumbrest/touch", "thumb", "/user/hand/right/input/thumbrest/touch",
        "thumb", "/user/hand/left/input/x/touch", "thumb", "/user/hand/left/input/y/touch",
        "thumb", "/user/hand/right/input/a/touch", "thumb", "/user/hand/right/input/b/touch",
    };
    suggest("/interaction_profiles/oculus/touch_controller", touch, sizeof touch / sizeof *touch);
    static const char *const simple[] = {
        "aim", "/user/hand/left/input/aim/pose", "aim", "/user/hand/right/input/aim/pose",
        "fire", "/user/hand/left/input/select/click", "fire", "/user/hand/right/input/select/click",
        "menu", "/user/hand/left/input/menu/click",
        "haptic", "/user/hand/left/output/haptic", "haptic", "/user/hand/right/output/haptic",
    };
    suggest("/interaction_profiles/khr/simple_controller", simple, sizeof simple / sizeof *simple);
#if !XR_GLES
    // PC runtimes: the other controllers SteamVR and Windows have. No menu button on the Index (a thumbstick's double
    // click opens the menu: update_input)
    static const char *const index[] = {
        "aim", "/user/hand/left/input/aim/pose", "aim", "/user/hand/right/input/aim/pose",
        "fire", "/user/hand/left/input/trigger/click", "fire", "/user/hand/right/input/trigger/click",
        "reload", "/user/hand/left/input/squeeze/value", "reload", "/user/hand/right/input/squeeze/value",
        "back", "/user/hand/left/input/b/click", "back", "/user/hand/right/input/b/click",
        "stick", "/user/hand/left/input/thumbstick", "stick", "/user/hand/right/input/thumbstick",
        "skip", "/user/hand/left/input/a/click", "skip", "/user/hand/right/input/a/click",
        "mode", "/user/hand/left/input/thumbstick/click", "mode", "/user/hand/right/input/thumbstick/click",
        "haptic", "/user/hand/left/output/haptic", "haptic", "/user/hand/right/output/haptic",
        "trig", "/user/hand/left/input/trigger/value", "trig", "/user/hand/right/input/trigger/value",
        "thumb", "/user/hand/left/input/thumbstick/touch", "thumb", "/user/hand/right/input/thumbstick/touch",
        "thumb", "/user/hand/left/input/a/touch", "thumb", "/user/hand/right/input/a/touch",
        "thumb", "/user/hand/left/input/b/touch", "thumb", "/user/hand/right/input/b/touch",
    };
    suggest("/interaction_profiles/valve/index_controller", index, sizeof index / sizeof *index);
    static const char *const vive[] = {
        "aim", "/user/hand/left/input/aim/pose", "aim", "/user/hand/right/input/aim/pose",
        "fire", "/user/hand/left/input/trigger/click", "fire", "/user/hand/right/input/trigger/click",
        "reload", "/user/hand/left/input/squeeze/click", "reload", "/user/hand/right/input/squeeze/click",
        "stick", "/user/hand/left/input/trackpad", "stick", "/user/hand/right/input/trackpad",
        "menu", "/user/hand/left/input/menu/click", "skip", "/user/hand/right/input/menu/click",
        "mode", "/user/hand/left/input/trackpad/click", "mode", "/user/hand/right/input/trackpad/click",
        "haptic", "/user/hand/left/output/haptic", "haptic", "/user/hand/right/output/haptic",
        "trig", "/user/hand/left/input/trigger/value", "trig", "/user/hand/right/input/trigger/value",
        "thumb", "/user/hand/left/input/trackpad/touch", "thumb", "/user/hand/right/input/trackpad/touch",
    };
    suggest("/interaction_profiles/htc/vive_controller", vive, sizeof vive / sizeof *vive);
    static const char *const wmr[] = {
        "aim", "/user/hand/left/input/aim/pose", "aim", "/user/hand/right/input/aim/pose",
        "fire", "/user/hand/left/input/trigger/value", "fire", "/user/hand/right/input/trigger/value",
        "reload", "/user/hand/left/input/squeeze/click", "reload", "/user/hand/right/input/squeeze/click",
        "back", "/user/hand/left/input/trackpad/click", "back", "/user/hand/right/input/trackpad/click",
        "stick", "/user/hand/left/input/thumbstick", "stick", "/user/hand/right/input/thumbstick",
        "menu", "/user/hand/left/input/menu/click", "skip", "/user/hand/right/input/menu/click",
        "mode", "/user/hand/left/input/thumbstick/click", "mode", "/user/hand/right/input/thumbstick/click",
        "haptic", "/user/hand/left/output/haptic", "haptic", "/user/hand/right/output/haptic",
        "trig", "/user/hand/left/input/trigger/value", "trig", "/user/hand/right/input/trigger/value",
        "thumb", "/user/hand/left/input/trackpad/touch", "thumb", "/user/hand/right/input/trackpad/touch",
    };
    suggest("/interaction_profiles/microsoft/motion_controller", wmr, sizeof wmr / sizeof *wmr);
#endif
    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo sc = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        sc.action = S.a_aim; sc.subactionPath = S.hand[h];
        sc.poseInActionSpace.orientation.w = 1;
        XR(xrCreateActionSpace(S.sess, &sc, &S.aim[h]));
    }
    XrSessionActionSetsAttachInfo at = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    at.countActionSets = 1; at.actionSets = &S.aset;
    XR(xrAttachSessionActionSets(S.sess, &at));
}

static int has_ext(const XrExtensionProperties *p, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(p[i].extensionName, name)) return 1;
    return 0;
}
static char runtime_name[XR_MAX_RUNTIME_NAME_SIZE];
const char *xr_runtime_name(void) { return runtime_name; }
static int refresh_ext, threads_ext, perf_ext;
int xr_instance(void) {
    xr_os_loader_init();
    uint32_t n = 0;
    if (!XR(xrEnumerateInstanceExtensionProperties(NULL, 0, &n, NULL))) return 0;   // (no runtime)
    XrExtensionProperties *props = calloc(n ? n : 1, sizeof *props);
    for (uint32_t i = 0; i < n; i++) props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    XR(xrEnumerateInstanceExtensionProperties(NULL, n, &n, props));
    const char *ext[12];
    uint32_t ne = 0;
    if (!has_ext(props, n, XR_GL_ENABLE_EXTENSION_NAME)) { LOGE("OpenXR: the runtime has no %s", XR_GL_ENABLE_EXTENSION_NAME); free(props); return 0; }
    ext[ne++] = XR_GL_ENABLE_EXTENSION_NAME;
#if defined(XR_USE_PLATFORM_ANDROID)
    ext[ne++] = XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME;
    threads_ext = has_ext(props, n, XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME);
    if (threads_ext) ext[ne++] = XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME;
#endif
    refresh_ext = has_ext(props, n, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    perf_ext = has_ext(props, n, XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    if (refresh_ext) ext[ne++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;
    if (perf_ext) ext[ne++] = XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME;
    PT.ext = has_ext(props, n, XR_FB_PASSTHROUGH_EXTENSION_NAME);
    if (PT.ext) ext[ne++] = XR_FB_PASSTHROUGH_EXTENSION_NAME;
    HT.ext = has_ext(props, n, XR_EXT_HAND_TRACKING_EXTENSION_NAME) && has_ext(props, n, XR_FB_HAND_TRACKING_AIM_EXTENSION_NAME);
    if (HT.ext) { ext[ne++] = XR_EXT_HAND_TRACKING_EXTENSION_NAME; ext[ne++] = XR_FB_HAND_TRACKING_AIM_EXTENSION_NAME; }
    free(props);

    XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };
    ci.next = xr_os_instance_next();
    snprintf(ci.applicationInfo.applicationName, sizeof ci.applicationInfo.applicationName, "HOTD recomp VR");
    snprintf(ci.applicationInfo.engineName, sizeof ci.applicationInfo.engineName, "hotd-recomp");
    ci.applicationInfo.applicationVersion = 1;
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = ne; ci.enabledExtensionNames = ext;
    if (!XR(xrCreateInstance(&ci, &S.inst))) return 0;
    XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };
    if (XR_SUCCEEDED(xrGetInstanceProperties(S.inst, &ip))) {
        LOGI("OpenXR runtime: %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
        snprintf(runtime_name, sizeof runtime_name, "%s", ip.runtimeName);
        S.steamvr = strstr(ip.runtimeName, "SteamVR") != NULL;
    }
    if (refresh_ext) {
        xrGetInstanceProcAddr(S.inst, "xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction *)&S.enum_rates);
        xrGetInstanceProcAddr(S.inst, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction *)&S.req_rate);
    }
#if XR_GLES
    if (threads_ext) xrGetInstanceProcAddr(S.inst, "xrSetAndroidApplicationThreadKHR", (PFN_xrVoidFunction *)&S.set_thread);
#endif
    if (perf_ext) xrGetInstanceProcAddr(S.inst, "xrPerfSettingsSetPerformanceLevelEXT", (PFN_xrVoidFunction *)&S.perf_level);

    XrSystemGetInfo sg = { XR_TYPE_SYSTEM_GET_INFO };
    sg.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!XR(xrGetSystem(S.inst, &sg, &S.sys))) {        // (a runtime, but no headset on it now)
        xrDestroyInstance(S.inst);
        S.inst = XR_NULL_HANDLE;
        return 0;
    }
    {
        XrSystemProperties sp = { XR_TYPE_SYSTEM_PROPERTIES };
        if (XR_SUCCEEDED(xrGetSystemProperties(S.inst, S.sys, &sp))) LOGI("OpenXR system: %s", sp.systemName);
    }
    return 1;
}
int xr_session(void) {
    const void *binding = NULL;
    if (!xr_os_graphics_binding(S.inst, S.sys, &binding)) return 0;
    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
    sci.next = binding; sci.systemId = S.sys;
    if (!XR(xrCreateSession(S.inst, &sci, &S.sess))) return 0;

    XrReferenceSpaceCreateInfo rs = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rs.poseInReferenceSpace.orientation.w = 1;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (!XR(xrCreateReferenceSpace(S.sess, &rs, &S.local))) return 0;
    pt_init();
    ht_init();
    {
        XrReferenceSpaceCreateInfo vs = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
        vs.poseInReferenceSpace.orientation.w = 1;
        vs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        XR(xrCreateReferenceSpace(S.sess, &vs, &S.viewsp));
    }
    uint32_t nsp = 0;
    XR(xrEnumerateReferenceSpaces(S.sess, 0, &nsp, NULL));
    XrReferenceSpaceType *sp = calloc(nsp ? nsp : 1, sizeof *sp);
    XR(xrEnumerateReferenceSpaces(S.sess, nsp, &nsp, sp));
    for (uint32_t i = 0; i < nsp; i++)
        if (sp[i] == XR_REFERENCE_SPACE_TYPE_STAGE) { rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE; XR(xrCreateReferenceSpace(S.sess, &rs, &S.stage)); }
    free(sp);

    // eye buffers: the recommended size times the Resolution setting (QUEST_RES_SCALE overrides it)
    uint32_t nv = 0;
    S.vcv[0].type = S.vcv[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    if (!XR(xrEnumerateViewConfigurationViews(S.inst, S.sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nv, S.vcv))) return 0;
    const char *rsc = getenv("QUEST_RES_SCALE");
    float scale = rsc ? (float)atof(rsc) : qres_scale[S.opt[QOPT_RESOLUTION]];
    if (scale < 0.5f || scale > 2.0f) scale = 1.25f;
    S.sc_w = (int)(S.vcv[0].recommendedImageRectWidth * scale);
    S.sc_h = (int)(S.vcv[0].recommendedImageRectHeight * scale);
    if (S.sc_w > (int)S.vcv[0].maxImageRectWidth) S.sc_w = (int)S.vcv[0].maxImageRectWidth;
    if (S.sc_h > (int)S.vcv[0].maxImageRectHeight) S.sc_h = (int)S.vcv[0].maxImageRectHeight;
    uint32_t nf = 0;
    int64_t fmt = 0;
    XR(xrEnumerateSwapchainFormats(S.sess, 0, &nf, NULL));
    int64_t *fmts = calloc(nf ? nf : 1, sizeof *fmts);
    XR(xrEnumerateSwapchainFormats(S.sess, nf, &nf, fmts));
    for (uint32_t i = 0; i < nf; i++) if (fmts[i] == GL_SRGB8_ALPHA8) fmt = GL_SRGB8_ALPHA8;
    if (!fmt && nf) fmt = fmts[0];
    free(fmts);
    if (!fmt) { LOGE("no swapchain format"); return 0; }
#if !XR_GLES
    S.sc_fmt = (GLenum)fmt;
    LOGI("swapchains: format 0x%x%s", (unsigned)fmt, fmt == GL_SRGB8_ALPHA8 ? " (sRGB)" : "");
#endif
    for (int e = 0; e < 2; e++) {
        XrSwapchainCreateInfo sc = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sc.format = fmt; sc.sampleCount = 1; sc.width = (uint32_t)S.sc_w; sc.height = (uint32_t)S.sc_h;
        sc.faceCount = 1; sc.arraySize = 1; sc.mipCount = 1;
        if (!XR(xrCreateSwapchain(S.sess, &sc, &S.sc[e]))) return 0;
        XR(xrEnumerateSwapchainImages(S.sc[e], 0, &S.sc_count[e], NULL));
        S.sc_img[e] = calloc(S.sc_count[e], sizeof **S.sc_img);
        for (uint32_t i = 0; i < S.sc_count[e]; i++) S.sc_img[e][i].type = XR_TYPE_SWAPCHAIN_IMAGE_GL;
        XR(xrEnumerateSwapchainImages(S.sc[e], S.sc_count[e], &S.sc_count[e], (XrSwapchainImageBaseHeader *)S.sc_img[e]));
    }
    setup_actions();
    return 1;
}
int xr_gl_init(void) { return gl_init(); }

// once the session runs: display rate, thread hints, CPU level
static void session_started(void) {
    request_rate(1);         // 120 Hz (the default) shows each of the game's 60 frames exactly twice
#if XR_GLES
    if (S.set_thread) {
        XR(S.set_thread(S.sess, XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR, (uint32_t)gettid()));
        if (S.game_tid) XR(S.set_thread(S.sess, XR_ANDROID_THREAD_TYPE_APPLICATION_MAIN_KHR, (uint32_t)S.game_tid));
    }
#endif
    // the game is one CPU-bound thread; the room is nothing for the GPU
    if (S.perf_level) XR(S.perf_level(S.sess, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT));
}

void xr_events(void) {
    XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(S.inst, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrEventDataSessionStateChanged *e = (const XrEventDataSessionStateChanged *)&ev;
            S.state = e->state;
            LOGI("session state %d", (int)S.state);
            if (S.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR(xrBeginSession(S.sess, &bi))) { S.running = 1; session_started(); }
            } else if (S.state == XR_SESSION_STATE_STOPPING) {
                XR(xrEndSession(S.sess));
                S.running = 0;
            } else if (S.state == XR_SESSION_STATE_EXITING || S.state == XR_SESSION_STATE_LOSS_PENDING) {
                S.exit_requested = 1;
            }
            // the game waits while you are in the system menu or have taken the headset off
            update_pause();
            if (S.state != XR_SESSION_STATE_FOCUSED) release_all();
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            S.exit_requested = 1;
        } else if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
            // controllers or hands in use (the hands' reload follows: ht_who_plays)
            HT.touch = 0;
            for (int h = 0; h < 2; h++) {
                XrInteractionProfileState ps = { XR_TYPE_INTERACTION_PROFILE_STATE };
                char name[XR_MAX_PATH_LENGTH] = "none";
                uint32_t len = 0;
                if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(S.sess, S.hand[h], &ps)) && ps.interactionProfile != XR_NULL_PATH) {
                    xrPathToString(S.inst, ps.interactionProfile, sizeof name, &len, name);
                    if (strstr(name, "touch_controller")) HT.touch = 1;
                }
                LOGI("input, %s hand: %s", h ? "right" : "left", name);
            }
        }
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
        ev.next = NULL;
    }
}

void xr_frame(void) {
    XrFrameWaitInfo wi = { XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState fs = { XR_TYPE_FRAME_STATE };
    if (!XR(xrWaitFrame(S.sess, &wi, &fs))) return;
    XrFrameBeginInfo bi = { XR_TYPE_FRAME_BEGIN_INFO };
    if (!XR(xrBeginFrame(S.sess, &bi))) return;
    frame_update(fs.predictedDisplayTime, fs.shouldRender);
    XrCompositionLayerProjectionView pv[2];
    XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    const XrCompositionLayerBaseHeader *layers[2];
    uint32_t nl = 0;
    XrCompositionLayerPassthroughFB ptl = { XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB };
    if (PT.running && fs.shouldRender) {
        ptl.layerHandle = PT.layer;
        layers[nl++] = (const XrCompositionLayerBaseHeader *)&ptl;
    }
    if (fs.shouldRender) {
        XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
        XrViewState vs = { XR_TYPE_VIEW_STATE };
        XrViewLocateInfo li = { XR_TYPE_VIEW_LOCATE_INFO };
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = fs.predictedDisplayTime; li.space = S.local;
        uint32_t nv = 0;
        if (XR(xrLocateViews(S.sess, &li, &vs, 2, &nv, views)) && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
            if (S.stage) {     // the real floor, when the guardian knows it
                XrSpaceLocation sl = { XR_TYPE_SPACE_LOCATION };
                if (XR_SUCCEEDED(xrLocateSpace(S.stage, S.local, fs.predictedDisplayTime, &sl)) && (sl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
                    S.floor_y = sl.pose.position.y;
            }
            build_scene();
            {   // each eye's offset from the middle of the head, along the eyes' line (the face layer, m2_gl.c)
                const XrVector3f a = views[0].pose.position, b = views[1].pose.position;
                const float ipd = sqrtf((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
                arcade_eye_x[0] = -ipd / 2; arcade_eye_x[1] = ipd / 2;
            }
            for (int e = 0; e < 2; e++) {
                render_eye(e, &views[e]);
                pv[e] = (XrCompositionLayerProjectionView){ XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
                pv[e].pose = views[e].pose; pv[e].fov = views[e].fov;
                pv[e].subImage.swapchain = S.sc[e];
                pv[e].subImage.imageRect.extent.width = S.sc_w;
                pv[e].subImage.imageRect.extent.height = S.sc_h;
            }
            layer.space = S.local; layer.viewCount = 2; layer.views = pv;
            if (PT.running) layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            layers[nl++] = (const XrCompositionLayerBaseHeader *)&layer;
        }
    }
#if !XR_GLES
    {   // every 10 s in port.log: the frames drawn, the session's state, the inputs in hand (tests read it)
        static int frames, drawn; static uint64_t t0;
        frames++; drawn += nl > 0;
        const uint64_t now = plat_now_ns();
        if (!t0) t0 = now;
        if (now - t0 >= 10000000000ULL) {
            LOGI("xr: %d frames in %.1f s, %d drawn; state %d, %s view, aim %d %d, game %s", frames, (double)(now - t0) / 1e9,
                 drawn, (int)S.state, S.cabinet ? "cabinet" : "immersive", S.aim_ok[0], S.aim_ok[1],
                 S.game_failed ? "failed" : S.need_files ? "no ROM" : S.scene ? "showing" : "starting");
            frames = drawn = 0; t0 = now;
        }
    }
#endif
    XrFrameEndInfo ei = { XR_TYPE_FRAME_END_INFO };
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = nl; ei.layers = layers;
    XR(xrEndFrame(S.sess, &ei));
}

// ---------------------------------------------------------------- the host's loop
int xr_running(void) { return S.running && S.inst; }
void xr_end_session(void) { if (S.running) XR(xrRequestExitSession(S.sess)); }
#endif   // XR_OPENXR
void xr_menu_toggle(void) {
    if (S.game_failed || S.need_files) S.exit_requested = 1;
    else menu_set_open(!S.menu_open);
}
void xr_quit(void) {
    online_quit();                          // the partner hears us go at once (else they wait out a timeout)
    // The audio stream stops first: its callback thread would run on into what exit()'s destructors tear down (a
    // crash on every quit, "pthread_mutex_lock called on a destroyed mutex"); the host then ends the process without
    // them, as the game thread cannot be stopped from outside and is still running; its saves are written as it goes.
    quest_audio_shutdown();
}

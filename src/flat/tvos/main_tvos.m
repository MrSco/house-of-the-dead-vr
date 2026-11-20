// The flat app's Apple TV front end: a UIKit app (a scene, one view on a CAMetalLayer) drawing through ANGLE's OpenGL
// ES 3 on Metal, a frame each display refresh, as the iPhone and iPad one (src/flat/ios/main_ios.m) with what a TV
// differs in:
// - The Siri Remote is the gun, read through GameController (the controller is a GCEventViewController, so the
//   remote's presses are the game's and Menu doesn't leave the app): the touch surface moves the sight as a mouse
//   (relative, faster swipes go further, GAME: Remote speed), a click shoots (off the picture: reload, as on the
//   cabinet), Play/Pause reloads, Play/Pause held half a second gives coins and Start, Menu opens the menu. In the
//   menu, each third of the surface swiped is one step of the focus, a click chooses, Play/Pause resumes, Menu goes
//   back, and at the menu's top (nothing open in it) Menu goes to the Home screen, as Apple asks: there the
//   controller lets UIKit have the presses (controllerUserInteractionEnabled). UIKit's own presses (a keyboard, the
//   Simulator's remote, XCUIRemote) do the same as the remote's, a press from both counted once.
// - Pads (MFi, Xbox, PlayStation) as on iOS: either stick moves the sight, R2 / R1 shoot, L2 / L1 / X reload,
//   A / Menu coins and Start, Options the menu; in the menu the d-pad and A / B as the remote. Each pad and the remote
//   is a device of its own (flat.h's players): a second player joins with A on a pad, or Play/Pause held on the
//   remote while player 1 plays on a pad; that pad (or the remote) keeps a sight of its own.
// - The ROM set comes from a phone or computer: the TV shows an address and its QR code, and a page served there sends
//   the file (tv_upload.m). The files folder is Library/Caches/HOTDVR (a tvOS app can't write Documents), which tvOS
//   may empty when short of space: the settings and saves are kept in the app's defaults too (tv_store.m), and a ROM
//   set that is gone is asked for again.
// - No microphone of its own: voice chat listens (apple.m), and talks only when tvOS reports an input.
// An app can't start itself again: where the app would restart (online play begins and ends so, a new ROM set), it
// says so and closes, and the player opens it again (online-session.cfg waits ten minutes).
//
// Test switches (env.txt, beside flat_app.c's): FLAT_TEST_REMOTE="seconds action [args]; ..." plays the remote through
// the same code as the real one: swipe DX DY [SECONDS] (the touch surface, -1..1, up is +), click, play, playhold,
// menu, up, down, left, right; the sight and each press are logged. padstart, padshoot X Y: a stand-in pad (a device
// of its own, flat.h) presses Start or shoots at X Y (0..1 of the picture), for two players without a real pad. FLAT_TEST_PICTURE=lines,samples: HD's limit.
#import <GameController/GameController.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreImage/CoreImage.h>
#import <Metal/Metal.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <math.h>
#include "flat/flat.h"
#include "flat/tvos/tv.h"
#include "platform/gl.h"
#include "platform/platform.h"
#include "quest_menu.h"
#include "quest_options.h"
#include "online.h"
#include "quest.h"
#include "flat/apple_pads.h"

#ifndef EGL_PLATFORM_ANGLE_ANGLE
#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#endif
#ifndef EGL_PLATFORM_ANGLE_TYPE_ANGLE
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#endif
#ifndef EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE
#define EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE 0x3489
#endif

#define LOGI(...) plat_log(PLAT_LOG_INFO, __VA_ARGS__)
#define LOGE(...) plat_log(PLAT_LOG_ERROR, __VA_ARGS__)

static struct {
    char files[1024];
    int started, background, quit, closing, upload;   // upload: the ROM page is showing
    EGLDisplay dpy;
    EGLContext ctx;
    EGLSurface surf;
    int w, h;
    float sx, sy;                                      // the sight (window pixels): the remote's, and its player's pads'
    double last, kept_at;
    // the perf line every 10 s: frames drawn, those later than 1.5 refreshes, the frame's own time on the main thread
    double perf_at, frame_ms, frame_max, gap_max, draw_ms;
    int perf_frames, perf_late;
} A = { .sx = -1 };

@interface HotdView : UIView
@end
@interface HotdController : GCEventViewController
@property (nonatomic, strong) CADisplayLink *link;
@property (nonatomic, strong) UIView *uploadView;
@property (nonatomic, strong) UILabel *uploadUrl, *uploadStatus, *uploadHint;
@property (nonatomic, strong) UIImageView *uploadQr;
@property (nonatomic, strong) NSTimer *uploadTimer;
@property (nonatomic, copy) NSString *shownUrl;
- (void)showUpload:(NSString *)why;
- (void)romArrived;
@end
static HotdController *the_controller;

static double now_s(void) { return (double)plat_now_ns() * 1e-9; }

// ---------------------------------------------------------------- flat.h's front-end calls
void flat_os_fullscreen(int on) { (void)on; }
int flat_os_touch(void) { return 0; }
void flat_os_crash_reports(const char *files, const int *opt) { (void)files; (void)opt; }   // (none here yet)
int flat_os_has_reports(void) { return 0; }
void flat_os_quit(void) { A.quit = 1; }   // (no QUIT row on a TV; FLAT_QUIT_AT, tests)
void flat_os_import_rom(void) {
    dispatch_async(dispatch_get_main_queue(), ^{ flat_menu_set_open(0); [the_controller showUpload:nil]; });
}

// a message, then the app closes (no restart on a TV either)
static void close_with(NSString *title, NSString *text) {
    if (A.closing) return;
    A.closing = 1;
    the_controller.controllerUserInteractionEnabled = YES;   // (the remote presses the alert's button)
    [the_controller.link invalidate];
    flat_app_quit();
    tv_store_save(A.files);
    UIAlertController *a = [UIAlertController alertControllerWithTitle:title message:text preferredStyle:UIAlertControllerStyleAlert];
    [a addAction:[UIAlertAction actionWithTitle:@"Close" style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) { (void)x; exit(0); }]];
    [the_controller presentViewController:a animated:YES completion:nil];
}

static int has_rom(void) {
    char p[1100];
    snprintf(p, sizeof p, "%s/arcade/hotd.zip", A.files);
    if (plat_exists(p)) return 1;
    snprintf(p, sizeof p, "%s/arcade/hotd.7z", A.files);
    return plat_exists(p);
}

// ---------------------------------------------------------------- ANGLE's EGL on the view's layer
static int egl_init(CAMetalLayer *layer) {
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!get_display) { LOGE("ANGLE: no eglGetPlatformDisplayEXT"); return 0; }
    const EGLint dattr[] = { EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE, EGL_NONE };
    A.dpy = get_display(EGL_PLATFORM_ANGLE_ANGLE, (void *)EGL_DEFAULT_DISPLAY, dattr);
    EGLint maj = 0, min = 0;
    if (A.dpy == EGL_NO_DISPLAY || !eglInitialize(A.dpy, &maj, &min)) { LOGE("ANGLE: no Metal display"); return 0; }
    const EGLint cattr[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                             EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(A.dpy, cattr, &cfg, 1, &n) || n < 1) { LOGE("ANGLE: no ES 3 config"); return 0; }
    const EGLint xattr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    A.ctx = eglCreateContext(A.dpy, cfg, EGL_NO_CONTEXT, xattr);
    if (A.ctx == EGL_NO_CONTEXT) { LOGE("ANGLE: no ES 3 context"); return 0; }
    A.surf = eglCreateWindowSurface(A.dpy, cfg, (EGLNativeWindowType)(__bridge void *)layer, NULL);
    if (A.surf == EGL_NO_SURFACE || !eglMakeCurrent(A.dpy, A.surf, A.surf, A.ctx)) { LOGE("ANGLE: no surface on the layer"); return 0; }
    eglSwapInterval(A.dpy, 1);
    LOGI("ANGLE EGL %d.%d", maj, min);
    return 1;
}

// ---------------------------------------------------------------- the sight
static int sight_place(int *vx, int *vy, int *vw, int *vh) {
    flat_view_rect(vx, vy, vw, vh);
    if (*vw <= 0) return 0;
    if (A.sx < 0) { A.sx = *vx + *vw * 0.5f; A.sy = *vy + *vh * 0.5f; }   // from the middle
    return 1;
}
static void sight_move(float dx, float dy) {   // window pixels; kept on the picture, so its crosshair always shows
    int vx, vy, vw, vh;                          // (a TV reloads with a button: Play/Pause, the ring)
    if (!sight_place(&vx, &vy, &vw, &vh)) return;
    A.sx = fminf(fmaxf(A.sx + dx, vx + 2.0f), vx + vw - 3.0f);
    A.sy = fminf(fmaxf(A.sy + dy, vy + 2.0f), vy + vh - 3.0f);
    flat_dev_pointer(FLAT_DEV_REMOTE, (int)A.sx, (int)A.sy);
}

// ---------------------------------------------------------------- the Siri Remote
// Its buttons, from GameController (SRC_GC), a pad in the menu (SRC_PAD), UIKit (SRC_UI) or FLAT_TEST_REMOTE (SRC_TEST)
enum { RB_CLICK, RB_PLAY, RB_MENU, RB_UP, RB_DOWN, RB_LEFT, RB_RIGHT, RB_COUNT };
enum { SRC_GC = 1, SRC_PAD, SRC_UI, SRC_TEST };
static const char *const RB_NAME[RB_COUNT] = { "click", "play", "menu", "up", "down", "left", "right" };
static struct {
    int touching;
    float tx, ty;                  // the finger on the touch surface (-1..1, y up)
    double tt;
    float step_x, step_y;          // the menu: the travel since its last step
    int owner[RB_COUNT];           // the source holding each button
    int last_src[RB_COUNT];
    double last_at[RB_COUNT];
    double play_at;                // Play/Pause held since (coins and Start at half a second)
    int play_started;
    int test;                      // FLAT_TEST_REMOTE: log the presses and the sight
    int ring;                      // a 2nd generation (or later) remote: the ring clicks (Cardinal Direction Pad)
    float pdx, pdy;                // the touch's last move, applied when the next reading shows the finger still down
    int pending;
    int clicks;                    // clicks held (the surface's moves are frozen meanwhile)
    double freeze_until;           // ...and a moment after
    float hx[8], hy[8];            // the sight's last moves (window pixels), undone back to before a click's press
    double ht[8];
    int hn;
    int ring_b;                    // the menu: a ring direction held (RB_UP..), and its repeat
    double ring_since, ring_last;
} R;

// the top of the app: the ROM page before the game, or the menu with nothing open in it. Menu goes to the Home screen.
static int top_level(void) {
    if (A.upload) return !A.started;
    return flat_menu_is_open() && !qmenu_modal();
}

// The touch surface. As GoldenEye VR found on an Apple TV 4K (#268): the last reading as the finger lifts is not 0
// but a few hundredths (the sight jumped back towards where the surface's middle is), and pressing a click moves
// the finger. So a move is applied only when the next reading shows the finger still down; a reading near the
// middle straight after one far from it is the lift; a jump of more than a quarter of the surface in one reading is
// a new landing; the surface does nothing while a click is held and a moment after; and a click puts the sight back
// where it was 60 ms before its press.
static void sight_history(float dx, float dy, double now) {
    const int i = R.hn++ & 7;
    R.hx[i] = dx; R.hy[i] = dy; R.ht[i] = now;
}
static void sight_undo_since(double t) {
    float ux = 0, uy = 0;
    for (int i = 0; i < 8; i++) if (R.ht[i] >= t) { ux -= R.hx[i]; uy -= R.hy[i]; R.ht[i] = 0; }
    if (ux || uy) sight_move(ux, uy);
}
static void remote_touch(int down, float x, float y, double now) {
    if (!down) {
        if (R.touching && R.test && !flat_menu_is_open() && !A.upload) {
            int vx, vy, vw, vh;
            flat_view_rect(&vx, &vy, &vw, &vh);
            LOGI("tv: sight at %.0f,%.0f (the picture %d,%d %dx%d)", A.sx, A.sy, vx, vy, vw, vh);
        }
        R.touching = 0; R.pending = 0;
        return;
    }
    if (!R.touching) {   // it landed: the sight stays where it is (relative, as a mouse)
        R.touching = 1;
        R.tx = x; R.ty = y; R.tt = now;
        R.step_x = R.step_y = 0; R.pending = 0;
        return;
    }
    const float dx = x - R.tx, dy = y - R.ty;
    const float was = hypotf(R.tx, R.ty), is = hypotf(x, y);
    const double dt = now - R.tt > 1.0 / 240 ? now - R.tt : 1.0 / 240;
    R.tx = x; R.ty = y; R.tt = now;
    if (is < 0.08f && was > 0.2f) { R.touching = 0; R.pending = 0; return; }   // the lift's last reading
    if (hypotf(dx, dy) > 0.5f) { R.pending = 0; return; }                       // a jump: a new landing
    if (R.clicks || now < R.freeze_until) { R.pending = 0; return; }            // a click: the finger presses
    if (A.upload || A.closing) return;
    if (flat_menu_is_open()) {   // each third of the surface: one step of the focus (a remote with the ring: its clicks)
        if (R.ring) return;
        R.step_x += dx; R.step_y += dy;
        const float T = 0.6f;
        if (fabsf(R.step_x) >= T && fabsf(R.step_x) >= fabsf(R.step_y)) {
            flat_menu_key(R.step_x > 0 ? FLAT_KEY_RIGHT : FLAT_KEY_LEFT);
            R.step_x -= R.step_x > 0 ? T : -T; R.step_y = 0;
        } else if (fabsf(R.step_y) >= T) {
            flat_menu_key(R.step_y > 0 ? FLAT_KEY_UP : FLAT_KEY_DOWN);
            R.step_y -= R.step_y > 0 ? T : -T; R.step_x = 0;
        }
        return;
    }
    // the sight: a slow swipe across the whole surface (2 units) moves it half the picture; quicker ones up to 3x that
    int vx, vy, vw, vh;
    if (!sight_place(&vx, &vy, &vw, &vh)) return;
    if (R.pending) {   // the reading before this one: the finger stayed down, so it was a real move
        const float ox = A.sx, oy = A.sy;
        sight_move(R.pdx, R.pdy);
        sight_history(A.sx - ox, A.sy - oy, now);
    }
    const float v = hypotf(dx, dy) / (float)dt;   // surface units a second
    float accel = (v - 0.5f) / 3.5f;
    accel = 1 + 2 * (accel < 0 ? 0 : accel > 1 ? 1 : accel);
    const float k = (float)vw * 0.25f * accel * (float)flat_opt_get(QOPT_REMOTE_SPEED) / 100.0f;
    R.pdx = dx * k; R.pdy = -dy * k; R.pending = 1;
}

static void button_action(int b, int down, double now) {
    if (R.test) LOGI("tv: %s %s%s", RB_NAME[b], down ? "down" : "up", flat_menu_is_open() ? " (menu)" : A.upload ? " (ROM page)" : "");
    if (A.closing) return;
    if (A.upload) {   // the ROM page: Menu cancels it (over the game); before the game, Menu is the Home screen's
        if (b == RB_MENU && down && A.started) {
            tv_upload_stop();
            [the_controller.uploadView removeFromSuperview];
            [the_controller.uploadTimer invalidate];
            A.upload = 0;
        }
        return;
    }
    if (flat_menu_is_open()) {
        if (!down) return;
        switch (b) {
        case RB_CLICK: flat_menu_key(FLAT_KEY_ENTER); break;
        case RB_PLAY: flat_menu_set_open(0); break;
        case RB_MENU:
            if (qmenu_modal()) flat_menu_key(FLAT_KEY_BACK);
            else LOGI("tv: Menu at the menu's top: the Home screen");   // (UIKit's press takes the app there)
            break;
        case RB_UP: flat_menu_key(FLAT_KEY_UP); break;
        case RB_DOWN: flat_menu_key(FLAT_KEY_DOWN); break;
        case RB_LEFT: flat_menu_key(FLAT_KEY_LEFT); break;
        case RB_RIGHT: flat_menu_key(FLAT_KEY_RIGHT); break;
        }
        return;
    }
    switch (b) {
    case RB_CLICK: {
        int vx, vy, vw, vh;
        if (down) { R.pending = 0; sight_undo_since(now - 0.06); }   // (the press moved the finger)
        if (sight_place(&vx, &vy, &vw, &vh)) flat_dev_pointer(FLAT_DEV_REMOTE, (int)A.sx, (int)A.sy);
        flat_dev_trigger(FLAT_DEV_REMOTE, down);
        break;
    }
    case RB_UP: case RB_DOWN: case RB_LEFT: case RB_RIGHT:   // the ring's clicks (2nd generation): reload
        flat_dev_reload(FLAT_DEV_REMOTE, down);
        break;
    case RB_PLAY:   // reload at once; held: coins and Start (remote_frame)
        flat_dev_reload(FLAT_DEV_REMOTE, down);
        R.play_at = down ? now : 0;
        R.play_started = 0;
        break;
    case RB_MENU: if (down) flat_menu_set_open(1); break;
    default: break;
    }
}

// a press from one source, counted once: UIKit gets a remote's or pad's presses as well while the app is at its top
static void remote_button(int b, int down, int src, double now) {
    if (b < 0 || b >= RB_COUNT) return;
    if (down) {
        if (R.owner[b]) return;
        if (src != SRC_TEST && R.last_src[b] && R.last_src[b] != src && now - R.last_at[b] < 0.25) return;
        R.owner[b] = src; R.last_src[b] = src; R.last_at[b] = now;
    } else {
        if (R.owner[b] != src) return;
        R.owner[b] = 0;
    }
    if (b != RB_PLAY && b != RB_MENU && src != SRC_PAD) {   // a click of the touch surface (the centre or the ring)
        if (down) R.clicks++;
        else if (R.clicks > 0 && --R.clicks == 0) R.freeze_until = now + 0.12;
        R.pending = 0;
    }
    if (b >= RB_UP && src == SRC_GC) { R.ring_b = down ? b : 0; R.ring_since = R.ring_last = now; }
    button_action(b, down, now);
}

static void remote_frame(double now) {
    if (A.started && !A.upload && A.sx < 0) {   // the sight from the start, in the middle (so its crosshair shows)
        int vx, vy, vw, vh;
        if (sight_place(&vx, &vy, &vw, &vh)) flat_dev_pointer(FLAT_DEV_REMOTE, (int)A.sx, (int)A.sy);
    }
    if (R.ring_b && flat_menu_is_open() && now - R.ring_since > 0.45 && now - R.ring_last > 0.12) {   // a ring held
        R.ring_last = now;
        static const int key[] = { FLAT_KEY_UP, FLAT_KEY_DOWN, FLAT_KEY_LEFT, FLAT_KEY_RIGHT };
        flat_menu_key(key[R.ring_b - RB_UP]);
    }
    if (R.play_at > 0 && !R.play_started && now - R.play_at >= 0.5 && !flat_menu_is_open()) {
        R.play_started = 1;
        flat_dev_reload(FLAT_DEV_REMOTE, 0);
        flat_dev_start(FLAT_DEV_REMOTE, 1);
        flat_dev_start(FLAT_DEV_REMOTE, 0);
        if (R.test) LOGI("tv: play held: coins and Start");
    }
}

static void hook_remote(GCController *c) {
    if (c.extendedGamepad || !c.microGamepad) return;   // (a pad: pad_frame)
    GCMicroGamepad *m = c.microGamepad;
    m.reportsAbsoluteDpadValues = YES;   // the finger's place: the travel between two is the move
    m.allowsRotation = NO;
    m.dpad.valueChangedHandler = ^(GCControllerDirectionPad *d, float x, float y) { (void)d; remote_touch(x != 0 || y != 0, x, y, now_s()); };
    m.buttonX.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL p) { (void)b; (void)v; remote_button(RB_PLAY, p, SRC_GC, now_s()); };
    m.buttonMenu.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL p) { (void)b; (void)v; remote_button(RB_MENU, p, SRC_GC, now_s()); };
    // A 2nd generation (or later) remote: Button A is any click, the ring's too (GoldenEye VR's log, #268), so its
    // centre click (Button Center) shoots and the ring's four clicks (Cardinal Direction Pad) are their own: reload in
    // the game, the focus's steps in the menu. A 1st generation one: Button A, the whole surface's click, shoots.
    GCPhysicalInputProfile *pf = c.physicalInputProfile;
    GCControllerButtonInput *centre = pf.buttons[@"Button Center"];
    GCControllerDirectionPad *ring = pf.dpads[@"Cardinal Direction Pad"];
    if (centre && ring) {
        R.ring = 1;
        m.buttonA.pressedChangedHandler = nil;
        centre.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL p) { (void)b; (void)v; remote_button(RB_CLICK, p, SRC_GC, now_s()); };
        ring.up.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL p) { (void)b; (void)v; remote_button(RB_UP, p, SRC_GC, now_s()); };
        ring.down.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL p) { (void)b; (void)v; remote_button(RB_DOWN, p, SRC_GC, now_s()); };
        ring.left.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL p) { (void)b; (void)v; remote_button(RB_LEFT, p, SRC_GC, now_s()); };
        ring.right.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL p) { (void)b; (void)v; remote_button(RB_RIGHT, p, SRC_GC, now_s()); };
    } else {
        m.buttonA.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL p) { (void)b; (void)v; remote_button(RB_CLICK, p, SRC_GC, now_s()); };
    }
    LOGI("tv: remote %s (%s generation: %s)", c.vendorName.UTF8String ?: "(unnamed)", ring ? "2nd or later" : "1st",
         centre && ring ? "the centre shoots, the ring reloads" : "a click shoots");
}

// ---------------------------------------------------------------- FLAT_TEST_REMOTE (tests)
typedef struct { double t; int kind, b, down; float x, y; } TestEv;   // kind 0: a button, 1: the touch surface, 2: a pad's
static TestEv tev[512];
static int tev_n, tev_i;
static double tev_t0;
static void test_add(double t, int kind, int b, int down, float x, float y) {
    if (tev_n < (int)(sizeof tev / sizeof *tev)) tev[tev_n++] = (TestEv){ t, kind, b, down, x, y };
}
static int tev_cmp(const void *a, const void *b) {
    const double d = ((const TestEv *)a)->t - ((const TestEv *)b)->t;
    return d < 0 ? -1 : d > 0;
}
static void test_remote_load(void) {
    const char *e = getenv("FLAT_TEST_REMOTE");
    if (!e || !*e) return;
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", e);
    for (char *step = strtok(buf, ";"); step; step = strtok(NULL, ";")) {
        double t = 0, dur = 0.25;
        char what[16] = "";
        float dx = 0, dy = 0;
        const int n = sscanf(step, " %lf %15s %f %f %lf", &t, what, &dx, &dy, &dur);
        if (n < 2) continue;
        if (!strcmp(what, "padstart") || !strcmp(what, "padshoot")) {   // a stand-in pad (no virtual pads on tvOS)
            test_add(t, 2, !strcmp(what, "padshoot"), 1, dx, dy);
            if (!strcmp(what, "padshoot")) test_add(t + 0.1, 2, 1, 0, dx, dy);   // (the trigger held a few frames)
            continue;
        }
        if (!strcmp(what, "swipe")) {   // lands at -d/2, goes to +d/2
            for (int i = 0; i <= 8; i++)
                test_add(t + dur * i / 8, 1, 0, 1, -dx / 2 + dx * i / 8, -dy / 2 + dy * i / 8);
            test_add(t + dur + 0.02, 1, 0, 0, 0, 0);
            continue;
        }
        int b = -1;
        double hold = 0.08;
        for (int i = 0; i < RB_COUNT; i++) if (!strcmp(what, RB_NAME[i])) b = i;
        if (!strcmp(what, "playhold")) { b = RB_PLAY; hold = 0.8; }
        if (b < 0) { LOGE("FLAT_TEST_REMOTE: what is %s?", what); continue; }
        test_add(t, 0, b, 1, 0, 0);
        test_add(t + hold, 0, b, 0, 0, 0);
    }
    qsort(tev, (size_t)tev_n, sizeof *tev, tev_cmp);
    R.test = tev_n > 0;
    LOGI("FLAT_TEST_REMOTE: %d events", tev_n);
}
static void test_remote(double now) {
    if (!tev_t0) { tev_t0 = now; test_remote_load(); }
    for (; tev_i < tev_n && now - tev_t0 >= tev[tev_i].t; tev_i++) {
        const TestEv *e = &tev[tev_i];
        if (e->kind == 2) {   // the stand-in pad: Start, or a shot at X Y (0..1 of the picture)
            const int dev = FLAT_DEV_PAD + 15;
            int vx, vy, vw, vh;
            flat_view_rect(&vx, &vy, &vw, &vh);
            if (!e->b) { flat_dev_start(dev, 1); flat_dev_start(dev, 0); }
            else if (!e->down) flat_dev_trigger(dev, 0);
            else { flat_dev_pointer(dev, vx + (int)(e->x * vw), vy + (int)(e->y * vh)); flat_dev_trigger(dev, 1); }
            if (e->down) LOGI("tv: stand-in pad %s: player %d", e->b ? "shoots" : "Start", flat_dev_player(dev) + 1);
        } else if (e->kind) remote_touch(e->down, e->x, e->y, now);
        else remote_button(e->b, e->down, SRC_TEST, now);
    }
}

// ---------------------------------------------------------------- pads (GameController: apple_pads.h)
// Each pad a device; a pad of the remote's player moves the remote's sight (one sight for player 1, whichever moves
// it), player 2's pad its own. In the menu its buttons act as the remote's (UIKit sees them too there: counted once).
enum { PB_MENU = RB_COUNT, PB_OPT, PB_LB, PB_RB };
static void pad_frame(double dt, double now) {
    apple_pads_each(^(GCExtendedGamepad *g, ApplePad *p) {
        int cur[16] = { 0 };
        cur[RB_CLICK] = g.buttonA.pressed;
        cur[RB_MENU] = g.buttonB.pressed;
        cur[RB_PLAY] = g.buttonOptions.pressed;   // (Options in the menu: back to the game)
        cur[RB_UP] = g.dpad.up.pressed || g.leftThumbstick.yAxis.value > 0.5f;
        cur[RB_DOWN] = g.dpad.down.pressed || g.leftThumbstick.yAxis.value < -0.5f;
        cur[RB_LEFT] = g.dpad.left.pressed || g.leftThumbstick.xAxis.value < -0.5f;
        cur[RB_RIGHT] = g.dpad.right.pressed || g.leftThumbstick.xAxis.value > 0.5f;
        cur[PB_MENU] = g.buttonMenu.pressed; cur[PB_OPT] = g.buttonOptions.pressed;
        cur[PB_LB] = g.leftShoulder.pressed; cur[PB_RB] = g.rightShoulder.pressed;
        int pressed[16];
        for (int i = 0; i < 16; i++) pressed[i] = cur[i] && !p->prev[i];
        const int open = flat_menu_is_open() && !A.upload;
        for (int b = 0; b < RB_COUNT; b++) {
            if (pressed[b] && open) remote_button(b, 1, SRC_PAD, now);
            if (!cur[b] && p->prev[b] && R.owner[b] == SRC_PAD) remote_button(b, 0, SRC_PAD, now);
        }
        memcpy(p->prev, cur, sizeof p->prev);
        if (open) {
            apple_pad_release(p);
            if (pressed[PB_RB]) flat_menu_key(FLAT_KEY_TAB);
            if (pressed[PB_LB]) flat_menu_key(FLAT_KEY_TAB_BACK);
        } else if (!A.upload) {
            if (pressed[PB_OPT]) { flat_menu_set_open(1); return; }
            int vx, vy, vw, vh;
            const int shared = flat_dev_player(p->dev) == flat_dev_player(FLAT_DEV_REMOTE);
            if (shared && !sight_place(&vx, &vy, &vw, &vh)) return;
            apple_pad_game(g, p, dt, shared ? &A.sx : &p->sx, shared ? &A.sy : &p->sy, -1, pressed[RB_CLICK] || pressed[PB_MENU]);
        }
    });
}

// ---------------------------------------------------------------- UIKit's presses (a keyboard, the Simulator's remote)
static int press_button(UIPress *p) {
    switch (p.type) {
    case UIPressTypeSelect: return RB_CLICK;
    case UIPressTypeMenu: return RB_MENU;
    case UIPressTypePlayPause: return RB_PLAY;
    case UIPressTypeUpArrow: return RB_UP;
    case UIPressTypeDownArrow: return RB_DOWN;
    case UIPressTypeLeftArrow: return RB_LEFT;
    case UIPressTypeRightArrow: return RB_RIGHT;
    default: break;
    }
    switch (p.key.keyCode) {   // a keyboard's keys that aren't one of those
    case UIKeyboardHIDUsageKeyboardReturnOrEnter: case UIKeyboardHIDUsageKeypadEnter: return RB_CLICK;
    case UIKeyboardHIDUsageKeyboardEscape: return RB_MENU;
    case UIKeyboardHIDUsageKeyboardSpacebar: return RB_PLAY;
    default: return -1;
    }
}

// ---------------------------------------------------------------- the perf line (the Apple TV HD's headroom)
static const char *thermal_name(void) {
    switch (NSProcessInfo.processInfo.thermalState) {
    case NSProcessInfoThermalStateNominal: return "nominal";
    case NSProcessInfoThermalStateFair: return "fair";
    case NSProcessInfoThermalStateSerious: return "serious";
    case NSProcessInfoThermalStateCritical: return "critical";
    }
    return "?";
}
static void perf_frame(double start, double drawn, double refresh) {   // after a frame: begun, drawn (before the swap)
    const double end = now_s(), ms = (end - start) * 1000;
    A.draw_ms += (drawn - start) * 1000;
    static double prev;
    if (prev) {
        const double gap = start - prev;
        if (gap > A.gap_max) A.gap_max = gap;
        if (refresh > 0 && gap > refresh * 1.5) A.perf_late++;
    }
    prev = start;
    A.perf_frames++;
    A.frame_ms += ms;
    if (ms > A.frame_max) A.frame_max = ms;
    if (!A.perf_at) A.perf_at = start;
    if (end - A.perf_at < 10) return;
    LOGI("tv: perf: %.1f fps (refresh %.0f Hz), %d late, longest gap %.1f ms; main thread %.2f ms a frame (most %.1f; "
         "%.2f drawing, the rest the swap's wait); %dx%d; thermal %s", A.perf_frames / (end - A.perf_at),
         refresh > 0 ? 1 / refresh : 0, A.perf_late, A.gap_max * 1000, A.frame_ms / A.perf_frames, A.frame_max,
         A.draw_ms / A.perf_frames, A.w, A.h, thermal_name());
    A.perf_at = end;
    A.perf_frames = A.perf_late = 0;
    A.frame_ms = A.frame_max = A.gap_max = A.draw_ms = 0;
}

@implementation HotdView
+ (Class)layerClass { return [CAMetalLayer class]; }
@end

// ---------------------------------------------------------------- the view controller
static NSMutableSet *passed_presses;   // presses UIKit keeps (the Menu that goes to the Home screen)

@implementation HotdController
- (void)loadView {
    HotdView *v = [[HotdView alloc] init];
    v.backgroundColor = UIColor.blackColor;
    self.view = v;
    self.controllerUserInteractionEnabled = NO;   // the remote's and pads' presses are the game's
}
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    for (GCController *c in GCController.controllers) hook_remote(c);
    [NSNotificationCenter.defaultCenter addObserverForName:GCControllerDidConnectNotification object:nil queue:NSOperationQueue.mainQueue
                                                usingBlock:^(NSNotification *n) { hook_remote(n.object); }];
    if (A.started || A.upload) return;
    if (!has_rom() && !getenv("FLAT_NO_ROM_DIALOG")) {
        [self showUpload:tv_store_had_rom() ? @"tvOS removed the ROM set to free space on the Apple TV (your settings and scores are kept). Send it again."
                                             : nil];
        return;
    }
    [self startGame];
}
- (void)startGame {
    A.started = 1;
    tv_store_set_had_rom(has_rom());
    // the Apple TV HD (A8: Metal's Apple2 family, below Apple3) has the GPU of a 2014 phone: the game's texture decode
    // in each pixel (m2_gl.c) kept it at 15 fps for HD's 1440 x 1080 with 4x MSAA, 38 at 720 lines without, and 43 at
    // 540; the arcade's own size holds 60 (2026-11-20). There Picture starts as Original, and HD is 720 lines at most.
    id<MTLDevice> gpu = MTLCreateSystemDefaultDevice();
    const int small_gpu = gpu && ![gpu supportsFamily:MTLGPUFamilyApple3];
    if (small_gpu) flat_app_default(QOPT_PICTURE, QPICTURE_ORIGINAL);
    LOGI("tv: GPU %s%s", gpu ? gpu.name.UTF8String : "?", small_gpu ? " (Apple2: Picture Original by default)" : "");
    online_set_platform("APPLETV");
    if (!flat_app_start(A.files)) { close_with(@"The game could not start", @"See port.log in the app's folder."); return; }
    flat_menu_tv(1);
    CAMetalLayer *layer = (CAMetalLayer *)self.view.layer;
    layer.contentsScale = self.view.traitCollection.displayScale > 0 ? self.view.traitCollection.displayScale : 1;
    layer.drawableSize = CGSizeMake(self.view.bounds.size.width * layer.contentsScale, self.view.bounds.size.height * layer.contentsScale);
    if (!egl_init(layer) || !flat_gl_init()) { close_with(@"No graphics", @"OpenGL ES 3 through ANGLE did not start."); return; }
    // the HD picture at most 1080 lines (1440 x 1080, 4x MSAA), scaled up on a 4K TV: the whole 2160 lines with 4x
    // MSAA kept the Apple TV 4K (A15) at 40 to 48 fps, its GPU filling 25 million samples a frame (2026-11-20)
    if (small_gpu) flat_picture_limit(720, 1);
    else flat_picture_limit(1080, 0);
    {   // FLAT_TEST_PICTURE=lines,samples (tests): another limit
        int l, n;
        const char *e = getenv("FLAT_TEST_PICTURE");
        if (e && sscanf(e, "%d,%d", &l, &n) == 2) { flat_picture_limit(l, n); LOGI("tv: the picture at most %d lines, %d samples", l, n); }
    }
    { extern int port_crosshair_big; port_crosshair_big = 1; }   // (seen from a sofa: larger, outlined)
    self.link = [CADisplayLink displayLinkWithTarget:self selector:@selector(frame)];
    [self.link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}
- (void)frame {
    if (A.closing) return;
    if (A.quit) { tv_store_save(A.files); flat_app_quit(); exit(0); }
    if (flat_app_restart_requested()) {
        close_with(@"Open the app again", @"Online play starts (or ends) when the app opens again. Open House of the Dead VR again now.");
        return;
    }
    const double now = now_s();
    test_remote(now);
    remote_frame(now);
    pad_frame(A.last ? now - A.last : 0, now);
    A.last = now;
    self.controllerUserInteractionEnabled = top_level();   // (the top: Menu goes to UIKit, which leaves the app)
    {   // no screensaver while someone plays or online play is on: tvOS suspends an app under it, and online the
        // partner's game would wait (they may play while this player only watches); the attract scenes alone may have it.
        // (A player in the game by its state: in a scene too, where host_player_playing is 0.)
        const BOOL busy = online_session() || host_player_playing(0) || host_player_playing(1) ||
                          host_player_state(0) == HOST_P_IN || host_player_state(1) == HOST_P_IN;
        if (UIApplication.sharedApplication.idleTimerDisabled != busy) {
            UIApplication.sharedApplication.idleTimerDisabled = busy;
            LOGI("tv: screensaver %s", busy ? "held off (playing or online)" : "allowed");
        }
    }
    if (now - A.kept_at > 20) { A.kept_at = now; tv_store_save(A.files); }
    flat_app_frame(A.background || A.upload);
    if (A.background) return;
    CAMetalLayer *layer = (CAMetalLayer *)self.view.layer;
    const CGSize want = CGSizeMake(self.view.bounds.size.width * layer.contentsScale, self.view.bounds.size.height * layer.contentsScale);
    if (!CGSizeEqualToSize(want, layer.drawableSize)) layer.drawableSize = want;
    A.w = (int)layer.drawableSize.width;
    A.h = (int)layer.drawableSize.height;
    flat_draw(A.w, A.h, 0);
    const double drawn = now_s();
    eglSwapBuffers(A.dpy, A.surf);
    perf_frame(now, drawn, self.link.duration);
}

// UIKit's presses: what the remote's buttons do, but Menu at the top, which UIKit takes (the Home screen)
- (void)pressesBegan:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)e {
    if (!passed_presses) passed_presses = [NSMutableSet set];
    NSMutableSet *pass = [NSMutableSet set];
    for (UIPress *p in presses) {
        const int b = press_button(p);
        if (b < 0 || (b == RB_MENU && top_level()) || A.closing) { [pass addObject:p]; [passed_presses addObject:p]; continue; }
        remote_button(b, 1, SRC_UI, now_s());
    }
    if (pass.count) [super pressesBegan:pass withEvent:e];
}
- (void)lift:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)e ended:(BOOL)ended {
    NSMutableSet *pass = [NSMutableSet set];
    for (UIPress *p in presses) {
        if ([passed_presses containsObject:p]) { [pass addObject:p]; [passed_presses removeObject:p]; continue; }
        remote_button(press_button(p), 0, SRC_UI, now_s());
    }
    if (!pass.count) return;
    if (ended) [super pressesEnded:pass withEvent:e];
    else [super pressesCancelled:pass withEvent:e];
}
- (void)pressesEnded:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)e { [self lift:presses withEvent:e ended:YES]; }
- (void)pressesCancelled:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)e { [self lift:presses withEvent:e ended:NO]; }

// ---- the ROM page: this TV's address and its QR code, the set sent from a phone or computer (tv_upload.m)
static UILabel *label(UIView *in, CGFloat size, UIFontWeight weight, UIColor *color) {
    UILabel *l = [[UILabel alloc] init];
    l.font = [UIFont systemFontOfSize:size weight:weight];
    l.textColor = color;
    l.numberOfLines = 0;
    l.textAlignment = NSTextAlignmentCenter;
    l.translatesAutoresizingMaskIntoConstraints = NO;
    [in addSubview:l];
    return l;
}
static UIImage *qr_image(NSString *text) {
    CIFilter *f = [CIFilter filterWithName:@"CIQRCodeGenerator"];
    [f setValue:[text dataUsingEncoding:NSUTF8StringEncoding] forKey:@"inputMessage"];
    [f setValue:@"M" forKey:@"inputCorrectionLevel"];
    CIImage *img = [f.outputImage imageByApplyingTransform:CGAffineTransformMakeScale(12, 12)];
    if (!img) return nil;
    CGImageRef cg = [[CIContext contextWithOptions:nil] createCGImage:img fromRect:img.extent];
    UIImage *u = [UIImage imageWithCGImage:cg];
    CGImageRelease(cg);
    return u;
}
static void rom_arrived(const char *path) { (void)path; [the_controller romArrived]; }
- (void)showUpload:(NSString *)why {
    if (A.upload) return;
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/arcade", A.files);
    plat_mkdir(A.files);
    plat_mkdir(dir);
    if (!tv_upload_start(dir, rom_arrived)) { LOGE("tv: no socket for the ROM page"); }
    A.upload = 1;
    UIView *v = [[UIView alloc] initWithFrame:self.view.bounds];
    v.backgroundColor = [UIColor colorWithRed:0.047 green:0.075 blue:0.114 alpha:A.started ? 0.94 : 1];
    v.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    UIColor *text = [UIColor colorWithRed:0.93 green:0.95 blue:0.96 alpha:1], *muted = [UIColor colorWithRed:0.6 green:0.65 blue:0.7 alpha:1];
    UIColor *gold = [UIColor colorWithRed:1 green:0.79 blue:0.43 alpha:1];
    UILabel *title = label(v, 54, UIFontWeightBold, gold);
    title.text = @"Send the ROM set to this Apple TV";
    UILabel *how = label(v, 30, UIFontWeightRegular, text);
    how.text = @"On a phone or computer on the same network, scan the code or open this address,\nthen choose MAME's hotd set (Revision A): hotd.zip or hotd.7z.";
    self.uploadUrl = label(v, 46, UIFontWeightSemibold, [UIColor colorWithRed:0.21 green:0.72 blue:0.81 alpha:1]);
    self.uploadQr = [[UIImageView alloc] init];
    self.uploadQr.translatesAutoresizingMaskIntoConstraints = NO;
    self.uploadQr.layer.magnificationFilter = kCAFilterNearest;
    [v addSubview:self.uploadQr];
    self.uploadStatus = label(v, 30, UIFontWeightMedium, why ? gold : text);
    self.uploadStatus.text = why ?: @"Waiting for the file.";
    self.uploadHint = label(v, 24, UIFontWeightRegular, muted);
    self.uploadHint.text = A.started ? @"Menu: back to the game" : @"The file goes from your device to the Apple TV only, nowhere else.";
    UILayoutGuide *g = v.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [title.topAnchor constraintEqualToAnchor:g.topAnchor constant:40],
        [title.centerXAnchor constraintEqualToAnchor:v.centerXAnchor],
        [how.topAnchor constraintEqualToAnchor:title.bottomAnchor constant:24],
        [how.centerXAnchor constraintEqualToAnchor:v.centerXAnchor],
        [how.widthAnchor constraintLessThanOrEqualToAnchor:v.widthAnchor multiplier:0.85],
        [self.uploadQr.topAnchor constraintEqualToAnchor:how.bottomAnchor constant:36],
        [self.uploadQr.centerXAnchor constraintEqualToAnchor:v.centerXAnchor],
        [self.uploadQr.widthAnchor constraintEqualToConstant:400],
        [self.uploadQr.heightAnchor constraintEqualToConstant:400],
        [self.uploadUrl.topAnchor constraintEqualToAnchor:self.uploadQr.bottomAnchor constant:28],
        [self.uploadUrl.centerXAnchor constraintEqualToAnchor:v.centerXAnchor],
        [self.uploadStatus.topAnchor constraintEqualToAnchor:self.uploadUrl.bottomAnchor constant:24],
        [self.uploadStatus.centerXAnchor constraintEqualToAnchor:v.centerXAnchor],
        [self.uploadStatus.widthAnchor constraintLessThanOrEqualToAnchor:v.widthAnchor multiplier:0.8],
        [self.uploadHint.bottomAnchor constraintEqualToAnchor:g.bottomAnchor constant:-30],
        [self.uploadHint.centerXAnchor constraintEqualToAnchor:v.centerXAnchor],
    ]];
    self.uploadView = v;
    [self.view addSubview:v];
    self.shownUrl = nil;
    [self uploadTick];
    self.uploadTimer = [NSTimer scheduledTimerWithTimeInterval:0.25 target:self selector:@selector(uploadTick) userInfo:nil repeats:YES];
    self.controllerUserInteractionEnabled = top_level();
    LOGI("tv: asking for the ROM set");
}
- (void)uploadTick {
    char url[128];
    tv_upload_url(url, sizeof url);
    NSString *u = url[0] ? [NSString stringWithUTF8String:url] : @"";
    if (![u isEqualToString:self.shownUrl ?: @"-"]) {
        self.shownUrl = u;
        self.uploadUrl.text = u.length ? u : @"No network: connect the Apple TV to your Wi-Fi or Ethernet.";
        self.uploadQr.image = u.length ? qr_image(u) : nil;
        if (u.length) LOGI("tv: the ROM page is at %s", url);
    }
    long long got, total;
    if (tv_upload_progress(&got, &total))
        self.uploadStatus.text = [NSString stringWithFormat:@"Receiving: %lld of %lld MB (%d%%)", got >> 20, total >> 20, (int)(got * 100 / total)];
    else if (tv_upload_error()[0])
        self.uploadStatus.text = [NSString stringWithUTF8String:tv_upload_error()];
    if (!A.started) {   // (before the game: no display link; the test switches that need no game)
        self.controllerUserInteractionEnabled = top_level();
    }
}
- (void)romArrived {
    if (!A.upload) return;
    tv_upload_stop();
    [self.uploadTimer invalidate];
    [self.uploadView removeFromSuperview];
    A.upload = 0;
    tv_store_set_had_rom(1);
    if (!A.started) [self startGame];
    else close_with(@"Your ROM set is in", @"It loads when the app opens again. Open House of the Dead VR again now.");
}
@end

// ---------------------------------------------------------------- the app and its scene
@interface HotdScene : UIResponder <UIWindowSceneDelegate>
@property (nonatomic, strong) UIWindow *window;
@end
@implementation HotdScene
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)s options:(UISceneConnectionOptions *)o {
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    the_controller = [[HotdController alloc] init];
    self.window.rootViewController = the_controller;
    [self.window makeKeyAndVisible];
}
- (void)sceneDidEnterBackground:(UIScene *)scene {
    LOGI("tv: to the background (tvOS suspends the app: online, the partner waits)");
    A.background = 1; if (A.dpy) glFinish(); tv_store_save(A.files);
}
- (void)sceneWillEnterForeground:(UIScene *)scene { if (A.background) LOGI("tv: back in front"); A.background = 0; }
@end

@interface HotdApp : UIResponder <UIApplicationDelegate>
@end
@implementation HotdApp
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)o {
    // the files folder: in the caches (a tvOS app can't write Documents); what must last is kept in the defaults too
    NSString *caches = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject;
    snprintf(A.files, sizeof A.files, "%s/HOTDVR", caches.UTF8String);
    plat_mkdir(A.files);
    char log[1100];
    snprintf(log, sizeof log, "%s/port.log", A.files);
    if (!getenv("HOTD_LOG_STDERR")) { freopen(log, "w", stderr); setvbuf(stderr, NULL, _IONBF, 0); }
    tv_store_restore(A.files);
    // the game's sound; voice chat records only where the TV has an input (a headset through it)
    AVAudioSession *s = AVAudioSession.sharedInstance;
    if (!(s.isInputAvailable && [s setCategory:AVAudioSessionCategoryPlayAndRecord mode:AVAudioSessionModeDefault
                                       options:AVAudioSessionCategoryOptionMixWithOthers error:nil]))
        [s setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeDefault options:AVAudioSessionCategoryOptionMixWithOthers error:nil];
    [s setActive:YES error:nil];
    return YES;
}
- (UISceneConfiguration *)application:(UIApplication *)app configurationForConnectingSceneSession:(UISceneSession *)s
                              options:(UISceneConnectionOptions *)o {
    UISceneConfiguration *c = [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:s.role];
    c.delegateClass = [HotdScene class];
    return c;
}
@end

int main(int argc, char *argv[]) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass([HotdApp class])); }
}

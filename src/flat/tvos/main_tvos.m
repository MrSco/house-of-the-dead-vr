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
//   A / Menu coins and Start, Options the menu; in the menu the d-pad and A / B as the remote.
// - The ROM set comes from a phone or computer: the TV shows an address and its QR code, and a page served there sends
//   the file (tv_upload.m). The files folder is Library/Caches/HOTDVR (a tvOS app can't write Documents), which tvOS
//   may empty when short of space: the settings and saves are kept in the app's defaults too (tv_store.m), and a ROM
//   set that is gone is asked for again.
// - No microphone of its own: voice chat listens (apple.m), and talks only when tvOS reports an input.
// An app can't start itself again: where the app would restart (online play begins and ends so, a new ROM set), it
// says so and closes, and the player opens it again (online-session.cfg waits ten minutes).
//
// Test switch (env.txt, beside flat_app.c's): FLAT_TEST_REMOTE="seconds action [args]; ..." plays the remote through
// the same code as the real one: swipe DX DY [SECONDS] (the touch surface, -1..1, up is +), click, play, playhold,
// menu, up, down, left, right; the sight and each press are logged.
#import <GameController/GameController.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreImage/CoreImage.h>
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
    float sx, sy;                                      // the sight (window pixels), the remote's and the pads'
    int aim_on, pad_fire, pad_reload;
    double last, kept_at;
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
static void sight_move(float dx, float dy) {   // window pixels; a little past the picture's edges (that reloads)
    int vx, vy, vw, vh;
    if (!sight_place(&vx, &vy, &vw, &vh)) return;
    A.sx = fminf(fmaxf(A.sx + dx, vx - vw * 0.06f), vx + vw * 1.06f);
    A.sy = fminf(fmaxf(A.sy + dy, vy - vh * 0.06f), vy + vh * 1.06f);
    A.aim_on = 1;
    flat_pointer((int)A.sx, (int)A.sy);
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
} R;

// the top of the app: the ROM page before the game, or the menu with nothing open in it. Menu goes to the Home screen.
static int top_level(void) {
    if (A.upload) return !A.started;
    return flat_menu_is_open() && !qmenu_modal();
}

static void remote_touch(int down, float x, float y, double now) {
    if (!down) {
        if (R.touching && R.test && !flat_menu_is_open() && !A.upload) {
            int vx, vy, vw, vh;
            flat_view_rect(&vx, &vy, &vw, &vh);
            LOGI("tv: sight at %.0f,%.0f (the picture %d,%d %dx%d)", A.sx, A.sy, vx, vy, vw, vh);
        }
        R.touching = 0;
        return;
    }
    if (!R.touching) {   // it landed: the sight stays where it is (relative, as a mouse)
        R.touching = 1;
        R.tx = x; R.ty = y; R.tt = now;
        R.step_x = R.step_y = 0;
        return;
    }
    const float dx = x - R.tx, dy = y - R.ty;
    const double dt = now - R.tt > 1.0 / 240 ? now - R.tt : 1.0 / 240;
    R.tx = x; R.ty = y; R.tt = now;
    if (A.upload || A.closing) return;
    if (flat_menu_is_open()) {   // each third of the surface: one step of the focus
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
    const float v = hypotf(dx, dy) / (float)dt;   // surface units a second
    float accel = (v - 0.5f) / 3.5f;
    accel = 1 + 2 * (accel < 0 ? 0 : accel > 1 ? 1 : accel);
    const float k = (float)vw * 0.25f * accel * (float)flat_opt_get(QOPT_REMOTE_SPEED) / 100.0f;
    sight_move(dx * k, -dy * k);
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
        if (sight_place(&vx, &vy, &vw, &vh)) { A.aim_on = 1; flat_pointer((int)A.sx, (int)A.sy); }
        flat_trigger(down);
        break;
    }
    case RB_PLAY:   // reload at once; held: coins and Start (remote_frame)
        flat_reload(down);
        R.play_at = down ? now : 0;
        R.play_started = 0;
        break;
    case RB_MENU: if (down) flat_menu_set_open(1); break;
    default: break;   // (the arrows: nothing in the game)
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
    button_action(b, down, now);
}

static void remote_frame(double now) {
    if (R.play_at > 0 && !R.play_started && now - R.play_at >= 0.5 && !flat_menu_is_open()) {
        R.play_started = 1;
        flat_reload(0);
        flat_start(1);
        flat_start(0);
        if (R.test) LOGI("tv: play held: coins and Start");
    }
}

static void hook_remote(GCController *c) {
    if (c.extendedGamepad || !c.microGamepad) return;   // (a pad: pad_frame)
    GCMicroGamepad *m = c.microGamepad;
    m.reportsAbsoluteDpadValues = YES;   // the finger's place: the travel between two is the move
    m.allowsRotation = NO;
    m.dpad.valueChangedHandler = ^(GCControllerDirectionPad *d, float x, float y) { (void)d; remote_touch(x != 0 || y != 0, x, y, now_s()); };
    m.buttonA.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL p) { (void)b; (void)v; remote_button(RB_CLICK, p, SRC_GC, now_s()); };
    m.buttonX.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL p) { (void)b; (void)v; remote_button(RB_PLAY, p, SRC_GC, now_s()); };
    m.buttonMenu.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL p) { (void)b; (void)v; remote_button(RB_MENU, p, SRC_GC, now_s()); };
    LOGI("tv: remote %s", c.vendorName.UTF8String ?: "(unnamed)");
}

// ---------------------------------------------------------------- FLAT_TEST_REMOTE (tests)
typedef struct { double t; int kind, b, down; float x, y; } TestEv;   // kind 0: a button, 1: the touch surface
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
        if (e->kind) remote_touch(e->down, e->x, e->y, now);
        else remote_button(e->b, e->down, SRC_TEST, now);
    }
}

// ---------------------------------------------------------------- pads (GameController), as on iOS
static float dz(float v) { const float d = 0.2f; if (fabsf(v) < d) return 0; const float g = (fabsf(v) - d) / (1 - d); return (v < 0 ? -g : g) * g; }
static void pad_frame(double dt, double now) {
    GCExtendedGamepad *g = GCController.current.extendedGamepad;
    if (!g) for (GCController *c in GCController.controllers) if ((g = c.extendedGamepad)) break;
    if (!g) return;
    static int prev[RB_COUNT], prev_menu, prev_opt, prev_lb, prev_rb;
    // the buttons that act as the remote's in the menu (UIKit sees them too there: counted once)
    int cur[RB_COUNT] = { 0 };
    cur[RB_CLICK] = g.buttonA.pressed;
    cur[RB_MENU] = g.buttonB.pressed;
    cur[RB_PLAY] = g.buttonOptions.pressed;   // (Options in the menu: back to the game)
    cur[RB_UP] = g.dpad.up.pressed || g.leftThumbstick.yAxis.value > 0.5f;
    cur[RB_DOWN] = g.dpad.down.pressed || g.leftThumbstick.yAxis.value < -0.5f;
    cur[RB_LEFT] = g.dpad.left.pressed || g.leftThumbstick.xAxis.value < -0.5f;
    cur[RB_RIGHT] = g.dpad.right.pressed || g.leftThumbstick.xAxis.value > 0.5f;
    const int menu = g.buttonMenu.pressed, opt = g.buttonOptions.pressed;
    const int lb = g.leftShoulder.pressed, rb = g.rightShoulder.pressed;
    const int open = flat_menu_is_open() && !A.upload;
    for (int b = 0; b < RB_COUNT; b++) {
        if (cur[b] && !prev[b] && open) remote_button(b, 1, SRC_PAD, now);
        if (!cur[b] && prev[b] && R.owner[b] == SRC_PAD) remote_button(b, 0, SRC_PAD, now);
    }
    if (open) {
        if (rb && !prev_rb) flat_menu_key(FLAT_KEY_TAB);
        if (lb && !prev_lb) flat_menu_key(FLAT_KEY_TAB_BACK);
    } else if (!A.upload) {
        if (opt && !prev_opt) flat_menu_set_open(1);
        if ((cur[RB_CLICK] && !prev[RB_CLICK]) || (menu && !prev_menu)) { flat_start(1); flat_start(0); }
        int vx, vy, vw, vh;
        if (sight_place(&vx, &vy, &vw, &vh)) {
            const float lx = dz(g.leftThumbstick.xAxis.value), ly = dz(g.leftThumbstick.yAxis.value);
            const float rx = dz(g.rightThumbstick.xAxis.value), ry = dz(g.rightThumbstick.yAxis.value);
            const float mx = fabsf(rx) > fabsf(lx) ? rx : lx, my = fabsf(ry) > fabsf(ly) ? ry : ly;
            if (mx || my) sight_move(mx * vw * 2.0f * (float)dt, -my * vw * 2.0f * (float)dt);
            const int fire = g.rightTrigger.value > 0.4f || rb, rel = g.leftTrigger.value > 0.4f || lb || g.buttonX.pressed;
            if (fire != A.pad_fire) { A.aim_on = 1; flat_pointer((int)A.sx, (int)A.sy); flat_trigger(fire); A.pad_fire = fire; }
            if (rel != A.pad_reload) { flat_reload(rel); A.pad_reload = rel; }
        }
    }
    memcpy(prev, cur, sizeof prev);
    prev_menu = menu; prev_opt = opt; prev_lb = lb; prev_rb = rb;
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
    if (!flat_app_start(A.files)) { close_with(@"The game could not start", @"See port.log in the app's folder."); return; }
    flat_menu_tv(1);
    CAMetalLayer *layer = (CAMetalLayer *)self.view.layer;
    layer.contentsScale = self.view.traitCollection.displayScale > 0 ? self.view.traitCollection.displayScale : 1;
    layer.drawableSize = CGSizeMake(self.view.bounds.size.width * layer.contentsScale, self.view.bounds.size.height * layer.contentsScale);
    if (!egl_init(layer) || !flat_gl_init()) { close_with(@"No graphics", @"OpenGL ES 3 through ANGLE did not start."); return; }
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
    if (now - A.kept_at > 20) { A.kept_at = now; tv_store_save(A.files); }
    flat_app_frame(A.background || A.upload);
    if (A.background) return;
    CAMetalLayer *layer = (CAMetalLayer *)self.view.layer;
    const CGSize want = CGSizeMake(self.view.bounds.size.width * layer.contentsScale, self.view.bounds.size.height * layer.contentsScale);
    if (!CGSizeEqualToSize(want, layer.drawableSize)) layer.drawableSize = want;
    A.w = (int)layer.drawableSize.width;
    A.h = (int)layer.drawableSize.height;
    flat_draw(A.w, A.h, 0);
    eglSwapBuffers(A.dpy, A.surf);
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
- (void)sceneDidEnterBackground:(UIScene *)scene { A.background = 1; if (A.dpy) glFinish(); tv_store_save(A.files); }
- (void)sceneWillEnterForeground:(UIScene *)scene { A.background = 0; }
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

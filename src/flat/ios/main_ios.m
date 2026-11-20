// The flat app's iOS / iPadOS front end: a UIKit app (a scene, one view on a CAMetalLayer) drawing through ANGLE's
// OpenGL ES 3 on Metal, a frame each display refresh. Touch as on Android (flat_touch.c: a tap shoots there, a second
// finger reloads, RELOAD / START / MENU beside the picture); game pads (apple_pads.h: either stick moves a sight,
// R2 / R1 shoot, L2 / L1 / X reload, A / Menu coins and Start, Options the menu; a second player on another pad, or
// the touch screen while player 1 is on a pad). Documents is the files folder
// (arcade/ holds the ROM set; the Files app shows it), the ROM set chosen in the document picker at the first start.
// An iOS app cannot start itself again: where the app would restart (online play begins and ends so, a new ROM set),
// it says so and closes, and the player opens it again (online-session.cfg waits ten minutes).
#import <GameController/GameController.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreMotion/CoreMotion.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <math.h>
#include "flat/flat.h"
#include "flat/apple_pads.h"
#include "platform/gl.h"
#include "platform/platform.h"
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
    int started, background, quit, closing;
    EGLDisplay dpy;
    EGLContext ctx;
    EGLSurface surf;
    int w, h;
    // touch: the finger that aims, the one that reloads, the button each finger holds
    __unsafe_unretained UITouch *aim, *reload;
    __unsafe_unretained UITouch *held[4];   // by FLAT_BTN_*
    double last;
} A;

@interface HotdView : UIView
@end
static CMMotionManager *motion;
@interface HotdController : UIViewController <UIDocumentPickerDelegate>
@property (nonatomic, strong) CADisplayLink *link;
- (void)pickRom;
@end
static HotdController *the_controller;

// ---------------------------------------------------------------- flat.h's front-end calls
void flat_os_fullscreen(int on) { (void)on; }
int flat_os_touch(void) { return 1; }
void flat_os_crash_reports(const char *files, const int *opt) { (void)files; (void)opt; }   // (none here yet)
int flat_os_has_reports(void) { return 0; }
void flat_os_quit(void) { A.quit = 1; }
void flat_os_import_rom(void) { dispatch_async(dispatch_get_main_queue(), ^{ [the_controller pickRom]; }); }

// a message, then the app closes (iOS: no restart)
static void close_with(NSString *title, NSString *text) {
    if (A.closing) return;
    A.closing = 1;
    [the_controller.link invalidate];
    flat_app_quit();
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

// ---------------------------------------------------------------- touch
static void button(int b, int down) {
    flat_touch_pressed(b, down);
    if (b == FLAT_BTN_START) flat_dev_start(FLAT_DEV_TOUCH, down);
    else if (b == FLAT_BTN_RELOAD) flat_dev_reload(FLAT_DEV_TOUCH, down);
    else if (b == FLAT_BTN_MENU && down) { flat_menu_set_open(1); flat_touch_pressed(b, 0); A.held[b] = nil; }
}

@implementation HotdView
+ (Class)layerClass { return [CAMetalLayer class]; }
- (CGPoint)px:(UITouch *)t {   // view points -> drawable pixels
    CGPoint p = [t locationInView:self];
    const CGFloat k = self.layer.contentsScale;
    return CGPointMake(p.x * k, p.y * k);
}
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)e {
    for (UITouch *t in touches) {
        const CGPoint p = [self px:t];
        if (flat_menu_is_open()) { if (!A.aim) { A.aim = t; flat_menu_mouse((int)p.x, (int)p.y, 1); } continue; }
        const int b = flat_touch_button((int)p.x, (int)p.y);
        if (b) { A.held[b] = t; button(b, 1); continue; }
        if (!A.aim) { A.aim = t; if (!flat_gyro_on()) flat_dev_pointer(FLAT_DEV_TOUCH, (int)p.x, (int)p.y); flat_dev_trigger(FLAT_DEV_TOUCH, 1); }
        else if (!A.reload) { A.reload = t; flat_dev_reload(FLAT_DEV_TOUCH, 1); }
    }
}
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)e {
    for (UITouch *t in touches) {
        if (t != A.aim) continue;
        const CGPoint p = [self px:t];
        if (flat_menu_is_open()) flat_menu_mouse((int)p.x, (int)p.y, 1);
        else if (!flat_gyro_on()) flat_dev_pointer(FLAT_DEV_TOUCH, (int)p.x, (int)p.y);
    }
}
- (void)lift:(NSSet<UITouch *> *)touches {
    for (UITouch *t in touches) {
        const CGPoint p = [self px:t];
        if (t == A.aim) {
            A.aim = nil;
            if (flat_menu_is_open()) flat_menu_mouse((int)p.x, (int)p.y, 0);
            flat_dev_trigger(FLAT_DEV_TOUCH, 0);
        } else if (t == A.reload) { A.reload = nil; flat_dev_reload(FLAT_DEV_TOUCH, 0); }
        for (int b = 1; b < 4; b++) if (A.held[b] == t) { A.held[b] = nil; button(b, 0); }
    }
}
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)e { [self lift:touches]; }
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)e { [self lift:touches]; }
@end

// ---------------------------------------------------------------- tests: virtual pads (iOS 17's GCVirtualController)
// FLAT_TEST_PAD=n (1..4): n hidden virtual pads, each a GameController pad as a real one; FLAT_TEST_PAD_SCRIPT drives
// them as on Windows (pad_win.c: WHO@SECONDS:WHAT; A, RT, LT, RX+40, ...), K being the touch screen here (SPACE: its
// START; X;Y: a tap there, 0..1 of the picture). Each pad's player and sight are logged a second after each step.
static GCVirtualController *tp_vc[4];
static struct { int n; double start; struct { int who; double at; char what[16]; } step[64]; int nsteps, done[64], logged[64], let_go[64]; } TP = { .n = -1 };
static void test_pads(double now) {
    if (TP.n == -1) {
        const char *e = getenv("FLAT_TEST_PAD"), *sc = getenv("FLAT_TEST_PAD_SCRIPT");
        TP.n = e ? atoi(e) : 0;
        if (TP.n > 4) TP.n = 4;
        if (TP.n <= 0) { TP.n = 0; return; }
        if (@available(iOS 17.0, *)) {
            for (int k = 0; k < TP.n; k++) {
                GCVirtualControllerConfiguration *cfg = [[GCVirtualControllerConfiguration alloc] init];
                cfg.elements = [NSSet setWithObjects:GCInputLeftThumbstick, GCInputRightThumbstick, GCInputButtonA, GCInputButtonB,
                                GCInputButtonX, GCInputLeftTrigger, GCInputRightTrigger, nil];
                cfg.hidden = YES;
                tp_vc[k] = [[GCVirtualController alloc] initWithConfiguration:cfg];
                const int kk = k;
                [tp_vc[k] connectWithReplyHandler:^(NSError *err) { LOGI("test: virtual pad %d %s", kk + 1, err ? err.localizedDescription.UTF8String : "connected"); }];
            }
        } else { LOGI("test: virtual pads need iOS 17"); TP.n = 0; return; }
        for (const char *q = sc; q && *q && TP.nsteps < 64;) {
            char who[4] = "";
            int k = TP.nsteps;
            if (sscanf(q, "%3[^@]@%lf:%15[^,]", who, &TP.step[k].at, TP.step[k].what) == 3) {
                TP.step[k].who = who[0] == 'K' || who[0] == 'k' ? 0 : atoi(who);
                TP.nsteps++;
            }
            q = strchr(q, ',');
            if (q) q++;
        }
        TP.start = now;
        LOGI("test: %d virtual pads, %d steps", TP.n, TP.nsteps);
        return;
    }
    if (!TP.n) return;
    const double t = now - TP.start;
    if (@available(iOS 17.0, *)) {
        for (int k = 0; k < TP.n; k++) {
            float a = 0, rt = 0, lt = 0, sx[2] = { 0, 0 }, sy[2] = { 0, 0 };
            for (int i = 0; i < TP.nsteps; i++) {
                if (TP.step[i].who != k + 1) continue;
                const double d = t - TP.step[i].at;
                const char *w = TP.step[i].what;
                if (d < 0) continue;
                if (d < 0.2) {
                    if (!strcmp(w, "A") || !strcmp(w, "START")) a = 1;
                    else if (!strcmp(w, "RT")) rt = 1;
                    else if (!strcmp(w, "LT")) lt = 1;
                }
                if (d < 0.3 && (w[0] == 'L' || w[0] == 'R') && (w[1] == 'X' || w[1] == 'Y') && (w[2] == '+' || w[2] == '-')) {
                    const float v = (w[2] == '+' ? 1.f : -1.f) * (w[3] ? (float)atoi(w + 3) / 100.f : 1.f);
                    if (w[1] == 'X') sx[w[0] == 'R'] = v; else sy[w[0] == 'R'] = v;
                }
            }
            [tp_vc[k] setValue:a forButtonElement:GCInputButtonA];
            [tp_vc[k] setValue:rt forButtonElement:GCInputRightTrigger];
            [tp_vc[k] setValue:lt forButtonElement:GCInputLeftTrigger];
            [tp_vc[k] setPosition:CGPointMake(sx[0], sy[0]) forDirectionPadElement:GCInputLeftThumbstick];
            [tp_vc[k] setPosition:CGPointMake(sx[1], sy[1]) forDirectionPadElement:GCInputRightThumbstick];
        }
    }
    for (int i = 0; i < TP.nsteps; i++) {
        if (!TP.done[i] && t >= TP.step[i].at) {
            TP.done[i] = 1;
            LOGI("test: step %d@%.1f:%s", TP.step[i].who, TP.step[i].at, TP.step[i].what);
            if (!TP.step[i].who) {   // the touch screen
                float fx, fy;
                int vx, vy, vw, vh;
                flat_view_rect(&vx, &vy, &vw, &vh);
                if (!strcmp(TP.step[i].what, "SPACE")) { flat_dev_start(FLAT_DEV_TOUCH, 1); flat_dev_start(FLAT_DEV_TOUCH, 0); }
                else if (sscanf(TP.step[i].what, "%f;%f", &fx, &fy) == 2 && vw > 0) {
                    flat_dev_pointer(FLAT_DEV_TOUCH, vx + (int)(fx * vw), vy + (int)(fy * vh));
                    flat_dev_trigger(FLAT_DEV_TOUCH, 1);   // (let go a moment later, below)
                }
            }
        }
        if (!TP.step[i].who && !TP.let_go[i] && t >= TP.step[i].at + 0.1) {
            TP.let_go[i] = 1;
            if (strcmp(TP.step[i].what, "SPACE")) flat_dev_trigger(FLAT_DEV_TOUCH, 0);
        }
        if (!TP.logged[i] && t >= TP.step[i].at + 1) {
            TP.logged[i] = 1;
            char line[256];
            int n = 0;
            for (int c = 0; c < APPLE_PADS; c++)
                if (apple_pad_ctl[c])
                    n += snprintf(line + n, sizeof line - (size_t)n, " [pad %d: player %d, sight %.0f,%.0f]", apple_pad[c].dev - FLAT_DEV_PAD + 1,
                                  flat_dev_player(apple_pad[c].dev) + 1, apple_pad[c].sx, apple_pad[c].sy);
            line[n] = 0;
            LOGI("test: after %.1f s:%s; player 2 %s", TP.step[i].at, line, flat_player2_in() ? "in" : "out");
        }
    }
}

// ---------------------------------------------------------------- the view controller
@implementation HotdController
- (void)loadView {
    HotdView *v = [[HotdView alloc] init];
    v.multipleTouchEnabled = YES;
    v.backgroundColor = UIColor.blackColor;
    self.view = v;
}
- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    if (A.started) return;
    if (!has_rom() && !getenv("FLAT_NO_ROM_DIALOG")) { [self pickRom]; return; }
    [self startGame];
}
- (void)startGame {
    A.started = 1;
    online_set_platform(UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad ? "IPAD" : "IPHONE");
    if (!flat_app_start(A.files)) { close_with(@"The game could not start", @"See port.log in the app's folder (the Files app)."); return; }
    CAMetalLayer *layer = (CAMetalLayer *)self.view.layer;
    layer.contentsScale = self.view.window.screen.nativeScale;
    layer.drawableSize = CGSizeMake(self.view.bounds.size.width * layer.contentsScale, self.view.bounds.size.height * layer.contentsScale);
    if (!egl_init(layer) || !flat_gl_init()) { close_with(@"No graphics", @"OpenGL ES 3 through ANGLE did not start."); return; }
    flat_touch_controls(1);
    self.link = [CADisplayLink displayLinkWithTarget:self selector:@selector(frame)];
    [self.link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}
- (void)frame {
    if (A.closing) return;
    if (A.quit) { close_with(@"House of the Dead VR", @"Closing."); exit(0); }
    if (flat_app_restart_requested()) {
        close_with(@"Open the app again", @"Online play starts (or ends) when the app opens again. Open House of the Dead VR again now.");
        return;
    }
    const double now = (double)plat_now_ns() * 1e-9;
    test_pads(now);
    apple_pads_frame(A.last ? now - A.last : 0);
    {   // gyro aim: CoreMotion's rotation rate (the device's portrait axes) about the screen's, landscape either way
        if (!motion) { motion = [[CMMotionManager alloc] init]; motion.deviceMotionUpdateInterval = 1.0 / 120; }
        const BOOL want = flat_gyro_on() && !A.background && motion.deviceMotionAvailable;
        if (want && !motion.deviceMotionActive) [motion startDeviceMotionUpdates];
        if (!want && motion.deviceMotionActive) [motion stopDeviceMotionUpdates];
        if (want && motion.deviceMotion) {
            const CMRotationRate r = motion.deviceMotion.rotationRate;
            const BOOL left = self.view.window.windowScene.interfaceOrientation == UIInterfaceOrientationLandscapeLeft;
            // LandscapeRight: the device turned anticlockwise (its top to the left); LandscapeLeft: clockwise
            const float right = left ? (float)r.y : (float)-r.y, up = left ? (float)-r.x : (float)r.x;
            flat_gyro(right, up, A.last ? now - A.last : 0);
        }
    }
    A.last = now;
    flat_app_frame(A.background);
    if (A.background) return;
    CAMetalLayer *layer = (CAMetalLayer *)self.view.layer;
    const CGSize want = CGSizeMake(self.view.bounds.size.width * layer.contentsScale, self.view.bounds.size.height * layer.contentsScale);
    if (!CGSizeEqualToSize(want, layer.drawableSize)) layer.drawableSize = want;
    A.w = (int)layer.drawableSize.width;
    A.h = (int)layer.drawableSize.height;
    flat_draw(A.w, A.h, 0);
    eglSwapBuffers(A.dpy, A.surf);
}
- (void)pickRom {
    NSArray<UTType *> *types = @[ UTTypeZIP, [UTType typeWithFilenameExtension:@"7z"] ?: UTTypeData ];
    UIDocumentPickerViewController *p = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:types asCopy:YES];
    p.delegate = self;
    [self presentViewController:p animated:YES completion:nil];
}
- (void)documentPicker:(UIDocumentPickerViewController *)c didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    NSURL *u = urls.firstObject;
    if (!u) return;
    const BOOL sevenZip = [u.pathExtension.lowercaseString isEqualToString:@"7z"];
    NSString *dir = [NSString stringWithFormat:@"%s/arcade", A.files];
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *dst = [dir stringByAppendingPathComponent:sevenZip ? @"hotd.7z" : @"hotd.zip"];
    NSString *other = [dir stringByAppendingPathComponent:sevenZip ? @"hotd.zip" : @"hotd.7z"];
    [NSFileManager.defaultManager removeItemAtPath:dst error:nil];
    [NSFileManager.defaultManager removeItemAtPath:other error:nil];   // (the game takes hotd.zip first: one set only)
    NSError *err = nil;
    if (![NSFileManager.defaultManager copyItemAtURL:u toURL:[NSURL fileURLWithPath:dst] error:&err]) {
        LOGE("ROM set: %s", err.localizedDescription.UTF8String);
        return;
    }
    LOGI("ROM set: %s copied in", u.lastPathComponent.UTF8String);
    if (!A.started) [self startGame];
    else close_with(@"Your ROM set is in", @"It loads when the app opens again. Open House of the Dead VR again now.");
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)c {
    if (!A.started) [self startGame];   // (the game then says what it needs)
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
- (void)sceneDidEnterBackground:(UIScene *)scene { A.background = 1; if (A.dpy) glFinish(); }
- (void)sceneWillEnterForeground:(UIScene *)scene { A.background = 0; }
@end

@interface HotdApp : UIResponder <UIApplicationDelegate>
@end
@implementation HotdApp
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)o {
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    snprintf(A.files, sizeof A.files, "%s", docs.UTF8String);
    char log[1100];
    snprintf(log, sizeof log, "%s/port.log", A.files);
    if (!getenv("HOTD_LOG_STDERR")) { freopen(log, "w", stderr); setvbuf(stderr, NULL, _IONBF, 0); }
    // the game's sound and voice chat: play and record, the speaker by default, Bluetooth headsets too
    AVAudioSession *s = AVAudioSession.sharedInstance;
    [s setCategory:AVAudioSessionCategoryPlayAndRecord mode:AVAudioSessionModeDefault
           options:AVAudioSessionCategoryOptionDefaultToSpeaker | AVAudioSessionCategoryOptionAllowBluetoothHFP | AVAudioSessionCategoryOptionMixWithOthers
             error:nil];
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

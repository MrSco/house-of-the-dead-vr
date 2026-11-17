// The flat app's iOS / iPadOS front end: a UIKit app (a scene, one view on a CAMetalLayer) drawing through ANGLE's
// OpenGL ES 3 on Metal, a frame each display refresh. Touch as on Android (flat_touch.c: a tap shoots there, a second
// finger reloads, RELOAD / START / MENU beside the picture); game pads (GameController: either stick moves a sight,
// R2 / R1 shoot, L2 / L1 / X reload, A / Menu coins and Start, Options the menu). Documents is the files folder
// (arcade/ holds the ROM set; the Files app shows it), the ROM set chosen in the document picker at the first start.
// An iOS app cannot start itself again: where the app would restart (online play begins and ends so, a new ROM set),
// it says so and closes, and the player opens it again (online-session.cfg waits ten minutes).
#import <GameController/GameController.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <AVFoundation/AVFoundation.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <math.h>
#include "flat/flat.h"
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
    // pads
    float sx, sy;
    int pad_on, pad_fire, pad_reload;
    double last;
} A = { .sx = -1 };

@interface HotdView : UIView
@end
@interface HotdController : UIViewController <UIDocumentPickerDelegate>
@property (nonatomic, strong) CADisplayLink *link;
- (void)pickRom;
@end
static HotdController *the_controller;

// ---------------------------------------------------------------- flat.h's front-end calls
void flat_os_fullscreen(int on) { (void)on; }
int flat_os_touch(void) { return 1; }
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
    if (b == FLAT_BTN_START) flat_start(down);
    else if (b == FLAT_BTN_RELOAD) flat_reload(down);
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
        if (!A.aim) { A.aim = t; flat_pointer((int)p.x, (int)p.y); flat_trigger(1); }
        else if (!A.reload) { A.reload = t; flat_reload(1); }
    }
}
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)e {
    for (UITouch *t in touches) {
        if (t != A.aim) continue;
        const CGPoint p = [self px:t];
        if (flat_menu_is_open()) flat_menu_mouse((int)p.x, (int)p.y, 1);
        else flat_pointer((int)p.x, (int)p.y);
    }
}
- (void)lift:(NSSet<UITouch *> *)touches {
    for (UITouch *t in touches) {
        const CGPoint p = [self px:t];
        if (t == A.aim) {
            A.aim = nil;
            if (flat_menu_is_open()) flat_menu_mouse((int)p.x, (int)p.y, 0);
            flat_trigger(0);
        } else if (t == A.reload) { A.reload = nil; flat_reload(0); }
        for (int b = 1; b < 4; b++) if (A.held[b] == t) { A.held[b] = nil; button(b, 0); }
    }
}
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)e { [self lift:touches]; }
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)e { [self lift:touches]; }
@end

// ---------------------------------------------------------------- pads (GameController)
static float dz(float v) { const float d = 0.2f; if (fabsf(v) < d) return 0; const float g = (fabsf(v) - d) / (1 - d); return (v < 0 ? -g : g) * g; }
static void pad_frame(double dt) {
    GCController *c = GCController.current ?: GCController.controllers.firstObject;
    GCExtendedGamepad *g = c.extendedGamepad;
    if (!g) return;
    static int prev_a, prev_menu, prev_opt, prev_b, prev_lb, prev_rb, prev_up, prev_down, prev_left, prev_right;
    const int a = g.buttonA.pressed, menu = g.buttonMenu.pressed, opt = g.buttonOptions.pressed, b = g.buttonB.pressed;
    const int lb = g.leftShoulder.pressed, rb = g.rightShoulder.pressed;
    const int up = g.dpad.up.pressed || g.leftThumbstick.yAxis.value > 0.5f, down = g.dpad.down.pressed || g.leftThumbstick.yAxis.value < -0.5f;
    const int left = g.dpad.left.pressed || g.leftThumbstick.xAxis.value < -0.5f, right = g.dpad.right.pressed || g.leftThumbstick.xAxis.value > 0.5f;
    if (flat_menu_is_open()) {
        if (a && !prev_a) flat_menu_key(FLAT_KEY_ENTER);
        if ((b && !prev_b) || (opt && !prev_opt)) flat_menu_key(FLAT_KEY_BACK);
        if (rb && !prev_rb) flat_menu_key(FLAT_KEY_TAB);
        if (lb && !prev_lb) flat_menu_key(FLAT_KEY_TAB_BACK);
        if (up && !prev_up) flat_menu_key(FLAT_KEY_UP);
        if (down && !prev_down) flat_menu_key(FLAT_KEY_DOWN);
        if (left && !prev_left) flat_menu_key(FLAT_KEY_LEFT);
        if (right && !prev_right) flat_menu_key(FLAT_KEY_RIGHT);
    } else {
        if (opt && !prev_opt) flat_menu_set_open(1);
        if ((a && !prev_a) || (menu && !prev_menu)) { flat_start(1); flat_start(0); }
        int vx, vy, vw, vh;
        flat_view_rect(&vx, &vy, &vw, &vh);
        if (vw > 0) {
            if (A.sx < 0) { A.sx = vx + vw * 0.5f; A.sy = vy + vh * 0.5f; }
            const float lx = dz(g.leftThumbstick.xAxis.value), ly = dz(g.leftThumbstick.yAxis.value);
            const float rx = dz(g.rightThumbstick.xAxis.value), ry = dz(g.rightThumbstick.yAxis.value);
            const float mx = fabsf(rx) > fabsf(lx) ? rx : lx, my = fabsf(ry) > fabsf(ly) ? ry : ly;
            if (mx || my) {
                A.pad_on = 1;
                A.sx = fminf(fmaxf(A.sx + mx * vw * 2.0f * (float)dt, vx - vw * 0.05f), vx + vw * 1.05f);
                A.sy = fminf(fmaxf(A.sy - my * vw * 2.0f * (float)dt, vy - vh * 0.05f), vy + vh * 1.05f);
            }
            if (A.pad_on) flat_pointer((int)A.sx, (int)A.sy);
            const int fire = g.rightTrigger.value > 0.4f || rb, rel = g.leftTrigger.value > 0.4f || lb || g.buttonX.pressed;
            if (fire != A.pad_fire) { A.pad_on = 1; flat_pointer((int)A.sx, (int)A.sy); flat_trigger(fire); A.pad_fire = fire; }
            if (rel != A.pad_reload) { flat_reload(rel); A.pad_reload = rel; }
        }
    }
    prev_a = a; prev_menu = menu; prev_opt = opt; prev_b = b; prev_lb = lb; prev_rb = rb;
    prev_up = up; prev_down = down; prev_left = left; prev_right = right;
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
    pad_frame(A.last ? now - A.last : 0);
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

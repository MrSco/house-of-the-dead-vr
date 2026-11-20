// The flat app's macOS front end (House of the Dead VR.app): a window whose view is a CAMetalLayer, drawn through
// ANGLE's OpenGL ES 3 on Metal, a frame each display refresh. The mouse is the gun (light guns are mice), Space or
// Return give coins and Start, Esc opens the menu, Ctrl+Cmd+F or F11 full screen, T talks (push to talk); game pads
// through GameController as on iOS. The files folder is ~/Library/Application Support/House of the Dead VR (arcade/
// holds the ROM set, chosen in an Open panel at the first start). Online play's restart opens the app again.
#import <AppKit/AppKit.h>
// (CoreHaptics before AVFoundation: its engine header names AVAudioSession, which macOS marks unavailable once
// AVFoundation has declared it; first, it is only a forward declaration. apple_pads.h's rumble needs it.)
#import <CoreHaptics/CoreHaptics.h>
#import <AVFoundation/AVFoundation.h>
#import <GameController/GameController.h>
#import <QuartzCore/CAMetalLayer.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <math.h>
#include "flat/flat.h"
#include "flat/apple_pads.h"
#include "platform/gl.h"
#include "platform/platform.h"
#include "online.h"
#include "quest_options.h"

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
    EGLDisplay dpy;
    EGLContext ctx;
    EGLSurface surf;
    int quit, restart, mouse_down, cursor_hidden;
    double last;
} A;

@interface HotdMacView : NSView
@end
@interface HotdMacApp : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property (nonatomic, strong) NSWindow *window;
@property (nonatomic, strong) HotdMacView *view;
@property (nonatomic, strong) NSTimer *timer;
@end
static HotdMacApp *the_app;

// ---------------------------------------------------------------- flat.h's front-end calls
void flat_os_fullscreen(int on) {
    dispatch_async(dispatch_get_main_queue(), ^{
        const BOOL is = (the_app.window.styleMask & NSWindowStyleMaskFullScreen) != 0;
        if (is != (on != 0)) [the_app.window toggleFullScreen:nil];
    });
}
int flat_os_touch(void) { return 0; }
void crash_apple_keep_log(const char *files);   // platform/apple/crash_apple.m: the last run's port.log kept
void crash_apple_start(const char *files, const int *opt);   // platform/apple/crash_apple.m
void flat_os_crash_reports(const char *files, const int *opt) { crash_apple_start(files, opt); }
int flat_os_has_reports(void) { return 1; }
void flat_os_rumble(int dev, int what, float low, float high, int ms) { (void)what; apple_pad_rumble(dev, low, high, ms); }
void flat_os_trigger(int dev, int right, int mode, float amp, int ms) { apple_pad_trigger(dev, right, mode, amp, ms); }   // (pads only)
void flat_os_quit(void) { A.quit = 1; }
static int pick_rom(void) {
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.title = @"Your ROM set of The House of the Dead (MAME's hotd, Revision A)";
    p.allowedFileTypes = @[ @"zip", @"7z" ];
    p.canChooseDirectories = NO;
    if ([p runModal] != NSModalResponseOK || !p.URL) return 0;
    const BOOL sevenZip = [p.URL.pathExtension.lowercaseString isEqualToString:@"7z"];
    NSString *dir = [NSString stringWithFormat:@"%s/arcade", A.files];
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *dst = [dir stringByAppendingPathComponent:sevenZip ? @"hotd.7z" : @"hotd.zip"];
    [NSFileManager.defaultManager removeItemAtPath:dst error:nil];
    [NSFileManager.defaultManager removeItemAtPath:[dir stringByAppendingPathComponent:sevenZip ? @"hotd.zip" : @"hotd.7z"] error:nil];
    NSError *err = nil;
    if (![NSFileManager.defaultManager copyItemAtURL:p.URL toURL:[NSURL fileURLWithPath:dst] error:&err]) {
        LOGE("ROM set: %s", err.localizedDescription.UTF8String);
        return 0;
    }
    LOGI("ROM set: %s copied in", p.URL.lastPathComponent.UTF8String);
    return 1;
}
void flat_os_import_rom(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (the_app.window.styleMask & NSWindowStyleMaskFullScreen) [the_app.window toggleFullScreen:nil];
        if (pick_rom()) A.restart = 1;   // (the new set's images are made at the next start)
    });
}

static int has_rom(void) {
    char p[1100];
    snprintf(p, sizeof p, "%s/arcade/hotd.zip", A.files);
    if (plat_exists(p)) return 1;
    snprintf(p, sizeof p, "%s/arcade/hotd.7z", A.files);
    return plat_exists(p);
}

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
    A.surf = eglCreateWindowSurface(A.dpy, cfg, (EGLNativeWindowType)(__bridge void *)layer, NULL);
    if (A.ctx == EGL_NO_CONTEXT || A.surf == EGL_NO_SURFACE || !eglMakeCurrent(A.dpy, A.surf, A.surf, A.ctx)) {
        LOGE("ANGLE: no context or surface");
        return 0;
    }
    eglSwapInterval(A.dpy, 1);
    LOGI("ANGLE EGL %d.%d", maj, min);
    return 1;
}

// ---------------------------------------------------------------- the view: mouse and keys
@implementation HotdMacView
- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer *)makeBackingLayer { return [CAMetalLayer layer]; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }
- (NSPoint)px:(NSEvent *)e {   // window points -> drawable pixels, y down
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    const CGFloat k = self.window.backingScaleFactor;
    return NSMakePoint(p.x * k, (self.bounds.size.height - p.y) * k);
}
- (void)updateTrackingAreas {
    for (NSTrackingArea *t in self.trackingAreas) [self removeTrackingArea:t];
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:self.bounds
        options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
        owner:self userInfo:nil]];
}
- (void)cursorAt:(NSPoint)p {   // the system pointer hidden over the picture (the game's sight shows)
    const int hide = flat_pointer_in_view((int)p.x, (int)p.y) && !flat_menu_is_open() && flat_opt_get(QOPT_CROSSHAIR);
    if (hide != A.cursor_hidden) { if (hide) [NSCursor hide]; else [NSCursor unhide]; A.cursor_hidden = hide; }
}
- (void)mouseExited:(NSEvent *)e { if (A.cursor_hidden) { [NSCursor unhide]; A.cursor_hidden = 0; } }
- (void)moved:(NSEvent *)e {
    const NSPoint p = [self px:e];
    apple_pads_pointer_moved(FLAT_DEV_KBM);
    [self cursorAt:p];
    flat_menu_mouse((int)p.x, (int)p.y, A.mouse_down);
    if (!flat_menu_is_open()) flat_dev_pointer(FLAT_DEV_KBM, (int)p.x, (int)p.y);
}
- (void)mouseMoved:(NSEvent *)e { [self moved:e]; }
- (void)mouseDragged:(NSEvent *)e { [self moved:e]; }
- (void)mouseDown:(NSEvent *)e {
    A.mouse_down = 1;
    const NSPoint p = [self px:e];
    flat_menu_mouse((int)p.x, (int)p.y, 1);
    if (!flat_menu_is_open()) { flat_dev_pointer(FLAT_DEV_KBM, (int)p.x, (int)p.y); flat_dev_trigger(FLAT_DEV_KBM, 1); }
}
- (void)mouseUp:(NSEvent *)e {
    A.mouse_down = 0;
    const NSPoint p = [self px:e];
    flat_menu_mouse((int)p.x, (int)p.y, 0);
    flat_dev_trigger(FLAT_DEV_KBM, 0);
}
- (void)rightMouseDown:(NSEvent *)e { if (!flat_menu_is_open()) flat_dev_reload(FLAT_DEV_KBM, 1); }
- (void)rightMouseUp:(NSEvent *)e { flat_dev_reload(FLAT_DEV_KBM, 0); }
- (void)key:(NSEvent *)e down:(int)down {
    const unsigned short k = e.keyCode;   // (kVK_*: Return 36, Tab 48, Space 49, Esc 53, arrows 123-126, F11 103, T 17)
    if (down && k == 103) { [self.window toggleFullScreen:nil]; return; }
    if (flat_menu_is_open()) {
        if (!down) return;
        switch (k) {
        case 126: flat_menu_key(FLAT_KEY_UP); break;
        case 125: flat_menu_key(FLAT_KEY_DOWN); break;
        case 123: flat_menu_key(FLAT_KEY_LEFT); break;
        case 124: flat_menu_key(FLAT_KEY_RIGHT); break;
        case 36: case 49: if (!e.ARepeat) flat_menu_key(FLAT_KEY_ENTER); break;
        case 53: if (!e.ARepeat) flat_menu_key(FLAT_KEY_BACK); break;
        case 48: flat_menu_key((e.modifierFlags & NSEventModifierFlagShift) ? FLAT_KEY_TAB_BACK : FLAT_KEY_TAB); break;
        }
        return;
    }
    if (k == 36 || k == 49) { if (!(down && e.ARepeat)) flat_dev_start(FLAT_DEV_KBM, down); return; }
    if (k == 17) { online_talk_key(down != 0); return; }
    if (down && k == 53 && !e.ARepeat) flat_menu_set_open(1);
}
- (void)keyDown:(NSEvent *)e { [self key:e down:1]; }
- (void)keyUp:(NSEvent *)e { [self key:e down:0]; }
@end

// ---------------------------------------------------------------- the app
@implementation HotdMacApp
- (void)applicationDidFinishLaunching:(NSNotification *)n {
    NSString *support = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject;
    NSString *dir = [support stringByAppendingPathComponent:@"House of the Dead VR"];
    const char *over = getenv("HOTD_FILES");   // (tests: another folder, as HOTDVR.exe's --files)
    snprintf(A.files, sizeof A.files, "%s", over && *over ? over : dir.UTF8String);
    [NSFileManager.defaultManager createDirectoryAtPath:[NSString stringWithUTF8String:A.files] withIntermediateDirectories:YES attributes:nil error:nil];
    char log[1100];
    snprintf(log, sizeof log, "%s/port.log", A.files);
    crash_apple_keep_log(A.files);   // (for a crash report of the last run)
    freopen(log, "w", stderr);
    setvbuf(stderr, NULL, _IONBF, 0);
    // 1280 x 960 of picture (4:3)
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1280, 960)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"House of the Dead VR";
    self.window.delegate = self;
    self.window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    self.window.acceptsMouseMovedEvents = YES;
    self.view = [[HotdMacView alloc] initWithFrame:self.window.contentView.bounds];
    self.view.wantsLayer = YES;
    self.window.contentView = self.view;
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:self.view];
    [NSApp activateIgnoringOtherApps:YES];
    if (!has_rom() && !getenv("FLAT_NO_ROM_DIALOG")) {
        NSAlert *al = [[NSAlert alloc] init];
        al.messageText = @"Load your ROM set";
        al.informativeText = @"This app holds none of The House of the Dead: it plays the 1996 arcade game from your own MAME ROM set "
                             @"(hotd, Revision A: hotd.zip or hotd.7z). Choose it in the next window; it is copied into the app's folder.";
        [al runModal];
        pick_rom();
    }
    online_set_platform("MAC");
    if (!flat_app_start(A.files)) { [NSApp terminate:nil]; return; }
    CAMetalLayer *layer = (CAMetalLayer *)self.view.layer;
    layer.contentsScale = self.window.backingScaleFactor;
    layer.drawableSize = CGSizeMake(self.view.bounds.size.width * layer.contentsScale, self.view.bounds.size.height * layer.contentsScale);
    if (!egl_init(layer) || !flat_gl_init()) { [NSApp terminate:nil]; return; }
    if (flat_opt_get(QOPT_FULLSCREEN)) [self.window toggleFullScreen:nil];
    // a frame each display refresh (the swap waits for it); a timer, not the view's display link, which stops while
    // the screen sleeps or is locked (and with it the game's own clockwork: quitting, the menu's timers)
    const double hz = self.window.screen.maximumFramesPerSecond > 0 ? (double)self.window.screen.maximumFramesPerSecond : 60.0;
    self.timer = [NSTimer timerWithTimeInterval:1.0 / hz target:self selector:@selector(frame) userInfo:nil repeats:YES];
    [NSRunLoop.mainRunLoop addTimer:self.timer forMode:NSRunLoopCommonModes];
}
- (void)frame {
    if (A.quit || A.restart || flat_app_restart_requested()) { [self finish]; return; }
    const double now = (double)plat_now_ns() * 1e-9;
    apple_pads_frame(A.last ? now - A.last : 0);
    A.last = now;
    const int hidden = self.window.miniaturized || !self.window.visible;
    flat_app_frame(hidden);
    if (hidden) return;
    CAMetalLayer *layer = (CAMetalLayer *)self.view.layer;
    layer.contentsScale = self.window.backingScaleFactor;
    const CGSize want = CGSizeMake(self.view.bounds.size.width * layer.contentsScale, self.view.bounds.size.height * layer.contentsScale);
    if (!CGSizeEqualToSize(want, layer.drawableSize)) layer.drawableSize = want;
    flat_draw((int)want.width, (int)want.height, 0);
    eglSwapBuffers(A.dpy, A.surf);
}
- (void)finish {
    [self.timer invalidate];
    const int again = A.restart || flat_app_restart_requested();
    flat_app_quit();
    if (again) {   // online play begins (or ends), a new ROM set: the app again, a new copy of it
        NSWorkspaceOpenConfiguration *c = [NSWorkspaceOpenConfiguration configuration];
        c.createsNewApplicationInstance = YES;
        c.environment = NSProcessInfo.processInfo.environment;   // (HOTD_FILES, tests)
        [NSWorkspace.sharedWorkspace openApplicationAtURL:NSBundle.mainBundle.bundleURL configuration:c
                                        completionHandler:^(NSRunningApplication *r, NSError *e) { (void)r; (void)e; exit(0); }];
        return;
    }
    exit(0);
}
- (BOOL)windowShouldClose:(NSWindow *)w { A.quit = 1; return NO; }
- (void)windowDidEnterFullScreen:(NSNotification *)n { if (!flat_opt_get(QOPT_FULLSCREEN)) flat_opt_set(QOPT_FULLSCREEN, 1); }
- (void)windowDidExitFullScreen:(NSNotification *)n { if (flat_opt_get(QOPT_FULLSCREEN)) flat_opt_set(QOPT_FULLSCREEN, 0); }
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a { return YES; }
- (void)applicationWillTerminate:(NSNotification *)n { flat_app_quit(); }   // (Cmd+Q: the partner hears us go)
@end

int main(int argc, const char *argv[]) {
    (void)argc; (void)argv;
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        // a menu bar: the app's menu with Quit, and Full Screen
        NSMenu *bar = [[NSMenu alloc] init];
        NSMenuItem *item = [[NSMenuItem alloc] init];
        [bar addItem:item];
        NSMenu *m = [[NSMenu alloc] init];
        [m addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"].keyEquivalentModifierMask =
            NSEventModifierFlagControl | NSEventModifierFlagCommand;
        [m addItemWithTitle:@"Quit House of the Dead VR" action:@selector(terminate:) keyEquivalent:@"q"];
        item.submenu = m;
        app.mainMenu = bar;
        the_app = [[HotdMacApp alloc] init];
        app.delegate = the_app;
        [app run];
    }
    return 0;
}

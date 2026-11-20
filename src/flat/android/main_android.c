// The flat app's Android front end (phones and tablets, hotdvr-android.apk): a NativeActivity with an EGL / GL ES 3.0
// window, landscape. Touch: a tap on the picture shoots there, a second finger reloads, the buttons beside the
// picture (flat_touch.c) give START, RELOAD and the menu; Back opens the menu. Game pads: a sight moved by either
// stick, R2 / R1 shooting, L2 / L1 / X reloading, A or Start for coins and Start, Select / Back for the menu.
// The files folder is the app's (Android/data/<package>/files): arcade/ holds the ROM set, which ImportActivity
// (src/quest/java) asks for at the first start.
#include <android/input.h>
#include <android/keycodes.h>
#include <android/native_activity.h>
#include <android/sensor.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "flat/flat.h"
#include "platform/gl.h"
#include "platform/platform.h"
#include "platform/android/android.h"
#include "online.h"

#define LOGI(...) plat_log(PLAT_LOG_INFO, __VA_ARGS__)
#define LOGE(...) plat_log(PLAT_LOG_ERROR, __VA_ARGS__)

static struct {
    struct android_app *app;
    EGLDisplay dpy;
    EGLContext ctx;
    EGLSurface surf;
    EGLConfig cfg;
    int w, h, visible, gl_ready, quit;
    // touch: the finger that aims (its pointer id), and the button each finger holds
    int aim_id, aim_down, reload_down;
    int btn_of[10];
    // pads
    float lx, ly, rx, ry, l2, r2;
    float sx, sy;
    int pad_fire, pad_reload, pad_on;
    // the gyroscope (Gyro aim): its queue, and the display's rotation (0-3: Surface.ROTATION_*)
    ASensorManager *sensors;
    const ASensor *gyro;
    ASensorEventQueue *gyro_q;
    int gyro_live, rotation;
    float rate_right, rate_up;
} A = { .aim_id = -1, .sx = -1 };

// ---- flat.h's front-end calls
void flat_os_fullscreen(int on) { (void)on; }
void flat_os_quit(void) { A.quit = 1; }
int flat_os_touch(void) { return 1; }
void flat_os_crash_reports(const char *files, const int *opt) { (void)files; (void)opt; }   // (none here yet)
int flat_os_has_reports(void) { return 0; }
void flat_os_import_rom(void) { plat_android_start("com.mrsco.hotdvr.ImportActivity"); }   // (it restarts the app)

// ---------------------------------------------------------------- EGL
static int egl_init(void) {
    if (!A.dpy) {
        A.dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        eglInitialize(A.dpy, NULL, NULL);
        const EGLint attrs[] = { EGL_RENDERABLE_TYPE, 0x40 /* EGL_OPENGL_ES3_BIT */, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                                 EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
        EGLint n = 0;
        if (!eglChooseConfig(A.dpy, attrs, &A.cfg, 1, &n) || !n) { LOGE("no EGL config for GL ES 3"); return 0; }
        const EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
        A.ctx = eglCreateContext(A.dpy, A.cfg, EGL_NO_CONTEXT, ca);
        if (A.ctx == EGL_NO_CONTEXT) { LOGE("no GL ES 3 context"); return 0; }
    }
    A.surf = eglCreateWindowSurface(A.dpy, A.cfg, A.app->window, NULL);
    if (A.surf == EGL_NO_SURFACE || !eglMakeCurrent(A.dpy, A.surf, A.surf, A.ctx)) { LOGE("no EGL surface"); return 0; }
    eglSwapInterval(A.dpy, 1);
    eglQuerySurface(A.dpy, A.surf, EGL_WIDTH, &A.w);
    eglQuerySurface(A.dpy, A.surf, EGL_HEIGHT, &A.h);
    if (!A.gl_ready) {
        if (!flat_gl_init()) return 0;
        flat_touch_controls(1);
        A.gl_ready = 1;
    }
    LOGI("window %dx%d", A.w, A.h);
    return 1;
}
static void egl_lose_surface(void) {
    if (A.dpy && A.surf != EGL_NO_SURFACE) {
        eglMakeCurrent(A.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, A.ctx);   // (the context and its GL objects stay)
        eglDestroySurface(A.dpy, A.surf);
    }
    A.surf = EGL_NO_SURFACE;
}

// ---------------------------------------------------------------- input
static void button(int b, int down) {
    flat_touch_pressed(b, down);
    if (b == FLAT_BTN_START) flat_start(down);
    else if (b == FLAT_BTN_RELOAD) flat_reload(down);
    else if (b == FLAT_BTN_MENU && down) {   // (its finger then belongs to the menu: the button is let go here)
        flat_menu_set_open(1);
        flat_touch_pressed(b, 0);
        for (int i = 0; i < 10; i++) if (A.btn_of[i] == b) A.btn_of[i] = 0;
    }
}

static int32_t on_motion(AInputEvent *e) {
    const int32_t src = AInputEvent_getSource(e);
    if ((src & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {   // a pad's sticks and triggers
        A.lx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_X, 0);
        A.ly = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Y, 0);
        A.rx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Z, 0);
        A.ry = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RZ, 0);
        A.l2 = fmaxf(AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_LTRIGGER, 0), AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_BRAKE, 0));
        A.r2 = fmaxf(AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RTRIGGER, 0), AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_GAS, 0));
        return 1;
    }
    const int32_t act = AMotionEvent_getAction(e), kind = act & AMOTION_EVENT_ACTION_MASK;
    const size_t idx = (size_t)((act & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    const int id = AMotionEvent_getPointerId(e, idx) & 7;
    const int x = (int)AMotionEvent_getX(e, idx), y = (int)AMotionEvent_getY(e, idx);
    if (flat_menu_is_open()) {   // the menu: the first finger points and clicks
        if (kind == AMOTION_EVENT_ACTION_DOWN || kind == AMOTION_EVENT_ACTION_MOVE) flat_menu_mouse(x, y, 1);
        else if (kind == AMOTION_EVENT_ACTION_UP || kind == AMOTION_EVENT_ACTION_CANCEL) flat_menu_mouse(x, y, 0);
        if (A.aim_down) { flat_trigger(0); A.aim_down = 0; }
        A.aim_id = -1;
        return 1;
    }
    switch (kind) {
    case AMOTION_EVENT_ACTION_DOWN:
    case AMOTION_EVENT_ACTION_POINTER_DOWN: {
        const int b = flat_touch_button(x, y);
        if (b) { A.btn_of[id] = b; button(b, 1); break; }
        if (A.aim_id < 0) {      // the aiming finger: shoots where it touches (with gyro aim: at the sight)
            A.aim_id = id;
            if (!flat_gyro_on()) flat_pointer(x, y);
            flat_trigger(1);
            A.aim_down = 1;
        } else if (!A.reload_down) {   // a second finger: reload
            A.reload_down = 1;
            A.btn_of[id] = -1;
            flat_reload(1);
        }
        break;
    }
    case AMOTION_EVENT_ACTION_MOVE:
        for (size_t i = 0; i < AMotionEvent_getPointerCount(e); i++)
            if ((AMotionEvent_getPointerId(e, i) & 7) == A.aim_id && !flat_gyro_on())
                flat_pointer((int)AMotionEvent_getX(e, i), (int)AMotionEvent_getY(e, i));
        break;
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_POINTER_UP:
    case AMOTION_EVENT_ACTION_CANCEL:
        if (kind == AMOTION_EVENT_ACTION_CANCEL || kind == AMOTION_EVENT_ACTION_UP) {   // every finger is off
            for (int i = 0; i < 10; i++) if (A.btn_of[i] > 0) button(A.btn_of[i], 0);
            memset(A.btn_of, 0, sizeof A.btn_of);
            if (A.aim_down) flat_trigger(0);
            if (A.reload_down) flat_reload(0);
            A.aim_id = -1; A.aim_down = A.reload_down = 0;
            break;
        }
        if (A.btn_of[id] > 0) button(A.btn_of[id], 0);
        else if (A.btn_of[id] < 0) { flat_reload(0); A.reload_down = 0; }
        else if (id == A.aim_id) { flat_trigger(0); A.aim_down = 0; A.aim_id = -1; }
        A.btn_of[id] = 0;
        break;
    }
    return 1;
}

static int32_t on_key(AInputEvent *e) {
    const int32_t code = AKeyEvent_getKeyCode(e), down = AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN;
    const int repeat = AKeyEvent_getRepeatCount(e) > 0;
    if (code == AKEYCODE_VOLUME_UP || code == AKEYCODE_VOLUME_DOWN || code == AKEYCODE_VOLUME_MUTE) return 0;   // the system's
    if (flat_menu_is_open()) {
        if (!down) return 1;
        switch (code) {
        case AKEYCODE_DPAD_UP: flat_menu_key(FLAT_KEY_UP); break;
        case AKEYCODE_DPAD_DOWN: flat_menu_key(FLAT_KEY_DOWN); break;
        case AKEYCODE_DPAD_LEFT: flat_menu_key(FLAT_KEY_LEFT); break;
        case AKEYCODE_DPAD_RIGHT: flat_menu_key(FLAT_KEY_RIGHT); break;
        case AKEYCODE_BUTTON_A: case AKEYCODE_DPAD_CENTER: case AKEYCODE_ENTER: if (!repeat) flat_menu_key(FLAT_KEY_ENTER); break;
        case AKEYCODE_BACK: case AKEYCODE_BUTTON_B: case AKEYCODE_ESCAPE: case AKEYCODE_BUTTON_SELECT:
            if (!repeat) flat_menu_key(FLAT_KEY_BACK);
            break;
        case AKEYCODE_BUTTON_R1: flat_menu_key(FLAT_KEY_TAB); break;
        case AKEYCODE_BUTTON_L1: flat_menu_key(FLAT_KEY_TAB_BACK); break;
        }
        return 1;
    }
    switch (code) {
    case AKEYCODE_BACK: case AKEYCODE_ESCAPE: case AKEYCODE_BUTTON_SELECT: case AKEYCODE_MENU:
        if (down && !repeat) flat_menu_set_open(1);
        return 1;
    case AKEYCODE_BUTTON_A: case AKEYCODE_BUTTON_START: case AKEYCODE_SPACE: case AKEYCODE_ENTER:
        if (!repeat) flat_start(down);
        return 1;
    case AKEYCODE_BUTTON_R1: A.pad_on = 1; if (!repeat) { flat_pointer((int)A.sx, (int)A.sy); flat_trigger(down); } return 1;
    case AKEYCODE_BUTTON_L1: case AKEYCODE_BUTTON_X: if (!repeat) flat_reload(down); return 1;
    case AKEYCODE_T: online_talk_key(down != 0); return 1;
    }
    return 0;
}

static int32_t on_input(struct android_app *app, AInputEvent *e) {
    (void)app;
    if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_MOTION) return on_motion(e);
    if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_KEY) return on_key(e);
    return 0;
}

// the pad's sight, every frame
static float dead(float v) { const float d = 0.22f; if (fabsf(v) < d) return 0; const float g = (fabsf(v) - d) / (1 - d); return (v < 0 ? -g : g) * g; }
static void pad_frame(double dt) {
    if (flat_menu_is_open()) return;
    const float lx = dead(A.lx), ly = dead(A.ly), rx = dead(A.rx), ry = dead(A.ry);
    const float mx = fabsf(rx) > fabsf(lx) ? rx : lx, my = fabsf(ry) > fabsf(ly) ? ry : ly;
    int vx, vy, vw, vh;
    flat_view_rect(&vx, &vy, &vw, &vh);
    if (vw <= 0) return;
    if (A.sx < 0) { A.sx = vx + vw * 0.5f; A.sy = vy + vh * 0.5f; }
    if (mx || my) {
        A.pad_on = 1;
        A.sx += mx * vw * 2.0f * (float)dt;
        A.sy += my * vw * 2.0f * (float)dt;   // (Android's y axes point down)
        A.sx = fminf(fmaxf(A.sx, vx - vw * 0.05f), vx + vw * 1.05f);
        A.sy = fminf(fmaxf(A.sy, vy - vh * 0.05f), vy + vh * 1.05f);
    }
    if (A.pad_on) flat_pointer((int)A.sx, (int)A.sy);
    const int fire = A.r2 > 0.4f, reload = A.l2 > 0.4f;
    if (fire != A.pad_fire) { A.pad_on = 1; flat_pointer((int)A.sx, (int)A.sy); flat_trigger(fire); A.pad_fire = fire; }
    if (reload != A.pad_reload) { flat_reload(reload); A.pad_reload = reload; }
}

// ---------------------------------------------------------------- gyro aim
// The gyroscope's rates are about the device's own axes (its natural, portrait, orientation: x right, y up, z out of
// the screen); the sight wants them about the screen's as it is turned (the display's rotation).
static void gyro_drain(void);
static void gyro_service(double dt) {
    if (!A.sensors) {
        A.sensors = ASensorManager_getInstanceForPackage("com.mrsco.hotdvr.android");
        A.gyro = A.sensors ? ASensorManager_getDefaultSensor(A.sensors, ASENSOR_TYPE_GYROSCOPE) : NULL;
        // (its own looper id: the main loop drains it when it says so, else ALooper_pollOnce would keep returning)
        if (A.gyro) A.gyro_q = ASensorManager_createEventQueue(A.sensors, ALooper_forThread(), LOOPER_ID_USER, NULL, NULL);
        plat_log(PLAT_LOG_INFO, "gyro: %s", A.gyro ? ASensor_getName(A.gyro) : "none");
    }
    if (!A.gyro_q) return;
    const int want = flat_gyro_on() && A.visible;
    if (want != A.gyro_live) {   // (on only while it aims: it costs battery)
        if (want) { ASensorEventQueue_enableSensor(A.gyro_q, A.gyro); ASensorEventQueue_setEventRate(A.gyro_q, A.gyro, 10000); }
        else ASensorEventQueue_disableSensor(A.gyro_q, A.gyro);
        A.gyro_live = want;
        A.rotation = plat_android_display_rotation();
    }
    gyro_drain();
    if (A.gyro_live) flat_gyro(A.rate_right, A.rate_up, dt);
    else A.rate_right = A.rate_up = 0;
}
// the gyroscope's events waiting: the newest rates, in the screen's axes
static void gyro_drain(void) {
    if (!A.gyro_q) return;
    ASensorEvent ev[16];
    ssize_t n;
    while ((n = ASensorEventQueue_getEvents(A.gyro_q, ev, 16)) > 0)
        for (ssize_t i = 0; i < n; i++) {
            if (ev[i].type != ASENSOR_TYPE_GYROSCOPE) continue;
            const float x = ev[i].vector.x, y = ev[i].vector.y;
            switch (A.rotation) {   // device axes -> the screen's right and up
            case 1: A.rate_right = -y; A.rate_up = x; break;
            case 2: A.rate_right = -x; A.rate_up = -y; break;
            case 3: A.rate_right = y; A.rate_up = -x; break;
            default: A.rate_right = x; A.rate_up = y; break;
            }
        }
}

// ---------------------------------------------------------------- the activity
static void on_cmd(struct android_app *app, int32_t cmd) {
    switch (cmd) {
    case APP_CMD_INIT_WINDOW: if (app->window && !egl_init()) A.quit = 1; break;
    case APP_CMD_TERM_WINDOW: egl_lose_surface(); break;
    case APP_CMD_GAINED_FOCUS: case APP_CMD_RESUME: A.visible = 1; break;
    case APP_CMD_LOST_FOCUS: case APP_CMD_PAUSE: A.visible = 0; break;
    case APP_CMD_CONFIG_CHANGED: case APP_CMD_WINDOW_RESIZED:
        A.rotation = plat_android_display_rotation();   // (turned round: landscape the other way)
        if (A.surf != EGL_NO_SURFACE) { eglQuerySurface(A.dpy, A.surf, EGL_WIDTH, &A.w); eglQuerySurface(A.dpy, A.surf, EGL_HEIGHT, &A.h); }
        break;
    case APP_CMD_DESTROY: A.quit = 1; break;
    }
}

static int has_rom(const char *files) {
    char p[700];
    snprintf(p, sizeof p, "%s/arcade/hotd.zip", files);
    if (plat_exists(p)) return 1;
    snprintf(p, sizeof p, "%s/arcade/hotd.7z", files);
    return plat_exists(p);
}

void android_main(struct android_app *app) {
    A.app = app;
    app->onAppCmd = on_cmd;
    app->onInputEvent = on_input;
    ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_FULLSCREEN | AWINDOW_FLAG_KEEP_SCREEN_ON, 0);
    const char *d = app->activity->externalDataPath ? app->activity->externalDataPath : app->activity->internalDataPath;
    char files[600];
    snprintf(files, sizeof files, "%s", d ? d : "/sdcard/hotdvr");
    plat_android_set_activity(app->activity);   // (the microphone's permission, the Java activities)
    if (!has_rom(files)) flat_os_import_rom();  // the first start: your ROM set (ImportActivity starts the app again)
    online_set_platform("ANDROID");
    if (!flat_app_start(files)) { ANativeActivity_finish(app->activity); return; }
    if (online_session()) plat_android_multicast_lock();   // (a LAN host hears its partner's search only so)
    double last = 0;
    while (!A.quit && !flat_app_restart_requested()) {
        int events;
        struct android_poll_source *src;
        int id;
        while ((id = ALooper_pollOnce(A.visible && A.surf != EGL_NO_SURFACE ? 0 : 50, NULL, &events, (void **)&src)) >= 0) {
            if (src) src->process(app, src);
            if (id == LOOPER_ID_USER) gyro_drain();
            if (app->destroyRequested) A.quit = 1;
        }
        const double now = (double)plat_now_ns() * 1e-9;
        pad_frame(last ? now - last : 0);
        gyro_service(last ? now - last : 0);
        last = now;
        flat_app_frame(!A.visible || A.surf == EGL_NO_SURFACE);
        if (!A.visible || A.surf == EGL_NO_SURFACE || !A.gl_ready) continue;
        flat_draw(A.w, A.h, 0);
        eglSwapBuffers(A.dpy, A.surf);
    }
    const int restart = flat_app_restart_requested();
    if (restart) plat_android_start("com.mrsco.hotdvr.RestartActivity");   // (it waits for this process to end)
    flat_app_quit();
    LOGI("%s", restart ? "restarting" : "quitting");
    exit(0);
}

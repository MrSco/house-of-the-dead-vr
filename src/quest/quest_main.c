// The Quest app's host (xr.h): the VR shell (src/xr/xr_shell.c) on a NativeActivity (no Java but the small activities
// in src/quest/java), with EGL and OpenGL ES 3; the files folder is the app's external files dir
// (/sdcard/Android/data/<package>/files). What only Android has lives here: logcat for stdout and stderr, the crash
// handler (crash.txt, quest_report.c sends it), Wi-Fi's multicast lock, the updater (Updater.java), the restart
// (RestartActivity), the ROM set's import (ImportActivity), the OpenXR loader's JavaVM, the APK's assets.
#include <android_native_app_glue.h>
#include <android/asset_manager.h>
#include <android/log.h>
#include <jni.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "xr.h"
#include "quest.h"
#include "quest_report.h"
#include "online.h"
#include "platform/android/android.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <ucontext.h>

#define TAG "hotdvr"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static struct {
    struct android_app *app;
    char files[512];
    int resumed, finishing;
    // EGL
    EGLDisplay dpy; EGLConfig cfg; EGLContext ctx; EGLSurface pbuf;
    XrGraphicsBindingOpenGLESAndroidKHR binding;
    XrInstanceCreateInfoAndroidKHR create;
} Q;

// ---------------------------------------------------------------- stdout / stderr -> logcat
static int log_fd[2];
static void *log_reader(void *a) {
    (void)a;
    char line[2048];
    FILE *f = fdopen(log_fd[0], "r");
    while (f && fgets(line, sizeof line, f)) __android_log_write(ANDROID_LOG_INFO, "hotd", line);
    return NULL;
}
static void log_redirect(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (pipe(log_fd)) return;
    dup2(log_fd[1], 1);
    dup2(log_fd[1], 2);
    pthread_t t;
    pthread_create(&t, NULL, log_reader, NULL);
    pthread_detach(t);
}

// ---------------------------------------------------------------- crashes -> crash.txt
// A crash (a bad pointer, an abort) appends to crash.txt in the files folder: the signal, the faulting address and
// the call chain (pc, lr, then the frame pointers), each as library+offset, which llvm-symbolizer turns into source
// lines with build_quest's unstripped libhotdvr.so (the build-id line names it). Then the default action runs (the
// system's own report). The next start sends it as a crash report (quest_report.c) when Crash reports is on.
static int crash_fd = -1;
static char crash_build[64];
static void crash_put(const char *s) { if (crash_fd >= 0 && write(crash_fd, s, strlen(s)) < 0) {} }
static void crash_addr(const char *what, uintptr_t a) {
    char b[300];
    Dl_info di;
    if (a && dladdr((void *)a, &di) && di.dli_fname) {
        const char *f = strrchr(di.dli_fname, '/');
        snprintf(b, sizeof b, "  %s %s+0x%lx (%s)\n", what, f ? f + 1 : di.dli_fname, (unsigned long)(a - (uintptr_t)di.dli_fbase),
                 di.dli_sname ? di.dli_sname : "?");
    } else snprintf(b, sizeof b, "  %s %p\n", what, (void *)a);
    crash_put(b);
}
static void on_crash(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    char b[200];
    snprintf(b, sizeof b, "crash: signal %d code %d addr %p thread %d\n", sig, si->si_code, si->si_addr, gettid());
    crash_put(b);
    crash_put(crash_build);
#if defined(__aarch64__)
    crash_addr("pc", (uintptr_t)uc->uc_mcontext.pc);
    crash_addr("lr", (uintptr_t)uc->uc_mcontext.regs[30]);
    uintptr_t *fp = (uintptr_t *)uc->uc_mcontext.regs[29];
    for (int i = 0; i < 48 && fp && ((uintptr_t)fp & 7) == 0; i++) {
        uintptr_t next = fp[0], ret = fp[1];
        crash_addr("at", ret);
        if (next <= (uintptr_t)fp) break;
        fp = (uintptr_t *)next;
    }
#endif
    signal(sig, SIG_DFL);
    raise(sig);
}
static void crash_handler(const char *dir) {
    char p[600];
    snprintf(p, sizeof p, "%s/crash.txt", dir);
    crash_fd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0660);
    snprintf(crash_build, sizeof crash_build, "  build-id %s\n", qreport_build_id());
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    static const int sigs[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };
    for (int i = 0; i < 5; i++) sigaction(sigs[i], &sa, NULL);
}
static void mkdirs(const char *p) {
    char b[600];
    snprintf(b, sizeof b, "%s", p);
    for (char *s = b + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(b, 0775); *s = '/'; }
    mkdir(b, 0775);
}
// An online game hosted on the local network hears a partner's search (a broadcast) only once Wi-Fi's multicast
// filter is lifted (WifiManager.MulticastLock, held for the rest of the run)
static void multicast_lock(void) {
    ANativeActivity *a = Q.app->activity;
    JNIEnv *env;
    if ((*a->vm)->AttachCurrentThread(a->vm, &env, NULL) != JNI_OK) return;
    jclass ac = (*env)->GetObjectClass(env, a->clazz);
    jobject app = (*env)->CallObjectMethod(env, a->clazz, (*env)->GetMethodID(env, ac, "getApplicationContext", "()Landroid/content/Context;"));
    jobject wifi = app ? (*env)->CallObjectMethod(env, app, (*env)->GetMethodID(env, (*env)->GetObjectClass(env, app), "getSystemService",
                                                  "(Ljava/lang/String;)Ljava/lang/Object;"), (*env)->NewStringUTF(env, "wifi")) : NULL;
    if (wifi && !(*env)->ExceptionCheck(env)) {
        jclass wc = (*env)->GetObjectClass(env, wifi);
        jobject lk = (*env)->CallObjectMethod(env, wifi, (*env)->GetMethodID(env, wc, "createMulticastLock",
                                              "(Ljava/lang/String;)Landroid/net/wifi/WifiManager$MulticastLock;"), (*env)->NewStringUTF(env, "hotdvr-lan"));
        if (lk && !(*env)->ExceptionCheck(env)) {
            jclass lc = (*env)->GetObjectClass(env, lk);
            (*env)->CallVoidMethod(env, lk, (*env)->GetMethodID(env, lc, "setReferenceCounted", "(Z)V"), JNI_FALSE);
            (*env)->CallVoidMethod(env, lk, (*env)->GetMethodID(env, lc, "acquire", "()V"));
            (*env)->NewGlobalRef(env, lk);
        }
    }
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); LOGE("Wi-Fi multicast lock refused"); }
    else LOGI("Wi-Fi multicast lock held");
    (*a->vm)->DetachCurrentThread(a->vm);
}
// ---------------------------------------------------------------- updates (src/quest/java/.../Updater.java)
// The project's GitHub releases: checked once a launch and from the SYSTEM tab, which shows the status (polled while
// the menu is open) and starts the update (UpdateActivity, a 2D panel). The class through the app's class loader:
// FindClass from this thread sees only the system's.
static struct { jclass cls; jmethodID status, command; char line[512]; double polled; } UPD;
static int upd_env(JNIEnv **env) { return (*Q.app->activity->vm)->AttachCurrentThread(Q.app->activity->vm, env, NULL) == JNI_OK; }
static void upd_done(JNIEnv *env, const char *what) {
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); LOGE("updater: %s failed", what); }
    (*Q.app->activity->vm)->DetachCurrentThread(Q.app->activity->vm);
}
static void updater_command(const char *cmd) {
    JNIEnv *env;
    if (!UPD.cls || !upd_env(&env)) return;
    jstring s = (*env)->NewStringUTF(env, cmd);
    (*env)->CallStaticVoidMethod(env, UPD.cls, UPD.command, s);
    upd_done(env, cmd);
    UPD.polled = -1;   // (the status again at once)
    LOGI("updater: %s", cmd);
}
static void updater_poll(double now) {
    if (!UPD.cls || (now - UPD.polled < 0.3 && UPD.polled >= 0)) return;
    UPD.polled = now;
    JNIEnv *env;
    if (!upd_env(&env)) return;
    jstring s = (jstring)(*env)->CallStaticObjectMethod(env, UPD.cls, UPD.status);
    if (s && !(*env)->ExceptionCheck(env)) {
        const char *c = (*env)->GetStringUTFChars(env, s, NULL);
        if (c) { snprintf(UPD.line, sizeof UPD.line, "%s", c); (*env)->ReleaseStringUTFChars(env, s, c); }
    }
    upd_done(env, "status");
}
static void updater_start(void) {
    ANativeActivity *a = Q.app->activity;
    JNIEnv *env;
    if (!upd_env(&env)) return;
    jclass ac = (*env)->GetObjectClass(env, a->clazz);
    jobject loader = (*env)->CallObjectMethod(env, a->clazz, (*env)->GetMethodID(env, ac, "getClassLoader", "()Ljava/lang/ClassLoader;"));
    jclass lc = loader ? (*env)->GetObjectClass(env, loader) : NULL;
    jclass cls = lc ? (jclass)(*env)->CallObjectMethod(env, loader, (*env)->GetMethodID(env, lc, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;"),
                                                       (*env)->NewStringUTF(env, "com.mrsco.hotdvr.Updater")) : NULL;
    if (cls && !(*env)->ExceptionCheck(env)) {
        jmethodID init = (*env)->GetStaticMethodID(env, cls, "init", "(Landroid/content/Context;)V");
        UPD.status = (*env)->GetStaticMethodID(env, cls, "status", "()Ljava/lang/String;");
        UPD.command = (*env)->GetStaticMethodID(env, cls, "command", "(Ljava/lang/String;)V");
        if (init && UPD.status && UPD.command && !(*env)->ExceptionCheck(env)) {
            (*env)->CallStaticVoidMethod(env, cls, init, a->clazz);
            if (!(*env)->ExceptionCheck(env)) UPD.cls = (jclass)(*env)->NewGlobalRef(env, cls);
        }
    }
    upd_done(env, "start");
    if (UPD.cls) updater_command("check");   // one look a launch
}
// RestartActivity (src/quest/java), in a process of its own, waits for this one to end and opens the game again.
static void relaunch(void) {
    ANativeActivity *a = Q.app->activity;
    JNIEnv *env;
    if ((*a->vm)->AttachCurrentThread(a->vm, &env, NULL) != JNI_OK) return;
    jclass ic = (*env)->FindClass(env, "android/content/Intent"), ac = (*env)->GetObjectClass(env, a->clazz);
    jobject in = (*env)->NewObject(env, ic, (*env)->GetMethodID(env, ic, "<init>", "()V"));
    jstring pkg = (jstring)(*env)->CallObjectMethod(env, a->clazz, (*env)->GetMethodID(env, ac, "getPackageName", "()Ljava/lang/String;"));
    jstring cls = (*env)->NewStringUTF(env, "com.mrsco.hotdvr.RestartActivity"), key = (*env)->NewStringUTF(env, "pid");
    (*env)->CallObjectMethod(env, in, (*env)->GetMethodID(env, ic, "setClassName", "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;"), pkg, cls);
    (*env)->CallObjectMethod(env, in, (*env)->GetMethodID(env, ic, "putExtra", "(Ljava/lang/String;I)Landroid/content/Intent;"), key, (jint)getpid());
    (*env)->CallObjectMethod(env, in, (*env)->GetMethodID(env, ic, "addFlags", "(I)Landroid/content/Intent;"), (jint)0x10000000);   // NEW_TASK
    (*env)->CallVoidMethod(env, a->clazz, (*env)->GetMethodID(env, ac, "startActivity", "(Landroid/content/Intent;)V"), in);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); LOGE("restart: startActivity failed"); }
    else LOGI("restart: RestartActivity started");
    (*a->vm)->DetachCurrentThread(a->vm);
}
// ImportActivity (src/quest/java), in a process of its own: it lets you choose your ROM set, copies it, ends this
// process and starts the app again
int xr_os_import_rom(void) {
    ANativeActivity *a = Q.app->activity;
    JNIEnv *env;
    int ok = 1;
    if ((*a->vm)->AttachCurrentThread(a->vm, &env, NULL) != JNI_OK) return 0;
    jclass ic = (*env)->FindClass(env, "android/content/Intent"), ac = (*env)->GetObjectClass(env, a->clazz);
    jobject in = (*env)->NewObject(env, ic, (*env)->GetMethodID(env, ic, "<init>", "()V"));
    jstring pkg = (jstring)(*env)->CallObjectMethod(env, a->clazz, (*env)->GetMethodID(env, ac, "getPackageName", "()Ljava/lang/String;"));
    jstring cls = (*env)->NewStringUTF(env, "com.mrsco.hotdvr.ImportActivity"), key = (*env)->NewStringUTF(env, "pid");
    (*env)->CallObjectMethod(env, in, (*env)->GetMethodID(env, ic, "setClassName", "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;"), pkg, cls);
    (*env)->CallObjectMethod(env, in, (*env)->GetMethodID(env, ic, "putExtra", "(Ljava/lang/String;I)Landroid/content/Intent;"), key, (jint)getpid());
    (*env)->CallObjectMethod(env, in, (*env)->GetMethodID(env, ic, "addFlags", "(I)Landroid/content/Intent;"), (jint)0x10000000);   // NEW_TASK
    (*env)->CallVoidMethod(env, a->clazz, (*env)->GetMethodID(env, ac, "startActivity", "(Landroid/content/Intent;)V"), in);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); LOGE("import: startActivity failed"); ok = 0; }
    else LOGI("import: ImportActivity started");
    (*a->vm)->DetachCurrentThread(a->vm);
    return ok;
}

static int egl_init(void) {
    Q.dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(Q.dpy, NULL, NULL)) { LOGE("eglInitialize"); return 0; }
    const EGLint attr[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 0,
                            EGL_STENCIL_SIZE, 0, EGL_SAMPLES, 0, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
    EGLint n = 0;
    if (!eglChooseConfig(Q.dpy, attr, &Q.cfg, 1, &n) || !n) { LOGE("eglChooseConfig"); return 0; }
    const EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    Q.ctx = eglCreateContext(Q.dpy, Q.cfg, EGL_NO_CONTEXT, ca);
    const EGLint pa[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    Q.pbuf = eglCreatePbufferSurface(Q.dpy, Q.cfg, pa);
    if (!Q.ctx || !Q.pbuf || !eglMakeCurrent(Q.dpy, Q.pbuf, Q.pbuf, Q.ctx)) { LOGE("EGL context: %x", eglGetError()); return 0; }
    return 1;
}


// ---------------------------------------------------------------- xr.h's host
void xr_os_loader_init(void) {
    PFN_xrInitializeLoaderKHR init_loader = NULL;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&init_loader);
    if (init_loader) {
        XrLoaderInitInfoAndroidKHR li = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
        li.applicationVM = Q.app->activity->vm; li.applicationContext = Q.app->activity->clazz;
        XrResult r = init_loader((const XrLoaderInitInfoBaseHeaderKHR *)&li);
        if (XR_FAILED(r)) LOGE("xrInitializeLoaderKHR: %d", (int)r);
    }
}
const void *xr_os_instance_next(void) {
    Q.create = (XrInstanceCreateInfoAndroidKHR){ XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
    Q.create.applicationVM = Q.app->activity->vm; Q.create.applicationActivity = Q.app->activity->clazz;
    return &Q.create;
}
int xr_os_graphics_binding(XrInstance inst, XrSystemId sys, const void **binding) {
    PFN_xrGetOpenGLESGraphicsRequirementsKHR greq = NULL;
    xrGetInstanceProcAddr(inst, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&greq);
    XrGraphicsRequirementsOpenGLESKHR gr = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
    if (!greq || XR_FAILED(greq(inst, sys, &gr))) { LOGE("xrGetOpenGLESGraphicsRequirementsKHR failed"); return 0; }
    if (!egl_init()) return 0;
    Q.binding = (XrGraphicsBindingOpenGLESAndroidKHR){ XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
    Q.binding.display = Q.dpy; Q.binding.config = Q.cfg; Q.binding.context = Q.ctx;
    *binding = &Q.binding;
    return 1;
}
void xr_os_crash_reports(const char *files, const int *opt) { qreport_start(Q.app->activity, files, opt); }
void xr_os_online_started(void) { multicast_lock(); }
const char *xr_os_update_line(void) { return UPD.cls ? UPD.line : NULL; }
void xr_os_update_cmd(const char *cmd) { updater_command(cmd); }
void xr_os_update_poll(double now) { updater_poll(now); }
int xr_os_mirror(int *w, int *h) { (void)w; (void)h; return 0; }

// the APK's assets (src/quest/assets)
static AAssetManager *assets;
struct qasset { AAsset *a; };
qasset *qasset_open(const char *name) {
    AAsset *a = assets ? AAssetManager_open(assets, name, AASSET_MODE_BUFFER) : NULL;
    if (!a) return NULL;
    qasset *q = malloc(sizeof *q);
    if (!q) { AAsset_close(a); return NULL; }
    q->a = a;
    return q;
}
int qasset_read(qasset *q, void *p, size_t n) { return AAsset_read(q->a, p, n) == (int)n; }
const void *qasset_buffer(qasset *q, size_t *len) {
    *len = (size_t)AAsset_getLength(q->a);
    return AAsset_getBuffer(q->a);
}
void qasset_close(qasset *q) { if (q) { AAsset_close(q->a); free(q); } }

// ---------------------------------------------------------------- activity
static void on_cmd(struct android_app *app, int32_t cmd) {
    (void)app;
    if (cmd == APP_CMD_RESUME) Q.resumed = 1;
    else if (cmd == APP_CMD_PAUSE) Q.resumed = 0;
}

void android_main(struct android_app *app) {
    Q.app = app;
    assets = app->activity->assetManager;
    app->onAppCmd = on_cmd;
    log_redirect();
    {   // the files folder: made, gone into (port.log, PORT_SHOT pictures land here), crashes caught from now on
        const char *d = app->activity->externalDataPath ? app->activity->externalDataPath : app->activity->internalDataPath;
        snprintf(Q.files, sizeof Q.files, "%s", d ? d : "/sdcard/hotdvr");
        mkdirs(Q.files);
        if (chdir(Q.files)) LOGE("chdir %s: %s", Q.files, strerror(errno));
        crash_handler(Q.files);
    }
    plat_android_set_activity(app->activity);   // (the microphone's permission: platform/android/mic.c)
    // the Quest and the Steam Frame run this library: the package tells them apart (the files folder is the package's)
    online_set_platform(strstr(Q.files, ".frame/") ? "FRAME" : "QUEST");
    xr_setup(Q.files);
    updater_start();
    if (!xr_instance() || !xr_session() || !xr_gl_init()) {
        LOGE("OpenXR / GL setup failed");
        ANativeActivity_finish(app->activity);
        Q.finishing = 1;
    } else {
        xr_start();
    }
    while (!app->destroyRequested) {
        for (;;) {
            // block while the activity is paused; otherwise only drain what is there
            int timeout = (!Q.resumed && !xr_running()) ? -1 : xr_running() ? 0 : 10;
            struct android_poll_source *src = NULL;
            int events;
            if (ALooper_pollOnce(timeout, NULL, &events, (void **)&src) < 0) break;
            if (src) src->process(app, src);
            if (app->destroyRequested) break;
        }
        if (app->destroyRequested || !xr_runtime_name()[0]) break;   // (no OpenXR instance)
        xr_events();
        quest_audio_service();
        static int restarting;
        if (xr_restart_requested() && !Q.finishing) { relaunch(); restarting = 1; }
        if ((restarting || xr_should_exit()) && !Q.finishing) {
            LOGI("finishing");
            Q.finishing = 1;
            xr_end_session();
            ANativeActivity_finish(app->activity);
        }
        if (xr_running()) xr_frame();
    }
    LOGI("activity destroyed: exiting");
    xr_quit();
    fflush(NULL);
    _exit(0);
}

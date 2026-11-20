// Apple Vision Pro (docs/platforms.md, P5): the VR shell's host (src/xr/xr.h, built with XR_OPENXR 0), as
// src/quest/quest_main.c is the Quest's and src/xr/xr_win.c the PC's. The shell (src/xr/xr_shell.c: the game's world,
// the cabinet view, guns, hands, the menu, online play's poses) is every headset's; what is the Vision Pro's is here:
// ANGLE's GL context, Compositor Services' frames, ARKit's head, hands and Sense controllers' poses
// (VosControllers.swift has their buttons), each eye's picture into the drawable, the document picker
// (VosApp.swift's window), the app's files and assets, and the app closing.
//
// The compositor's thread does every display frame, as the Quest app's main loop does: Compositor Services' frame
// (its timing and drawable), ARKit's head, hands and Sense controllers at the frame's presentation time (an XrhInput),
// the shell's update (xr_host_frame), one GL pass per eye into a Metal texture of our own (xr_host_eye), then a Metal
// pass that copies each eye into the drawable and writes its depth, and the present. The game runs on its own thread
// (arcade.cpp), never touching GL.
//
// Found by GoldenEye VR's native visionOS build (its port/visionos: vos_egl.mm, vos_xr.mm), which found it from
// Project Midas and Klepton:
//  - ANGLE's GL has no window here: a pbuffer context, and Metal textures as GL textures (EGL_METAL_TEXTURE_ANGLE, on
//    ANGLE's own MTLDevice, shared storage); GL's work reaches Metal's queue through the MTLSharedEvent ANGLE signals
//    for an EGL_SYNC_METAL_SHARED_EVENT_ANGLE sync.
//  - visionOS throws away a frame whose depth is all 0 (reverse-Z infinity) as empty: each eye's depth is written at
//    2 m. (The simulator doesn't do this; the headset does.)
//  - ARKit's world tracking and its accessory tracking (the Sense controllers) in ONE session; the drawable stamped
//    with the device anchor its pictures were drawn from.
// Unlike GoldenEye's, each eye is drawn with its own real (canted) pose and field of view, so the copy is one to one.
//
// Colours: the eyes' textures are RGBA8 sRGB, so GL encodes as into the Quest's sRGB swapchains; Metal reads them
// back to linear and the drawable (bgra8Unorm_srgb) encodes again. Rows: GL's row 0 is its bottom, as Metal's
// memory row 0: the copy turns it over.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <CompositorServices/CompositorServices.h>
#import <ARKit/ARKit.h>
#import <AVFoundation/AVFoundation.h>
#import <GameController/GameController.h>
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include "xr.h"
#include "quest.h"
#include "vos_bridging.h"
#include "online.h"
#include "platform/platform.h"

#define LOGI(...) plat_log(PLAT_LOG_INFO, __VA_ARGS__)
#define LOGE(...) plat_log(PLAT_LOG_ERROR, __VA_ARGS__)

static struct {
    char files[1024], assets[1024];
    void *layer;                         // cp_layer_renderer_t, CFBridgingRetain'd
    int started;
    // the picker's answer, for the compositor's thread (0 none, 1 chosen, 2 cancelled)
    atomic_int picked;
    // QUEST_TEST_PICK's file (tests): answered as the picker does, at pick_at (CACurrentMediaTime) once it opened
    char pick_file[128];
    double pick_at;
} V;

// ================================================================ xr.h's host
// the app's models and sounds: the bundle's assets folder (project.yml copies src/quest/assets there)
struct qasset { uint8_t *data; size_t len, pos; };
qasset *qasset_open(const char *name) {
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", V.assets, name);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    qasset *a = calloc(1, sizeof *a);
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (a && n > 0 && (a->data = malloc((size_t)n)) && fread(a->data, 1, (size_t)n, f) == (size_t)n) a->len = (size_t)n;
    else if (a) { free(a->data); free(a); a = NULL; }
    fclose(f);
    return a;
}
int qasset_read(qasset *a, void *p, size_t n) {
    if (a->len - a->pos < n) return 0;
    memcpy(p, a->data + a->pos, n);
    a->pos += n;
    return 1;
}
const void *qasset_buffer(qasset *a, size_t *len) { *len = a->len; return a->data; }
void qasset_close(qasset *a) { if (a) { free(a->data); free(a); } }

void crash_apple_keep_log(const char *files);   // platform/apple/crash_apple.m: the last run's port.log kept
void crash_apple_start(const char *files, const int *opt);   // platform/apple/crash_apple.m
void xr_os_crash_reports(const char *files, const int *opt) { crash_apple_start(files, opt); }
void xr_os_online_started(void) {}
// the app updates through the App Store (TestFlight): no updater in the SYSTEM tab
const char *xr_os_update_line(void) { return NULL; }
void xr_os_update_cmd(const char *cmd) { (void)cmd; }
void xr_os_update_poll(double now) { (void)now; }
int xr_os_mirror(int *w, int *h) { (void)w; (void)h; return 0; }
void xr_os_haptic(int hand, float amplitude, int ms) { vosSenseHaptic(hand, amplitude, ms / 1000.0); }
// the document picker, in a window beside the space; its answer comes to the compositor's thread (V.picked)
int xr_os_import_rom(void) {
    dispatch_async(dispatch_get_main_queue(), ^{ vosPickerOpen("Choose your ROM set (hotd.zip or hotd.7z)"); });
    // QUEST_TEST_PICK=seconds,file (env.txt, tests; the simulator can't pinch): the picker answers with that file, in
    // Documents, 3 s after it opened, as when you choose it (the shell's switch opened it)
    const char *e = getenv("QUEST_TEST_PICK");
    if (e && strchr(e, ',') && !V.pick_at) {
        snprintf(V.pick_file, sizeof V.pick_file, "%s", strchr(e, ',') + 1);
        V.pick_at = CACurrentMediaTime() + 3;
    }
    return 1;
}
// the test's answer: a copy in the temporary folder, as the picker gives one
static void pick_test(void) {
    if (V.pick_at <= 0 || CACurrentMediaTime() < V.pick_at) return;
    V.pick_at = -1;                       // (once)
    char src[1200], dst[1200];
    static char buf[1 << 16];
    snprintf(src, sizeof src, "%s/%s", V.files, V.pick_file);
    const char *tmp = getenv("TMPDIR");
    snprintf(dst, sizeof dst, "%s/%s", tmp ? tmp : V.files, strrchr(V.pick_file, '/') ? strrchr(V.pick_file, '/') + 1 : V.pick_file);
    FILE *in = fopen(src, "rb"), *out = in ? fopen(dst, "wb") : NULL;
    size_t n;
    int ok = in && out;
    while (ok && (n = fread(buf, 1, sizeof buf, in)) > 0) ok = fwrite(buf, 1, n, out) == n;
    if (in) fclose(in);
    if (out && fclose(out)) ok = 0;
    LOGI("test: the picker chose %s (%s)", V.pick_file, ok ? "copied" : "could not copy");
    vosPickerDone(ok ? 1 : -1, ok ? dst : NULL);
}

// ================================================================ ANGLE: a GL context with no window
static EGLDisplay s_dpy = EGL_NO_DISPLAY;
static EGLContext s_ctx = EGL_NO_CONTEXT;
static EGLSurface s_surf = EGL_NO_SURFACE;
static id<MTLDevice> s_dev;

static int egl_init(void) {
    const EGLint dattr[] = { EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE, EGL_NONE };
    s_dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, (void *)EGL_DEFAULT_DISPLAY, dattr);
    EGLint maj = 0, min = 0;
    if (s_dpy == EGL_NO_DISPLAY || !eglInitialize(s_dpy, &maj, &min)) { LOGE("ANGLE: no Metal display (0x%x)", eglGetError()); return 0; }
    const EGLint cattr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                             EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(s_dpy, cattr, &cfg, 1, &n) || n < 1) { LOGE("ANGLE: no ES 3 pbuffer config"); return 0; }
    const EGLint xattr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    s_ctx = eglCreateContext(s_dpy, cfg, EGL_NO_CONTEXT, xattr);
    if (s_ctx == EGL_NO_CONTEXT) { LOGE("ANGLE: no ES 3 context (0x%x)", eglGetError()); return 0; }
    const EGLint sattr[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    s_surf = eglCreatePbufferSurface(s_dpy, cfg, sattr);
    if (s_surf == EGL_NO_SURFACE || !eglMakeCurrent(s_dpy, s_surf, s_surf, s_ctx)) { LOGE("ANGLE: no pbuffer (0x%x)", eglGetError()); return 0; }
    // ANGLE's own device: its EGL images refuse a Metal texture made on another
    EGLAttrib dev = 0, mtl = 0;
    if (eglQueryDisplayAttribEXT(s_dpy, EGL_DEVICE_EXT, &dev) && dev &&
        eglQueryDeviceAttribEXT((EGLDeviceEXT)dev, EGL_METAL_DEVICE_ANGLE, &mtl) && mtl)
        s_dev = (__bridge id<MTLDevice>)(void *)mtl;
    else
        s_dev = MTLCreateSystemDefaultDevice();
    LOGI("ANGLE EGL %d.%d, %s, %s, device %s", maj, min, (const char *)glGetString(GL_RENDERER),
         (const char *)glGetString(GL_VERSION), s_dev.name.UTF8String);
    return 1;
}

// a Metal texture as a GL texture of this context (GL_TEXTURE_2D); 0 on failure
static GLuint gl_from_metal(id<MTLTexture> t) {
    EGLImageKHR img = eglCreateImageKHR(s_dpy, EGL_NO_CONTEXT, EGL_METAL_TEXTURE_ANGLE, (__bridge EGLClientBuffer)t, NULL);
    if (img == EGL_NO_IMAGE_KHR) { LOGE("eglCreateImageKHR(EGL_METAL_TEXTURE_ANGLE) %lux%lu format %lu: 0x%x",
                                        (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.pixelFormat, eglGetError()); return 0; }
    while (glGetError() != GL_NO_ERROR) {}
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)img);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    const GLenum err = glGetError();
    if (err != GL_NO_ERROR) { LOGE("glEGLImageTargetTexture2DOES: 0x%x", err); glDeleteTextures(1, &tex); return 0; }
    return tex;   // (the image lives as long as the app: the eyes' textures are made once)
}

// glFlush, and the shared event ANGLE signals once the GL work so far has run (nil: glFinish ran instead)
static id<MTLSharedEvent> gl_fence(uint64_t *value) {
    EGLSync sync = eglCreateSync(s_dpy, EGL_SYNC_METAL_SHARED_EVENT_ANGLE, NULL);
    if (sync == EGL_NO_SYNC) {
        static int said;
        if (!said++) LOGE("no EGL_SYNC_METAL_SHARED_EVENT_ANGLE (0x%x): glFinish every frame", eglGetError());
        glFinish();
        return nil;
    }
    glFlush();   // (the sync is encoded with the frame's draws)
    void *evt = eglCopyMetalSharedEventANGLE(s_dpy, sync);
    EGLAttrib lo = 0, hi = 0;
    eglGetSyncAttrib(s_dpy, sync, EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_LO_ANGLE, &lo);
    eglGetSyncAttrib(s_dpy, sync, EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_HI_ANGLE, &hi);
    eglDestroySync(s_dpy, sync);
    if (!evt) { glFinish(); return nil; }
    *value = ((uint64_t)(uint32_t)hi << 32) | (uint32_t)lo;
    return (__bridge_transfer id<MTLSharedEvent>)evt;
}

// ================================================================ ARKit: the head, the hands, the Sense controllers
static ar_session_t s_ar;
static ar_world_tracking_provider_t s_world;
static ar_hand_tracking_provider_t s_hands;
static ar_accessory_tracking_provider_t s_acc;
static int s_world_ok, s_hands_on, s_acc_on;
static ar_device_anchor_t s_good_anchor;     // the last anchor the tracker answered (never an unanswered one)
static simd_float4x4 s_last_head;
static float s_h0 = -1;                      // LOCAL's height over ARKit's origin: the first head's
static NSLock *s_acc_lock;
static ar_accessory_anchor_t s_acc_anchor[2];

static void ar_run(void) {
    ar_data_providers_t p = ar_data_providers_create();
    if (s_world) ar_data_providers_add_data_provider(p, s_world);
    if (s_hands_on) ar_data_providers_add_data_provider(p, s_hands);
    if (s_acc_on) ar_data_providers_add_data_provider(p, s_acc);
    ar_session_run(s_ar, p);
}

// a provider tracking these controllers, its anchors kept by hand (0 left, 1 right)
static void acc_make(ar_accessories_t list) {
    ar_accessory_tracking_configuration_t cfg = ar_accessory_tracking_configuration_create();
    ar_accessory_tracking_configuration_set_accessories(cfg, list);
    s_acc = ar_accessory_tracking_provider_create(cfg);
    ar_accessory_tracking_provider_set_update_handler(s_acc, nil, ^(ar_accessory_anchors_t added, ar_accessory_anchors_t updated, ar_accessory_anchors_t removed) {
        void (^keep)(ar_accessory_anchors_t, bool) = ^(ar_accessory_anchors_t set, bool gone) {
            ar_accessory_anchors_enumerate_anchors(set, ^bool(ar_accessory_anchor_t a) {
                const int h = ar_accessory_get_inherent_chirality(ar_accessory_anchor_get_accessory(a)) == ar_accessory_chirality_left ? 0 : 1;
                [s_acc_lock lock];
                s_acc_anchor[h] = gone ? nil : a;
                [s_acc_lock unlock];
                return true;
            });
        };
        keep(added, false); keep(updated, false); keep(removed, true);
    });
}
// the controllers' list changed: visionOS 27 swaps it on the running provider; 26 needs a new provider
static void acc_set(ar_accessories_t list) {
    if (@available(visionOS 27.0, *)) {
        ar_accessory_tracking_provider_update_accessories(s_acc, list, ^(bool ok, ar_error_t e) {
            (void)e;
            LOGI("controllers: ARKit tracks %s", ok ? "them" : "none (the update failed)");
        });
    } else {
        acc_make(list);
        ar_run();
        LOGI("controllers: ARKit tracks them (a new provider)");
    }
}
// the controllers ARKit tracks: the spatial ones GameController has (set again as they come and go)
static void acc_reload(void) {
    if (!s_acc) return;
    NSArray<GCController *> *all = GCController.controllers;
    ar_accessories_t list = ar_accessories_create();
    __block int pending = 0;
    for (GCController *c in all) if (c.productCategory == GCProductCategorySpatialController) pending++;
    if (!pending) { acc_set(list); return; }
    for (GCController *c in all) {
        if (c.productCategory != GCProductCategorySpatialController) continue;
        ar_accessory_load_from_device(c, ^(id<GCDevice> d, bool ok, ar_error_t e, ar_accessory_t a) {
            (void)d; (void)e;
            dispatch_async(dispatch_get_main_queue(), ^{
                if (ok && a) ar_accessories_add_accessory(list, a);
                if (--pending == 0) acc_set(list);
            });
        });
    }
}
static void acc_start(void) {
    acc_make(ar_accessories_create());
    s_acc_on = 1;
    ar_run();
    acc_reload();
    for (NSString *n in @[ GCControllerDidConnectNotification, GCControllerDidDisconnectNotification ])
        [NSNotificationCenter.defaultCenter addObserverForName:n object:nil queue:NSOperationQueue.mainQueue
                                                    usingBlock:^(NSNotification *note) { (void)note; acc_reload(); }];
    LOGI("controllers: accessory tracking on");
}
static void ar_start(void) {
    s_last_head = matrix_identity_float4x4;
    s_last_head.columns[3] = simd_make_float4(0, 1.6f, 0, 1);
    s_acc_lock = [NSLock new];
    s_ar = ar_session_create();
    ar_session_set_data_provider_state_change_handler(s_ar, nil, ^(ar_data_providers_t ps, ar_data_provider_state_t st, ar_error_t e, ar_data_provider_t failed) {
        (void)ps; (void)failed;
        LOGI("ARKit: providers %s%s", st == ar_data_provider_state_running ? "running" : st == ar_data_provider_state_paused ? "paused"
             : st == ar_data_provider_state_stopped ? "stopped" : "initialized", e ? " (one failed)" : "");
    });
    if (ar_world_tracking_provider_is_supported()) {
        s_world = ar_world_tracking_provider_create(ar_world_tracking_configuration_create());
        s_world_ok = 1;
    } else LOGI("ARKit: no world tracking here: the head stays 1.6 m up");
    ar_run();
    // the hands and the controllers each ask first; a refusal leaves the head running
    if (ar_hand_tracking_provider_is_supported()) {
        s_hands = ar_hand_tracking_provider_create(ar_hand_tracking_configuration_create());
        ar_session_request_authorization(s_ar, ar_hand_tracking_provider_get_required_authorization_type(), ^(ar_authorization_results_t r, ar_error_t e) {
            (void)e;
            __block bool ok = false;
            ar_authorization_results_enumerate_results(r, ^bool(ar_authorization_result_t x) { ok |= ar_authorization_result_get_status(x) == ar_authorization_status_allowed; return true; });
            LOGI("ARKit: hand tracking %s", ok ? "allowed" : "not allowed");
            if (ok) dispatch_async(dispatch_get_main_queue(), ^{ s_hands_on = 1; ar_run(); });
        });
    } else LOGI("ARKit: no hand tracking here (the simulator)");
    if (ar_accessory_tracking_provider_is_supported()) {
        ar_session_request_authorization(s_ar, ar_accessory_tracking_provider_get_required_authorization_type(), ^(ar_authorization_results_t r, ar_error_t e) {
            (void)e;
            __block bool ok = false;
            ar_authorization_results_enumerate_results(r, ^bool(ar_authorization_result_t x) { ok |= ar_authorization_result_get_status(x) == ar_authorization_status_allowed; return true; });
            LOGI("ARKit: controller tracking %s", ok ? "allowed" : "not allowed");
            if (ok) dispatch_async(dispatch_get_main_queue(), ^{ acc_start(); });
        });
    } else LOGI("ARKit: no accessory tracking here (the simulator): no tracked controllers");
}

// the head (origin <- device) at a time; returns the anchor for the drawable (nil before the first)
static ar_device_anchor_t head_at(double t, simd_float4x4 *out, int *ok) {
    *ok = 0;
    *out = s_last_head;
    if (!s_world) return nil;
    ar_device_anchor_t a = ar_device_anchor_create();
    if (ar_world_tracking_provider_query_device_anchor_at_timestamp(s_world, t, a) != ar_device_anchor_query_status_success) {
        static unsigned misses;
        if (misses++ % 600 == 0) LOGI("ARKit: no head for a frame (%u so far)", misses);
        return s_good_anchor;
    }
    s_good_anchor = a;
    s_last_head = ar_anchor_get_origin_from_anchor_transform((ar_anchor_t)a);
    *out = s_last_head;
    *ok = 1;
    return a;
}

// ---- matrices (simd, column-major as GL's)
static void put_m(float *o, simd_float4x4 m) { memcpy(o, &m, 64); }
static simd_float4x4 rigid_from(simd_float3 pos, simd_float3 fwd, simd_float3 up_hint) {
    const simd_float3 z = simd_normalize(-fwd);
    simd_float3 x = simd_cross(up_hint, z);
    if (simd_length(x) < 1e-4f) x = simd_make_float3(1, 0, 0);
    x = simd_normalize(x);
    const simd_float3 y = simd_cross(z, x);
    simd_float4x4 m = { { simd_make_float4(x, 0), simd_make_float4(y, 0), simd_make_float4(z, 0), simd_make_float4(pos, 1) } };
    return m;
}
static simd_float3 pos_of(simd_float4x4 m) { return m.columns[3].xyz; }

// one hand from ARKit's skeleton (LOCAL): its aim ray from the shoulder through the hand, as the Quest's runtime
// makes it, and the pinches (with hysteresis: on under 1.5 cm between the tips, off over 3 cm)
static void hand_from(ar_hand_anchor_t a, int h, simd_float4x4 local, simd_float4x4 head, XrhHand *out) {
    static int pinch[2], mid[2];
    out->tracked = 0;
    if (!a || !ar_trackable_anchor_is_tracked((ar_trackable_anchor_t)a)) { pinch[h] = mid[h] = 0; return; }
    ar_hand_skeleton_t sk = ar_hand_anchor_get_hand_skeleton(a);
    if (!sk) return;
    const simd_float4x4 base = simd_mul(local, ar_hand_anchor_get_origin_from_anchor_transform(a));
    simd_float3 j[27];
    for (int k = 0; k < 27; k++) {
        ar_skeleton_joint_t jt = ar_hand_skeleton_get_joint_named(sk, (ar_hand_skeleton_joint_name_t)k);
        j[k] = jt ? pos_of(simd_mul(base, ar_skeleton_joint_get_anchor_from_joint_transform(jt))) : pos_of(base);
    }
    const simd_float3 wrist = j[ar_hand_skeleton_joint_name_wrist], thumb = j[ar_hand_skeleton_joint_name_thumb_tip];
    const simd_float3 index = j[ar_hand_skeleton_joint_name_index_finger_tip], middle = j[ar_hand_skeleton_joint_name_middle_finger_tip];
    const simd_float3 iknuckle = j[ar_hand_skeleton_joint_name_index_finger_knuckle], tknuckle = j[ar_hand_skeleton_joint_name_thumb_knuckle];
    const simd_float3 lknuckle = j[ar_hand_skeleton_joint_name_little_finger_knuckle];
    // the shoulder: below and beside the head, by the head's heading only
    const simd_float3 hp = pos_of(head), hf = -head.columns[2].xyz;
    simd_float3 fwd = simd_make_float3(hf.x, 0, hf.z);
    fwd = simd_length(fwd) > 1e-3f ? simd_normalize(fwd) : simd_make_float3(0, 0, -1);
    const simd_float3 right = simd_make_float3(-fwd.z, 0, fwd.x);
    const simd_float3 shoulder = hp + simd_make_float3(0, -0.2f, 0) + right * (h ? 0.17f : -0.17f);
    const simd_float3 at = (iknuckle + tknuckle) * 0.5f;
    simd_float3 dir = at - shoulder;
    if (simd_length(dir) < 0.05f) return;
    put_m(out->aim, rigid_from(at, simd_normalize(dir), simd_make_float3(0, 1, 0)));
    const float dp = simd_distance(thumb, index), dm = simd_distance(thumb, middle);
    pinch[h] = pinch[h] ? dp < 0.03f : dp < 0.015f;
    mid[h] = mid[h] ? dm < 0.03f : dm < 0.015f;
    out->pinch = pinch[h];
    out->middle_pinch = mid[h];
    out->pinch_strength = simd_clamp((0.06f - dp) / 0.045f, 0.0f, 1.0f);
    // the palm's normal (index knuckle and little knuckle from the wrist; mirrored for the left hand)
    simd_float3 n = simd_cross(iknuckle - wrist, lknuckle - wrist);
    if (!h) n = -n;
    out->palm_to_face = simd_length(n) > 1e-5f && simd_dot(simd_normalize(n), simd_normalize(hp - wrist)) > 0.6f;
    out->tracked = 1;
}

// a Sense controller's aim pose (LOCAL) at a time
static int controller_aim(int h, double t, simd_float4x4 local, float *out) {
    if (!s_acc_on) return 0;
    [s_acc_lock lock];
    ar_accessory_anchor_t a = s_acc_anchor[h];
    [s_acc_lock unlock];
    if (!a) return 0;
    const double now = CACurrentMediaTime(), ahead = fmin(fmax(t - now, 0.0), 0.05);
    ar_accessory_anchor_t p = ar_accessory_anchor_create();
    if (!ar_accessory_tracking_provider_predict_anchor_at_timestamp(s_acc, a, now + ahead, p)) p = a;
    if (!ar_accessory_anchor_is_tracked(p)) return 0;
    const simd_float4x4 m = simd_mul(ar_accessory_anchor_get_origin_from_anchor_transform_with_correction(p, ar_transform_correction_rendered),
                                     ar_accessory_anchor_get_anchor_from_location_transform_with_correction(p, ar_accessory_location_name_aim,
                                                                                                             ar_transform_correction_rendered));
    simd_float4x4 r = simd_mul(local, m);
    r.columns[0] = simd_make_float4(simd_normalize(r.columns[0].xyz), 0);   // (ARKit's can carry a hair of scale)
    r.columns[1] = simd_make_float4(simd_normalize(r.columns[1].xyz), 0);
    r.columns[2] = simd_make_float4(simd_normalize(r.columns[2].xyz), 0);
    put_m(out, r);
    return 1;
}

// ================================================================ the copy into the drawable (Metal)
static id<MTLCommandQueue> s_queue;
static id<MTLRenderPipelineState> s_pipe;
static id<MTLDepthStencilState> s_depth_always;
static id<MTLSamplerState> s_sampler;
static MTLPixelFormat s_color_fmt, s_depth_fmt;
static const char *kShaders =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    "vertex VOut eye_vs(uint vid [[vertex_id]], constant float &depth [[buffer(0)]]) {\n"
    "    float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "    VOut o; o.pos = float4(p * 2.0 - 1.0, depth, 1.0); o.uv = p; return o;\n"   // (uv from the bottom: GL's rows)
    "}\n"
    "fragment float4 eye_fs(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]]) {\n"
    "    return float4(tex.sample(s, in.uv).rgb, 1.0);\n"
    "}\n";
static int metal_pipes(MTLPixelFormat color, MTLPixelFormat depth) {
    if (s_pipe && color == s_color_fmt && depth == s_depth_fmt) return 1;
    NSError *err = nil;
    id<MTLLibrary> lib = [s_dev newLibraryWithSource:@(kShaders) options:nil error:&err];
    if (!lib) { LOGE("Metal shaders: %s", err.localizedDescription.UTF8String); return 0; }
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"eye_vs"];
    d.fragmentFunction = [lib newFunctionWithName:@"eye_fs"];
    d.colorAttachments[0].pixelFormat = color;
    d.depthAttachmentPixelFormat = depth;
    s_pipe = [s_dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!s_pipe) { LOGE("Metal pipeline: %s", err.localizedDescription.UTF8String); return 0; }
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = MTLCompareFunctionAlways;
    dd.depthWriteEnabled = YES;
    s_depth_always = [s_dev newDepthStencilStateWithDescriptor:dd];
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    s_sampler = [s_dev newSamplerStateWithDescriptor:sd];
    s_color_fmt = color; s_depth_fmt = depth;
    LOGI("compositor: drawable colour format %lu, depth %lu", (unsigned long)color, (unsigned long)depth);
    return 1;
}

// the eyes' pictures: two sets (one drawn while Metal may still read the other), up to two views each
#define SETS 2
static struct { id<MTLTexture> mtl[2]; GLuint gl[2]; id<MTLCommandBuffer> last; } s_set[SETS];
static int s_eye_w, s_eye_h;
static int eyes_make(int w, int h) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB
                                                                                  width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    for (int s = 0; s < SETS; s++)
        for (int e = 0; e < 2; e++) {
            s_set[s].mtl[e] = [s_dev newTextureWithDescriptor:td];
            if (!s_set[s].mtl[e] || !(s_set[s].gl[e] = gl_from_metal(s_set[s].mtl[e]))) return 0;
        }
    s_eye_w = w; s_eye_h = h;
    LOGI("eyes: %dx%d each, sRGB, %d sets", w, h, SETS);
    return 1;
}

// ================================================================ the frame loop
static void close_app(const char *why) {
    LOGI("closing: %s", why);
    xr_quit();
    fflush(NULL);
    _exit(0);   // (the game thread can't be stopped from outside; its saves are written as it goes)
}

// the shell's frame (xr_host_frame), and what this host does round it each frame: the sound's output stream, online
// play's start or end (an app on Vision Pro can't start itself again: the page says so, and a pull closes it), the
// QUEST_TEST_PICK answer
static void shell_frame(const XrhInput *in) {
    quest_audio_service();
    if (xr_restart_requested())
        xr_close_with("ONLINE PLAY\n\nOnline play starts (or ends) when the app opens again.\n\n"
                      "Pinch (or pull a trigger) to close the app, then open House of the Dead VR again now.");
    pick_test();
    xr_host_frame(in);
}

static void *render_main(void *arg) {
    (void)arg;
    pthread_setname_np("hotd-compositor");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    cp_layer_renderer_t layer = (__bridge cp_layer_renderer_t)V.layer;
    if (!egl_init()) close_app("no OpenGL ES 3 through ANGLE");
    s_queue = [s_dev newCommandQueue];
    {   // the files folder (Documents), gone into (port.log is there already: vosAppStart), then the shell's setup and the
        // game (or the setup page, without a ROM set)
        plat_mkdir(V.files);
        if (chdir(V.files)) LOGE("cannot go into %s", V.files);
        online_set_platform("VISIONPRO");
        xr_setup(V.files);
        xr_start();
    }
    ar_start();
    static XrhInput in;
    int gl_ready = 0, set = 0;
    double last_present = 0;
    uint64_t last_idx = 0, frames = 0;
    float period = 1.0f / 90;
    for (;;) {
        @autoreleasepool {
            switch (cp_layer_renderer_get_state(layer)) {
            case cp_layer_renderer_state_paused:
                LOGI("compositor paused");
                in.focused = 0;
                if (gl_ready) shell_frame(&in);   // (the game waits while the space is hidden)
                cp_layer_renderer_wait_until_running(layer);
                LOGI("compositor running");
                continue;
            case cp_layer_renderer_state_invalidated:
                close_app("the immersive space closed");
                return NULL;
            default: break;
            }
            if (xr_should_exit()) close_app("the game asked");
            cp_frame_t frame = cp_layer_renderer_query_next_frame(layer);
            if (!frame) { usleep(1000); continue; }
            cp_frame_start_update(frame);
            cp_frame_end_update(frame);
            cp_frame_timing_t timing = cp_frame_predict_timing(frame);
            if (timing) cp_time_wait_until(cp_frame_timing_get_optimal_input_time(timing));
            // the drawables before the submission starts: a frame without one is left unstarted (ending one that
            // presented nothing aborts)
            cp_drawable_array_t drawables = cp_frame_query_drawables(frame);
            if (!drawables || cp_drawable_array_get_count(drawables) == 0) continue;
            cp_drawable_t dr = cp_drawable_array_get_drawable(drawables, 0);
            cp_frame_start_submission(frame);
            const double present = cp_time_to_cf_time_interval(cp_frame_timing_get_presentation_time(cp_drawable_get_frame_timing(dr)));
            const uint64_t idx = cp_frame_get_frame_index(frame);
            if (last_present > 0 && idx > last_idx && present > last_present) {
                const double p = (present - last_present) / (double)(idx - last_idx);
                if (p > 1.0 / 121 && p < 1.0 / 59) period += ((float)p - period) * 0.05f;
            }
            last_present = present; last_idx = idx;
            const size_t nviews = cp_drawable_get_view_count(dr) > 2 ? 2 : cp_drawable_get_view_count(dr);
            // the head at the presentation, and LOCAL: ARKit's origin raised to the first head's height
            simd_float4x4 head;
            int head_ok;
            ar_device_anchor_t anchor = head_at(present, &head, &head_ok);
            if (head_ok && s_h0 < 0) { s_h0 = head.columns[3].y; LOGI("first head %.2f m over the floor", s_h0); }
            simd_float4x4 local = matrix_identity_float4x4;
            local.columns[3].y = -(s_h0 < 0 ? 1.6f : s_h0);
            simd_float4x4 eye[2];
            float tan[2][4];
            MTLViewport vps[2];
            for (size_t v = 0; v < nviews; v++) {
                cp_view_t view = cp_drawable_get_view(dr, v);
                eye[v] = simd_mul(local, simd_mul(head, cp_view_get_transform(view)));
                const simd_float4x4 p = cp_drawable_compute_projection(dr, cp_axis_direction_convention_right_up_back, v);
                tan[v][0] = (p.columns[2].x - 1) / p.columns[0].x; tan[v][1] = (p.columns[2].x + 1) / p.columns[0].x;
                tan[v][2] = (p.columns[2].y + 1) / p.columns[1].y; tan[v][3] = (p.columns[2].y - 1) / p.columns[1].y;
                vps[v] = cp_view_texture_map_get_viewport(cp_view_get_view_texture_map(view));
            }
            // the shell's head: the eyes' midpoint, turned as the head is
            simd_float4x4 mid = simd_mul(local, head);
            mid.columns[3].xyz = nviews == 2 ? (eye[0].columns[3].xyz + eye[1].columns[3].xyz) * 0.5f : eye[0].columns[3].xyz;
            in.time = present;
            in.focused = 1;
            in.head_ok = head_ok || s_h0 >= 0;
            put_m(in.head, mid);
            // the floor: ARKit's origin is on it (the head's first height over it), except where that height is
            // implausible (the simulator's head is at the origin): then a standing player's 1.6 m under the eyes
            in.floor_y = s_h0 >= 0.5f ? local.columns[3].y : -1.6f;
            in.ipd = nviews == 2 ? simd_distance(eye[0].columns[3].xyz, eye[1].columns[3].xyz) : 0;
            in.hz = (int)(1.0f / period + 0.5f);
            in.hands_ok = s_hands_on;
            if (s_hands_on) {
                ar_hand_anchor_t l = ar_hand_anchor_create(), r = ar_hand_anchor_create();
                ar_hand_tracking_provider_query_anchors_at_timestamp(s_hands, present, l, r);
                hand_from(l, 0, local, mid, &in.hand[0]);
                hand_from(r, 1, local, mid, &in.hand[1]);
            }
            {   // the Sense controllers: buttons (VosControllers.swift), poses (ARKit)
                VosSenseHand sh[2];
                vosSenseSync(present, sh);
                for (int h = 0; h < 2; h++) {
                    XrhController *c = &in.ctl[h];
                    const int was = c->fire;
                    memset(c, 0, sizeof *c);
                    c->connected = sh[h].connected;
                    if (!c->connected) continue;
                    c->trigger = sh[h].trigger; c->squeeze = sh[h].squeeze;
                    c->stick_x = sh[h].stickX; c->stick_y = sh[h].stickY;
                    c->fire = c->trigger > (was ? 0.35f : 0.55f);
                    c->a = sh[h].a; c->b = sh[h].b; c->menu = sh[h].menu; c->stick_click = sh[h].stickClick;
                    c->thumb = sh[h].triggerTouch || sh[h].a || sh[h].b;
                    c->pose_ok = controller_aim(h, present, local, c->aim);
                }
            }
            // the picker's answer: the game starts with the set now, or (the game was running) when the app opens again
            {
                const int p = atomic_exchange(&V.picked, 0);
                if (p && !xr_rom_chosen(p == 1) && p == 1)
                    xr_close_with("YOUR ROM SET IS IN\n\nIt loads when the app opens again.\n\n"
                                  "Pinch (or pull a trigger) to close the app, then open House of the Dead VR again.");
            }
            if (!gl_ready) {
                const int w = (int)vps[0].width, h = (int)vps[0].height;
                if (!metal_pipes(cp_drawable_get_color_texture(dr, 0).pixelFormat, cp_drawable_get_depth_texture(dr, 0).pixelFormat) ||
                    !eyes_make(w, h) || !xr_host_gl_init(w, h))
                    close_app("the eyes' pictures could not be made");
                LOGI("compositor: %zu view(s), %dx%d, tangents L(%.3f %.3f %.3f %.3f)", nviews, w, h, tan[0][0], tan[0][1], tan[0][2], tan[0][3]);
                gl_ready = 1;
            }
            shell_frame(&in);
            // the eyes, in GL (the set Metal is done with)
            set = (set + 1) % SETS;
            if (s_set[set].last) [s_set[set].last waitUntilCompleted];
            for (size_t v = 0; v < nviews; v++) {
                float em[16];
                put_m(em, eye[v]);
                xr_host_eye((int)v, em, tan[v], s_set[set].gl[v]);
            }
            uint64_t gl_value = 0;
            id<MTLSharedEvent> gl_done = gl_fence(&gl_value);
            // the copy into the drawable: each view's picture, its depth at 2 m
            id<MTLCommandBuffer> cmd = [s_queue commandBuffer];
            if (gl_done) [cmd encodeWaitForEvent:gl_done value:gl_value];
            for (size_t v = 0; v < cp_drawable_get_view_count(dr); v++) {
                cp_view_t view = cp_drawable_get_view(dr, v);
                cp_view_texture_map_t map = cp_view_get_view_texture_map(view);
                const size_t ti = cp_view_texture_map_get_texture_index(map), slice = cp_view_texture_map_get_slice_index(map);
                MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = cp_drawable_get_color_texture(dr, ti);
                pass.colorAttachments[0].slice = slice;
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
                pass.depthAttachment.texture = cp_drawable_get_depth_texture(dr, ti);
                pass.depthAttachment.slice = slice;
                pass.depthAttachment.loadAction = MTLLoadActionClear;
                pass.depthAttachment.storeAction = MTLStoreActionStore;
                pass.depthAttachment.clearDepth = 0;
                id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
                [enc setViewport:cp_view_texture_map_get_viewport(map)];
                [enc setRenderPipelineState:s_pipe];
                [enc setDepthStencilState:s_depth_always];
                const simd_float4x4 p = cp_drawable_compute_projection(dr, cp_axis_direction_convention_right_up_back, v);
                const simd_float4 at2 = simd_mul(p, simd_make_float4(0, 0, -2, 1));
                const float depth = at2.w > 0 ? at2.z / at2.w : 0.05f;
                [enc setVertexBytes:&depth length:sizeof depth atIndex:0];
                [enc setFragmentTexture:s_set[set].mtl[v < nviews ? v : 0] atIndex:0];
                [enc setFragmentSamplerState:s_sampler atIndex:0];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [enc endEncoding];
            }
            if (anchor) cp_drawable_set_device_anchor(dr, anchor);
            cp_drawable_encode_present(dr, cmd);
            [cmd commit];
            s_set[set].last = cmd;
            cp_frame_end_submission(frame);
            if (++frames == 1 || frames % 3000 == 0) LOGI("compositor: frame %llu, %d Hz", (unsigned long long)frames, in.hz);
        }
    }
    return NULL;
}

// ================================================================ the app's side (VosApp.swift calls these)
void vosLayerReady(void *layer) {
    if (V.started) { LOGI("another layer renderer: the game keeps the first"); return; }
    V.started = 1;
    V.layer = (void *)CFBridgingRetain((__bridge id)layer);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 8u << 20);
    pthread_t t;
    if (pthread_create(&t, &at, render_main, NULL)) { LOGE("no compositor thread"); abort(); }
    pthread_attr_destroy(&at);
    pthread_detach(t);
}

void vosAppStart(void) {
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    snprintf(V.files, sizeof V.files, "%s", docs.UTF8String);
    snprintf(V.assets, sizeof V.assets, "%s/assets", NSBundle.mainBundle.resourcePath.UTF8String);
    char log[1100];
    snprintf(log, sizeof log, "%s/port.log", V.files);
    crash_apple_keep_log(V.files);   // (for a crash report of the last run)
    if (!getenv("HOTD_LOG_STDERR")) { freopen(log, "w", stderr); setvbuf(stderr, NULL, _IONBF, 0); }
    // the game's sound and voice chat: play and record
    AVAudioSession *s = AVAudioSession.sharedInstance;
    [s setCategory:AVAudioSessionCategoryPlayAndRecord mode:AVAudioSessionModeDefault
           options:AVAudioSessionCategoryOptionAllowBluetoothHFP | AVAudioSessionCategoryOptionMixWithOthers error:nil];
    [s setActive:YES error:nil];
    LOGI("House of the Dead VR for Vision Pro: files %s", V.files);
}

// the picker's file (a copy in the app's temporary folder), into Documents/arcade as hotd.zip or hotd.7z
void vosPickerDone(int status, const char *path) {
    int ok = 0;
    if (status == 1 && path) {
        NSString *src = @(path);
        const BOOL seven = [src.pathExtension.lowercaseString isEqualToString:@"7z"];
        NSString *dir = [NSString stringWithFormat:@"%s/arcade", V.files];
        [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
        NSString *dst = [dir stringByAppendingPathComponent:seven ? @"hotd.7z" : @"hotd.zip"];
        [NSFileManager.defaultManager removeItemAtPath:dst error:nil];
        [NSFileManager.defaultManager removeItemAtPath:[dir stringByAppendingPathComponent:seven ? @"hotd.zip" : @"hotd.7z"] error:nil];
        NSError *err = nil;
        ok = [NSFileManager.defaultManager moveItemAtPath:src toPath:dst error:&err];
        if (ok) LOGI("ROM set: %s in", src.lastPathComponent.UTF8String);
        else LOGE("ROM set: %s", err.localizedDescription.UTF8String);
    }
    atomic_store(&V.picked, ok ? 1 : 2);
}

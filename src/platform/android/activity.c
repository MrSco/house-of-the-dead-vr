// android.h's activity calls: one of the app's own Java activities (src/quest/java: RestartActivity, ImportActivity)
// started from native code in a process of its own, told this process's pid (it waits for it to end, or ends it).
#include "android.h"
#include "../platform.h"
#include <android/api-level.h>
#include <android/native_activity.h>
#include <jni.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

void plat_android_start(const char *java_class) {
    extern ANativeActivity *plat_android_activity(void);
    ANativeActivity *a = plat_android_activity();
    if (!a) return;
    JNIEnv *env;
    if ((*a->vm)->AttachCurrentThread(a->vm, &env, NULL) != JNI_OK) return;
    jclass ic = (*env)->FindClass(env, "android/content/Intent"), ac = (*env)->GetObjectClass(env, a->clazz);
    jobject in = (*env)->NewObject(env, ic, (*env)->GetMethodID(env, ic, "<init>", "()V"));
    jstring pkg = (jstring)(*env)->CallObjectMethod(env, a->clazz, (*env)->GetMethodID(env, ac, "getPackageName", "()Ljava/lang/String;"));
    jstring cls = (*env)->NewStringUTF(env, java_class), key = (*env)->NewStringUTF(env, "pid");
    (*env)->CallObjectMethod(env, in, (*env)->GetMethodID(env, ic, "setClassName", "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;"), pkg, cls);
    (*env)->CallObjectMethod(env, in, (*env)->GetMethodID(env, ic, "putExtra", "(Ljava/lang/String;I)Landroid/content/Intent;"), key, (jint)getpid());
    (*env)->CallObjectMethod(env, in, (*env)->GetMethodID(env, ic, "addFlags", "(I)Landroid/content/Intent;"), (jint)0x10000000);   // NEW_TASK
    (*env)->CallVoidMethod(env, a->clazz, (*env)->GetMethodID(env, ac, "startActivity", "(Landroid/content/Intent;)V"), in);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); plat_log(PLAT_LOG_ERROR, "%s: startActivity failed", java_class); }
    else plat_log(PLAT_LOG_INFO, "%s started", java_class);
    (*a->vm)->DetachCurrentThread(a->vm);
}

// Wi-Fi's multicast filter lifted for this app while it runs (a host on the local network hears a partner's search, a
// broadcast, only so): WifiManager.createMulticastLock("hotdvr-lan").acquire(), held for good. As the Quest app's
// (quest_xr.c multicast_lock); needs CHANGE_WIFI_MULTICAST_STATE.
void plat_android_multicast_lock(void) {
    extern ANativeActivity *plat_android_activity(void);
    ANativeActivity *a = plat_android_activity();
    if (!a) return;
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
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); plat_log(PLAT_LOG_ERROR, "Wi-Fi multicast lock refused"); }
    else plat_log(PLAT_LOG_INFO, "Wi-Fi multicast lock held");
    (*a->vm)->DetachCurrentThread(a->vm);
}

// Display.getRotation() of the activity's window (0: natural, 1: 90 degrees, 2: 180, 3: 270), for turning the
// gyroscope's axes into the screen's
int plat_android_display_rotation(void) {
    extern ANativeActivity *plat_android_activity(void);
    ANativeActivity *a = plat_android_activity();
    if (!a) return 0;
    JNIEnv *env;
    if ((*a->vm)->AttachCurrentThread(a->vm, &env, NULL) != JNI_OK) return 0;
    int r = 0;
    jclass ac = (*env)->GetObjectClass(env, a->clazz);
    jobject wm = (*env)->CallObjectMethod(env, a->clazz, (*env)->GetMethodID(env, ac, "getWindowManager", "()Landroid/view/WindowManager;"));
    if (wm && !(*env)->ExceptionCheck(env)) {
        jobject d = (*env)->CallObjectMethod(env, wm, (*env)->GetMethodID(env, (*env)->GetObjectClass(env, wm), "getDefaultDisplay", "()Landroid/view/Display;"));
        if (d && !(*env)->ExceptionCheck(env)) r = (*env)->CallIntMethod(env, d, (*env)->GetMethodID(env, (*env)->GetObjectClass(env, d), "getRotation", "()I"));
    }
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); r = 0; }
    (*a->vm)->DetachCurrentThread(a->vm);
    return r & 3;
}

// ---- vibration (android.h plat_android_vibrate): the requests queued, sent from a thread attached to Java once, so
// the frame never waits for the binder. A pad's: InputDevice.getDevice(id).getVibratorManager() (Android 12) and a
// CombinedVibration on all its motors, else its getVibrator(); the phone's: the "vibrator" service.
static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    struct { int dev, effect, amp, ms; } q[16];
    int head, n, started;
} V = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER };

static int jni_ok(JNIEnv *env) {
    if (!(*env)->ExceptionCheck(env)) return 1;
    (*env)->ExceptionClear(env);
    return 0;
}
// the VibrationEffect for a request (NULL for STOP)
static jobject vib_effect(JNIEnv *env, int effect, int amp, int ms) {
    jclass ve = (*env)->FindClass(env, "android/os/VibrationEffect");
    if (!jni_ok(env) || !ve) return NULL;
    jobject e = NULL;
    if (effect == PLAT_VIB_ONE_SHOT)
        e = (*env)->CallStaticObjectMethod(env, ve, (*env)->GetStaticMethodID(env, ve, "createOneShot", "(JI)Landroid/os/VibrationEffect;"),
                                           (jlong)(ms > 0 ? ms : 1), (jint)(amp < 1 ? 1 : amp > 255 ? 255 : amp));
    else if (effect == PLAT_VIB_CLICK || effect == PLAT_VIB_TICK)   // (VibrationEffect.EFFECT_CLICK 0, EFFECT_TICK 2)
        e = (*env)->CallStaticObjectMethod(env, ve, (*env)->GetStaticMethodID(env, ve, "createPredefined", "(I)Landroid/os/VibrationEffect;"),
                                           (jint)(effect == PLAT_VIB_CLICK ? 0 : 2));
    return jni_ok(env) ? e : NULL;
}
static void vib_send(JNIEnv *env, jobject activity, int dev, int effect, int amp, int ms) {
    (*env)->PushLocalFrame(env, 32);
    jobject vib = NULL, vm = NULL;   // a Vibrator, or (a pad, Android 12) a VibratorManager
    if (dev >= 0) {
        jclass idc = (*env)->FindClass(env, "android/view/InputDevice");
        jobject d = jni_ok(env) && idc ? (*env)->CallStaticObjectMethod(env, idc, (*env)->GetStaticMethodID(env, idc, "getDevice",
                                                                        "(I)Landroid/view/InputDevice;"), (jint)dev) : NULL;
        if (jni_ok(env) && d) {
            if (android_get_device_api_level() >= 31) {
                vm = (*env)->CallObjectMethod(env, d, (*env)->GetMethodID(env, idc, "getVibratorManager", "()Landroid/os/VibratorManager;"));
                if (!jni_ok(env)) vm = NULL;
                if (vm) {   // (no motors: no ids)
                    jclass vmc = (*env)->GetObjectClass(env, vm);
                    jintArray ids = (*env)->CallObjectMethod(env, vm, (*env)->GetMethodID(env, vmc, "getVibratorIds", "()[I"));
                    if (!jni_ok(env) || !ids || !(*env)->GetArrayLength(env, ids)) vm = NULL;
                }
            } else {
                vib = (*env)->CallObjectMethod(env, d, (*env)->GetMethodID(env, idc, "getVibrator", "()Landroid/os/Vibrator;"));
                if (!jni_ok(env)) vib = NULL;
            }
        }
    } else {
        jclass ac = (*env)->GetObjectClass(env, activity);
        vib = (*env)->CallObjectMethod(env, activity, (*env)->GetMethodID(env, ac, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;"),
                                       (*env)->NewStringUTF(env, "vibrator"));
        if (!jni_ok(env)) vib = NULL;
    }
    if (vib) {
        jclass vc = (*env)->GetObjectClass(env, vib);
        const jboolean has = (*env)->CallBooleanMethod(env, vib, (*env)->GetMethodID(env, vc, "hasVibrator", "()Z"));
        if (!jni_ok(env) || !has) vib = NULL;
    }
    if (vm) {
        jclass vmc = (*env)->GetObjectClass(env, vm);
        if (effect == PLAT_VIB_STOP) (*env)->CallVoidMethod(env, vm, (*env)->GetMethodID(env, vmc, "cancel", "()V"));
        else {
            jobject e = vib_effect(env, effect, amp, ms);
            jclass cv = (*env)->FindClass(env, "android/os/CombinedVibration");
            jobject c = e && jni_ok(env) && cv ? (*env)->CallStaticObjectMethod(env, cv, (*env)->GetStaticMethodID(env, cv, "createParallel",
                                                  "(Landroid/os/VibrationEffect;)Landroid/os/CombinedVibration;"), e) : NULL;
            if (jni_ok(env) && c) (*env)->CallVoidMethod(env, vm, (*env)->GetMethodID(env, vmc, "vibrate", "(Landroid/os/CombinedVibration;)V"), c);
        }
    } else if (vib) {
        jclass vc = (*env)->GetObjectClass(env, vib);
        if (effect == PLAT_VIB_STOP) (*env)->CallVoidMethod(env, vib, (*env)->GetMethodID(env, vc, "cancel", "()V"));
        else {
            jobject e = vib_effect(env, effect, amp, ms);
            if (e) (*env)->CallVoidMethod(env, vib, (*env)->GetMethodID(env, vc, "vibrate", "(Landroid/os/VibrationEffect;)V"), e);
        }
    }
    if (!jni_ok(env)) plat_log(PLAT_LOG_ERROR, "vibrate: device %d refused (VIBRATE?)", dev);
    static int told_none[2];
    if (!vm && !vib && !told_none[dev >= 0]) { told_none[dev >= 0] = 1; plat_log(PLAT_LOG_INFO, "vibrate: %s has no vibrator", dev >= 0 ? "a pad" : "this device"); }
    (*env)->PopLocalFrame(env, NULL);
}
static void *vib_thread(void *arg) {
    ANativeActivity *a = arg;
    JNIEnv *env;
    if ((*a->vm)->AttachCurrentThread(a->vm, &env, NULL) != JNI_OK) return NULL;
    for (;;) {
        pthread_mutex_lock(&V.mu);
        while (!V.n) pthread_cond_wait(&V.cv, &V.mu);
        const int i = V.head, dev = V.q[i].dev, effect = V.q[i].effect, amp = V.q[i].amp, ms = V.q[i].ms;
        V.head = (V.head + 1) % 16; V.n--;
        pthread_mutex_unlock(&V.mu);
        vib_send(env, a->clazz, dev, effect, amp, ms);
    }
    return NULL;
}
void plat_android_vibrate(int input_device, int effect, int amplitude, int ms) {
    extern ANativeActivity *plat_android_activity(void);
    ANativeActivity *a = plat_android_activity();
    if (!a) return;
    pthread_mutex_lock(&V.mu);
    if (!V.started) {
        pthread_t t;
        V.started = pthread_create(&t, NULL, vib_thread, a) == 0 ? 1 : -1;
        if (V.started == 1) pthread_detach(t);
    }
    if (V.started == 1 && V.n < 16) {   // (full: dropped; a frame's worth at most)
        const int i = (V.head + V.n) % 16;
        V.q[i].dev = input_device; V.q[i].effect = effect; V.q[i].amp = amplitude; V.q[i].ms = ms;
        V.n++;
        pthread_cond_signal(&V.cv);
    }
    pthread_mutex_unlock(&V.mu);
}

// InputDevice.getDevice(id) is there: a game pad still connected (the flat app's pads, by their input device id)
int plat_android_input_device_present(int id) {
    extern ANativeActivity *plat_android_activity(void);
    ANativeActivity *a = plat_android_activity();
    if (!a) return 1;
    JNIEnv *env;
    if ((*a->vm)->AttachCurrentThread(a->vm, &env, NULL) != JNI_OK) return 1;
    int r = 1;
    jclass c = (*env)->FindClass(env, "android/view/InputDevice");
    if (c && !(*env)->ExceptionCheck(env)) {
        jmethodID m = (*env)->GetStaticMethodID(env, c, "getDevice", "(I)Landroid/view/InputDevice;");
        if (m && !(*env)->ExceptionCheck(env)) {
            jobject d = (*env)->CallStaticObjectMethod(env, c, m, (jint)id);
            if (!(*env)->ExceptionCheck(env)) r = d != NULL;
        }
    }
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); r = 1; }
    (*a->vm)->DetachCurrentThread(a->vm);
    return r;
}

// android.h's activity calls: one of the app's own Java activities (src/quest/java: RestartActivity, ImportActivity)
// started from native code in a process of its own, told this process's pid (it waits for it to end, or ends it).
#include "android.h"
#include "../platform.h"
#include <android/native_activity.h>
#include <jni.h>
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

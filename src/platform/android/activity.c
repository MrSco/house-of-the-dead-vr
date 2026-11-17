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

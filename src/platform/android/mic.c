// platform.h's microphone on Android: an AAudio input stream with the voice-communication preset, and the
// RECORD_AUDIO permission asked for through JNI on the NativeActivity (the Quest app has no Java of its own for it).
#include "../platform.h"
#include "android.h"
#include <aaudio/AAudio.h>
#include <android/native_activity.h>
#include <jni.h>
#include <stdatomic.h>

#define PERMISSION "android.permission.RECORD_AUDIO"
static ANativeActivity *activity;
static JNIEnv *jenv;                      // the calling thread's (voice.c's worker), attached once
static AAudioStream *mic;
static plat_mic_push push_fn;
static int mic_rate, asked;
static atomic_int mic_failed;

void plat_android_set_activity(void *native_activity) { activity = native_activity; }

// Context.checkSelfPermission / Activity.requestPermissions on the NativeActivity object. NativeActivity ignores the
// answer (onRequestPermissionsResult), so the caller asks again with checkSelfPermission once a second.
#define J(x) x; if ((*e)->ExceptionCheck(e)) { (*e)->ExceptionClear(e); goto done; }
static int perm_granted(void) {
    JNIEnv *e = jenv;
    int ok = 0;
    if (!e || !activity || (*e)->PushLocalFrame(e, 8)) return 0;
    J(jclass cls = (*e)->GetObjectClass(e, activity->clazz))
    J(jmethodID check = (*e)->GetMethodID(e, cls, "checkSelfPermission", "(Ljava/lang/String;)I"))
    J(jstring p = (*e)->NewStringUTF(e, PERMISSION))
    J(ok = (*e)->CallIntMethod(e, activity->clazz, check, p) == 0)   // PackageManager.PERMISSION_GRANTED
done:
    (*e)->PopLocalFrame(e, NULL);
    return ok;
}
static void perm_request(void) {
    JNIEnv *e = jenv;
    if (!e || !activity || (*e)->PushLocalFrame(e, 8)) return;
    J(jclass cls = (*e)->GetObjectClass(e, activity->clazz))
    J(jmethodID req = (*e)->GetMethodID(e, cls, "requestPermissions", "([Ljava/lang/String;I)V"))
    J(jclass str = (*e)->FindClass(e, "java/lang/String"))
    J(jstring p = (*e)->NewStringUTF(e, PERMISSION))
    J(jobjectArray list = (*e)->NewObjectArray(e, 1, str, p))
    J((*e)->CallVoidMethod(e, activity->clazz, req, list, (jint)0x766f))
    plat_log(PLAT_LOG_INFO, "voice: asking for the microphone");
done:
    (*e)->PopLocalFrame(e, NULL);
}
#undef J

int plat_mic_permission(void) {
    if (!jenv && activity && activity->vm &&
        (*activity->vm)->AttachCurrentThread(activity->vm, (void *)&jenv, NULL) != JNI_OK) jenv = NULL;
    if (perm_granted()) return 1;
    if (!asked) { asked = 1; perm_request(); return 0; }
    return -1;
}

static aaudio_data_callback_result_t mic_data(AAudioStream *s, void *ud, void *data, int32_t frames) {
    (void)s; (void)ud;
    push_fn((const int16_t *)data, frames, mic_rate);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
static void mic_error(AAudioStream *s, void *ud, aaudio_result_t e) {
    (void)s; (void)ud;
    plat_log(PLAT_LOG_ERROR, "voice: microphone error %d (%s)", e, AAudio_convertResultToText(e));
    atomic_store(&mic_failed, 1);         // reopened by the caller (not from this callback)
}

void plat_mic_close(void) {
    if (!mic) return;
    AAudioStream_requestStop(mic);
    AAudioStream_close(mic);
    mic = NULL;
}

int plat_mic_open(int rate, plat_mic_push push) {
    AAudioStreamBuilder *b;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return 0;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 1);
    AAudioStreamBuilder_setSampleRate(b, rate);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    // voice communication: the platform's own capture path for calls (its echo and noise handling for the
    // headset's speakers). Not the low-latency mode: that may take the MMAP path, which skips those effects.
    AAudioStreamBuilder_setInputPreset(b, AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_NONE);
    AAudioStreamBuilder_setDataCallback(b, mic_data, NULL);
    AAudioStreamBuilder_setErrorCallback(b, mic_error, NULL);
    push_fn = push;
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &mic);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) { plat_log(PLAT_LOG_ERROR, "voice: no microphone: %s", AAudio_convertResultToText(r)); mic = NULL; return 0; }
    mic_rate = AAudioStream_getSampleRate(mic);
    atomic_store(&mic_failed, 0);
    r = AAudioStream_requestStart(mic);
    plat_log(PLAT_LOG_INFO, "voice: microphone on, %d Hz, burst %d frames, start %s", mic_rate,
             AAudioStream_getFramesPerBurst(mic), AAudio_convertResultToText(r));
    if (r != AAUDIO_OK) { plat_mic_close(); return 0; }
    return 1;
}

int plat_mic_failed(void) { return atomic_load(&mic_failed); }

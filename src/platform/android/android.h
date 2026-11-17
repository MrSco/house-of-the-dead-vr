// What only the Android apps' shells call in src/platform/android.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// the app's ANativeActivity (the microphone's permission is asked for on it, mic.c); before voice_init
void plat_android_set_activity(void *native_activity);
// one of the app's Java activities ("com.mrsco.hotdvr.RestartActivity", ...ImportActivity), in its own process,
// told this process's pid (activity.c)
void plat_android_start(const char *java_class);

#ifdef __cplusplus
}
#endif

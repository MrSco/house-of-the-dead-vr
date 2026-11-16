// What only the Android apps' shells call in src/platform/android.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// the app's ANativeActivity (the microphone's permission is asked for on it, mic.c); before voice_init
void plat_android_set_activity(void *native_activity);

#ifdef __cplusplus
}
#endif

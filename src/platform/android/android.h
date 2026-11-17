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
// Wi-Fi's multicast filter lifted while the app runs (LAN play: a host hears a partner's search); activity.c
void plat_android_multicast_lock(void);
// the display's rotation from the device's natural orientation, as Surface.ROTATION_* (0-3); activity.c
int plat_android_display_rotation(void);

#ifdef __cplusplus
}
#endif

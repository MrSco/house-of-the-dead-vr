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
// an input device (by AInputEvent_getDeviceId) still connected; activity.c
int plat_android_input_device_present(int id);
// Vibration (the flat app's rumble; activity.c): input_device a game pad's motors (by its input device id), or -1 the
// phone's own. effect PLAT_VIB_ONE_SHOT: amplitude 1..255 for ms; PLAT_VIB_CLICK / PLAT_VIB_TICK: the system's short
// effects; PLAT_VIB_STOP: what's running stops. Queued for a thread of its own: it never waits. Needs VIBRATE.
enum { PLAT_VIB_STOP, PLAT_VIB_ONE_SHOT, PLAT_VIB_CLICK, PLAT_VIB_TICK };
void plat_android_vibrate(int input_device, int effect, int amplitude, int ms);

#ifdef __cplusplus
}
#endif

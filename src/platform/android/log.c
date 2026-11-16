// platform.h's log on Android: logcat, tag "hotdarcade" (adb logcat -s hotdarcade).
#include "../platform.h"
#include <android/log.h>

void plat_vlog(int level, const char *fmt, va_list a) {
    __android_log_vprint(level == PLAT_LOG_ERROR ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO, "hotdarcade", fmt, a);
}

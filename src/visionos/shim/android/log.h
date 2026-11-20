// The Quest's log calls into platform.h's log (Vision Pro: the unified log and port.log)
#pragma once
#include "platform/platform.h"
enum { ANDROID_LOG_INFO = 4, ANDROID_LOG_ERROR = 6 };
#define __android_log_print(prio, tag, ...) ((void)(tag), plat_log((prio) >= ANDROID_LOG_ERROR ? PLAT_LOG_ERROR : PLAT_LOG_INFO, __VA_ARGS__), 0)

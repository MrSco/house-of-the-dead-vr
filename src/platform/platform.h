// The seam between the shared game code (src/core) and each operating system (docs/platforms.md, "How the code
// splits"): what the game thread and the menus need from the OS, with one body per OS. POSIX systems (Android,
// iOS, macOS) share posix.c; the log is each OS's own (android/log.c). Windows comes with the flat build.
#pragma once
#include <stdarg.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- the log (Android: logcat, tag "hotdarcade")
enum { PLAT_LOG_INFO, PLAT_LOG_ERROR };
void plat_vlog(int level, const char *fmt, va_list a);
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
void plat_log(int level, const char *fmt, ...);

// ---- time: a monotonic clock in nanoseconds, and sleeping until a time on it
uint64_t plat_now_ns(void);
void plat_sleep_until_ns(uint64_t t);

// ---- threads: the calling thread below the game's and the display's (a helper on a spare core)
void plat_thread_background(void);

// ---- files (UTF-8 paths)
int plat_exists(const char *path);                                  // 1 when the path names a file or a folder
int plat_file_info(const char *path, uint64_t *size, int64_t *mtime); // 0, or -1 when there is none
int plat_mkdir(const char *path);                                   // 0, or -1 (also when it is there already)

#ifdef __cplusplus
}
#endif

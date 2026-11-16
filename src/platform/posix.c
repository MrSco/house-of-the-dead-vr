// platform.h on POSIX systems (Android; iOS and macOS later): time, thread priority, files. The log is each OS's.
#include "platform.h"
#include <errno.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>

void plat_log(int level, const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    plat_vlog(level, fmt, a);
    va_end(a);
}

uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void plat_sleep_until_ns(uint64_t t) {
#if defined(__APPLE__)
    // (no clock_nanosleep on Apple: a relative sleep to the same time)
    const uint64_t now = plat_now_ns();
    if (t <= now) return;
    struct timespec ts = { (time_t)((t - now) / 1000000000ull), (long)((t - now) % 1000000000ull) };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
#else
    struct timespec ts = { (time_t)(t / 1000000000ull), (long)(t % 1000000000ull) };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
#endif
}

// (on Linux and Android this thread only; elsewhere the process, which is why it is only used for helpers)
void plat_thread_background(void) { setpriority(PRIO_PROCESS, 0, 10); }

int plat_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int plat_file_info(const char *path, uint64_t *size, int64_t *mtime) {
    struct stat st;
    if (stat(path, &st)) return -1;
    if (size) *size = (uint64_t)st.st_size;
    if (mtime) *mtime = (int64_t)st.st_mtime;
    return 0;
}

int plat_mkdir(const char *path) { return mkdir(path, 0775) ? -1 : 0; }

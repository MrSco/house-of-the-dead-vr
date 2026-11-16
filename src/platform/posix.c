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

// ---- threads and locks (pthreads)
#include <stdlib.h>
struct plat_thread { pthread_t t; int (*fn)(void *); void *data; int ret; };
static void *thread_main(void *p) { plat_thread *t = p; t->ret = t->fn(t->data); return NULL; }
plat_thread *plat_thread_start(int (*fn)(void *), void *data) {
    plat_thread *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fn = fn; t->data = data;
    if (pthread_create(&t->t, NULL, thread_main, t)) { free(t); return NULL; }
    return t;
}
int plat_thread_join(plat_thread *t) {
    pthread_join(t->t, NULL);
    const int ret = t->ret;
    free(t);
    return ret;
}

void plat_lock_take(plat_lock *l) { pthread_mutex_lock(l); }
void plat_lock_give(plat_lock *l) { pthread_mutex_unlock(l); }
void plat_cond_wait(plat_cond *c, plat_lock *l) { pthread_cond_wait(c, l); }
void plat_cond_broadcast(plat_cond *c) { pthread_cond_broadcast(c); }

struct plat_rmutex { pthread_mutex_t m; };
plat_rmutex *plat_rmutex_new(void) {
    plat_rmutex *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m->m, &a);
    pthread_mutexattr_destroy(&a);
    return m;
}
void plat_rmutex_lock(plat_rmutex *m) { pthread_mutex_lock(&m->m); }
void plat_rmutex_unlock(plat_rmutex *m) { pthread_mutex_unlock(&m->m); }

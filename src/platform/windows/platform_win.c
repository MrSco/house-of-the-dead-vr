// platform.h on Windows: the log (the debugger's output and port.log's stderr), a QueryPerformanceCounter clock,
// sleeping on a high-resolution waitable timer, threads, slim reader/writer locks, files with UTF-8 paths.
// Sound and the microphone: audio_win.c (WASAPI).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "../platform.h"

// ---- the log
void plat_vlog(int level, const char *fmt, va_list a) {
    char line[2048];
    vsnprintf(line, sizeof line, fmt, a);
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    fprintf(stderr, "%s%s\n", level == PLAT_LOG_ERROR ? "error: " : "", line);
    fflush(stderr);
    OutputDebugStringA(line);
    OutputDebugStringA("\n");
}
void plat_log(int level, const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    plat_vlog(level, fmt, a);
    va_end(a);
}

// ---- time
static LARGE_INTEGER qpf;
uint64_t plat_now_ns(void) {
    if (!qpf.QuadPart) QueryPerformanceFrequency(&qpf);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const uint64_t s = (uint64_t)(c.QuadPart / qpf.QuadPart), r = (uint64_t)(c.QuadPart % qpf.QuadPart);
    return s * 1000000000ull + r * 1000000000ull / (uint64_t)qpf.QuadPart;
}
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
void plat_sleep_until_ns(uint64_t t) {
    // a high-resolution timer per thread (Windows 10 1803 and later: ~0.5 ms; else the 1 ms scheduler, with
    // timeBeginPeriod), then a short spin for the last part
    static __declspec(thread) HANDLE timer;
    if (!timer) {
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    }
    for (;;) {
        const uint64_t now = plat_now_ns();
        if (now >= t) return;
        const uint64_t left = t - now;
        if (left < 300000) { YieldProcessor(); continue; }      // under 0.3 ms: spin
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)((left - 250000) / 100);     // relative, 100 ns units, a little early
        if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, INFINITE);
        else Sleep((DWORD)(left / 1000000));
    }
}

// ---- threads
void plat_thread_background(void) { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); }
struct plat_thread { HANDLE h; int (*fn)(void *); void *data; int ret; };
static unsigned __stdcall thread_main(void *p) { plat_thread *t = p; t->ret = t->fn(t->data); return 0; }
plat_thread *plat_thread_start_stack(int (*fn)(void *), void *data, size_t stack_bytes) {
    plat_thread *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fn = fn; t->data = data;
    t->h = (HANDLE)_beginthreadex(NULL, (unsigned)stack_bytes, thread_main, t, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!t->h) { free(t); return NULL; }
    return t;
}
plat_thread *plat_thread_start(int (*fn)(void *), void *data) { return plat_thread_start_stack(fn, data, 0); }
int plat_thread_join(plat_thread *t) {
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
    const int ret = t->ret;
    free(t);
    return ret;
}

// (plat_lock is an SRWLOCK, plat_cond a CONDITION_VARIABLE: both one pointer, zero when fresh)
void plat_lock_take(plat_lock *l) { AcquireSRWLockExclusive((PSRWLOCK)l); }
void plat_lock_give(plat_lock *l) { ReleaseSRWLockExclusive((PSRWLOCK)l); }
void plat_cond_wait(plat_cond *c, plat_lock *l) { SleepConditionVariableSRW((PCONDITION_VARIABLE)c, (PSRWLOCK)l, INFINITE, 0); }
void plat_cond_wait_ms(plat_cond *c, plat_lock *l, int ms) { SleepConditionVariableSRW((PCONDITION_VARIABLE)c, (PSRWLOCK)l, (DWORD)ms, 0); }
void plat_cond_broadcast(plat_cond *c) { WakeAllConditionVariable((PCONDITION_VARIABLE)c); }

struct plat_rmutex { CRITICAL_SECTION cs; };
plat_rmutex *plat_rmutex_new(void) {
    plat_rmutex *m = calloc(1, sizeof *m);
    if (m) InitializeCriticalSection(&m->cs);
    return m;
}
void plat_rmutex_lock(plat_rmutex *m) { EnterCriticalSection(&m->cs); }
void plat_rmutex_unlock(plat_rmutex *m) { LeaveCriticalSection(&m->cs); }

// ---- files: UTF-8 paths to the wide API
static wchar_t *wide(const char *s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
int plat_exists(const char *path) {
    wchar_t *w = wide(path);
    const DWORD a = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
    free(w);
    return a != INVALID_FILE_ATTRIBUTES;
}
int plat_file_info(const char *path, uint64_t *size, int64_t *mtime) {
    wchar_t *w = wide(path);
    struct _stat64 st = { 0 };
    const int bad = !w || _wstat64(w, &st);
    free(w);
    if (bad) return -1;
    if (size) *size = (uint64_t)st.st_size;
    if (mtime) *mtime = (int64_t)st.st_mtime;
    return 0;
}
int plat_mkdir(const char *path) {
    wchar_t *w = wide(path);
    const int ok = w && CreateDirectoryW(w, NULL);
    free(w);
    return ok ? 0 : -1;
}
int plat_rmtree(const char *path) {
    char pat[1024];
    snprintf(pat, sizeof pat, "%s\\*", path);
    wchar_t *wp = wide(pat);
    WIN32_FIND_DATAW fd;
    HANDLE h = wp ? FindFirstFileW(wp, &fd) : INVALID_HANDLE_VALUE;
    free(wp);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            char name[600], q[1024];
            WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL);
            snprintf(q, sizeof q, "%s\\%s", path, name);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) plat_rmtree(q);
            else { wchar_t *wq = wide(q); if (wq) DeleteFileW(wq); free(wq); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    wchar_t *w = wide(path);
    const int ok = w && RemoveDirectoryW(w);
    free(w);
    return ok ? 0 : -1;
}
int plat_chdir(const char *path) {
    wchar_t *w = wide(path);
    const int ok = w && SetCurrentDirectoryW(w);
    free(w);
    return ok ? 0 : -1;
}
int plat_writable(const char *path) {
    wchar_t *w = wide(path);
    const int ok = w && _waccess(w, 2) == 0;
    free(w);
    return ok;
}
int plat_setenv(const char *name, const char *value) { return _putenv_s(name, value) ? -1 : 0; }

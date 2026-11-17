// The seam between the shared game code (src/core) and each operating system (docs/platforms.md, "How the code
// splits"): what the game thread and the menus need from the OS, with one body per OS. POSIX systems (Android,
// iOS, macOS) share posix.c; the log is each OS's own (android/log.c). Windows comes with the flat build.
#pragma once
#include <stdarg.h>
#include <stddef.h>
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

// ---- threads
// the calling thread below the game's and the display's (a helper on a spare core)
void plat_thread_background(void);
typedef struct plat_thread plat_thread;
plat_thread *plat_thread_start(int (*fn)(void *), void *data);   // NULL when it could not start
// with a stack of its own size (the game thread: recompiled code nests deeply on the host stack)
plat_thread *plat_thread_start_stack(int (*fn)(void *), void *data, size_t stack_bytes);
int plat_thread_join(plat_thread *t);                             // fn's result; t is gone after

// Locks and condition variables that can be defined statically (= PLAT_LOCK_INIT / PLAT_COND_INIT); not recursive.
#if defined(_WIN32)
typedef struct { void *opaque; } plat_lock;   // an SRWLOCK
typedef struct { void *opaque; } plat_cond;   // a CONDITION_VARIABLE
#define PLAT_LOCK_INIT { 0 }
#define PLAT_COND_INIT { 0 }
#else
#include <pthread.h>
typedef pthread_mutex_t plat_lock;
typedef pthread_cond_t plat_cond;
#define PLAT_LOCK_INIT PTHREAD_MUTEX_INITIALIZER
#define PLAT_COND_INIT PTHREAD_COND_INITIALIZER
#endif
void plat_lock_take(plat_lock *l);
void plat_lock_give(plat_lock *l);
void plat_cond_wait(plat_cond *c, plat_lock *l);
void plat_cond_broadcast(plat_cond *c);
void plat_cond_wait_ms(plat_cond *c, plat_lock *l, int ms);   // also returns after ms milliseconds
// a recursive mutex (SDL's), made at run time
typedef struct plat_rmutex plat_rmutex;
plat_rmutex *plat_rmutex_new(void);
void plat_rmutex_lock(plat_rmutex *m);
void plat_rmutex_unlock(plat_rmutex *m);

// ---- sound out (Android: AAudio): one 16-bit stereo stream. fill() runs on the OS's audio thread and fills
// `frames` frames at the stream's own `rate`, which may differ from the rate asked for (the caller resamples).
typedef void (*plat_audio_fill)(int16_t *out, int frames, int rate);
// 1 and the stream started (*burst: its frames per callback), or 0 and why in err
int plat_audio_open(int rate, plat_audio_fill fill, int *burst, char *err, int err_len);
void plat_audio_close(void);
int plat_audio_lost(void);   // 1 once the stream has failed (the output device changed): close it, open another

// ---- the microphone for voice chat (Android: AAudio with the voice-communication preset, the RECORD_AUDIO
// permission over JNI). Called from voice.c's worker thread only; push() runs on the OS's audio thread.
typedef void (*plat_mic_push)(const int16_t *mono, int frames, int rate);
int plat_mic_permission(void);     // 1 granted, 0 not yet, -1 asked and refused (asks the first time it is called)
int plat_mic_open(int rate, plat_mic_push push);   // 1: on (at `rate` or another: push gives the rate)
void plat_mic_close(void);
int plat_mic_failed(void);         // 1 once the stream has failed: close it, open it again later

// ---- files (UTF-8 paths)
int plat_exists(const char *path);                                  // 1 when the path names a file or a folder
int plat_file_info(const char *path, uint64_t *size, int64_t *mtime); // 0, or -1 when there is none
int plat_mkdir(const char *path);                                   // 0, or -1 (also when it is there already)
int plat_rmtree(const char *path);                                  // a folder and all in it; 0, or -1
int plat_chdir(const char *path);                                   // 0, or -1
int plat_writable(const char *path);                                // 1 when the file can be written

// ---- the environment (the apps' env.txt switches)
int plat_setenv(const char *name, const char *value);

#ifdef __cplusplus
}
#endif

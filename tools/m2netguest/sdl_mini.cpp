// The eight SDL2 calls main's network code (src/net: online.c, net.c, net_host.c) makes, for m2netguest on the
// desktop without SDL2 (the Quest app has them from quest_sdl.c). Declared as SDL2's headers declare them (Windows:
// SDL_CreateThread with the C runtime's thread functions, its header's macro).
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>

struct SDL_mutex { std::recursive_mutex m; };
struct SDL_Thread { std::thread t; int ret = 0; };
typedef int (*SDL_ThreadFunction)(void *data);

namespace {
const auto t0 = std::chrono::steady_clock::now();
}

extern "C" {
SDL_mutex *SDL_CreateMutex(void) { return new SDL_mutex; }
int SDL_LockMutex(SDL_mutex *m) { if (!m) return -1; m->m.lock(); return 0; }
int SDL_UnlockMutex(SDL_mutex *m) { if (!m) return -1; m->m.unlock(); return 0; }
void SDL_Delay(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
uint32_t SDL_GetTicks(void) {
    return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
}
uint64_t SDL_GetPerformanceCounter(void) {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
}
uint64_t SDL_GetPerformanceFrequency(void) { return 1000000000ull; }
#ifdef _WIN32
SDL_Thread *SDL_CreateThread(SDL_ThreadFunction fn, const char *name, void *data, void *begin, void *end) {
    (void)name; (void)begin; (void)end;
#else
SDL_Thread *SDL_CreateThread(SDL_ThreadFunction fn, const char *name, void *data) {
    (void)name;
#endif
    SDL_Thread *t = new SDL_Thread;
    t->t = std::thread([t, fn, data] { t->ret = fn(data); });
    return t;
}
void SDL_WaitThread(SDL_Thread *t, int *status) {
    if (!t) return;
    if (t->t.joinable()) t->t.join();
    if (status) *status = t->ret;
    delete t;
}
}

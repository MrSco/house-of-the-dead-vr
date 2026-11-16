// The part of SDL2 the game thread (arcade_quest.cpp) and online play (src/net) use, implemented for Meta Quest
// (Android, no Java): they compile against the real SDL2 headers and link with this file instead of libSDL2.
//  - video: SDL_Renderer is a software canvas the size of the game's frame; SDL_RenderPresent hands the finished
//    frame (the game's 2D layers plus the crosshairs arcade_quest.cpp draws) to the VR thread, which shows it;
//  - events: a queue the VR thread fills (controller aim and buttons as mouse and keys);
//  - audio: one AAudio output stream driving the SDL callback (the arcade's SCSP sound);
//  - timers, threads, mutexes.
#include <SDL.h>
#include "quest.h"
#include "voice.h"
#include <aaudio/AAudio.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

static char err[256];
static int set_error(const char *fmt, const char *arg) { snprintf(err, sizeof err, fmt, arg); return -1; }
const char *SDL_GetError(void) { return err; }

// ---------------------------------------------------------------- init, timers, threads
static uint64_t t_start;
static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
Uint32 SDL_GetTicks(void) { if (!t_start) t_start = now_ns(); return (Uint32)((now_ns() - t_start) / 1000000ull); }
Uint64 SDL_GetPerformanceCounter(void) { return now_ns(); }
void SDL_Delay(Uint32 ms) {
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

struct SDL_mutex { pthread_mutex_t m; };
SDL_mutex *SDL_CreateMutex(void) {
    SDL_mutex *m = calloc(1, sizeof *m);
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m->m, &a);
    return m;
}
int SDL_LockMutex(SDL_mutex *m) { return m ? pthread_mutex_lock(&m->m) : -1; }
// threads (src/net/online.c)
struct SDL_Thread { pthread_t t; SDL_ThreadFunction fn; void *data; int ret; };
static void *thread_start(void *p) { SDL_Thread *t = p; t->ret = t->fn(t->data); return NULL; }
SDL_Thread *SDL_CreateThread(SDL_ThreadFunction fn, const char *name, void *data) {
    (void)name;
    SDL_Thread *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fn = fn; t->data = data;
    if (pthread_create(&t->t, NULL, thread_start, t)) { free(t); return NULL; }
    return t;
}
void SDL_WaitThread(SDL_Thread *t, int *status) {
    if (!t) return;
    pthread_join(t->t, NULL);
    if (status) *status = t->ret;
    free(t);
}
int SDL_UnlockMutex(SDL_mutex *m) { return m ? pthread_mutex_unlock(&m->m) : -1; }

// ---------------------------------------------------------------- pause (system menu, headset off)
static pthread_mutex_t pause_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pause_cv = PTHREAD_COND_INITIALIZER;
static volatile int paused;
void quest_set_paused(int p) {
    pthread_mutex_lock(&pause_mu);
    paused = p;
    pthread_cond_broadcast(&pause_cv);
    pthread_mutex_unlock(&pause_mu);
}
static void wait_unpaused(void) {
    pthread_mutex_lock(&pause_mu);
    while (paused) pthread_cond_wait(&pause_cv, &pause_mu);
    pthread_mutex_unlock(&pause_mu);
}

// ---------------------------------------------------------------- events
#define EVQ 256
static SDL_Event evq[EVQ];
static int ev_head, ev_tail;
static pthread_mutex_t ev_mu = PTHREAD_MUTEX_INITIALIZER;
static int ptr_x, ptr_y, ptr_buttons;

static void push_locked(const SDL_Event *e) {
    // a stream of pointer moves collapses into the newest one
    if (e->type == SDL_MOUSEMOTION && ev_head != ev_tail) {
        SDL_Event *last = &evq[(ev_tail + EVQ - 1) % EVQ];
        if (last->type == SDL_MOUSEMOTION) { *last = *e; return; }
    }
    int n = (ev_tail + 1) % EVQ;
    if (n == ev_head) return;
    evq[ev_tail] = *e;
    ev_tail = n;
}
int SDL_PollEvent(SDL_Event *e) {
    pthread_mutex_lock(&ev_mu);
    int have = ev_head != ev_tail;
    if (have && e) { *e = evq[ev_head]; ev_head = (ev_head + 1) % EVQ; }
    pthread_mutex_unlock(&ev_mu);
    return have;
}

void quest_pointer(int x, int y) {
    pthread_mutex_lock(&ev_mu);
    if (x != ptr_x || y != ptr_y) {
        SDL_Event e;
        memset(&e, 0, sizeof e);
        e.type = SDL_MOUSEMOTION;
        e.motion.timestamp = SDL_GetTicks();
        e.motion.windowID = 1;
        e.motion.state = (Uint32)ptr_buttons;
        e.motion.x = x; e.motion.y = y;
        e.motion.xrel = x - ptr_x; e.motion.yrel = y - ptr_y;
        ptr_x = x; ptr_y = y;
        push_locked(&e);
    }
    pthread_mutex_unlock(&ev_mu);
}
void quest_mouse_button(int button, int down) {
    pthread_mutex_lock(&ev_mu);
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    e.button.timestamp = SDL_GetTicks();
    e.button.windowID = 1;
    e.button.button = (Uint8)button;
    e.button.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.button.clicks = 1;
    e.button.x = ptr_x; e.button.y = ptr_y;
    if (down) ptr_buttons |= SDL_BUTTON(button); else ptr_buttons &= ~SDL_BUTTON(button);
    push_locked(&e);
    pthread_mutex_unlock(&ev_mu);
}
void quest_key(int scancode, int keycode, int down) {
    pthread_mutex_lock(&ev_mu);
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.timestamp = SDL_GetTicks();
    e.key.windowID = 1;
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.keysym.scancode = (SDL_Scancode)scancode;
    e.key.keysym.sym = keycode;
    push_locked(&e);
    pthread_mutex_unlock(&ev_mu);
}
void quest_quit(void) {
    pthread_mutex_lock(&ev_mu);
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_QUIT;
    push_locked(&e);
    pthread_mutex_unlock(&ev_mu);
}

struct SDL_Cursor { int id; };

// ---------------------------------------------------------------- window and software renderer
struct SDL_Window { int id; };
struct SDL_Renderer { int id; };
struct SDL_Texture { int w, h; Uint32 format; Uint16 *px; };
static struct SDL_Window the_window = { 1 };
static struct SDL_Renderer the_renderer = { 1 };

static uint32_t *canvas;            // RGBA8 (bytes R, G, B, A), cw x ch
static int cw, ch;
static uint8_t dr, dg, db;          // draw colour

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags) {
    (void)title; (void)x; (void)y; (void)w; (void)h; (void)flags;
    return &the_window;
}

static void canvas_size(int w, int h) {
    if (w <= 0 || h <= 0 || (w == cw && h == ch && canvas)) return;
    free(canvas);
    canvas = calloc((size_t)w * h, 4);
    cw = w; ch = h;
}
SDL_Renderer *SDL_CreateRenderer(SDL_Window *w, int index, Uint32 flags) {
    (void)w; (void)index; (void)flags;
    canvas_size(640, 480);
    return &the_renderer;
}
int SDL_RenderSetLogicalSize(SDL_Renderer *r, int w, int h) { (void)r; canvas_size(w, h); return 0; }
int SDL_SetRenderDrawColor(SDL_Renderer *r, Uint8 R, Uint8 G, Uint8 B, Uint8 A) { (void)r; (void)A; dr = R; dg = G; db = B; return 0; }
static inline uint32_t rgba(uint8_t r, uint8_t g, uint8_t b) { return (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | 0xFF000000u; }
int SDL_RenderClear(SDL_Renderer *r) {
    (void)r;
    uint32_t c = rgba(dr, dg, db);
    for (int i = 0; i < cw * ch; i++) canvas[i] = c;
    return 0;
}

SDL_Texture *SDL_CreateTexture(SDL_Renderer *r, Uint32 format, int access, int w, int h) {
    (void)r; (void)access;
    if (format != SDL_PIXELFORMAT_RGB565) { set_error("%s", "only RGB565 textures"); return NULL; }
    SDL_Texture *t = calloc(1, sizeof *t);
    t->w = w; t->h = h; t->format = format;
    t->px = calloc((size_t)w * h, 2);
    return t;
}
int SDL_UpdateTexture(SDL_Texture *t, const SDL_Rect *rc, const void *pixels, int pitch) {
    int x = rc ? rc->x : 0, y = rc ? rc->y : 0, w = rc ? rc->w : t->w, h = rc ? rc->h : t->h;
    for (int j = 0; j < h && y + j < t->h; j++)
        memcpy(t->px + (size_t)(y + j) * t->w + x, (const uint8_t *)pixels + (size_t)j * pitch, (size_t)(w < t->w - x ? w : t->w - x) * 2);
    return 0;
}
int quest_overlay_key = -1;         // an RGB565 value copied as transparent (the VR overlay, d3d_vr.h)
static inline uint32_t from565(uint16_t p) {
    if (p == quest_overlay_key) return 0;
    uint32_t r = p >> 11, g = (p >> 5) & 63, b = p & 31;
    return rgba((uint8_t)(r << 3 | r >> 2), (uint8_t)(g << 2 | g >> 4), (uint8_t)(b << 3 | b >> 2));
}
int SDL_RenderCopy(SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *src, const SDL_Rect *dst) {
    (void)r; (void)src; (void)dst;    // the port always copies the whole texture onto the whole frame
    if (!t || !canvas) return -1;
    if (t->w == cw && t->h == ch) {
        for (int i = 0; i < cw * ch; i++) canvas[i] = from565(t->px[i]);
    } else {
        for (int y = 0; y < ch; y++) {
            const uint16_t *row = t->px + (size_t)(y * t->h / ch) * t->w;
            for (int x = 0; x < cw; x++) canvas[(size_t)y * cw + x] = from565(row[x * t->w / cw]);
        }
    }
    return 0;
}

// ---- the frame shared with the VR thread
static pthread_mutex_t fr_mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t *shared;
static int sw, sh, shared_cap;
static uint32_t shared_seq, taken_seq;
static volatile int message_shown;      // a message page replaced the game picture for good

static void publish(const uint32_t *px, int w, int h) {
    pthread_mutex_lock(&fr_mu);
    if (w * h > shared_cap) { free(shared); shared = malloc((size_t)w * h * 4); shared_cap = w * h; }
    memcpy(shared, px, (size_t)w * h * 4);
    sw = w; sh = h;
    shared_seq++;
    pthread_mutex_unlock(&fr_mu);
}
int qframe_take(uint32_t *dst, int max_pixels, int *w, int *h) {
    int got = 0;
    pthread_mutex_lock(&fr_mu);
    if (shared_seq != taken_seq && sw * sh <= max_pixels) {
        memcpy(dst, shared, (size_t)sw * sh * 4);
        *w = sw; *h = sh;
        taken_seq = shared_seq;
        got = 1;
    }
    pthread_mutex_unlock(&fr_mu);
    return got;
}
void qframe_size(int *w, int *h) {
    pthread_mutex_lock(&fr_mu);
    *w = sw; *h = sh;
    pthread_mutex_unlock(&fr_mu);
}
void SDL_RenderPresent(SDL_Renderer *r) {
    (void)r;
    if (!message_shown) publish(canvas, cw, ch);
    wait_unpaused();
}

// ---- 5x7 text (messages, self test)
static const struct { char c; uint8_t rows[7]; } font[] = {
    {'0', {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}}, {'1', {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2', {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}}, {'3', {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}},
    {'4', {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}}, {'5', {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    {'6', {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}}, {'7', {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8', {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}}, {'9', {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
    {'A', {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}}, {'B', {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    {'C', {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}}, {'D', {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}},
    {'E', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}}, {'F', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    {'G', {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}}, {'H', {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'I', {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}}, {'J', {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}},
    {'K', {0x11,0x12,0x14,0x18,0x14,0x12,0x11}}, {'L', {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    {'M', {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}}, {'N', {0x11,0x11,0x19,0x15,0x13,0x11,0x11}},
    {'O', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'P', {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {'Q', {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}}, {'R', {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    {'S', {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}}, {'T', {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'U', {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'V', {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
    {'W', {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}}, {'X', {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
    {'Y', {0x11,0x11,0x11,0x0A,0x04,0x04,0x04}}, {'Z', {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
    {'.', {0,0,0,0,0,0x0C,0x0C}}, {',', {0,0,0,0,0x0C,0x04,0x08}}, {':', {0,0x0C,0x0C,0,0x0C,0x0C,0}},
    {'/', {0,0x01,0x02,0x04,0x08,0x10,0}}, {'-', {0,0,0,0x1F,0,0,0}}, {'_', {0,0,0,0,0,0,0x1F}},
    {'(', {0x02,0x04,0x08,0x08,0x08,0x04,0x02}}, {')', {0x08,0x04,0x02,0x02,0x02,0x04,0x08}},
    {'!', {0x04,0x04,0x04,0x04,0x04,0,0x04}}, {'?', {0x0E,0x11,0x01,0x02,0x04,0,0x04}},
    {'\'', {0x04,0x04,0x08,0,0,0,0}}, {'=', {0,0,0x1F,0,0x1F,0,0}}, {'+', {0,0x04,0x04,0x1F,0x04,0x04,0}},
    {'>', {0x08,0x04,0x02,0x01,0x02,0x04,0x08}}, {'<', {0x02,0x04,0x08,0x10,0x08,0x04,0x02}},
    {'%', {0x18,0x19,0x02,0x04,0x08,0x13,0x03}}, {'#', {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A}},
    {'*', {0,0x04,0x15,0x0E,0x15,0x04,0}},
};
static const uint8_t *glyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (size_t i = 0; i < sizeof font / sizeof *font; i++) if (font[i].c == c) return font[i].rows;
    return NULL;
}
static void draw_text(uint32_t *px, int w, int h, int x, int y, int scale, uint32_t col, const char *s) {
    for (int x0 = x; *s; s++) {
        if (*s == '\n') { x = x0; y += 9 * scale; continue; }
        const uint8_t *g = glyph(*s);
        if (g || *s != ' ')
            for (int r = 0; r < 7; r++) for (int b = 0; b < 5; b++)
                if (g ? (g[r] >> (4 - b)) & 1 : (r == 0 || r == 6 || b == 0 || b == 4))
                    for (int sy = 0; sy < scale; sy++) for (int sx = 0; sx < scale; sx++) {
                        int px_ = x + b * scale + sx, py_ = y + r * scale + sy;
                        if (px_ >= 0 && py_ >= 0 && px_ < w && py_ < h) px[(size_t)py_ * w + px_] = col;
                    }
        x += 6 * scale;
    }
}
// text into the current frame (the self test's overlay)
// the same font into an RGB565 picture (w x h): the arcade bridge's overlay (arcade_quest.cpp, its RELOAD sign)
void quest_text565(uint16_t *px, int w, int h, int x, int y, int scale, uint16_t col, const char *s) {
    for (int x0 = x; *s; s++) {
        if (*s == '\n') { x = x0; y += 9 * scale; continue; }
        const uint8_t *g = glyph(*s);
        if (g)
            for (int r = 0; r < 7; r++) for (int b = 0; b < 5; b++)
                if ((g[r] >> (4 - b)) & 1)
                    for (int sy = 0; sy < scale; sy++) for (int sx = 0; sx < scale; sx++) {
                        int u = x + b * scale + sx, v = y + r * scale + sy;
                        if (u >= 0 && v >= 0 && u < w && v < h) px[(size_t)v * w + u] = col;
                    }
        x += 6 * scale;
    }
}
void quest_render_text(int x, int y, int scale, uint32_t col, const char *s) {
    if (canvas) draw_text(canvas, cw, ch, x, y, scale, col, s);
}
// a page of text in place of the game picture, word-wrapped
void quest_show_message(const char *text) {
    enum { W = 640, H = 480, S = 2, CW = 6 * S, LH = 10 * S, COLS = (W - 40) / CW };
    static uint32_t page[W * H];
    for (int i = 0; i < W * H; i++) page[i] = rgba(16, 20, 36);
    char line[COLS + 1];
    int y = 30, n = 0;
    for (const char *p = text;; ) {
        // next word (or newline / end)
        const char *e = p;
        while (*e && *e != ' ' && *e != '\n') e++;
        int len = (int)(e - p);
        if (n && n + 1 + len > COLS) { line[n] = 0; draw_text(page, W, H, 20, y, S, rgba(235, 235, 235), line); y += LH; n = 0; }
        if (n && len) line[n++] = ' ';
        for (int i = 0; i < len && n < COLS; i++) line[n++] = p[i];
        if (!*e || *e == '\n') { line[n] = 0; draw_text(page, W, H, 20, y, S, rgba(235, 235, 235), line); y += LH; n = 0; }
        if (!*e) break;
        p = e + 1;
    }
    message_shown = 1;
    publish(page, W, H);
}


// ---------------------------------------------------------------- audio: AAudio output
static struct {
    SDL_AudioCallback cb;
    void *ud;
    int rate;                 // the rate the game mixes at
    int dev_rate;             // the stream's rate (resampled when different)
    pthread_mutex_t mu;
    volatile int dev_paused, reopen;
    AAudioStream *st;
    int16_t fifo[2 * 4096];   // resampler input
    int fifo_n;
    double fpos;
} au = { .mu = PTHREAD_MUTEX_INITIALIZER };

volatile int quest_volume = 100;
static aaudio_data_callback_result_t au_data(AAudioStream *s, void *ud, void *data, int32_t frames) {
    (void)s; (void)ud;
    int16_t *out = data;
    if (au.dev_paused || paused || !au.cb) {
        memset(out, 0, (size_t)frames * 4);
        if (!paused) voice_mix_out(out, frames, au.dev_rate);   // the partner is heard while the game's sound is off
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    pthread_mutex_lock(&au.mu);
    if (au.dev_rate == au.rate) {
        au.cb(au.ud, (Uint8 *)out, frames * 4);
    } else {
        double step = (double)au.rate / au.dev_rate;
        for (int i = 0; i < frames; i++) {
            while (au.fpos + 1 >= au.fifo_n) {
                int drop = (int)au.fpos;
                if (drop > au.fifo_n) drop = au.fifo_n;
                memmove(au.fifo, au.fifo + drop * 2, (size_t)(au.fifo_n - drop) * 4);
                au.fifo_n -= drop; au.fpos -= drop;
                int add = 512;
                au.cb(au.ud, (Uint8 *)(au.fifo + au.fifo_n * 2), add * 4);
                au.fifo_n += add;
            }
            int i0 = (int)au.fpos;
            float t = (float)(au.fpos - i0);
            out[i * 2] = (int16_t)(au.fifo[i0 * 2] * (1 - t) + au.fifo[i0 * 2 + 2] * t);
            out[i * 2 + 1] = (int16_t)(au.fifo[i0 * 2 + 1] * (1 - t) + au.fifo[i0 * 2 + 3] * t);
            au.fpos += step;
        }
    }
    pthread_mutex_unlock(&au.mu);
    int vol = quest_volume;
    if (vol < 100) for (int i = 0; i < frames * 2; i++) out[i] = (int16_t)(out[i] * vol / 100);
    // voice chat: the partner's voice at its own volume (voice.c), and what plays is the echo canceller's reference
    voice_mix_out(out, frames, au.dev_rate);
    {   // QUEST_AUDIO_DUMP=seconds (env.txt, tests): what the headset plays, as audio_dump.raw (16-bit stereo at the
        // stream's rate), from the first sound on
        static int on = -1; static FILE *f; static long left;
        if (on < 0) { const char *e = getenv("QUEST_AUDIO_DUMP"); on = e && atoi(e) > 0; left = on ? (long)atoi(e) * au.dev_rate : 0; }
        if (on && left > 0) {
            if (!f) f = fopen("audio_dump.raw", "wb");
            if (f) { long n = frames < left ? frames : left; fwrite(out, 4, (size_t)n, f); left -= n; if (left <= 0) { fclose(f); f = NULL; } }
        }
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
static void au_error(AAudioStream *s, void *ud, aaudio_result_t e) {
    (void)s; (void)ud;
    fprintf(stderr, "audio: stream error %d (%s), reopening\n", e, AAudio_convertResultToText(e));
    au.reopen = 1;
}
static int au_open(void) {
    AAudioStreamBuilder *b;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return 0;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, au.rate);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_SONIFICATION);
    AAudioStreamBuilder_setDataCallback(b, au_data, NULL);
    AAudioStreamBuilder_setErrorCallback(b, au_error, NULL);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &au.st);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) { set_error("AAudio: %s", AAudio_convertResultToText(r)); au.st = NULL; return 0; }
    au.dev_rate = AAudioStream_getSampleRate(au.st);
    au.fifo_n = 0; au.fpos = 0;
    r = AAudioStream_requestStart(au.st);
    fprintf(stderr, "audio: AAudio %d Hz (game mixes at %d Hz), burst %d frames, start %s\n", au.dev_rate, au.rate,
            AAudioStream_getFramesPerBurst(au.st), AAudio_convertResultToText(r));
    return 1;
}
static void au_close(void) {
    if (!au.st) return;
    AAudioStream_requestStop(au.st);
    AAudioStream_close(au.st);
    au.st = NULL;
}
void quest_audio_service(void) {
    if (!au.reopen) return;
    au.reopen = 0;
    au_close();
    au_open();
}
SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device, int iscapture, const SDL_AudioSpec *want, SDL_AudioSpec *have, int changes) {
    (void)device; (void)changes;
    if (iscapture) { set_error("%s", "no capture yet"); return 0; }
    if (want->format != AUDIO_S16SYS || want->channels != 2) { set_error("%s", "audio: only 16-bit stereo"); return 0; }
    au.cb = want->callback; au.ud = want->userdata; au.rate = want->freq;
    au.dev_paused = 1;
    if (!au_open()) return 0;
    if (have) { *have = *want; have->samples = (Uint16)AAudioStream_getFramesPerBurst(au.st); }
    return 2;
}
void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on) { (void)dev; au.dev_paused = pause_on; }
void SDL_CloseAudioDevice(SDL_AudioDeviceID dev) { (void)dev; au_close(); au.cb = NULL; }

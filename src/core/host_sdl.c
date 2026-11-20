// The part of SDL2 the game thread (arcade.cpp) and online play (src/net) use, for every app: they compile against
// the real SDL2 headers and link with this file instead of libSDL2. The app's shell (the VR one, or the flat one)
// is on the other side of it (quest.h):
//  - video: SDL_Renderer is a software canvas the size of the game's frame; SDL_RenderPresent hands the finished
//    frame (the game's 2D layers plus the crosshairs arcade.cpp draws) to the shell, which shows it;
//  - events: a queue the shell fills (the gun's aim and buttons as mouse and keys);
//  - audio: the platform's output stream (platform.h: AAudio on Android) driving the SDL callback (the arcade's SCSP
//    sound), resampled when the device's rate differs, with the voice chat mixed in;
//  - timers, threads, mutexes, on platform.h's.
#include <SDL.h>
#include "quest.h"
#include "voice.h"
#include "platform/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char err[256];
static int set_error(const char *fmt, const char *arg) { snprintf(err, sizeof err, fmt, arg); return -1; }
const char *SDL_GetError(void) { return err; }

// ---------------------------------------------------------------- init, timers, threads
static uint64_t t_start;
Uint32 SDL_GetTicks(void) { if (!t_start) t_start = plat_now_ns(); return (Uint32)((plat_now_ns() - t_start) / 1000000ull); }
Uint64 SDL_GetPerformanceCounter(void) { return plat_now_ns(); }
void SDL_Delay(Uint32 ms) { plat_sleep_until_ns(plat_now_ns() + (uint64_t)ms * 1000000ull); }

// (SDL's mutexes are recursive; never destroyed here)
SDL_mutex *SDL_CreateMutex(void) { return (SDL_mutex *)plat_rmutex_new(); }
int SDL_LockMutex(SDL_mutex *m) { if (!m) return -1; plat_rmutex_lock((plat_rmutex *)m); return 0; }
int SDL_UnlockMutex(SDL_mutex *m) { if (!m) return -1; plat_rmutex_unlock((plat_rmutex *)m); return 0; }
// threads (src/net/online.c)
// (on Windows SDL2's header makes SDL_CreateThread a macro passing the C runtime's thread functions: unused here)
#ifdef SDL_PASSED_BEGINTHREAD_ENDTHREAD
#undef SDL_CreateThread
SDL_Thread *SDL_CreateThread(SDL_ThreadFunction fn, const char *name, void *data, pfnSDL_CurrentBeginThread begin,
                             pfnSDL_CurrentEndThread end) {
    (void)begin; (void)end;
#else
SDL_Thread *SDL_CreateThread(SDL_ThreadFunction fn, const char *name, void *data) {
#endif
    (void)name;
    return (SDL_Thread *)plat_thread_start(fn, data);
}
void SDL_WaitThread(SDL_Thread *t, int *status) {
    if (!t) return;
    const int ret = plat_thread_join((plat_thread *)t);
    if (status) *status = ret;
}

// ---------------------------------------------------------------- pause (system menu, headset off)
static plat_lock pause_mu = PLAT_LOCK_INIT;
static plat_cond pause_cv = PLAT_COND_INIT;
static volatile int paused;
void quest_set_paused(int p) {
    plat_lock_take(&pause_mu);
    paused = p;
    plat_cond_broadcast(&pause_cv);
    plat_lock_give(&pause_mu);
}
static void wait_unpaused(void) {
    plat_lock_take(&pause_mu);
    while (paused) plat_cond_wait(&pause_cv, &pause_mu);
    plat_lock_give(&pause_mu);
}

// ---------------------------------------------------------------- events
#define EVQ 256
static SDL_Event evq[EVQ];
static int ev_head, ev_tail;
static plat_lock ev_mu = PLAT_LOCK_INIT;
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
    plat_lock_take(&ev_mu);
    int have = ev_head != ev_tail;
    if (have && e) { *e = evq[ev_head]; ev_head = (ev_head + 1) % EVQ; }
    plat_lock_give(&ev_mu);
    return have;
}

void quest_pointer(int x, int y) {
    plat_lock_take(&ev_mu);
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
    plat_lock_give(&ev_mu);
}
void quest_mouse_button(int button, int down) {
    plat_lock_take(&ev_mu);
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
    plat_lock_give(&ev_mu);
}
void quest_key(int scancode, int keycode, int down) {
    plat_lock_take(&ev_mu);
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.timestamp = SDL_GetTicks();
    e.key.windowID = 1;
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.keysym.scancode = (SDL_Scancode)scancode;
    e.key.keysym.sym = keycode;
    push_locked(&e);
    plat_lock_give(&ev_mu);
}
void quest_quit(void) {
    plat_lock_take(&ev_mu);
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_QUIT;
    push_locked(&e);
    plat_lock_give(&ev_mu);
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
static plat_lock fr_mu = PLAT_LOCK_INIT;
static uint32_t *shared;
static int sw, sh, shared_cap;
static uint32_t shared_seq, taken_seq;
static volatile int message_shown;      // a message page replaced the game picture for good

static void publish(const uint32_t *px, int w, int h) {
    plat_lock_take(&fr_mu);
    if (w * h > shared_cap) { free(shared); shared = malloc((size_t)w * h * 4); shared_cap = w * h; }
    memcpy(shared, px, (size_t)w * h * 4);
    sw = w; sh = h;
    shared_seq++;
    plat_lock_give(&fr_mu);
}
int qframe_take(uint32_t *dst, int max_pixels, int *w, int *h) {
    int got = 0;
    plat_lock_take(&fr_mu);
    if (shared_seq != taken_seq && sw * sh <= max_pixels) {
        memcpy(dst, shared, (size_t)sw * sh * 4);
        *w = sw; *h = sh;
        taken_seq = shared_seq;
        got = 1;
    }
    plat_lock_give(&fr_mu);
    return got;
}
void qframe_size(int *w, int *h) {
    plat_lock_take(&fr_mu);
    *w = sw; *h = sh;
    plat_lock_give(&fr_mu);
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
// the game's own pictures again (Vision Pro: the ROM set chosen while the setup page showed, then the game started)
void quest_clear_message(void) { message_shown = 0; }


// ---------------------------------------------------------------- audio: the platform's output stream
static struct {
    SDL_AudioCallback cb;
    void *ud;
    int rate;                 // the rate the game mixes at
    plat_lock mu;
    volatile int dev_paused, open;
    int16_t fifo[2 * 4096];   // resampler input
    int fifo_n;
    double fpos;
} au = { .mu = PLAT_LOCK_INIT };

volatile int quest_volume = 100;
// the platform's audio thread: `frames` stereo frames at the stream's rate
static void au_fill(int16_t *out, int frames, int dev_rate) {
    if (au.dev_paused || paused || !au.cb) {
        memset(out, 0, (size_t)frames * 4);
        if (!paused) voice_mix_out(out, frames, dev_rate);   // the partner is heard while the game's sound is off
        return;
    }
    plat_lock_take(&au.mu);
    if (dev_rate == au.rate) {
        au.cb(au.ud, (Uint8 *)out, frames * 4);
    } else {
        double step = (double)au.rate / dev_rate;
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
    plat_lock_give(&au.mu);
    int vol = quest_volume;
    if (vol < 100) for (int i = 0; i < frames * 2; i++) out[i] = (int16_t)(out[i] * vol / 100);
    // voice chat: the partner's voice at its own volume (voice.c), and what plays is the echo canceller's reference
    voice_mix_out(out, frames, dev_rate);
    {   // QUEST_AUDIO_DUMP=seconds (env.txt, tests): what the device plays, as audio_dump.raw (16-bit stereo at the
        // stream's rate), from the first sound on
        static int on = -1; static FILE *f; static long left;
        if (on < 0) { const char *e = getenv("QUEST_AUDIO_DUMP"); on = e && atoi(e) > 0; left = on ? (long)atoi(e) * dev_rate : 0; }
        if (on && left > 0) {
            if (!f) f = fopen("audio_dump.raw", "wb");
            if (f) { long n = frames < left ? frames : left; fwrite(out, 4, (size_t)n, f); left -= n; if (left <= 0) { fclose(f); f = NULL; } }
        }
    }
}
static int au_open(int *burst) {
    au.fifo_n = 0; au.fpos = 0;
    char e[160];
    if (!plat_audio_open(au.rate, au_fill, burst, e, sizeof e)) { set_error("%s", e); return 0; }
    au.open = 1;
    return 1;
}
static void au_close(void) {
    if (!au.open) return;
    plat_audio_close();
    au.open = 0;
}
// (before the process exits: its callback thread otherwise runs on into the C++ globals exit() destroys)
void quest_audio_shutdown(void) {
    au.dev_paused = 1;
    au_close();
    au.cb = NULL;
}
// the stream failed (the output device changed): open it again
void quest_audio_service(void) {
    if (!au.open || !plat_audio_lost()) return;
    au_close();
    int burst = 0;
    au_open(&burst);
}
SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device, int iscapture, const SDL_AudioSpec *want, SDL_AudioSpec *have, int changes) {
    (void)device; (void)changes;
    if (iscapture) { set_error("%s", "no capture yet"); return 0; }
    if (want->format != AUDIO_S16SYS || want->channels != 2) { set_error("%s", "audio: only 16-bit stereo"); return 0; }
    au.cb = want->callback; au.ud = want->userdata; au.rate = want->freq;
    au.dev_paused = 1;
    int burst = want->samples;
    if (!au_open(&burst)) return 0;
    if (have) { *have = *want; have->samples = (Uint16)burst; }
    return 2;
}
void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on) { (void)dev; au.dev_paused = pause_on; }
void SDL_CloseAudioDevice(SDL_AudioDeviceID dev) { (void)dev; au_close(); au.cb = NULL; }

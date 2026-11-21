// flat.h's picture: the cabinet view of the VR app (quest_xr.c render_cabinet_view) at the window's size. The 3D
// is the game camera's own view drawn by the shared renderer (m2_gl.c) with the game's projection, the 2D layers
// (HUD, text, the game's own crosshairs) over it, then the game's fades. It is drawn in linear light into an sRGB
// picture (4x MSAA in HD) and shown 4:3 in the middle of the window, black bars at its sides or above and below.
#include "flat.h"
#include "platform/gl.h"
#include "platform/platform.h"
#include "quest.h"
#include "d3d_vr.h"
#include "m2_vr.h"
#include "online.h"
#include "quest_options.h"
#include <SDL.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOGI(...) plat_log(PLAT_LOG_INFO, __VA_ARGS__)
#define LOGE(...) plat_log(PLAT_LOG_ERROR, __VA_ARGS__)

#define FRAME_MAX (1024 * 768)

static struct {
    int picture;
    GLuint vao, vbo;
    GLuint prog_col, prog_tex, prog_out;
    GLint u_col_mvp, u_tex_mvp, u_tex_size, u_tex_alpha, u_out_size, u_out_sharp;
    GLuint screen_tex; int tex_w, tex_h;       // the game's 2D layers: the newest of the ring's
    // three in turn: a frame never goes into a texture the GPU may still be reading (ANGLE's Metal then waited for it:
    // 11 to 15 ms a frame on the Apple TV HD's main thread, 2026-11-20)
    GLuint ring[3]; int ring_w[3], ring_h[3], ring_i;
    uint32_t *frame;
    // the picture: a resolved sRGB texture (and a multisampled framebuffer drawn into first, in HD)
    GLuint pic_fbo, pic_tex, pic_depth, ms_fbo, ms_col, ms_depth;
    int pic_w, pic_h, ms;
    int max_lines, max_samples;                // flat_picture_limit (0: none)
    const VrScene *scene;
    uint32_t scene_serial;
    int dirty;
    // where the picture is in the window
    int vx, vy, vw, vh;
} F = { .picture = FLAT_PICTURE_HD };

// ---------------------------------------------------------------- GL helpers
static const char *VS_COL =
    "layout(location=0) in vec3 pos;\nlayout(location=1) in vec4 col;\nuniform mat4 mvp;\nout vec4 v_col;\n"
    "void main() { gl_Position = mvp * vec4(pos, 1.0); v_col = col; }\n";
static const char *FS_COL =
    "precision mediump float;\nin vec4 v_col;\nout vec4 o;\nvoid main() { o = v_col; }\n";
static const char *VS_TEX =
    "uniform mat4 mvp;\nlayout(location=0) in vec3 pos;\nlayout(location=1) in vec2 uv;\nout vec2 v_uv;\n"
    "void main() { gl_Position = mvp * vec4(pos, 1.0); v_uv = uv; }\n";
// "sharp bilinear" (as the VR app's): crisp game pixels when magnified, smooth when minified
static const char *FS_TEX =
    "precision highp float;\nuniform sampler2D tex;\nuniform vec2 texsize;\nuniform int alpha;\nin vec2 v_uv;\nout vec4 o;\n"
    "void main() {\n"
    "  vec2 px = v_uv * texsize;\n"
    "  vec2 seam = floor(px + 0.5);\n"
    "  vec2 w = max(fwidth(px), vec2(1e-4));\n"
    "  px = seam + clamp((px - seam) / w, -0.5, 0.5);\n"
    "  vec4 c = texture(tex, px / texsize);\n"
    "  o = vec4(c.rgb, alpha != 0 ? c.a : 1.0);\n"
    "}\n";
// the picture onto the window: its sRGB texture read (linear light), encoded for the window's plain framebuffer;
// sharp: the arcade's pixels scaled up crisply (Original)
static const char *FS_OUT =
    "precision highp float;\nuniform sampler2D tex;\nuniform vec2 texsize;\nuniform int sharp;\nin vec2 v_uv;\nout vec4 o;\n"
    "vec3 enc(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)); }\n"
    "void main() {\n"
    "  vec2 uv = v_uv;\n"
    "  if (sharp != 0) {\n"
    "    vec2 px = uv * texsize, seam = floor(px + 0.5), w = max(fwidth(px), vec2(1e-4));\n"
    "    uv = (seam + clamp((px - seam) / w, -0.5, 0.5)) / texsize;\n"
    "  }\n"
    "  o = vec4(enc(texture(tex, uv).rgb), 1.0);\n"
    "}\n";

static GLuint shader(GLenum type, const char *src) {
    const char *parts[2] = { PLAT_GLSL_VERSION, src };
    GLuint s = glCreateShader(type);
    glShaderSource(s, 2, parts, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof log, NULL, log); LOGE("flat shader: %s", log); }
    return s;
}
static GLuint program(const char *vs, const char *fs) {
    GLuint p = glCreateProgram(), v = shader(GL_VERTEX_SHADER, vs), f = shader(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[1024]; glGetProgramInfoLog(p, sizeof log, NULL, log); LOGE("flat program: %s", log); }
    glDeleteShader(v);
    glDeleteShader(f);
    return ok ? p : 0;
}
static const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
unsigned flat_gl_program(const char *vs, const char *fs) { return program(vs, fs); }
// a colour byte (sRGB-ish) in linear light (as the VR app's)
static float lin(uint32_t c8) {   // (the exact sRGB curve, as the framebuffer's: m2_gl.c to_linear)
    const float c = (float)(c8 & 255) / 255.0f;
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

// six vertices of `stride` floats from memory, through our own buffer (core profiles have no client-side arrays)
static void stream(const float *v, int n, int stride) {
    glBindVertexArray(F.vao);
    glBindBuffer(GL_ARRAY_BUFFER, F.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(n * stride * sizeof(float)), v, GL_STREAM_DRAW);
}
static void tex_quad(const float *mvp, const float *q, GLuint tex, float tw, float th, int alpha) {
    glUseProgram(F.prog_tex);
    glUniformMatrix4fv(F.u_tex_mvp, 1, GL_FALSE, mvp);
    glUniform2f(F.u_tex_size, tw, th);
    glUniform1i(F.u_tex_alpha, alpha);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    stream(q, 6, 5);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

// a rectangle in window pixels (y down), uv u0,v0 at its top left, with a program taking pos and uv
void flat_gl_rect_uv(unsigned prog, int mvp_uniform, float x, float y, float w, float h, int win_w, int win_h,
                     float u0, float v0, float u1, float v1) {
    const float x0 = 2 * x / (float)win_w - 1, x1 = 2 * (x + w) / (float)win_w - 1;
    const float y0 = 1 - 2 * y / (float)win_h, y1 = 1 - 2 * (y + h) / (float)win_h;
    const float q[] = { x0, y0, 0, u0, v0,  x1, y0, 0, u1, v0,  x1, y1, 0, u1, v1,
                        x0, y0, 0, u0, v0,  x1, y1, 0, u1, v1,  x0, y1, 0, u0, v1 };
    glUseProgram(prog);
    glUniformMatrix4fv(mvp_uniform, 1, GL_FALSE, identity);
    stream(q, 6, 5);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

void flat_gl_rect(unsigned prog, int mvp_uniform, float x, float y, float w, float h, int win_w, int win_h) {
    flat_gl_rect_uv(prog, mvp_uniform, x, y, w, h, win_w, win_h, 0, 0, 1, 1);
}

int flat_gl_init(void) {
    LOGI("GL: %s / %s", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    glGenVertexArrays(1, &F.vao);
    glGenBuffers(1, &F.vbo);
    F.prog_col = program(VS_COL, FS_COL);
    F.u_col_mvp = glGetUniformLocation(F.prog_col, "mvp");
    F.prog_tex = program(VS_TEX, FS_TEX);
    F.u_tex_mvp = glGetUniformLocation(F.prog_tex, "mvp");
    F.u_tex_size = glGetUniformLocation(F.prog_tex, "texsize");
    F.u_tex_alpha = glGetUniformLocation(F.prog_tex, "alpha");
    F.prog_out = program(VS_TEX, FS_OUT);
    F.u_out_size = glGetUniformLocation(F.prog_out, "texsize");
    F.u_out_sharp = glGetUniformLocation(F.prog_out, "sharp");
    glUseProgram(F.prog_tex);
    glUniform1i(glGetUniformLocation(F.prog_tex, "tex"), 0);
    glUseProgram(F.prog_out);
    glUniform1i(glGetUniformLocation(F.prog_out, "tex"), 0);
    glUniformMatrix4fv(glGetUniformLocation(F.prog_out, "mvp"), 1, GL_FALSE, identity);
    if (!F.prog_col || !F.prog_tex || !F.prog_out) return 0;
    glGenTextures(3, F.ring);
    for (int i = 0; i < 3; i++) {
        glBindTexture(GL_TEXTURE_2D, F.ring[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    F.screen_tex = F.ring[0];
    F.frame = malloc(FRAME_MAX * 4);
    GLint max_samples = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
    F.ms = max_samples >= 4 ? 4 : 1;
    m2_gl_init();
    {   // a flat picture: the arcade's order decides what is in front, as on the arcade (M2_GL_ORDER=0, tests: real
        // depth, as the headset draws)
        const char *e = getenv("M2_GL_ORDER");
        m2_gl_arcade_order(!(e && *e == '0'));
    }
    quest_overlay_key = D3D_VR_KEY;   // the game's 2D layers: this colour is see-through (arcade.cpp)
    return F.frame != NULL;
}

void flat_redraw(void) { F.dirty = 1; }   // (a setting changed while the game is paused)
void flat_set_picture(int mode) {
    if (mode == F.picture) return;
    F.picture = mode;
    F.dirty = 1;
}

void flat_picture_limit(int lines, int samples) {
    F.max_lines = lines > 0 ? lines : 0;
    F.max_samples = samples > 0 ? samples : 0;
}

// the picture's framebuffers at w x h (made again when the size changes)
static void picture_size(int w, int h, int ms) {
    if (w == F.pic_w && h == F.pic_h && ms == (F.ms_fbo ? F.ms : 1) && F.pic_fbo) return;
    if (F.pic_fbo) {
        glDeleteFramebuffers(1, &F.pic_fbo);
        glDeleteTextures(1, &F.pic_tex);
        glDeleteRenderbuffers(1, &F.pic_depth);
        F.pic_fbo = F.pic_tex = F.pic_depth = 0;
    }
    if (F.ms_fbo) {
        glDeleteFramebuffers(1, &F.ms_fbo);
        glDeleteRenderbuffers(1, &F.ms_col);
        glDeleteRenderbuffers(1, &F.ms_depth);
        F.ms_fbo = F.ms_col = F.ms_depth = 0;
    }
    F.pic_w = w; F.pic_h = h;
    glGenTextures(1, &F.pic_tex);
    glBindTexture(GL_TEXTURE_2D, F.pic_tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_SRGB8_ALPHA8, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenRenderbuffers(1, &F.pic_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, F.pic_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glGenFramebuffers(1, &F.pic_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, F.pic_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, F.pic_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, F.pic_depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) LOGE("flat: picture framebuffer incomplete");
    if (ms > 1) {
        glGenRenderbuffers(1, &F.ms_col);
        glBindRenderbuffer(GL_RENDERBUFFER, F.ms_col);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, ms, GL_SRGB8_ALPHA8, w, h);
        glGenRenderbuffers(1, &F.ms_depth);
        glBindRenderbuffer(GL_RENDERBUFFER, F.ms_depth);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, ms, GL_DEPTH24_STENCIL8, w, h);
        glGenFramebuffers(1, &F.ms_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, F.ms_fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, F.ms_col);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, F.ms_depth);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            LOGE("flat: multisampled framebuffer incomplete: no MSAA");
            glDeleteFramebuffers(1, &F.ms_fbo);
            F.ms_fbo = 0;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    F.dirty = 1;
}

// ---------------------------------------------------------------- the game's picture
// the game's own projection (its picture) as a GL matrix: clip <- game camera space (quest_xr.c game_proj)
static void game_proj(float *m, const VrScene *sc) {
    const float fx = sc ? sc->fx : 640, fy = sc ? sc->fy : -620, w = sc ? (float)sc->w : 640, h = sc ? (float)sc->h : 480;
    const float n = 1.0f, f = 4000.0f;
    memset(m, 0, 16 * sizeof(float));
    m[0] = fx / (w * 0.5f);
    m[5] = -fy / (h * 0.5f);
    m[10] = (f + n) / (f - n); m[14] = -2 * f * n / (f - n);
    m[11] = 1;
}
static void draw_fade(void) {
    const VrScene *sc = F.scene;
    if (!sc || !sc->fade || !(sc->fade >> 24)) return;
    const uint32_t c = sc->fade;
    const float r = lin(c >> 16), g = lin(c >> 8), b = lin(c), a = (float)(c >> 24) / 255.0f;
    const float xy[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
    float v[6 * 7];
    for (int i = 0; i < 6; i++) {
        float *p = v + i * 7;
        p[0] = xy[i][0]; p[1] = xy[i][1]; p[2] = 0; p[3] = r; p[4] = g; p[5] = b; p[6] = a;
    }
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(F.prog_col);
    glUniformMatrix4fv(F.u_col_mvp, 1, GL_FALSE, identity);
    stream(v, 6, 7);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void *)0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void *)(3 * sizeof(float)));
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisable(GL_BLEND);
}
#ifdef _WIN32
// FLAT_TEST_AVATAR=pose[,turn[,distance]] (tests, HOTDVR.exe; with M2_FIGURES=1, which makes the figures' file on a
// screen too): the partner's figures (quest_avatar.c, m2_gl_figure_*) drawn over the picture, Rogan on the left and G
// on the right, turned by that many degrees (0: facing you), that many metres off (3). Poses: 0 standing, no hands
// (the arms hang, the figures' own hands); 1 aiming two-handed at you; 2 crouching, the right hand up, the head
// turned (after a second standing); 3 walking sideways and turning (the feet step); 4 seated low (a figure smaller than
// its least); 5 reaching up and across.
#include "xr/quest_avatar.h"
static void flat_test_avatar(const float *p) {
    static int pose = -2; static float turn, dist = 3; static Avatar av[2]; static uint64_t t0, last;
    static unsigned mask = ~0u;   // (FLAT_TEST_AVATAR_PARTS: only those parts, a mask of 1 << M2_FIGURE_*)
    if (pose == -2) {
        const char *e = getenv("FLAT_TEST_AVATAR");
        pose = e ? atoi(e) : -1;
        if (e) sscanf(e, "%*d,%f,%f", &turn, &dist);
        if (getenv("FLAT_TEST_AVATAR_PARTS")) mask = (unsigned)strtoul(getenv("FLAT_TEST_AVATAR_PARTS"), NULL, 0);
        t0 = last = plat_now_ns();
    }
    if (pose < 0 || !m2_gl_figures_ready()) return;
    const uint64_t now = plat_now_ns();
    const float t = (float)(now - t0) * 1e-9f, dt = (float)(now - last) * 1e-9f;
    last = now;
    static const float id[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    glClear(GL_DEPTH_BUFFER_BIT);
    m2_gl_figure_begin(p, id);
    for (int who = 0; who < 2; who++) {
        AvatarPose ap;
        memset(&ap, 0, sizeof ap);
        float hx = 0, hy = 1.66f, hz = 0, yaw = 0, pitch = 0;
        if (pose == 1) {
            ap.hand[0] = ap.hand[1] = 1;
            ap.wrist[1][0] = 0.10f; ap.wrist[1][1] = 1.36f; ap.wrist[1][2] = -0.34f;
            ap.wrist[0][0] = 0.04f; ap.wrist[0][1] = 1.33f; ap.wrist[0][2] = -0.36f;
        } else if (pose == 2) {
            hy = t < 1 ? 1.66f : 1.15f; yaw = 0.6f; pitch = -0.2f; hz = t < 1 ? 0 : -0.15f;
            ap.hand[1] = 1; ap.wrist[1][0] = 0.35f; ap.wrist[1][1] = 1.45f; ap.wrist[1][2] = -0.25f;
        } else if (pose == 3) {
            hx = 0.6f * sinf(t * 1.5f); yaw = 1.2f * sinf(t * 0.7f);
            ap.hand[1] = 1; ap.wrist[1][0] = hx + 0.2f; ap.wrist[1][1] = 1.3f; ap.wrist[1][2] = -0.4f;
        } else if (pose == 4) hy = 1.15f;
        else if (pose == 5) {   // reaching: one hand far out to the side and up, the other across the body
            ap.hand[0] = ap.hand[1] = 1;
            ap.wrist[1][0] = 0.75f; ap.wrist[1][1] = 1.75f; ap.wrist[1][2] = -0.1f;
            ap.wrist[0][0] = 0.25f; ap.wrist[0][1] = 1.1f; ap.wrist[0][2] = -0.3f;
        }
        const float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
        // the head: turned about y (yaw, + to its left), then looking up or down
        const float h[16] = { cy, 0, sy, 0,  -sy * sp, cp, cy * sp, 0,  -sy * cp, -sp, cy * cp, 0,  hx, hy, hz, 1 };
        memcpy(ap.head, h, sizeof h);
        float part[M2_FIGURE_PARTS][16];
        int draw[M2_FIGURE_PARTS];
        // FLAT_TEST_AVATAR_HANDS=1: the figure's own hands on the arms with a wrist (the right one's with the game's
        // gun), each aimed ahead and a little inwards; the wrists are then those hands' own
        static int own = -1;
        if (own < 0) own = getenv("FLAT_TEST_AVATAR_HANDS") && atoi(getenv("FLAT_TEST_AVATAR_HANDS"));
        float aim[2][16];
        for (int hd = 0; own && hd < 2; hd++) {
            if (!ap.hand[hd]) continue;
            const float ya = hd ? 0.15f : -0.5f, ca = cosf(ya), sa = sinf(ya);
            const float am[16] = { ca, 0, -sa, 0,  0, 1, 0, 0,  sa, 0, ca, 0,  ap.wrist[hd][0], ap.wrist[hd][1] + 0.06f, ap.wrist[hd][2] - 0.1f, 1 };
            memcpy(aim[hd], am, sizeof am);
            avatar_wrist(aim[hd], hd, hd == 1, ap.wrist[hd]);
        }
        avatar_solve(&av[who], &ap, pose == 4 && t < 0.5f ? 60.0f : dt, part, draw);
        for (int hd = 0; own && hd < 2; hd++) {
            if (!ap.hand[hd]) continue;
            const int hp = hd ? M2_FIGURE_HAND_R : M2_FIGURE_HAND_L;
            avatar_hand(part[hp], aim[hd], hd, hd == 1);
            if (hd == 1) memcpy(part[M2_FIGURE_GUN_HAND], part[hp], sizeof part[hp]);
            draw[hd == 1 ? M2_FIGURE_GUN_HAND : hp] = 1;
        }
        // game camera space (x right, y up, z ahead, the game's units: 0.11 m as the headsets') <- avatar space:
        // turned to face the camera, then z negated
        const float a = (180 + turn) * 3.14159265f / 180, c = cosf(a), sn = sinf(a), k = 1 / 0.11f;
        const float x0 = who ? 0.55f : -0.55f;
        const float m[16] = { c * k, 0, sn * k, 0,  0, k, 0, 0,  sn * k, 0, -c * k, 0,  x0 * k, -0.95f * k, dist * k, 1 };
        for (int i = 0; i < M2_FIGURE_PARTS; i++) {
            float w[16];
            for (int col = 0; col < 4; col++)
                for (int r = 0; r < 4; r++) {
                    float sum = 0;
                    for (int q = 0; q < 4; q++) sum += m[q * 4 + r] * part[i][col * 4 + q];
                    w[col * 4 + r] = sum;
                }
            memcpy(part[i], w, sizeof w);
            if (draw[i] && (mask >> i & 1)) m2_gl_figure_part(who, i, w);
        }
        m2_gl_figure_waist(who, part[M2_FIGURE_PELVIS], part[M2_FIGURE_CHEST]);
    }
    m2_gl_figure_end();
    glEnable(GL_DEPTH_TEST);
}
#endif
static void render_picture(void) {
    const VrScene *sc = F.scene;
#ifdef GL_FRAMEBUFFER_SRGB
    // desktop GL (Windows) encodes into an sRGB target only when asked (GLES always does): without it the picture
    // kept linear values and showed far too dark (MAME's mid tones at 0.6, measured)
    glEnable(GL_FRAMEBUFFER_SRGB);
#endif
    glBindFramebuffer(GL_FRAMEBUFFER, F.ms_fbo ? F.ms_fbo : F.pic_fbo);
    glViewport(0, 0, F.pic_w, F.pic_h);
    const uint32_t c = sc ? sc->clear : 0;
    glClearColor(lin(c >> 16), lin(c >> 8), lin(c), 1);
    glClearDepthf(1);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    float p[16];
    game_proj(p, sc);
    float near_u = 1.0f;
    {   // FLAT_TEST_NEARFAR=near,far (game units; tests): the depth's planes another renderer uses (the Quest's world:
        // 0.2 m and 1000 m at its 0.11 m a unit, 1.818,9091), its depth buffer's precision with them
        static int on = -1; static float n, f;
        if (on < 0) { const char *e = getenv("FLAT_TEST_NEARFAR"); on = e && sscanf(e, "%f,%f", &n, &f) == 2 && n > 0 && f > n; }
        if (on) { p[10] = (f + n) / (f - n); p[14] = -2 * f * n / (f - n); near_u = n; }
    }
    {   // FLAT_TEST_LOOK=pitch,yaw,fov[,dx,dy,dz] (degrees, game units; tests): the camera turned (pitch + looks down)
        // with that field of view, and moved by dx, dy, dz in its own space (z ahead), as a headset looks round the
        // game's world from a head that leans away from the arcade's camera (the VR scenery hook sends all of it)
        static int on = -1; static float pitch, yaw, fov, d[3];
        if (on < 0) { const char *e = getenv("FLAT_TEST_LOOK"); on = e && sscanf(e, "%f,%f,%f,%f,%f,%f", &pitch, &yaw, &fov, &d[0], &d[1], &d[2]) >= 3; }
        if (on) {
            const float a = pitch * 3.14159265f / 180, b = yaw * 3.14159265f / 180, t = 1.0f / tanf(fov * 3.14159265f / 360);
            const float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b);
            // R = pitch (about x) after yaw (about y), column-major; then the projection's x and y scales set to t
            const float r[16] = { cb, -sa * sb, ca * sb, 0,   0, ca, sa, 0,   -sb, -sa * cb, ca * cb, 0,   0, 0, 0, 1 };
            float q[16];
            q[0] = t * (p[0] > 0 ? 1.f : -1.f); q[5] = t * (float)F.pic_w / (float)F.pic_h * (p[5] > 0 ? 1.f : -1.f);
            for (int i = 0; i < 16; i++) if (i != 0 && i != 5) q[i] = p[i];
            for (int c = 0; c < 4; c++)
                for (int rr = 0; rr < 4; rr++) {
                    float s = 0;
                    for (int k = 0; k < 4; k++) s += q[k * 4 + rr] * r[c * 4 + k];
                    p[c * 4 + rr] = s;
                }
            for (int rr = 0; rr < 4; rr++) p[12 + rr] -= p[rr] * d[0] + p[4 + rr] * d[1] + p[8 + rr] * d[2];
        }
    }
    m2_gl_near(near_u);
    m2_gl_draw(p);
#ifdef _WIN32
    flat_test_avatar(p);
#endif
    if (F.tex_w) {
        const float q[] = { -1, 1, 0, 0, 0,  1, 1, 0, 1, 0,  1, -1, 0, 1, 1,  -1, 1, 0, 0, 0,  1, -1, 0, 1, 1,  -1, -1, 0, 0, 1 };
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        tex_quad(identity, q, F.screen_tex, (float)F.tex_w, (float)F.tex_h, 1);
        glDisable(GL_BLEND);
    }
    draw_fade();
    if (F.ms_fbo) {   // resolve
        glBindFramebuffer(GL_READ_FRAMEBUFFER, F.ms_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, F.pic_fbo);
        glBlitFramebuffer(0, 0, F.pic_w, F.pic_h, 0, 0, F.pic_w, F.pic_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
#ifndef _WIN32
        // its samples aren't needed once resolved: a tiling GPU (ANGLE's Metal, phones) then doesn't write them out.
        // Without it the Apple TV 4K's 2880 x 2160 picture stored 4x colour and depth each frame (45 fps, 2026-11-20).
        const GLenum all[3] = { GL_COLOR_ATTACHMENT0, GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT };
        glInvalidateFramebuffer(GL_READ_FRAMEBUFFER, 3, all);
        glInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, 2, all + 1);   // (the resolved picture's depth: never drawn)
#endif
    }
#ifdef GL_FRAMEBUFFER_SRGB
    glDisable(GL_FRAMEBUFFER_SRGB);   // (the window's pass encodes itself)
#endif
}

void flat_draw(int w, int h, unsigned framebuffer) {
    // the newest scene and 2D frame from the game thread
    const VrScene *sc = d3d_vr_scene();
    if (sc && sc->serial != F.scene_serial) {
        F.scene = sc;
        F.scene_serial = sc->serial;
        m2_gl_sync();
        F.dirty = 1;
    }
    int fw, fh;
    if (qframe_take(F.frame, FRAME_MAX, &fw, &fh)) {
        const int r = F.ring_i = (F.ring_i + 1) % 3;
        F.screen_tex = F.ring[r];
        glBindTexture(GL_TEXTURE_2D, F.screen_tex);
        if (fw != F.ring_w[r] || fh != F.ring_h[r]) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, fw, fh, 0, GL_RGBA, GL_UNSIGNED_BYTE, F.frame);
            F.ring_w[r] = fw; F.ring_h[r] = fh;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, F.frame);
        }
        F.tex_w = fw; F.tex_h = fh;
        F.dirty = 1;
    }
    // the picture's place: 4:3 (the arcade monitor's shape), as large as the window allows
    if (w * 3 > h * 4) { F.vh = h; F.vw = h * 4 / 3; } else { F.vw = w; F.vh = w * 3 / 4; }
    F.vx = (w - F.vw) / 2; F.vy = (h - F.vh) / 2;
    if (F.vw < 1 || F.vh < 1) return;
    if (F.picture == FLAT_PICTURE_ORIGINAL) picture_size(sc ? sc->w : 496, sc ? sc->h : 384, 1);
    else if (F.max_lines && F.vh > F.max_lines)
        picture_size(F.max_lines * 4 / 3, F.max_lines, F.max_samples && F.max_samples < F.ms ? F.max_samples : F.ms);
    else picture_size(F.vw, F.vh, F.max_samples && F.max_samples < F.ms ? F.max_samples : F.ms);
    if (F.dirty) { F.dirty = 0; render_picture(); }
    // onto the window
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(F.vx, h - F.vy - F.vh, F.vw, F.vh);
    const float q[] = { -1, 1, 0, 0, 1,  1, 1, 0, 1, 1,  1, -1, 0, 1, 0,  -1, 1, 0, 0, 1,  1, -1, 0, 1, 0,  -1, -1, 0, 0, 0 };
    glUseProgram(F.prog_out);
    glUniform2f(F.u_out_size, (float)F.pic_w, (float)F.pic_h);
    glUniform1i(F.u_out_sharp, F.picture == FLAT_PICTURE_ORIGINAL);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, F.pic_tex);
    stream(q, 6, 5);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glViewport(0, 0, w, h);
    flat_touch_draw(w, h);
    flat_menu_draw(w, h);
    {   // M2_SHOTS (arcade.cpp, tests): the game waits after one of those frames until its picture is saved
        extern volatile uint64_t arcade_shot_serial;
        extern volatile uint32_t arcade_shot_frame;
        if (arcade_shot_serial && F.scene_serial == arcade_shot_serial) {
            char name[64];
            snprintf(name, sizeof name, "shot_%05u.ppm", (unsigned)arcade_shot_frame);
            unsigned char *px = malloc((size_t)F.vw * F.vh * 4);
            FILE *f = px ? fopen(name, "wb") : NULL;
            if (f) {   // the picture alone (its 4:3 place in the window)
                glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
                glReadPixels(F.vx, h - F.vy - F.vh, F.vw, F.vh, GL_RGBA, GL_UNSIGNED_BYTE, px);
                fprintf(f, "P6\n%d %d\n255\n", F.vw, F.vh);
                for (int y = F.vh - 1; y >= 0; y--)
                    for (int x = 0; x < F.vw; x++) fwrite(px + ((size_t)y * F.vw + x) * 4, 1, 3, f);
                fclose(f);
            }
            free(px);
            arcade_shot_serial = 0;
        }
    }
    {   // FLAT_SHOT=seconds[,seconds...] (tests): the picture as flat_shot.ppm in the files folder, that long after the
        // first frame (the later ones flat_shot_2.ppm, flat_shot_3.ppm, ...)
        static double at[8] = { -2 };
        static int n, next;
        static uint64_t t0;
        if (at[0] == -2) {
            const char *e = getenv("FLAT_SHOT");
            at[0] = -1;
            for (const char *p = e; p && *p && n < 8; p = strchr(p, ',') ? strchr(p, ',') + 1 : NULL) at[n++] = atof(p);
            t0 = plat_now_ns();
        }
        if (next < n && (double)(plat_now_ns() - t0) * 1e-9 >= at[next]) {
            char name[32] = "flat_shot.ppm";
            if (next) snprintf(name, sizeof name, "flat_shot_%d.ppm", next + 1);
            next++;
            unsigned char *px = malloc((size_t)w * h * 4);
            FILE *f = px ? fopen(name, "wb") : NULL;
            if (f) {
                glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
                glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
                fprintf(f, "P6\n%d %d\n255\n", w, h);
                for (int y = h - 1; y >= 0; y--)
                    for (int x = 0; x < w; x++) fwrite(px + ((size_t)y * w + x) * 4, 1, 3, f);
                fclose(f);
                LOGI("%s: %dx%d", name, w, h);
            }
            free(px);
        }
    }
}

// ---------------------------------------------------------------- input
// window pixels -> the game's frame pixels (outside the picture: outside the frame, which reloads)
static void to_frame(int x, int y, int *fx, int *fy) {
    int w, h;
    qframe_size(&w, &h);
    if (!w || !h) { w = 496; h = 384; }
    *fx = F.vw ? (int)floorf((float)(x - F.vx) * (float)w / (float)F.vw) : -1;
    *fy = F.vh ? (int)floorf((float)(y - F.vy) * (float)h / (float)F.vh) : -1;
}
int flat_pointer_in_view(int x, int y) { return x >= F.vx && x < F.vx + F.vw && y >= F.vy && y < F.vy + F.vh; }
void flat_view_rect(int *x, int *y, int *w, int *h) { *x = F.vx; *y = F.vy; *w = F.vw; *h = F.vh; }

// ---- the players (flat.h): which player each device drives
#define MAX_DEVS 32
static struct {
    struct { int dev, x, y, aimed, fire, reload, p1, pad_gone; } d[MAX_DEVS];   // the devices seen
    int n;
    int p2;            // player 2's device (0: none)
    int game_on;       // a game on (flat_players_frame), as last seen
    int pads;          // pad ids handed out (FLAT_DEV_PAD + 0 .. pads - 1)
    int last[2];       // the device each player used last (flat_player_device)
} P;
static int dev_slot(int dev) {
    for (int i = 0; i < P.n; i++) if (P.d[i].dev == dev) return i;
    if (P.n == MAX_DEVS) return MAX_DEVS - 1;   // (never: a few devices)
    memset(&P.d[P.n], 0, sizeof P.d[P.n]);
    P.d[P.n].dev = dev;
    return P.n++;
}
extern int net_local;   // (arcade_net.cpp: this player's slot online)
static void trig_forget(int dev);   // (rumble, below)
static int seat_open(void) { return !online_session(); }   // online, player 2 is the partner
int flat_dev_player(int dev) { return P.p2 && dev == P.p2 && seat_open() ? 1 : 0; }
int flat_player2_in(void) { return P.p2 && seat_open(); }

// a player's gun (0: quest_*, 1: quest_*2)
static void gun_aim(int pl, int x, int y) {
    int fx, fy;
    to_frame(x, y, &fx, &fy);
    if (pl) quest_pointer2(fx, fy); else quest_pointer(fx, fy);
}
static void gun_button(int pl, int button, int down) { if (pl) quest_mouse_button2(button, down); else quest_mouse_button(button, down); }

static void p2_leave(const char *why) {
    if (!P.p2) return;
    LOGI("players: player 2 (device %d) leaves: %s", P.p2, why);
    const int i = dev_slot(P.p2);
    P.d[i].fire = P.d[i].reload = 0;
    P.p2 = 0;
    P.last[1] = 0;
    quest_player2(0);
}
int flat_player_device(int p) { return p >= 0 && p < 2 ? P.last[p] : 0; }
static void used(int dev) { P.last[flat_dev_player(dev)] = dev; }
// every frame (flat_app_frame): online, or the game over (the ranking, the title), ends player 2's seat and forgets
// which devices were player 1's
void flat_players_frame(void) {
    if (P.p2 && online_session()) p2_leave("online play");
    // a game on: either player in it, coming in (the states between: 1 and 3 seen), or on the continue countdown; the
    // ranking ends it (0: the game's boot)
    const int s1 = host_player_state(0), s2 = host_player_state(1);
    const int on = (s1 && s1 != HOST_P_RANKING && s1 != HOST_P_NONE) || (s2 && s2 != HOST_P_RANKING && s2 != HOST_P_NONE);
    {   // (the log: each player's state and play, for the tests)
        static int was = -1;
        const int now = s1 | s2 << 8 | host_player_playing(0) << 16 | host_player_playing(1) << 17 | on << 18;
        if (now != was) LOGI("players: game %s; player 1 state %d%s, player 2 state %d%s", on ? "on" : "off", s1,
                             host_player_playing(0) ? " (playing)" : "", s2, host_player_playing(1) ? " (playing)" : "");
        was = now;
    }
    if (P.game_on && !on) {
        p2_leave("the game is over");
        for (int i = 0; i < P.n; i++) P.d[i].p1 = 0;
    }
    P.game_on = on;
}

void flat_dev_pointer(int dev, int x, int y) {
    const int i = dev_slot(dev);
    // a device aims when it moves (a pad's sight sent every frame doesn't take the aim from the mouse)
    if (P.d[i].aimed && P.d[i].x == x && P.d[i].y == y) return;
    P.d[i].x = x; P.d[i].y = y; P.d[i].aimed = 1;
    gun_aim(flat_dev_player(dev), x, y);
}
void flat_dev_trigger(int dev, int down) {
    const int i = dev_slot(dev), pl = flat_dev_player(dev);
    if (down && P.d[i].aimed) gun_aim(pl, P.d[i].x, P.d[i].y);   // the shot goes where this device aims
    if (down) used(dev);
    P.d[i].fire = down;
    gun_button(pl, SDL_BUTTON_LEFT, down);
}
void flat_dev_reload(int dev, int down) {
    const int i = dev_slot(dev);
    P.d[i].reload = down;
    if (down) used(dev);
    gun_button(flat_dev_player(dev), SDL_BUTTON_RIGHT, down);
}
// Start: player 2's on player 2's device. While player 1 plays (host_player_playing: not the continue countdown, a
// scene, the title) and player 2 isn't in the game (never joined, or out: their countdown, their game over), alone
// (not online), a Start on a device that isn't player 1's joins: that device is player 2's from then on. Every other
// Start is player 1's, and makes its device player 1's for the rest of this game (starting, continuing, skipping a
// scene).
void flat_dev_start(int dev, int down) {
    const int i = dev_slot(dev);
    if (!down) { if (!flat_dev_player(dev)) quest_key(SDL_SCANCODE_SPACE, SDLK_SPACE, 0); return; }
    if (flat_dev_player(dev)) { used(dev); quest_start2(); return; }
    if (seat_open() && !P.d[i].p1 && host_player_playing(0) && host_player_state(1) != HOST_P_IN) {
        if (P.p2) p2_leave("another device's Start");
        P.p2 = dev;
        LOGI("players: device %d joins as player 2", dev);
        quest_player2(1);
        used(dev);
        if (P.d[i].aimed) gun_aim(1, P.d[i].x, P.d[i].y);
        quest_start2();
        return;
    }
    P.d[i].p1 = 1;
    used(dev);
    quest_key(SDL_SCANCODE_SPACE, SDLK_SPACE, 1);
}

// ---- rumble (flat.h): each event's feel, the same on every pad; touch's haptics go by the event
static const struct { float low, high; int ms; const char *name; } rumble_feel[] = {
    [FLAT_RUMBLE_SHOT] = { 0.25f, 0.75f, 70, "shot" },      // a short kick
    [FLAT_RUMBLE_RELOAD] = { 0.20f, 0.20f, 90, "reload" },  // a softer pulse
    [FLAT_RUMBLE_HIT] = { 1.00f, 0.70f, 450, "hit" },       // strong and longer
};
static struct { int dev; uint64_t until; } running[4];   // rumbles that may still be going (stopped on a pause)
static int rumble_log = -1;
static void rumble(int pl, int dev, int what) {
    const uint64_t now = plat_now_ns();
    flat_os_rumble(dev, what, rumble_feel[what].low, rumble_feel[what].high, rumble_feel[what].ms);
    if (rumble_log) LOGI("rumble: player %d %s on device %d (low %.2f, high %.2f, %d ms)", pl + 1, rumble_feel[what].name, dev,
                         rumble_feel[what].low, rumble_feel[what].high, rumble_feel[what].ms);
    int k = -1;   // the device's slot, else the one that ends first
    for (int i = 0; i < 4; i++) if (running[i].dev == dev) k = i;
    if (k < 0) { k = 0; for (int i = 1; i < 4; i++) if (running[i].until < running[k].until) k = i; running[k].until = 0; }
    const uint64_t until = now + (uint64_t)rumble_feel[what].ms * 1000000u;
    running[k].dev = dev;
    if (running[k].until < until) running[k].until = until;
}
// Trigger feedback: each pad's triggers as last told ([0] left, [1] right), a buzz's end
#define TRIG_BUZZ_SHOT_MS 100
#define TRIG_BUZZ_RELOAD_MS 80
static struct Trig { int dev, sent[2]; uint64_t until[2]; } trig[4];
static const char *const trig_names[] = { "off", "weapon", "buzz" };
static struct Trig *trig_of(int dev, int make) {
    for (int i = 0; i < 4; i++) if (trig[i].dev == dev) return &trig[i];
    if (!make) return NULL;
    for (int i = 0; i < 4; i++)
        if (!trig[i].dev) { memset(&trig[i], 0, sizeof trig[i]); trig[i].dev = dev; return &trig[i]; }
    return NULL;   // (never: a pad or two)
}
static void trig_send(struct Trig *t, int right, int mode, float amp, int ms, const char *why) {
    flat_os_trigger(t->dev, right, mode, amp, ms);
    if (rumble_log) LOGI("trigger: device %d %s %s (%s)", t->dev, right ? "right" : "left", trig_names[mode], why);
    t->sent[right] = mode;
    t->until[right] = mode == FLAT_TRIG_BUZZ ? plat_now_ns() + (uint64_t)ms * 1000000u : 0;
}
static void trig_buzz(int dev, int right, float amp, int ms, const char *why) {
    struct Trig *t = trig_of(dev, 1);
    if (t) trig_send(t, right, FLAT_TRIG_BUZZ, amp, ms, why);
}
// a pad gone: its triggers forgotten (it comes back loose)
static void trig_forget(int dev) {
    struct Trig *t = trig_of(dev, 0);
    if (t) memset(t, 0, sizeof *t);
}

void flat_rumble_frame(int paused) {
    static HostEventsSeen seen;
    if (rumble_log < 0) rumble_log = getenv("FLAT_TEST_RUMBLE") && atoi(getenv("FLAT_TEST_RUMBLE"));
    const int off = paused || flat_menu_is_open() || !flat_opt_get(QOPT_RUMBLE);
    const int trig_on = !off && flat_opt_get(QOPT_TRIGGER);
    const int online = online_session();
    int weapon_dev[2] = { 0, 0 };   // the pads whose right trigger is a gun's now (their player plays)
    for (int slot = 0; slot < 2; slot++) {
        const int ev = host_events_new(&seen, slot);   // (taken while off too: they don't come later)
        // the player here who is this slot: online only this player (every device here drives it: player 0's
        // devices); alone player 1, and player 2 while a device holds the seat
        int pl = slot;
        if (online) { if (slot != net_local) continue; pl = 0; }
        else if (slot == 1 && !flat_player2_in()) continue;
        const int dev = P.last[pl];
        if (!dev) continue;
        const int pad = dev >= FLAT_DEV_PAD;   // (trigger feedback: pads only)
        if (pad && trig_on && host_player_playing(slot)) weapon_dev[pl] = dev;
        if (!ev || off) continue;
        // one at a time, the strongest: a hit over a shot over a reload
        rumble(slot, dev, ev & 1 << HOST_EV_HIT ? FLAT_RUMBLE_HIT : ev & 1 << HOST_EV_SHOT ? FLAT_RUMBLE_SHOT : FLAT_RUMBLE_RELOAD);
        if (pad && trig_on && ev & 1 << HOST_EV_SHOT) trig_buzz(dev, 1, 1.0f, TRIG_BUZZ_SHOT_MS, "a shot");
        if (pad && trig_on && ev & 1 << HOST_EV_RELOAD) trig_buzz(dev, 0, 0.35f, TRIG_BUZZ_RELOAD_MS, "a reload");
    }
    // each pad's triggers: a buzz runs out; then the right one is a gun's while its player plays, else loose
    const uint64_t now = plat_now_ns();
    for (int pl = 0; pl < 2; pl++) if (weapon_dev[pl]) trig_of(weapon_dev[pl], 1);
    for (int i = 0; i < 4; i++) {
        struct Trig *t = &trig[i];
        if (!t->dev) continue;
        const int weapon = t->dev == weapon_dev[0] || t->dev == weapon_dev[1];
        for (int right = 0; right < 2; right++) {
            if (t->sent[right] == FLAT_TRIG_BUZZ && trig_on && now < t->until[right]) continue;
            const int want = right && weapon ? FLAT_TRIG_WEAPON : FLAT_TRIG_OFF;
            if (t->sent[right] != want)
                trig_send(t, right, want, 0, 0, want ? "in play" : !trig_on ? (paused ? "paused" : "the menu, or the setting off")
                                                                          : "not playing");
        }
        if (!t->sent[0] && !t->sent[1] && !weapon) t->dev = 0;   // (loose: forgotten)
    }
    if (off) {   // paused, the menu, Rumble OFF: what's still going stops
        for (int i = 0; i < 4; i++)
            if (running[i].dev && running[i].until > now) {
                flat_os_rumble(running[i].dev, FLAT_RUMBLE_SHOT, 0, 0, 0);
                if (rumble_log) LOGI("rumble: stopped on device %d (%s)", running[i].dev, paused ? "paused" : "the menu or Rumble off");
                running[i].until = 0;
            }
    }
}
void flat_rumble_stop(void) {
    const uint64_t now = plat_now_ns();
    for (int i = 0; i < 4; i++)
        if (running[i].dev && running[i].until > now) { flat_os_rumble(running[i].dev, FLAT_RUMBLE_SHOT, 0, 0, 0); running[i].until = 0; }
    for (int i = 0; i < 4; i++) {
        if (!trig[i].dev) continue;
        for (int right = 0; right < 2; right++) if (trig[i].sent[right]) trig_send(&trig[i], right, FLAT_TRIG_OFF, 0, 0, "stopped");
        trig[i].dev = 0;
    }
}

// pads: an id each; a pad that comes back gets a gone pad's (player 2's first), so it drives the same player
int flat_pad_attach(void) {
    int id = 0;
    for (int i = 0; i < P.n && !id; i++) if (P.d[i].pad_gone && P.d[i].dev == P.p2) id = P.d[i].dev;
    for (int i = 0; i < P.n && !id; i++) if (P.d[i].pad_gone) id = P.d[i].dev;
    if (!id) id = FLAT_DEV_PAD + P.pads++;
    P.d[dev_slot(id)].pad_gone = 0;
    LOGI("players: pad %d connected%s", id - FLAT_DEV_PAD + 1, flat_dev_player(id) ? " (player 2's)" : "");
    return id;
}
void flat_pad_detach(int dev) {
    const int i = dev_slot(dev);
    if (P.d[i].fire) flat_dev_trigger(dev, 0);
    if (P.d[i].reload) flat_dev_reload(dev, 0);
    P.d[i].pad_gone = 1;
    P.d[i].aimed = 0;
    trig_forget(dev);
    LOGI("players: pad %d gone%s", dev - FLAT_DEV_PAD + 1, flat_dev_player(dev) ? " (player 2's: back when it reconnects)" : "");
}

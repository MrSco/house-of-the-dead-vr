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
#include <SDL.h>
#include <math.h>
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
    GLuint screen_tex; int tex_w, tex_h;       // the game's 2D layers
    uint32_t *frame;
    // the picture: a resolved sRGB texture (and a multisampled framebuffer drawn into first, in HD)
    GLuint pic_fbo, pic_tex, pic_depth, ms_fbo, ms_col, ms_depth;
    int pic_w, pic_h, ms;
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
    glGenTextures(1, &F.screen_tex);
    glBindTexture(GL_TEXTURE_2D, F.screen_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    F.frame = malloc(FRAME_MAX * 4);
    GLint max_samples = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
    F.ms = max_samples >= 4 ? 4 : 1;
    m2_gl_init();
    quest_overlay_key = D3D_VR_KEY;   // the game's 2D layers: this colour is see-through (arcade.cpp)
    return F.frame != NULL;
}

void flat_redraw(void) { F.dirty = 1; }   // (a setting changed while the game is paused)
void flat_set_picture(int mode) {
    if (mode == F.picture) return;
    F.picture = mode;
    F.dirty = 1;
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
static void render_picture(void) {
    const VrScene *sc = F.scene;
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
    m2_gl_near(1.0f);
    m2_gl_draw(p);
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
    }
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
        glBindTexture(GL_TEXTURE_2D, F.screen_tex);
        if (fw != F.tex_w || fh != F.tex_h) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, fw, fh, 0, GL_RGBA, GL_UNSIGNED_BYTE, F.frame);
            F.tex_w = fw; F.tex_h = fh;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, F.frame);
        }
        F.dirty = 1;
    }
    // the picture's place: 4:3 (the arcade monitor's shape), as large as the window allows
    if (w * 3 > h * 4) { F.vh = h; F.vw = h * 4 / 3; } else { F.vw = w; F.vh = w * 3 / 4; }
    F.vx = (w - F.vw) / 2; F.vy = (h - F.vh) / 2;
    if (F.vw < 1 || F.vh < 1) return;
    if (F.picture == FLAT_PICTURE_ORIGINAL) picture_size(sc ? sc->w : 496, sc ? sc->h : 384, 1);
    else picture_size(F.vw, F.vh, F.ms);
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
    {   // FLAT_SHOT=seconds (tests): the picture as flat_shot.ppm in the files folder, that long after the first frame
        static double at = -2;
        static uint64_t t0;
        if (at == -2) { const char *e = getenv("FLAT_SHOT"); at = e && *e ? atof(e) : -1; t0 = plat_now_ns(); }
        if (at >= 0 && (double)(plat_now_ns() - t0) * 1e-9 >= at) {
            at = -1;
            unsigned char *px = malloc((size_t)w * h * 4);
            FILE *f = px ? fopen("flat_shot.ppm", "wb") : NULL;
            if (f) {
                glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
                glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
                fprintf(f, "P6\n%d %d\n255\n", w, h);
                for (int y = h - 1; y >= 0; y--)
                    for (int x = 0; x < w; x++) fwrite(px + ((size_t)y * w + x) * 4, 1, 3, f);
                fclose(f);
                LOGI("flat_shot.ppm: %dx%d", w, h);
            }
            free(px);
        }
    }
}

// ---------------------------------------------------------------- input
static int last_x, last_y;
// window pixels -> the game's frame pixels (outside the picture: outside the frame, which reloads)
static void to_frame(int x, int y, int *fx, int *fy) {
    int w, h;
    qframe_size(&w, &h);
    if (!w || !h) { w = 496; h = 384; }
    *fx = F.vw ? (int)floorf((float)(x - F.vx) * (float)w / (float)F.vw) : -1;
    *fy = F.vh ? (int)floorf((float)(y - F.vy) * (float)h / (float)F.vh) : -1;
}
void flat_pointer(int x, int y) {
    last_x = x; last_y = y;
    int fx, fy;
    to_frame(x, y, &fx, &fy);
    quest_pointer(fx, fy);
}
int flat_pointer_in_view(int x, int y) { return x >= F.vx && x < F.vx + F.vw && y >= F.vy && y < F.vy + F.vh; }
void flat_view_rect(int *x, int *y, int *w, int *h) { *x = F.vx; *y = F.vy; *w = F.vw; *h = F.vh; }
void flat_trigger(int down) { flat_pointer(last_x, last_y); quest_mouse_button(SDL_BUTTON_LEFT, down); }
void flat_reload(int down) { quest_mouse_button(SDL_BUTTON_RIGHT, down); }
void flat_start(int down) { quest_key(SDL_SCANCODE_SPACE, SDLK_SPACE, down); }

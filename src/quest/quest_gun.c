// The gun model (quest_gun.h). Adapted from the Time Crisis 2 VR project's quest/quest_gun.c (MIT licence).
//
// TCGUN002: "TCGUN002", vertex count, index count (uint32), muzzle (3 floats), then per vertex position, normal,
// colour (sRGB 0..1) and part (0 body, 1 trigger, 2 slide: they move with the trigger and the recoil), then the
// indices. TCGUN001 (older): counts, texture size, muzzle; vertices with position, normal, uv; indices; RGBA texels.
#include <android/asset_manager.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "quest_gun.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "hotdvr", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "hotdvr", __VA_ARGS__)

typedef struct { float pos[3], normal[3], uv[2], color[3], part; } GunVertex;
static struct {
    GLuint vao, vbo, ibo, tex, prog, flash_prog, flash_vao, flash_vbo;
    GLuint cap_vao, cap_vbo, cap_ibo; GLsizei cap_count; float cap_top[3];   // the grip's base plate (qgun_draw_cap)
    GLsizei count;
    float muzzle[3];
    float grip[3];                       // the grip's base (qgun_grip_base)
    float butt[3];                       // its lowest point (qgun_butt): the grip's base before a cut
    int articulated;
    GLint u_view, u_proj, u_model, u_recoil, u_trigger, u_tint, u_albedo;
    GLint f_view, f_proj, f_model, f_tip, f_size, f_strength;
} G;

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char b[1024]; glGetShaderInfoLog(s, sizeof b, NULL, b); LOGE("gun shader: %s", b); glDeleteShader(s); return 0; }
    return s;
}
static GLuint link(const char *vs, const char *fs) {
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) { glDeleteShader(v); glDeleteShader(f); return 0; }
    GLuint p = glCreateProgram();
    glAttachShader(p, v); glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v); glDeleteShader(f);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { glDeleteProgram(p); return 0; }
    return p;
}

// The swapchain is sRGB: the shaders work and output in linear light.
static const char *VS =
    "#version 300 es\nprecision highp float;\n"
    "layout(location=0) in vec3 position; layout(location=1) in vec3 normal; layout(location=2) in vec2 texcoord;\n"
    "layout(location=3) in vec3 vcolor; layout(location=4) in float part;\n"
    "uniform mat4 view, projection, model; uniform float recoil, triggerPull;\n"
    "out vec3 n, p, color; out vec2 uv;\n"
    "void main() {\n"
    "  vec3 pos = position;\n"
    "  if (part > 1.5) pos.z += 0.034 * recoil; else if (part > 0.5) pos.z += 0.003 * triggerPull;\n"
    "  mat4 mv = view * model; vec4 v = mv * vec4(pos, 1.0);\n"
    "  p = v.xyz; n = mat3(mv) * normal; uv = texcoord; color = vcolor; gl_Position = projection * v;\n"
    "}\n";
static const char *FS =
    "#version 300 es\nprecision highp float;\n"
    "in vec3 n, p, color; in vec2 uv; uniform sampler2D albedo; uniform vec3 tint; out vec4 frag;\n"
    "void main() {\n"
    "  vec3 N = normalize(n); if (!gl_FrontFacing) N = -N;\n"
    "  vec3 L = normalize(vec3(-0.4, 0.8, 0.6)), V = normalize(-p), H = normalize(L + V);\n"
    "  vec3 base = texture(albedo, uv).rgb * color * tint;\n"
    "  float diffuse = 0.48 + 0.52 * max(dot(N, L), 0.0), spec = 0.16 * pow(max(dot(N, H), 0.0), 40.0);\n"
    "  frag = vec4(clamp(base * diffuse + spec, 0.0, 1.0), 1.0);\n"
    "}\n";
static const char *VS_FLASH =
    "#version 300 es\nprecision highp float;\n"
    "layout(location=0) in vec3 position; layout(location=1) in float edge;\n"
    "uniform mat4 view, projection, model; uniform vec3 tip; uniform float size; out float glow;\n"
    "void main() { glow = edge; gl_Position = projection * view * model * vec4(tip + position * size, 1.0); }\n";
static const char *FS_FLASH =
    "#version 300 es\nprecision highp float;\n"
    "in float glow; uniform float strength; out vec4 frag;\n"
    "void main() { float a = glow * strength; frag = vec4(vec3(1.0, 0.62 + 0.28 * glow, 0.12 + 0.60 * glow) * a, a); }\n";

static float srgb_to_linear(float c) { return c <= .04045f ? c / 12.92f : powf((c + .055f) / 1.055f, 2.4f); }

// reads exactly n bytes from the asset
static int rd(AAsset *a, void *p, size_t n) { return AAsset_read(a, p, n) == (int)n; }

int qgun_load(struct AAssetManager *am, const char *name) {
    AAsset *a = am ? AAssetManager_open(am, name, AASSET_MODE_STREAMING) : NULL;
    if (!a) { LOGE("gun: no asset %s", name); return 0; }
    char magic[8];
    uint32_t n[4] = { 0, 0, 1, 1 };
    float tip[3];
    GunVertex *v = NULL;
    uint32_t *idx = NULL;
    uint8_t *px = NULL;
    int ok = 0, legacy = 0;
    float low_y = 1e9f;
    if (!rd(a, magic, 8)) goto done;
    legacy = !memcmp(magic, "TCGUN001", 8);
    if (!legacy && memcmp(magic, "TCGUN002", 8)) goto done;
    if (!rd(a, n, 8) || (legacy && !rd(a, n + 2, 8)) || !rd(a, tip, 12)) goto done;
    if (!n[0] || n[0] > 400000 || !n[1] || n[1] > 1500000 || n[1] % 3 || !n[2] || n[2] > 2048 || !n[3] || n[3] > 2048) goto done;
    v = calloc(n[0], sizeof *v);
    idx = malloc((size_t)n[1] * 4);
    px = malloc((size_t)n[2] * n[3] * 4);
    if (!v || !idx || !px) goto done;
    for (uint32_t i = 0; i < n[0]; i++) {
        float raw[10];
        size_t cnt = legacy ? 8 : 10;
        if (!rd(a, raw, cnt * 4)) goto done;
        for (size_t j = 0; j < cnt; j++) if (!isfinite(raw[j])) goto done;
        if (raw[1] < low_y) low_y = raw[1];
        memcpy(v[i].pos, raw, 12); memcpy(v[i].normal, raw + 3, 12);
        if (legacy) { memcpy(v[i].uv, raw + 6, 8); v[i].color[0] = v[i].color[1] = v[i].color[2] = 1; }
        else {
            if (raw[9] != 0 && raw[9] != 1 && raw[9] != 2) goto done;
            v[i].part = raw[9];
            for (int j = 0; j < 3; j++) { if (raw[6 + j] < 0 || raw[6 + j] > 1) goto done; v[i].color[j] = srgb_to_linear(raw[6 + j]); }
        }
    }
    {   // the grip's base: the middle of the body's lowest centimetre
        double sum[3] = { 0, 0, 0 }; int k = 0;
        for (uint32_t i = 0; i < n[0]; i++) if (v[i].part == 0 && v[i].pos[1] < low_y + 0.01f) { for (int j = 0; j < 3; j++) sum[j] += v[i].pos[j]; k++; }
        if (k) for (int j = 0; j < 3; j++) G.grip[j] = (float)(sum[j] / k);
    }
    if (!rd(a, idx, (size_t)n[1] * 4)) goto done;
    if (legacy) { if (!rd(a, px, (size_t)n[2] * n[3] * 4)) goto done; }
    else memset(px, 255, 4);
    for (uint32_t i = 0; i < n[1]; i++) if (idx[i] >= n[0]) goto done;
    for (int i = 0; i < 3; i++) if (!isfinite(tip[i])) goto done;

    if (G.vao) {   // a model loaded before (the other player's gun): its GL objects go
        if (G.cap_vao) {
            glDeleteVertexArrays(1, &G.cap_vao);
            GLuint cb[2] = { G.cap_vbo, G.cap_ibo };
            glDeleteBuffers(2, cb);
            G.cap_vao = G.cap_vbo = G.cap_ibo = 0; G.cap_count = 0;
        }
        glDeleteVertexArrays(1, &G.vao); glDeleteVertexArrays(1, &G.flash_vao);
        GLuint b[3] = { G.vbo, G.ibo, G.flash_vbo };
        glDeleteBuffers(3, b);
        glDeleteTextures(1, &G.tex);
        glDeleteProgram(G.prog); glDeleteProgram(G.flash_prog);
        G.vao = G.flash_vao = G.vbo = G.ibo = G.flash_vbo = G.tex = G.prog = G.flash_prog = 0;
    }
    G.prog = link(VS, FS);
    G.flash_prog = link(VS_FLASH, FS_FLASH);
    if (!G.prog || !G.flash_prog) goto done;
    G.u_view = glGetUniformLocation(G.prog, "view"); G.u_proj = glGetUniformLocation(G.prog, "projection");
    G.u_model = glGetUniformLocation(G.prog, "model"); G.u_recoil = glGetUniformLocation(G.prog, "recoil");
    G.u_trigger = glGetUniformLocation(G.prog, "triggerPull"); G.u_tint = glGetUniformLocation(G.prog, "tint");
    G.u_albedo = glGetUniformLocation(G.prog, "albedo");
    G.f_view = glGetUniformLocation(G.flash_prog, "view"); G.f_proj = glGetUniformLocation(G.flash_prog, "projection");
    G.f_model = glGetUniformLocation(G.flash_prog, "model"); G.f_tip = glGetUniformLocation(G.flash_prog, "tip");
    G.f_size = glGetUniformLocation(G.flash_prog, "size"); G.f_strength = glGetUniformLocation(G.flash_prog, "strength");

    glGenTextures(1, &G.tex);
    glBindTexture(GL_TEXTURE_2D, G.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, n[2], n[3], 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenVertexArrays(1, &G.vao);
    glBindVertexArray(G.vao);
    glGenBuffers(1, &G.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)n[0] * sizeof *v, v, GL_STATIC_DRAW);
    glGenBuffers(1, &G.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, G.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)n[1] * 4, idx, GL_STATIC_DRAW);
    for (int i = 0; i < 3; i++) {
        glEnableVertexAttribArray(i);
        glVertexAttribPointer(i, i == 2 ? 2 : 3, GL_FLOAT, GL_FALSE, sizeof(GunVertex), (void *)(size_t)(i == 0 ? 0 : i == 1 ? 12 : 24));
    }
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(GunVertex), (void *)32);
    glEnableVertexAttribArray(4); glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(GunVertex), (void *)44);

    // the grip's base plate (TCGUN002 guns with no slide: the cabinet's): the triangles of the grip's lowest
    // QGUN_CAP_H, dark, yellow along their top, as their own copy
    G.cap_count = 0;
    {
        int slide = 0;
        for (uint32_t i = 0; i < n[0] && !legacy; i++) if (v[i].part > 1.5f) { slide = 1; break; }
        if (!legacy && !slide) {
            float lo = 1e9f;
            for (uint32_t i = 0; i < n[0]; i++) if (v[i].part < 0.5f && v[i].pos[1] < lo) lo = v[i].pos[1];
            const float top = lo + QGUN_CAP_H, band = top - 0.003f;
            uint32_t *map = malloc((size_t)n[0] * 4), *ci = malloc((size_t)n[1] * 4);
            GunVertex *cv = malloc((size_t)n[0] * sizeof *cv);
            uint32_t ncv = 0, nci = 0;
            double cx = 0, cz = 0;
            if (map && ci && cv) {
                for (uint32_t i = 0; i < n[0]; i++) map[i] = 0xffffffffu;
                for (uint32_t t = 0; t + 2 < n[1]; t += 3) {
                    // the triangles reaching below the cut, their corners above it brought down onto it (a clean top)
                    int below = 0, body = 1;
                    for (int k = 0; k < 3; k++) { const GunVertex *q = &v[idx[t + k]]; if (q->part > 0.5f) body = 0; if (q->pos[1] < top) below = 1; }
                    if (!below || !body) continue;
                    for (int k = 0; k < 3; k++) {
                        uint32_t o = idx[t + k];
                        if (map[o] == 0xffffffffu) {
                            map[o] = ncv;
                            cv[ncv] = v[o];
                            if (cv[ncv].pos[1] > top) cv[ncv].pos[1] = top;
                            const int yellow = cv[ncv].pos[1] > band;   // (linear colours: a dark plate, a yellow band)
                            cv[ncv].color[0] = yellow ? 0.80f : 0.012f; cv[ncv].color[1] = yellow ? 0.52f : 0.012f;
                            cv[ncv].color[2] = yellow ? 0.02f : 0.014f;
                            cv[ncv].part = 0;
                            cx += v[o].pos[0]; cz += v[o].pos[2];
                            ncv++;
                        }
                        ci[nci++] = map[o];
                    }
                }
            }
            // the cut's outline (the base's corners on it), its convex hull: a face closing the grip and the base
            float hx[512], hz[512];
            int nh = 0;
            const uint32_t ncv_mesh = ncv;   // (the slice's own corners: the centre below)
            if (nci) {
                float px_[4096], pz_[4096];
                int np = 0;
                for (uint32_t i = 0; i < ncv && np < 4096; i++)
                    if (cv[i].pos[1] >= top - 1e-6f) { px_[np] = cv[i].pos[0]; pz_[np] = cv[i].pos[2]; np++; }
                // monotone chain (x, then z)
                for (int a = 1; a < np; a++) {      // insertion sort, small n
                    float tx = px_[a], tz = pz_[a];
                    int b = a - 1;
                    while (b >= 0 && (px_[b] > tx || (px_[b] == tx && pz_[b] > tz))) { px_[b + 1] = px_[b]; pz_[b + 1] = pz_[b]; b--; }
                    px_[b + 1] = tx; pz_[b + 1] = tz;
                }
                float lx[1024], lz[1024];
                int k = 0;
                for (int pass = 0; pass < 2; pass++) {
                    const int start = k;
                    for (int a0 = 0; a0 < np; a0++) {
                        const int a = pass ? np - 1 - a0 : a0;
                        while (k - start >= 2 && k < 1024) {
                            const float cr = (lx[k - 1] - lx[k - 2]) * (pz_[a] - lz[k - 2]) - (lz[k - 1] - lz[k - 2]) * (px_[a] - lx[k - 2]);
                            if (cr <= 0) k--; else break;
                        }
                        if (k < 1024) { lx[k] = px_[a]; lz[k] = pz_[a]; k++; }
                    }
                    k--;   // (the last point starts the other half)
                }
                for (int a = 0; a < k && nh < 512; a++) { hx[nh] = lx[a]; hz[nh] = lz[a]; nh++; }
            }
            if (nci && nh >= 3 && ncv + nh + 1 <= n[0] && nci + 3 * nh <= n[1]) {   // the base's top: closed
                float mx = 0, mz = 0;
                for (int a = 0; a < nh; a++) { mx += hx[a]; mz += hz[a]; }
                mx /= nh; mz /= nh;
                const uint32_t c0 = ncv;
                GunVertex cvx = { { mx, top, mz }, { 0, 1, 0 }, { 0, 0 }, { 0.012f, 0.012f, 0.014f }, 0 };
                cv[ncv++] = cvx;
                for (int a = 0; a < nh; a++) {
                    GunVertex e = { { hx[a], top, hz[a] }, { 0, 1, 0 }, { 0, 0 }, { 0.012f, 0.012f, 0.014f }, 0 };
                    cv[ncv++] = e;
                }
                for (int a = 0; a < nh; a++) { ci[nci++] = c0; ci[nci++] = c0 + 1 + a; ci[nci++] = c0 + 1 + (a + 1) % nh; }
            }
            if (nci) {
                G.cap_top[0] = (float)(cx / ncv_mesh); G.cap_top[1] = top; G.cap_top[2] = (float)(cz / ncv_mesh);
                // the gun loses that slice: triangles wholly under the cut go, the corners under it of those across
                // it come up onto it (its grip ends flat at the cut, where the base's top is)
                uint32_t kept = 0;
                for (uint32_t t = 0; t + 2 < n[1]; t += 3) {
                    int under = 1;
                    for (int k = 0; k < 3; k++) { const GunVertex *q = &v[idx[t + k]]; if (q->part > 0.5f || q->pos[1] >= top) under = 0; }
                    if (under) continue;
                    idx[kept] = idx[t]; idx[kept + 1] = idx[t + 1]; idx[kept + 2] = idx[t + 2];
                    kept += 3;
                }
                for (uint32_t i = 0; i < n[0]; i++) if (v[i].part < 0.5f && v[i].pos[1] < top) v[i].pos[1] = top;
                if (nh >= 3) {   // the grip's end closed: a dark face across the cut (no hollow grip, no hand through it)
                    GunVertex *v2 = realloc(v, ((size_t)n[0] + nh + 1) * sizeof *v);
                    uint32_t *i2 = realloc(idx, ((size_t)kept + 3 * (size_t)nh) * 4);
                    if (v2) v = v2;
                    if (i2) idx = i2;
                    if (v2 && i2) {
                        float mx = 0, mz = 0;
                        for (int a = 0; a < nh; a++) { mx += hx[a]; mz += hz[a]; }
                        mx /= nh; mz /= nh;
                        const uint32_t c0 = n[0];
                        GunVertex c = { { mx, top, mz }, { 0, -1, 0 }, { 0, 0 }, { 0.02f, 0.02f, 0.022f }, 0 };
                        v[c0] = c;
                        for (int a = 0; a < nh; a++) {
                            GunVertex e = { { hx[a], top, hz[a] }, { 0, -1, 0 }, { 0, 0 }, { 0.02f, 0.02f, 0.022f }, 0 };
                            v[c0 + 1 + a] = e;
                        }
                        for (int a = 0; a < nh; a++) { idx[kept++] = c0; idx[kept++] = c0 + 1 + (a + 1) % nh; idx[kept++] = c0 + 1 + a; }
                        n[0] += nh + 1;
                    }
                }
                glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
                glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)n[0] * sizeof *v, v, GL_STATIC_DRAW);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, G.ibo);
                glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)kept * 4, idx, GL_STATIC_DRAW);
                n[1] = kept;
                G.butt[0] = G.grip[0]; G.butt[1] = G.grip[1]; G.butt[2] = G.grip[2];
                G.grip[0] = G.cap_top[0]; G.grip[1] = top; G.grip[2] = G.cap_top[2];   // magazines go in at the cut
                glGenVertexArrays(1, &G.cap_vao);
                glBindVertexArray(G.cap_vao);
                glGenBuffers(1, &G.cap_vbo);
                glBindBuffer(GL_ARRAY_BUFFER, G.cap_vbo);
                glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)ncv * sizeof *cv, cv, GL_STATIC_DRAW);
                glGenBuffers(1, &G.cap_ibo);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, G.cap_ibo);
                glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)nci * 4, ci, GL_STATIC_DRAW);
                for (int i = 0; i < 3; i++) {
                    glEnableVertexAttribArray(i);
                    glVertexAttribPointer(i, i == 2 ? 2 : 3, GL_FLOAT, GL_FALSE, sizeof(GunVertex), (void *)(size_t)(i == 0 ? 0 : i == 1 ? 12 : 24));
                }
                glEnableVertexAttribArray(3); glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(GunVertex), (void *)32);
                glEnableVertexAttribArray(4); glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(GunVertex), (void *)44);
                glBindVertexArray(0);
                G.cap_count = (GLsizei)nci;
                LOGI("gun: the grip's base plate, %u triangles", nci / 3);
            }
            free(map); free(ci); free(cv);
        }
    }

    // the flash: a small pointed star of triangles at the muzzle, solid from any angle
    float fv[8 * 6 * 4];
    int k = 0;
    for (int i = 0; i < 8; i++) {
        float a0 = i * (6.283185307f / 8), a1 = (i + 1) * (6.283185307f / 8);
        float ra = (i & 1) ? .16f : .38f, rb = (i & 1) ? .38f : .16f;
        const float tri[24] = { 0, 0, -.01f, 1,  ra * cosf(a0), ra * sinf(a0), -.35f, .25f,  rb * cosf(a1), rb * sinf(a1), -.35f, .25f,
                                0, 0, -1.3f, 0,  rb * cosf(a1), rb * sinf(a1), -.35f, .25f,  ra * cosf(a0), ra * sinf(a0), -.35f, .25f };
        memcpy(fv + k, tri, sizeof tri);
        k += 24;
    }
    glGenVertexArrays(1, &G.flash_vao);
    glBindVertexArray(G.flash_vao);
    glGenBuffers(1, &G.flash_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, G.flash_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof fv, fv, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 4 * sizeof(float), 0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(3 * sizeof(float)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    G.count = (GLsizei)n[1];
    memcpy(G.muzzle, tip, sizeof tip);
    G.articulated = 0;      // a slide (part 2) to kick back; without one the whole gun kicks
    for (uint32_t i = 0; i < n[0] && !legacy; i++) if (v[i].part > 1.5f) { G.articulated = 1; break; }
    ok = glGetError() == GL_NO_ERROR;
    if (ok) LOGI("gun: %s, %u triangles", name, n[1] / 3);
done:
    AAsset_close(a);
    free(v); free(idx); free(px);
    if (!ok) { LOGE("gun: %s could not be loaded", name); G.count = 0; }
    return ok;
}

float qgun_muzzle_z(void) { return G.count ? -G.muzzle[2] : 0.0f; }
float qgun_muzzle_y(void) { return G.count ? G.muzzle[1] : 0.0f; }
int qgun_has_slide(void) { return G.count && G.articulated; }
int qgun_cap_top(float out[3]) {
    if (!G.count || !G.cap_count) return 0;
    out[0] = G.cap_top[0]; out[1] = G.cap_top[1]; out[2] = G.cap_top[2];
    return 1;
}
void qgun_draw_cap(const float *view, const float *proj, const float *model) {
    if (!G.count || !G.cap_count) return;
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glUseProgram(G.prog);
    glUniformMatrix4fv(G.u_view, 1, GL_FALSE, view);
    glUniformMatrix4fv(G.u_proj, 1, GL_FALSE, proj);
    glUniformMatrix4fv(G.u_model, 1, GL_FALSE, model);
    glUniform1f(G.u_recoil, 0);
    glUniform1f(G.u_trigger, 0);
    glUniform3f(G.u_tint, 1, 1, 1);
    glUniform1i(G.u_albedo, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, G.tex);
    glBindVertexArray(G.cap_vao);
    glDrawElements(GL_TRIANGLES, G.cap_count, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
}
void qgun_butt(float out[3]) {
    if (G.count && G.cap_count) { out[0] = G.butt[0]; out[1] = G.butt[1]; out[2] = G.butt[2]; return; }
    qgun_grip_base(out);
}
void qgun_grip_base(float out[3]) {
    static const float fallback[3] = { 0, -0.11f, 0.11f };   // the boxes drawn without a model
    const float *g = G.count && (G.grip[1] || G.grip[2]) ? G.grip : fallback;
    out[0] = g[0]; out[1] = g[1]; out[2] = g[2];
}

void qgun_draw(const float *view, const float *proj, const float *model, float recoil, float trigger, float flash, const float *tint) {
    if (!G.count) return;
    float kick = fminf(1, fmaxf(0, recoil));
    float m[16];
    memcpy(m, model, sizeof m);
    if (!G.articulated) {     // no moving slide: the whole gun kicks back and up a little
        float c = cosf(.09f * kick), s = sinf(.09f * kick), r[16] = { 1, 0, 0, 0,  0, c, s, 0,  0, -s, c, 0,  0, 0, .014f * kick, 1 }, o[16];
        for (int col = 0; col < 4; col++)
            for (int row = 0; row < 4; row++)
                o[col * 4 + row] = m[row] * r[col * 4] + m[4 + row] * r[col * 4 + 1] + m[8 + row] * r[col * 4 + 2] + m[12 + row] * r[col * 4 + 3];
        memcpy(m, o, sizeof m);
    }
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glUseProgram(G.prog);
    glUniformMatrix4fv(G.u_view, 1, GL_FALSE, view);
    glUniformMatrix4fv(G.u_proj, 1, GL_FALSE, proj);
    glUniformMatrix4fv(G.u_model, 1, GL_FALSE, m);
    glUniform1f(G.u_recoil, kick);
    glUniform1f(G.u_trigger, fminf(1, fmaxf(0, trigger)));
    glUniform3f(G.u_tint, tint ? tint[0] : 1, tint ? tint[1] : 1, tint ? tint[2] : 1);
    glUniform1i(G.u_albedo, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, G.tex);
    glBindVertexArray(G.vao);
    glDrawElements(GL_TRIANGLES, G.count, GL_UNSIGNED_INT, 0);
    if (flash > 0) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glDepthMask(GL_FALSE);
        glUseProgram(G.flash_prog);
        glUniformMatrix4fv(G.f_view, 1, GL_FALSE, view);
        glUniformMatrix4fv(G.f_proj, 1, GL_FALSE, proj);
        glUniformMatrix4fv(G.f_model, 1, GL_FALSE, m);
        glUniform3f(G.f_tip, G.muzzle[0], G.muzzle[1], G.muzzle[2]);
        glUniform1f(G.f_size, .045f);
        glUniform1f(G.f_strength, fminf(1, flash));
        glBindVertexArray(G.flash_vao);
        glDrawArrays(GL_TRIANGLES, 0, 48);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

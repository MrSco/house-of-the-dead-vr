// The player's hands (quest_hand.h). TCHAND01, written by tools/vr/hands_import.py (the layout is in its header):
// per hand the vertices at rest (holding the gun's grip), the skeleton at that pose and, for each finger bone, the
// axis and angle it turns through from open to a fist; one RGBA texture (the skin, the sleeve).
#include <android/asset_manager.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "quest_hand.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "hotdvr", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "hotdvr", __VA_ARGS__)
#define MAX_BONES 32

typedef struct { int32_t parent; float local[16], inv[16], axis[3], angle; int32_t finger, joint; } Bone;
typedef struct { GLuint vao, vbo, ibo; GLsizei count; int nbones; Bone bone[MAX_BONES]; } Hand;
static struct {
    Hand hand[2];
    GLuint prog, tex;
    GLint u_view, u_proj, u_model, u_bones, u_tint, u_albedo;
    int ok;
} H;

// The swapchain is sRGB: the shaders work in linear light (as quest_gun.c). Back faces (the inside of the sleeve)
// are dark.
static const char *VS =
    "#version 300 es\nprecision highp float;\n"
    "layout(location=0) in vec3 position; layout(location=1) in vec3 normal; layout(location=2) in vec2 texcoord;\n"
    "layout(location=3) in vec4 joints; layout(location=4) in vec4 weights;\n"
    "uniform mat4 view, projection, model; uniform mat4 bones[" "32" "];\n"
    "out vec3 n, p; out vec2 uv;\n"
    "void main() {\n"
    "  mat4 s = bones[int(joints.x)] * weights.x + bones[int(joints.y)] * weights.y\n"
    "         + bones[int(joints.z)] * weights.z + bones[int(joints.w)] * weights.w;\n"
    "  mat4 mv = view * model; vec4 v = mv * (s * vec4(position, 1.0));\n"
    "  p = v.xyz; n = mat3(mv) * (mat3(s) * normal); uv = texcoord; gl_Position = projection * v;\n"
    "}\n";
static const char *FS =
    "#version 300 es\nprecision highp float;\n"
    "in vec3 n, p; in vec2 uv; uniform sampler2D albedo; uniform vec3 tint; out vec4 frag;\n"
    "void main() {\n"
    "  vec3 base = texture(albedo, uv).rgb * tint;\n"
    "  if (!gl_FrontFacing) { frag = vec4(base * 0.12, 1.0); return; }\n"
    "  vec3 N = normalize(n), L = normalize(vec3(-0.4, 0.8, 0.6)), V = normalize(-p), H = normalize(L + V);\n"
    "  float diffuse = 0.45 + 0.55 * max(dot(N, L), 0.0), spec = 0.06 * pow(max(dot(N, H), 0.0), 24.0);\n"
    "  frag = vec4(clamp(base * diffuse + spec, 0.0, 1.0), 1.0);\n"
    "}\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char b[1024]; glGetShaderInfoLog(s, sizeof b, NULL, b); LOGE("hand shader: %s", b); glDeleteShader(s); return 0; }
    return s;
}
static int rd(AAsset *a, void *p, size_t n) { return AAsset_read(a, p, n) == (int)n; }

// column-major 4x4: o = a * b
static void mul(float *o, const float *a, const float *b) {
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int k = 0; k < 4; k++)
            r[c * 4 + k] = a[k] * b[c * 4] + a[4 + k] * b[c * 4 + 1] + a[8 + k] * b[c * 4 + 2] + a[12 + k] * b[c * 4 + 3];
    memcpy(o, r, sizeof r);
}
// a turn of ang about the unit axis, as a column-major 4x4
static void axis_rot(float *m, const float *ax, float ang) {
    float c = cosf(ang), s = sinf(ang), t = 1 - c, x = ax[0], y = ax[1], z = ax[2];
    const float r[16] = { t * x * x + c, t * x * y + s * z, t * x * z - s * y, 0,
                          t * x * y - s * z, t * y * y + c, t * y * z + s * x, 0,
                          t * x * z + s * y, t * y * z - s * x, t * z * z + c, 0,  0, 0, 0, 1 };
    memcpy(m, r, sizeof r);
}

int qhand_load(struct AAssetManager *am, const char *name) {
    AAsset *a = am ? AAssetManager_open(am, name, AASSET_MODE_STREAMING) : NULL;
    if (!a) { LOGE("hands: no asset %s", name); return 0; }
    char magic[8];
    uint32_t hd[4];
    float *v = NULL; uint32_t *idx = NULL; uint8_t *px = NULL;
    int ok = 0;
    if (!rd(a, magic, 8) || memcmp(magic, "TCHAND01", 8) || !rd(a, hd, 16)) goto done;
    if (!hd[0] || hd[0] > MAX_BONES || hd[1] != 2 || !hd[2] || hd[2] > 4096 || !hd[3] || hd[3] > 4096) goto done;
    H.prog = 0;
    for (int s = 0; s < 2; s++) {
        Hand *h = &H.hand[s];
        uint32_t n[2];
        if (!rd(a, n, 8) || !n[0] || n[0] > 65536 || !n[1] || n[1] > 300000 || n[1] % 3) goto done;
        h->nbones = (int)hd[0];
        for (int b = 0; b < h->nbones; b++) {
            Bone *bn = &h->bone[b];
            if (!rd(a, &bn->parent, 4) || !rd(a, bn->local, 64) || !rd(a, bn->inv, 64) || !rd(a, bn->axis, 16) ||
                !rd(a, &bn->finger, 8)) goto done;
            if (bn->parent >= b || bn->parent < -1 || bn->finger < -1 || bn->finger > 4) goto done;
        }
        free(v); free(idx);
        v = malloc((size_t)n[0] * 64); idx = malloc((size_t)n[1] * 4);
        if (!v || !idx || !rd(a, v, (size_t)n[0] * 64) || !rd(a, idx, (size_t)n[1] * 4)) goto done;
        for (uint32_t i = 0; i < n[1]; i++) if (idx[i] >= n[0]) goto done;
        for (uint32_t i = 0; i < n[0]; i++)
            for (int k = 8; k < 12; k++) if (!(v[i * 16 + k] >= 0 && v[i * 16 + k] < h->nbones)) goto done;
        glGenVertexArrays(1, &h->vao);
        glBindVertexArray(h->vao);
        glGenBuffers(1, &h->vbo);
        glBindBuffer(GL_ARRAY_BUFFER, h->vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)n[0] * 64, v, GL_STATIC_DRAW);
        glGenBuffers(1, &h->ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, h->ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)n[1] * 4, idx, GL_STATIC_DRAW);
        static const int size[5] = { 3, 3, 2, 4, 4 }, off[5] = { 0, 12, 24, 32, 48 };
        for (int i = 0; i < 5; i++) {
            glEnableVertexAttribArray(i);
            glVertexAttribPointer(i, size[i], GL_FLOAT, GL_FALSE, 64, (void *)(size_t)off[i]);
        }
        glBindVertexArray(0);
        h->count = (GLsizei)n[1];
    }
    px = malloc((size_t)hd[2] * hd[3] * 4);
    if (!px || !rd(a, px, (size_t)hd[2] * hd[3] * 4)) goto done;
    {
        GLuint vs = compile(GL_VERTEX_SHADER, VS), fs = compile(GL_FRAGMENT_SHADER, FS);
        if (!vs || !fs) { glDeleteShader(vs); glDeleteShader(fs); goto done; }
        H.prog = glCreateProgram();
        glAttachShader(H.prog, vs); glAttachShader(H.prog, fs);
        glLinkProgram(H.prog);
        glDeleteShader(vs); glDeleteShader(fs);
        GLint linked; glGetProgramiv(H.prog, GL_LINK_STATUS, &linked);
        if (!linked) goto done;
    }
    H.u_view = glGetUniformLocation(H.prog, "view"); H.u_proj = glGetUniformLocation(H.prog, "projection");
    H.u_model = glGetUniformLocation(H.prog, "model"); H.u_bones = glGetUniformLocation(H.prog, "bones");
    H.u_tint = glGetUniformLocation(H.prog, "tint"); H.u_albedo = glGetUniformLocation(H.prog, "albedo");
    glGenTextures(1, &H.tex);
    glBindTexture(GL_TEXTURE_2D, H.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, hd[2], hd[3], 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    ok = glGetError() == GL_NO_ERROR;
    if (ok) LOGI("hands: %s, %d + %d triangles, %u bones", name, H.hand[0].count / 3, H.hand[1].count / 3, hd[0]);
done:
    AAsset_close(a);
    free(v); free(idx); free(px);
    if (!ok) LOGE("hands: %s could not be loaded", name);
    H.ok = ok;
    return ok;
}

void qhand_draw(const float *view, const float *proj, const float *model, int side, const float curl[5], const float *tint) {
    if (!H.ok || side < 0 || side > 1) return;
    Hand *h = &H.hand[side];
    // the pose: each finger bone turned from rest by its share of the fist; then the skin matrices
    float world[MAX_BONES][16], skin[MAX_BONES * 16];
    for (int b = 0; b < h->nbones; b++) {
        const Bone *bn = &h->bone[b];
        float l[16];
        memcpy(l, bn->local, sizeof l);
        if (bn->finger >= 0) {
            float c = curl[bn->finger], r[16];
            c = c < 0 ? 0 : c > 1.2f ? 1.2f : c;
            axis_rot(r, bn->axis, bn->angle * c);
            mul(l, l, r);
        }
        if (bn->parent >= 0) mul(world[b], world[bn->parent], l); else memcpy(world[b], l, sizeof l);
        mul(skin + b * 16, world[b], bn->inv);
    }
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glUseProgram(H.prog);
    glUniformMatrix4fv(H.u_view, 1, GL_FALSE, view);
    glUniformMatrix4fv(H.u_proj, 1, GL_FALSE, proj);
    glUniformMatrix4fv(H.u_model, 1, GL_FALSE, model);
    glUniformMatrix4fv(H.u_bones, h->nbones, GL_FALSE, skin);
    glUniform3f(H.u_tint, tint ? tint[0] : 1, tint ? tint[1] : 1, tint ? tint[2] : 1);
    glUniform1i(H.u_albedo, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, H.tex);
    glBindVertexArray(h->vao);
    glDrawElements(GL_TRIANGLES, h->count, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

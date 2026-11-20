// The cabinet model (quest_cabinet.h). TCCAB001, little-endian: "TCCAB001", vertex count, index count, texture size
// (uint32; square textures), the screen (5 floats), the cords' anchors (6 floats), per vertex position, normal, uv
// (8 floats), the indices (uint32), the base colour then the emissive texture (RGBA8, rows from v = 0). Made by
// tools/cabinet (Blender) from the CC-BY model credited in cabinet.json.
#include "xr.h"
#if defined(__ANDROID__)
#include <GLES3/gl3.h>
#include <android/log.h>
#define XR_GLSL "#version 300 es\n"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "hotdvr", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "hotdvr", __VA_ARGS__)
#else
#include "platform/gl.h"
#include "platform/platform.h"
#define XR_GLSL PLAT_GLSL_VERSION
#define LOGI(...) plat_log(PLAT_LOG_INFO, __VA_ARGS__)
#define LOGE(...) plat_log(PLAT_LOG_ERROR, __VA_ARGS__)
#endif
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "quest_cabinet.h"

static const char *VS =
    XR_GLSL ""
    "uniform mat4 view, projection, model;\n"
    "layout(location = 0) in vec3 pos;\n"
    "layout(location = 1) in vec3 normal;\n"
    "layout(location = 2) in vec2 uv;\n"
    "out vec3 v_n;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  gl_Position = projection * view * model * vec4(pos, 1.0);\n"
    "  v_n = mat3(model) * normal;\n"
    "  v_uv = uv;\n"
    "}\n";
// a dim arcade: a soft light from above and in front, the marquee and the control panel lit from inside
static const char *FS =
    XR_GLSL ""
    "precision mediump float;\n"
    "uniform sampler2D base, emissive;\n"
    "in vec3 v_n;\n"
    "in vec2 v_uv;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "  vec3 n = normalize(v_n);\n"
    "  float light = 0.28 + 0.72 * max(dot(n, normalize(vec3(0.25, 0.8, 0.55))), 0.0);\n"
    "  vec3 c = texture(base, v_uv).rgb * light + texture(emissive, v_uv).rgb;\n"
    "  o = vec4(c, 1.0);\n"
    "}\n";

static struct {
    GLuint prog, vao, vbo, ibo, tex[2];
    GLint u_view, u_proj, u_model;
    GLsizei n;
    float screen[5], anchor[6], front;
    int ok;
} C;

static GLuint shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof log, NULL, log); LOGE("cabinet shader: %s", log); }
    return s;
}
static int rd(qasset *a, void *p, size_t n) { return qasset_read(a, p, n); }

int qcab_load(const char *name) {
    qasset *a = qasset_open(name);
    float *v = NULL;
    uint32_t *idx = NULL, n[3];
    uint8_t *px = NULL;
    char magic[8];
    int ok = 0;
    if (!a) return 0;
    if (!rd(a, magic, 8) || memcmp(magic, "TCCAB001", 8) || !rd(a, n, 12) || !rd(a, C.screen, 20) || !rd(a, C.anchor, 24)) goto done;
    if (!n[0] || n[0] > 200000 || !n[1] || n[1] > 600000 || n[1] % 3 || !n[2] || n[2] > 2048) goto done;
    v = malloc((size_t)n[0] * 32);
    idx = malloc((size_t)n[1] * 4);
    px = malloc((size_t)n[2] * n[2] * 4);
    if (!v || !idx || !px || !rd(a, v, (size_t)n[0] * 32) || !rd(a, idx, (size_t)n[1] * 4)) goto done;
    for (uint32_t i = 0; i < n[1]; i++) if (idx[i] >= n[0]) goto done;
    for (uint32_t i = 0; i < n[0] * 8; i++) if (!isfinite(v[i])) goto done;
    C.front = 0;
    for (uint32_t i = 0; i < n[0]; i++) if (v[i * 8 + 2] > C.front) C.front = v[i * 8 + 2];

    GLuint vs = shader(GL_VERTEX_SHADER, VS), fs = shader(GL_FRAGMENT_SHADER, FS);
    C.prog = glCreateProgram();
    glAttachShader(C.prog, vs); glAttachShader(C.prog, fs);
    glLinkProgram(C.prog);
    glDeleteShader(vs); glDeleteShader(fs);
    GLint linked = 0;
    glGetProgramiv(C.prog, GL_LINK_STATUS, &linked);
    if (!linked) goto done;
    C.u_view = glGetUniformLocation(C.prog, "view");
    C.u_proj = glGetUniformLocation(C.prog, "projection");
    C.u_model = glGetUniformLocation(C.prog, "model");
    glUseProgram(C.prog);
    glUniform1i(glGetUniformLocation(C.prog, "base"), 0);
    glUniform1i(glGetUniformLocation(C.prog, "emissive"), 1);
    glUseProgram(0);
    glGenTextures(2, C.tex);
    for (int t = 0; t < 2; t++) {
        if (!rd(a, px, (size_t)n[2] * n[2] * 4)) goto done;
        glBindTexture(GL_TEXTURE_2D, C.tex[t]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, n[2], n[2], 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenVertexArrays(1, &C.vao);
    glGenBuffers(1, &C.vbo);
    glGenBuffers(1, &C.ibo);
    glBindVertexArray(C.vao);
    glBindBuffer(GL_ARRAY_BUFFER, C.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)n[0] * 32, v, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, C.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)n[1] * 4, idx, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 32, (void *)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 32, (void *)12);
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 32, (void *)24);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    C.n = (GLsizei)n[1];
    ok = C.ok = 1;
done:
    if (!ok) LOGE("cabinet: %s missing or broken", name);
    free(v); free(idx); free(px);
    qasset_close(a);
    return ok;
}

void qcab_draw(const float *view, const float *proj, const float *model) {
    if (!C.ok) return;
    glUseProgram(C.prog);
    glUniformMatrix4fv(C.u_view, 1, GL_FALSE, view);
    glUniformMatrix4fv(C.u_proj, 1, GL_FALSE, proj);
    glUniformMatrix4fv(C.u_model, 1, GL_FALSE, model);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, C.tex[1]);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, C.tex[0]);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);   // (both sides: the model's faces don't all wind one way)
    glBindVertexArray(C.vao);
    glDrawElements(GL_TRIANGLES, C.n, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, 0);
}

void qcab_screen(float out[5]) { memcpy(out, C.screen, sizeof C.screen); }
float qcab_front(void) { return C.front; }
void qcab_anchor(int side, float out[3]) { memcpy(out, C.anchor + (side ? 3 : 0), 3 * sizeof(float)); }

// The arcade game's polygons in the headset (GLES 3.0), shaded per pixel as the Model 2's rasterizer does:
// src/app/gpu/m2.hlsl's ps_poly (raster.cpp draw_tex_span) in GLSL. Texture RAM, the luma RAM and the gamma'd
// colour translation are integer textures read with texelFetch. The polygons are real 3D here (m2_vr.h: camera
// space), so the texel coordinates and the arcade camera's z interpolate perspective-correct, as the
// rasterizer's plane equations make them; the texture level is chosen from that z as the rasterizer does.
//
// The Model 2 has no depth buffer: the first polygon in its draw order to reach a pixel wins. Here the depth
// test decides, and each step in that order sits a little further out along its own view ray (M2Vert.prio), so
// that coplanar ones (shadows, decals) keep the arcade's choice. The screen's effects come last, over the world.
// A flat picture needs no real depth: there the order alone decides (m2_gl_arcade_order), as on the arcade, where a
// figure the game sorts in front stays in front even where its mesh is behind a wall's (the girl at the mansion's
// door in the intro: with real depth only her hand showed).
#include "m2_vr.h"
#include "platform/gl.h"
#include "platform/platform.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

#define LOGE(...) plat_log(PLAT_LOG_ERROR, __VA_ARGS__)
#define LOGI(...) plat_log(PLAT_LOG_INFO, __VA_ARGS__)

static const char *VS =
    PLAT_GLSL_VERSION
    "uniform mat4 mvp;\n"
    "layout(location = 0) in vec3 pos;\n"
    "layout(location = 1) in vec2 uv;\n"
    "layout(location = 2) in float depth;\n"
    "layout(location = 3) in float prio;\n"
    "uniform float prio_k;\n"
    "uniform float order;\n"   // > 0: the depth is the draw order's alone (prio x order: 0 near, 1 far)
    "uniform vec4 eye;\n"      // the eye in the polygons' space (xyz) and its clip z (w): m2_gl_draw
    "uniform float key_max;\n" // the farthest key plane ahead of the eye (game units) the far plane keeps
    "layout(location = 4) in vec3 col;\n"
    "layout(location = 5) in uvec4 info;\n"
    "layout(location = 6) in uvec2 more;\n"
    "layout(location = 7) in float key;\n"
    "out vec2 v_uv;\n"
    "out float v_z;\n"
    "flat out vec3 v_col;\n"
    "flat out uvec4 v_info;\n"
    "flat out uvec2 v_more;\n"
    "void main() {\n"
    // (out along its ray: m2_vr.h prio; far off, more: the depth buffer's step grows with the distance squared, so the
    // relative step must grow with the distance to stay a few of its steps; prio_k from the projection's near plane)
    "  gl_Position = mvp * vec4(pos, 1.0);\n"
    "  if (order > 0.0) {\n"
    "    gl_Position.z = (min(prio * order, 1.0) * 2.0 - 1.0) * gl_Position.w;\n"
    "  } else {\n"
    // the depth alone is where the point's ray from the eye meets the plane z = key (the z the arcade sorted its
    // polygon by), a step further for its place in the draw order: figures the arcade sorts in front of a floor or
    // wall their meshes dip into stay in front, as it draws them; the point itself stays where it is (no parallax).
    // Along the ray Q = E + t (P - E) clip space is (1 - t) E' + t P' with E' = (0, 0, eye.w, 0): Q's depth on P's
    // own w is z' = P'.z - eye.w + eye.w / t, t = (key - eye.z) / (pos.z - eye.z): linear in pos, so the clipper and
    // the rasterizer carry it across a polygon exactly (one reaching behind you, as the floor at your feet does)
    // A polygon too near the arcade's camera for its plane (key < 0: the floor at your feet, the sky's dome; rays from
    // the eye meet the plane behind you) goes out by a fixed step along the rays instead, t = 1.03 and the draw
    // order's step: z' = P'.z - eye.w + eye.w / t (a figure's feet dipping into the floor stay over it)
    "    float s = key - eye.z;\n"
    "    if (key > 0.0 && s > 1e-3) {\n"
    "      s = min(s, key_max) * (1.0 + prio * clamp(s * prio_k, 1.0, 16.0));\n"
    "      gl_Position.z += eye.w * ((pos.z - eye.z) / s - 1.0);\n"
    // (a ray at a grazing angle to the plane, as the sky's high up are, meets it past the far plane: kept inside)
    "      if (gl_Position.w > 0.0) gl_Position.z = min(gl_Position.z, 0.99999 * gl_Position.w);\n"
    "    } else if (key < 0.0) {\n"
    "      gl_Position.z += eye.w * (1.0 / (1.03 * (1.0 + prio)) - 1.0);\n"
    "    } else {\n"
    "      vec3 q = pos * (1.0 + prio * clamp(length(pos) * prio_k, 1.0, 16.0));\n"
    "      vec4 d = mvp * vec4(q, 1.0);\n"
    "      if (d.w > 0.0 && gl_Position.w > 0.0) gl_Position.z = clamp(d.z / d.w, -1.0, 0.99999) * gl_Position.w;\n"
    "    }\n"
    "  }\n"
    "  v_uv = uv; v_z = depth; v_col = col; v_info = info; v_more = more;\n"
    "}\n";

static const char *FS =
    PLAT_GLSL_VERSION
    "precision highp float;\n"
    "precision highp int;\n"
    "precision highp usampler2D;\n"
    "uniform usampler2D texram;\n"   // 1024 x 1024 R32UI: sheet 0 rows 0-511, sheet 1 rows 512-1023
    "uniform usampler2D lumaram;\n"  // 256 x 128 R8UI
    "uniform usampler2D xlat;\n"     // 256 x 96 R8UI
    "in vec2 v_uv;\n"
    "in float v_z;\n"
    "flat in vec3 v_col;\n"
    "flat in uvec4 v_info;\n"
    "flat in uvec2 v_more;\n"
    "out vec4 o;\n"
    "const uint log2_table[128] = uint[128](0u, 2u, 5u, 8u, 11u, 14u, 16u, 19u, 22u, 25u, 27u, 30u, 33u, 35u, 38u, 40u, 43u, 46u, 48u, 51u, 53u, 56u, 58u, 61u, 63u, 65u, 68u, 70u, 73u, 75u, 77u, 80u, 82u, 84u, 87u, 89u, 91u, 93u, 96u, 98u, 100u, 102u, 104u, 106u, 109u, 111u, 113u, 115u, 117u, 119u, 121u, 123u, 125u, 127u, 129u, 132u, 134u, 136u, 138u, 140u, 141u, 143u, 145u, 147u, 149u, 151u, 153u, 155u, 157u, 159u, 161u, 162u, 164u, 166u, 168u, 170u, 172u, 173u, 175u, 177u, 179u, 181u, 182u, 184u, 186u, 188u, 189u, 191u, 193u, 194u, 196u, 198u, 200u, 201u, 203u, 205u, 206u, 208u, 209u, 211u, 213u, 214u, 216u, 218u, 219u, 221u, 222u, 224u, 225u, 227u, 229u, 230u, 232u, 233u, 235u, 236u, 238u, 239u, 241u, 242u, 244u, 245u, 247u, 248u, 250u, 251u, 253u, 254u);\n"
    "int fast_log2(float value) {\n"
    "  if (value < 0.0) return 0;\n"
    "  uint ival = floatBitsToUint(value) >> 16;\n"
    "  int e = int(ival >> 7) - 127;\n"
    "  return (e << 8) | int(log2_table[ival & 127u]);\n"
    "}\n"
    "int to_s32(float f) {\n"
    "  if (!(f >= -2147483648.0 && f < 2147483648.0)) return int(0x80000000u);\n"
    "  return int(f);\n"
    "}\n"
    "uint lerp8(uint x, uint y, uint a) { return (x + (((y - x) * a) >> 8)) & 0x00ff00ffu; }\n"
    "uint get_texel(uint base_x, uint base_y, int x, int y, uint sheet) {\n"
    "  int x2 = int(base_x) + x;\n"
    "  int y2 = int(base_y) + y;\n"
    "  if (x2 >= 1024) { x2 -= 1024; y2 ^= 1024; }\n"
    "  uint offset = uint((y2 / 2) * 512 + (x2 / 2));\n"
    "  uint w = sheet * 0x80000u + ((offset >> 1) & 0x7ffffu);\n"
    "  uint texel = texelFetch(texram, ivec2(int(w & 1023u), int(w >> 10)), 0).r;\n"
    "  if ((offset & 1u) != 0u) texel >>= 16;\n"
    "  if ((y & 1) == 0) texel >>= 8;\n"
    "  if ((x & 1) == 0) texel >>= 4;\n"
    "  return texel & 0x0fu;\n"
    "}\n"
    "uint fetch_texel(uvec4 info, int level, int u, int v, bool translucent) {\n"
    "  uint flags = info.x;\n"
    "  bool swap = ((flags >> 10) & 1u) != 0u;\n"
    "  uint tex_width, tex_height, tex_x, tex_y, sheet;\n"
    "  if (level == -1) {\n"
    "    tex_width = 128u; tex_height = 128u;\n"
    "    tex_x = info.z & 0xffffu; tex_y = info.z >> 16;\n"
    "    sheet = swap ? 0u : 1u;\n"
    "    int sh = 1 << int((flags >> 8) & 3u);\n"
    "    u <<= sh; v <<= sh;\n"
    "  } else {\n"
    "    uint texx = info.y & 0xffffu, texy = info.y >> 16;\n"
    "    tex_width = (32u << ((flags >> 17) & 7u)) >> uint(level);\n"
    "    tex_height = (32u << ((flags >> 20) & 7u)) >> uint(level);\n"
    "    tex_x = ((texx - 2048u) >> uint(level)) & 2047u;\n"
    "    tex_y = ((texy - 1024u) >> uint(level)) & 1023u;\n"
    "    sheet = ((level & 1) != 0) != swap ? 1u : 0u;\n"
    "    u >>= level; v >>= level;\n"
    "  }\n"
    "  if (((flags >> 5) & 1u) != 0u && (u & int(tex_width << 8)) != 0) u = ~u;\n"
    "  if (((flags >> 6) & 1u) != 0u && (v & int(tex_height << 8)) != 0) v = ~v;\n"
    "  u -= 0x80; v -= 0x80;\n"
    "  uint ufrac = uint(u) & 0xffu;\n"
    "  uint vfrac = uint(v) & 0xffu;\n"
    "  int u0 = int(uint(u >> 8) & (tex_width - 1u));\n"
    "  int u1 = int(uint(u0 + 1) & (tex_width - 1u));\n"
    "  int v0 = int(uint(v >> 8) & (tex_height - 1u));\n"
    "  int v1 = int(uint(v0 + 1) & (tex_height - 1u));\n"
    "  if (((flags >> 3) & 1u) == 0u && u1 == 0) {\n"
    "    if (ufrac >= 0x80u) { u0 = u1; u1++; ufrac = 0u; } else { u1 = u0; u0--; ufrac = 0x100u; }\n"
    "  }\n"
    "  if (((flags >> 4) & 1u) == 0u && v1 == 0) {\n"
    "    if (vfrac >= 0x80u) { v0 = 0; v1++; vfrac = 0u; } else { v1 = v0; v0--; vfrac = 0x100u; }\n"
    "  }\n"
    "  uint t00 = get_texel(tex_x, tex_y, u0, v0, sheet) << 4;\n"
    "  uint t01 = get_texel(tex_x, tex_y, u1, v0, sheet) << 4;\n"
    "  uint t10 = get_texel(tex_x, tex_y, u0, v1, sheet) << 4;\n"
    "  uint t11 = get_texel(tex_x, tex_y, u1, v1, sheet) << 4;\n"
    "  if (translucent) {\n"
    "    if (t00 != 0xf0u) t00 |= 0x00800000u;\n"
    "    if (t01 != 0xf0u) t01 |= 0x00800000u;\n"
    "    if (t10 != 0xf0u) t10 |= 0x00800000u;\n"
    "    if (t11 != 0xf0u) t11 |= 0x00800000u;\n"
    "    if (t00 == 0xf0u) t00 = t01 & 0xffu;\n"
    "    if (t01 == 0xf0u) t01 = t00 & 0xffu;\n"
    "    if (t10 == 0xf0u) t10 = t11 & 0xffu;\n"
    "    if (t11 == 0xf0u) t11 = t10 & 0xffu;\n"
    "  }\n"
    "  uint t0x = lerp8(t00, t01, ufrac);\n"
    "  uint t1x = lerp8(t10, t11, ufrac);\n"
    "  if (translucent) {\n"
    "    if (t0x == 0xf0u) t0x = t1x & 0xffu;\n"
    "    if (t1x == 0xf0u) t1x = t0x & 0xffu;\n"
    "  }\n"
    "  return lerp8(t0x, t1x, vfrac);\n"
    "}\n"
    "uint table(usampler2D t, uint i) { return texelFetch(t, ivec2(int(i & 255u), int(i >> 8)), 0).r; }\n"
    // the arcade's colours (what its monitor got) into linear light for the sRGB framebuffer, exactly: the
    // framebuffer encodes them back with the sRGB curve, so they reach the screen as they were (a plain 2.2 power
    // here crushed the shadows: 5% came out under 2%)
    "uniform float lift;\n"   // Brightness: 1 as drawn; under 1 lifts the shadows (m2_gl_brightness)
    "vec3 to_linear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)); }\n"
    "void main() {\n"
    "  uint flags = v_info.x;\n"
    "  if (((flags >> 2) & 1u) != 0u) {\n"
    "    uvec2 p = uvec2(gl_FragCoord.xy);\n"
    "    if (((p.x ^ p.y) & 1u) == 0u) discard;\n"
    "  }\n"
    "  uint renderer = flags & 3u;\n"
    "  if (renderer < 2u) { o = vec4(to_linear(pow(v_col, vec3(lift))), 1.0); return; }\n"
    "  bool translucent = renderer == 3u;\n"
    "  int max_level = int((flags >> 12) & 31u);\n"
    "  int mml = -int(v_more.x) + fast_log2(v_z);\n"
    "  int level = clamp(mml >> 7, 0, max_level);\n"
    "  int u = to_s32(v_uv.x);\n"    // (already 8.8 fixed point: M2Vert.u)
    "  int v = to_s32(v_uv.y);\n"
    "  uint t = fetch_texel(v_info, level, u, v, translucent);\n"
    "  if (mml > 0 && level < max_level) {\n"
    "    uint t2 = fetch_texel(v_info, level + 1, u, v, translucent);\n"
    "    t = lerp8(t, t2, uint((mml & 127) << 1));\n"
    "  } else if (((flags >> 7) & 1u) != 0u && mml < 0) {\n"
    "    uint t2 = fetch_texel(v_info, -1, u, v, translucent);\n"
    "    t = lerp8(t, t2, uint(min(-mml >> int((flags >> 8) & 3u), 127)));\n"
    "  }\n"
    "  if (translucent) {\n"
    "    if (t < 0x00400000u) discard;\n"
    "    t &= 0xffu;\n"
    "  }\n"
    "  uint lumabase = v_info.w & 0xffffu, pluma = v_info.w >> 16;\n"
    "  uint luma = ((table(lumaram, lumabase + (t >> 1)) & 0xffu) * pluma / 256u) & 0xffu;\n"
    "  luma = min(luma, 0x3fu);\n"
    "  uint c = v_more.y;\n"
    "  uint cr = ((c >> 0) & 0x1fu) << 8, cg = 0x2000u + (((c >> 5) & 0x1fu) << 8), cb = 0x4000u + (((c >> 10) & 0x1fu) << 8);\n"
    "  vec3 x = vec3(float(table(xlat, cr + luma)), float(table(xlat, cg + luma)), float(table(xlat, cb + luma))) / 255.0;\n"
    "  o = vec4(to_linear(pow(x, vec3(lift))), 1.0);\n"
    "}\n";

static const char *VS_FACE =
    PLAT_GLSL_VERSION
    "uniform mat4 mvp;\n"
    "layout(location = 0) in vec3 pos;\n"
    "layout(location = 1) in vec2 uv;\n"
    "out vec2 v_uv;\n"
    "void main() { gl_Position = mvp * vec4(pos, 1.0); v_uv = uv; }\n";
static const char *FS_FACE =
    PLAT_GLSL_VERSION
    "precision mediump float;\n"
    "uniform sampler2D tex;\n"
    "in vec2 v_uv;\n"
    "out vec4 o;\n"
    "void main() { o = texture(tex, v_uv); }\n";

// The cabinet room's courtyard (arcade_quest.cpp make_courtyard: courtyard.m2w): its own vertices and tables, drawn
// with the same shading, levelled by the sky's up it was saved with.
static struct {
    GLuint vao, vbo, tex, luma, xlat;
    GLsizei nv;
    float level[9];         // camera space -> levelled (rows)
    int tried, ok;
} B;
extern const char *arcade_courtyard_file(void);

static struct {
    GLuint face_prog, face_tex, face_vao, face_vbo;
    GLint face_mvp;
    int face_any;
    uint32_t face_serial;
    GLuint prog, vao, vbo, tex, luma, xlat;
    GLint u_mvp, u_prio_k, u_lift, u_order, u_eye, u_key_max;
    float prio_k, lift, order;   // (order: m2_gl_arcade_order)
    GLsizei nv, nv_world;
    int ready;
} M;

static GLuint shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        LOGE("m2 shader: %s", log);
    }
    return s;
}

static void make_vao(GLuint *vao, GLuint *vbo) {
    glGenVertexArrays(1, vao);
    glGenBuffers(1, vbo);
    glBindVertexArray(*vao);
    glBindBuffer(GL_ARRAY_BUFFER, *vbo);
    const GLsizei st = sizeof(M2Vert);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, st, (void *)offsetof(M2Vert, x));
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, st, (void *)offsetof(M2Vert, u));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, st, (void *)offsetof(M2Vert, depth));
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, st, (void *)offsetof(M2Vert, prio));
    glEnableVertexAttribArray(7); glVertexAttribPointer(7, 1, GL_FLOAT, GL_FALSE, st, (void *)offsetof(M2Vert, key));
    glEnableVertexAttribArray(4); glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, st, (void *)offsetof(M2Vert, r));
    glEnableVertexAttribArray(5); glVertexAttribIPointer(5, 4, GL_UNSIGNED_INT, st, (void *)offsetof(M2Vert, info));
    glEnableVertexAttribArray(6); glVertexAttribIPointer(6, 2, GL_UNSIGNED_INT, st, (void *)offsetof(M2Vert, more));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}
static GLuint int_texture(GLenum fmt, int w, int h) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexStorage2D(GL_TEXTURE_2D, 1, fmt, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

void m2_gl_init(void) {
    GLuint vs = shader(GL_VERTEX_SHADER, VS), fs = shader(GL_FRAGMENT_SHADER, FS);
    M.prog = glCreateProgram();
    glAttachShader(M.prog, vs);
    glAttachShader(M.prog, fs);
    glLinkProgram(M.prog);
    GLint ok = 0;
    glGetProgramiv(M.prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(M.prog, sizeof log, NULL, log);
        LOGE("m2 program: %s", log);
        return;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    glUseProgram(M.prog);
    M.u_mvp = glGetUniformLocation(M.prog, "mvp");
    M.u_prio_k = glGetUniformLocation(M.prog, "prio_k");
    M.u_order = glGetUniformLocation(M.prog, "order");
    M.u_eye = glGetUniformLocation(M.prog, "eye");
    M.u_key_max = glGetUniformLocation(M.prog, "key_max");
    M.u_lift = glGetUniformLocation(M.prog, "lift");
    if (M.lift <= 0) M.lift = 1;
    glUniform1i(glGetUniformLocation(M.prog, "texram"), 2);
    glUniform1i(glGetUniformLocation(M.prog, "lumaram"), 3);
    glUniform1i(glGetUniformLocation(M.prog, "xlat"), 4);
    glUseProgram(0);

    M.tex = int_texture(GL_R32UI, 1024, 1024);
    M.luma = int_texture(GL_R8UI, 256, 128);
    M.xlat = int_texture(GL_R8UI, 256, 96);
    glBindTexture(GL_TEXTURE_2D, 0);

    make_vao(&M.vao, &M.vbo);

    // the face layer: a textured quad fixed to the head
    GLuint fvs = shader(GL_VERTEX_SHADER, VS_FACE), ffs = shader(GL_FRAGMENT_SHADER, FS_FACE);
    M.face_prog = glCreateProgram();
    glAttachShader(M.face_prog, fvs);
    glAttachShader(M.face_prog, ffs);
    glLinkProgram(M.face_prog);
    glDeleteShader(fvs);
    glDeleteShader(ffs);
    M.face_mvp = glGetUniformLocation(M.face_prog, "mvp");
    glUseProgram(M.face_prog);
    glUniform1i(glGetUniformLocation(M.face_prog, "tex"), 0);
    glUseProgram(0);
    glGenTextures(1, &M.face_tex);
    glBindTexture(GL_TEXTURE_2D, M.face_tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_SRGB8_ALPHA8, 496, 384);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    {   // in head space (metres, -z ahead): 0.4 m out, as wide as the arcade's picture is there (53 degrees)
        const float d = 0.4f, hw = d * 0.5f, hh = hw * 384.0f / 496.0f;
        const float q[] = { -hw, hh, -d, 0, 0,   hw, hh, -d, 1, 0,   hw, -hh, -d, 1, 1,
                            -hw, hh, -d, 0, 0,   hw, -hh, -d, 1, 1,  -hw, -hh, -d, 0, 1 };
        glGenVertexArrays(1, &M.face_vao);
        glGenBuffers(1, &M.face_vbo);
        glBindVertexArray(M.face_vao);
        glBindBuffer(GL_ARRAY_BUFFER, M.face_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
        glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
    M.ready = 1;
}

// After d3d_vr_scene() picked the newest scene (quest_xr.c game_sync): its vertices, the tables, the texture
// RAM rows that changed.
void m2_gl_sync(void) {
    if (!M.ready) return;
    const M2Scene *s = m2_vr_scene();
    if (!s) return;
    glBindBuffer(GL_ARRAY_BUFFER, M.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(s->nv * sizeof(M2Vert)), s->v, GL_STREAM_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    M.nv = (GLsizei)s->nv;
    M.nv_world = (GLsizei)s->nv_world;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (s->luma) {
        glBindTexture(GL_TEXTURE_2D, M.luma);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 128, GL_RED_INTEGER, GL_UNSIGNED_BYTE, s->luma);
    }
    if (s->xlat) {
        glBindTexture(GL_TEXTURE_2D, M.xlat);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 96, GL_RED_INTEGER, GL_UNSIGNED_BYTE, s->xlat);
    }
    if (s->tex) {
        glBindTexture(GL_TEXTURE_2D, M.tex);
        for (uint32_t r = 0; r < M2_TEX_ROWS;) {     // runs of changed rows
            if (!s->tex_all && !s->tex_row_dirty[r]) { r++; continue; }
            uint32_t e = r + 1;
            while (e < M2_TEX_ROWS && (s->tex_all || s->tex_row_dirty[e])) e++;
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, (GLint)r, 1024, (GLsizei)(e - r), GL_RED_INTEGER, GL_UNSIGNED_INT,
                            s->tex + (size_t)r * 1024);
            r = e;
        }
    }
    if (s->face && s->face_serial != M.face_serial) {   // the face layer, when it changed
        M.face_serial = s->face_serial;
        M.face_any = s->face_any;
        if (M.face_any) {
            glBindTexture(GL_TEXTURE_2D, M.face_tex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 496, 384, GL_RGBA, GL_UNSIGNED_BYTE, s->face);
        }
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

void m2_gl_draw_face(const float *proj, float eye_x) {
    if (!M.ready || !M.face_any) return;
    // head space -> this eye's: the head's middle is -eye_x along the eye's x
    float mvp[16];
    for (int i = 0; i < 16; i++) mvp[i] = proj[i];
    for (int r = 0; r < 4; r++) mvp[12 + r] = proj[12 + r] - eye_x * proj[r];
    glUseProgram(M.face_prog);
    glUniformMatrix4fv(M.face_mvp, 1, GL_FALSE, mvp);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, M.face_tex);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(M.face_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glBindTexture(GL_TEXTURE_2D, 0);
}

// The key depth's uniforms from mvp (column-major, clip <- the polygons' space; a perspective projection): the eye
// is the point clip x, y and w are 0 at, its clip z eye.w; key_max keeps the key planes inside the far plane
static void key_uniforms(const float *m) {
    // rows 0, 1, 3: r . (e, 1) = 0
    const float a[3][4] = { { m[0], m[4], m[8], m[12] }, { m[1], m[5], m[9], m[13] }, { m[3], m[7], m[11], m[15] } };
    const float det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                      a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    float e[4] = { 0, 0, 0, 0 }, kmax = 1e30f;
    if (det > 1e-20f || det < -1e-20f) {
        for (int c = 0; c < 3; c++) {   // Cramer: column c replaced by -r[3]
            float b[3][3];
            for (int r = 0; r < 3; r++)
                for (int k = 0; k < 3; k++) b[r][k] = k == c ? -a[r][3] : a[r][k];
            e[c] = (b[0][0] * (b[1][1] * b[2][2] - b[1][2] * b[2][1]) - b[0][1] * (b[1][0] * b[2][2] - b[1][2] * b[2][0]) +
                    b[0][2] * (b[1][0] * b[2][1] - b[1][1] * b[2][0])) / det;
        }
        e[3] = m[2] * e[0] + m[6] * e[1] + m[10] * e[2] + m[14];
        // the far plane (clip z = w) along the view axis: w's row is k x the view axis (k: units to the eye's), z's row
        // p10 x it (and the eye's z): far = e.w / (p10 + 1) in the eye's units, / k in ours
        const float k2 = m[3] * m[3] + m[7] * m[7] + m[11] * m[11];
        const float p10 = k2 > 0 ? -(m[2] * m[3] + m[6] * m[7] + m[10] * m[11]) / k2 : 0;
        const float far_d = p10 + 1.0f != 0 ? e[3] / (p10 + 1.0f) : -1;
        if (k2 > 0 && far_d > 0) kmax = 0.9f * far_d / sqrtf(k2);
    }
    glUniform4f(M.u_eye, e[0], e[1], e[2], e[3]);
    glUniform1f(M.u_key_max, kmax);
}

void m2_gl_draw(const float *mvp) {
    if (!M.ready || !M.nv) return;
    glUseProgram(M.prog);
    glUniformMatrix4fv(M.u_mvp, 1, GL_FALSE, mvp);
    key_uniforms(mvp);
    glUniform1f(M.u_prio_k, M.prio_k);
    glUniform1f(M.u_order, M.order);
    glUniform1f(M.u_lift, M.lift);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, M.tex);
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, M.luma);
    glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, M.xlat);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(M.vao);
    glDrawArrays(GL_TRIANGLES, 0, M.nv_world);
    if (M.nv > M.nv_world) {   // the shots' effects, on the surfaces they hit: in the arcade's order (m2_vr.h)
        glDepthMask(GL_FALSE);
        glDrawArrays(GL_TRIANGLES, M.nv_world, M.nv - M.nv_world);
        glDepthMask(GL_TRUE);
    }
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
}

// The courtyard, once its file exists (VR thread). The levelling: the rotation taking the saved up to +y about the
// axis between them (the heading kept).
static void load_backdrop(void) {
    const char *path = arcade_courtyard_file();
    if (!path || B.tried) return;
    B.tried = 1;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char magic[8];
    uint32_t n = 0;
    float up[3];
    M2Vert *v = NULL;
    uint32_t *tex = NULL;
    uint8_t *luma = NULL, *xlat = NULL;
    if (fread(magic, 1, 8, f) == 8 && !memcmp(magic, "M2WORLD1", 8) && fread(&n, 4, 1, f) == 1 && n && n < 2000000 &&
        fread(up, 4, 3, f) == 3 && (v = malloc((size_t)n * sizeof *v)) && fread(v, sizeof *v, n, f) == n &&
        (tex = malloc(M2_TEX_WORDS * 4)) && fread(tex, 4, M2_TEX_WORDS, f) == M2_TEX_WORDS &&
        (luma = malloc(M2_LUMA_ENTRIES)) && fread(luma, 1, M2_LUMA_ENTRIES, f) == M2_LUMA_ENTRIES &&
        (xlat = malloc(M2_XLAT_ENTRIES)) && fread(xlat, 1, M2_XLAT_ENTRIES, f) == M2_XLAT_ENTRIES) {
        make_vao(&B.vao, &B.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, B.vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)n * sizeof *v, v, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        B.tex = int_texture(GL_R32UI, 1024, 1024);
        B.luma = int_texture(GL_R8UI, 256, 128);
        B.xlat = int_texture(GL_R8UI, 256, 96);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glBindTexture(GL_TEXTURE_2D, B.tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1024, 1024, GL_RED_INTEGER, GL_UNSIGNED_INT, tex);
        glBindTexture(GL_TEXTURE_2D, B.luma);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 128, GL_RED_INTEGER, GL_UNSIGNED_BYTE, luma);
        glBindTexture(GL_TEXTURE_2D, B.xlat);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 96, GL_RED_INTEGER, GL_UNSIGNED_BYTE, xlat);
        glBindTexture(GL_TEXTURE_2D, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        B.nv = (GLsizei)n;
        // the levelling: Rodrigues about up x (0,1,0)
        float l = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
        float ux = up[0] / l, uy = up[1] / l, uz = up[2] / l;
        float ax = uz, az = -ux;                              // the axis: y x up
        float sn = sqrtf(ax * ax + az * az), cs = uy;
        float r[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        if (sn > 1e-6f) {
            ax /= sn; az /= sn;
            float t = 1 - cs;
            r[0] = cs + ax * ax * t; r[1] = az * sn;         r[2] = ax * az * t;
            r[3] = -az * sn;         r[4] = cs;              r[5] = ax * sn;
            r[6] = az * ax * t;      r[7] = -ax * sn;        r[8] = cs + az * az * t;
        }
        memcpy(B.level, r, sizeof r);
        B.ok = 1;
        LOGI("courtyard loaded: %u vertices", n);
    }
    free(v); free(tex); free(luma); free(xlat);
    fclose(f);
}

int m2_gl_backdrop_ready(void) {
    if (!B.tried) load_backdrop();
    return B.ok;
}

void m2_gl_draw_backdrop(const float *mvp) {
    if (!M.ready || !m2_gl_backdrop_ready()) return;
    // mvp x the levelling (column-major: m = mvp * L)
    float L[16] = { B.level[0], B.level[3], B.level[6], 0,  B.level[1], B.level[4], B.level[7], 0,
                    B.level[2], B.level[5], B.level[8], 0,  0, 0, 0, 1 }, m[16];
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            float a = 0;
            for (int k = 0; k < 4; k++) a += mvp[k * 4 + r] * L[c * 4 + k];
            m[c * 4 + r] = a;
        }
    glUseProgram(M.prog);
    glUniformMatrix4fv(M.u_mvp, 1, GL_FALSE, m);
    key_uniforms(m);
    glUniform1f(M.u_prio_k, M.prio_k);
    glUniform1f(M.u_order, 0.0f);
    glUniform1f(M.u_lift, M.lift);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, B.tex);
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, B.luma);
    glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, B.xlat);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(B.vao);
    glDrawArrays(GL_TRIANGLES, 0, B.nv);
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
}

// The projection's near plane (game units) for the next draws: a 24-bit depth buffer's step at distance d is about
// d * d / (near * 2^24); the draw order's step (arcade_quest.cpp kRankStep, 2.5e-5 of d) is kept at 3 of them.
// Brightness: the arcade's black level raised (arcade.cpp's gamma table: 0% MAME's, everything under a quarter
// black; 100% nothing crushed), and a gentle power on top (100%: 1/1.4)
void m2_gl_brightness(int percent) {
    extern volatile int arcade_brightness;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    arcade_brightness = percent;
    M.lift = 1.0f / (1.0f + 0.4f * (float)percent / 100.0f);
}
void m2_gl_arcade_order(int on) {
    // prio is rank x 2.5e-5 (arcade.cpp kRankStep): 65536 ranks over the depth range, each a step of it
    M.order = on ? 1.0f / (2.5e-5f * 65536.0f) : 0.0f;
}
void m2_gl_near(float near_game_units) {
    if (near_game_units < 1e-4f) near_game_units = 1e-4f;
    M.prio_k = 3.0f / 16777216.0f / near_game_units / 2.5e-5f;
}

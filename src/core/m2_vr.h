// The arcade game's 3D for the Quest build (scripts/build_quest.py): what the
// game thread (arcade_quest.cpp, the recompiled Model 2 game) hands the VR
// thread (m2_gl.c, called from src/quest/quest_xr.c) each frame.
//
// The polygons are in the game camera's space (x right, y up, z forward, the
// game's units), as d3d_vr.h's VrScene: the same triangles are published
// there too (one batch, for the guns' aim). Here each vertex also carries the
// polygon's Model 2 texture state, and the tables the rasterizer colours with
// come along, so m2_gl.c shades every pixel as src/app/gpu/m2.hlsl does.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float x, y, z;          // camera space
    float u, v;             // texel coordinates (8.8 fixed point, as floats): render_one's u and v
    float depth;            // the arcade camera's z (the texture LOD is chosen from it)
    float prio;             // the polygon's place in the arcade's draw order, as a step out along its view ray
                            // (drawn at position x (1 + prio))
    float r, g, b;          // a solid polygon's colour (gamma applied)
    uint32_t info[4];       // m2.hlsl PolyIn.info
    uint32_t more[2];       // m2.hlsl PolyIn.more
} M2Vert;

#define M2_TEX_WORDS 0x100000u   // texture RAM: sheet 0 then sheet 1, 0x80000 dwords each (1024 x 1024 R32UI)
#define M2_TEX_ROWS 1024u        // rows of 1024 dwords (4 KB: the board's texture page)
#define M2_LUMA_ENTRIES 0x8000u  // luma RAM, one byte per entry (256 x 128 R8UI)
#define M2_XLAT_ENTRIES 0x6000u  // the colour translation with the rasterizer's gamma (256 x 96 R8UI)

typedef struct {
    const M2Vert *v;
    uint32_t nv;                 // vertices, 3 per triangle
    uint32_t nv_world;           // the world's (depth tested); the rest are the screen's effects, drawn over it
    const uint32_t *tex;         // M2_TEX_WORDS
    const uint8_t *tex_row_dirty;// M2_TEX_ROWS flags: rows changed since the VR thread last took a scene
    int tex_all;                 // every row changed
    const uint8_t *luma;         // M2_LUMA_ENTRIES
    const uint8_t *xlat;         // M2_XLAT_ENTRIES
    uint32_t serial;
    // The face layer: the game's damage marks (a zombie's hit), shown on your face rather than in the picture:
    // RGBA bytes, 496 x 384 (the picture's size); face_any: something in it; face_serial moves when it changes.
    const uint32_t *face;
    int face_any;
    uint32_t face_serial;
} M2Scene;

// VR thread: the newest scene (NULL before the first). It stays valid until the next call; the dirty rows
// are those since the previous call, which the caller uploads.
const M2Scene *m2_vr_scene(void);

// m2_gl.c (VR thread, GL context current): set up, take the newest scene, draw it with mvp = clip <- camera space.
void m2_gl_init(void);
void m2_gl_sync(void);
void m2_gl_draw(const float *mvp);
// Brightness (quest_options.h QOPT_BRIGHTNESS, 0-100): the arcade's black level raised (arcade.cpp's gamma table)
// and its colours raised to a power under 1, lifting the shadows (0: as drawn; whites stay white)
void m2_gl_brightness(int percent);
// the projection's near plane in game units, for the next draws (how far the draw order's steps must reach)
void m2_gl_near(float near_game_units);
// The face layer, for one eye: proj is the eye's projection; eye_x the eye's offset from the middle of the head
// (metres, left eye negative). Drawn in front of everything, fixed to the head.
void m2_gl_draw_face(const float *proj, float eye_x);
// The cabinet room's courtyard (made at the first start: arcade_quest.cpp make_courtyard): 1 once loaded; draw with
// mvp = clip <- the camera space it was saved in (as the live scene's)
int m2_gl_backdrop_ready(void);
void m2_gl_draw_backdrop(const float *mvp);

#ifdef __cplusplus
}
#endif

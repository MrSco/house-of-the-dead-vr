// Each game frame's scene in the game camera's space (VrScene), published by the game thread (arcade_quest.cpp)
// for the VR thread: the guns' aim hits its triangles, and its projection, up and turn place the world and the HUD.
// (The picture itself is drawn from m2_vr.h's M2Scene by m2_gl.c.) The game's 2D layers come as an RGB565 frame
// whose D3D_VR_KEY pixels are see-through. (The layout began as the PC port's Direct3D scene, hence its names.)
#pragma once
#include <stdint.h>

#define D3D_VR_KEY 0x0821u            // RGB565 of the back buffer where nothing 2D was drawn (transparent)

// game camera space: x right, y up, z forward, in game units (the camera at the origin)
typedef struct { float x, y, z; uint32_t color, spec; float u, v; } VrVert;   // colours: D3DCOLOR 0xAARRGGBB
typedef struct {
    uint32_t first, count;            // vertices, 3 per triangle
    int32_t tex;                      // texture (surface id), -1: none
    uint8_t blend, src, dst;          // alpha blending; D3DBLEND source and destination factors
    uint8_t atest, afunc, aref;       // alpha test: on, D3DCMP function, reference 0..255
    uint8_t zen, zwrite, zfunc;       // depth test, depth writes, D3DCMP function
    uint8_t tblend, linear, wrap;     // D3DTBLEND mode, bilinear filtering, D3DTADDRESS mode
    uint8_t fog, specular, cull, ck;  // vertex fog (factor in spec alpha), specular added, D3DCULL mode, colour key
    int16_t sector;                   // drawn by the pass looking this way (degrees, game camera yaw, + = left, or
                                      // VR_SECTOR_DOWN)...
    uint8_t sectored;                 // ...which only keeps its own slice of directions (several passes), else all
    uint8_t group;                    // 0, or 1-4: drawn in an earlier frame, moved into this one by groupm[group - 1]
    uint8_t extra;                    // scenery of the areas around the game's (d3d.c vr_more_blocks): drawn after the
                                      // rest, only where nothing solid of the game's own world was drawn, and never
                                      // inside the game's own picture (it may overlap the room you are in)
    uint32_t fogcolor;                // D3DCOLOR
} VrBatch;
typedef struct {
    VrVert *v; uint32_t nv, vcap;
    VrBatch *b; uint32_t nb, bcap;
    uint32_t fade;                    // a full-screen colour drawn over everything (D3DCOLOR with alpha), 0: none
    uint32_t clear;                   // the background colour (0x00RRGGBB)
    float fx, fy, cx, cy;             // the game's projection: screen x = cx + fx x / z, y = cy + fy y / z
    float groupm[4][12];              // an earlier frame's camera space -> this one's (3x3 rows, then translation)
    int down;                         // 1: a pass looks straight down, 2: one up, 3: both; the others leave them what lies
                                      // below -VR_DOWN_SIN / above VR_DOWN_SIN
    int w, h;                         // the game's frame (640 x 480)
    float up[3]; int up_ok;           // the world's up in the game camera's space (from the scenery), when known
    float turn, turn_all;             // how fast the game camera turns, radians per game frame: its heading, all of it
    float move;                       // and how fast it moves, game units per game frame
    uint32_t serial;                  // counts published scenes
} VrScene;
// VR thread: the newest published scene (NULL before the first); it stays valid until the next call
const VrScene *d3d_vr_scene(void);

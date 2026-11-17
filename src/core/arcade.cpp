// The House of the Dead (arcade, recompiled) as the game of the Quest VR app
// (src/quest on main: OpenXR, the guns, the menus, online play, voice). The
// app runs SDL_main on its own thread against its small SDL2 subset
// (host_sdl.c); this is that SDL_main, and the few calls the VR side makes
// into its game:
//  - the game: rt::GameLoop on the images of the player's ROM set
//    (game/hotd.zip, or the importer's images in game/), paced at the
//    board's 57.52 frames a second;
//  - 3D: each frame's polygons (the geometrizer's, external-3D mode) in the
//    game camera's space, as d3d_vr.h's VrScene (for the guns' aim) and as
//    m2_vr.h's M2Scene with their Model 2 texture state (m2_gl.c draws them);
//  - 2D: the tilemap layers (the HUD, text, the 2D screens) through
//    SDL_RenderCopy, D3D_VR_KEY where nothing is drawn: the VR overlay;
//  - input: the gun's aim and trigger as the mouse, reload as the right
//    button, the skip button as two coins and Start; online, the lockstep of
//    arcade_net.cpp (two players, as at the cabinet);
//  - sound: the SCSP board's output through the SDL audio callback.
#include "core/m2_vr.h"
#include "core/arcade_net.h"
#include "platform/platform.h"

#include "games/game.h"
#include "runtime/board/cabinet_controls.h"
#include "runtime/board/game_loop.h"
#include "runtime/romset/rom_import.h"
#include "runtime/sound/scsp.h"
#include "tools/common/nvram.h"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <chrono>
#include <thread>

#include <tuple>
#include <vector>

extern "C" {
#include "d3d_vr.h"
#include "quest.h"
}

#define LOGI(...) plat_log(PLAT_LOG_INFO, __VA_ARGS__)
#define LOGE(...) plat_log(PLAT_LOG_ERROR, __VA_ARGS__)

// ---- what the VR side (quest_xr.c, host_sdl.c) shares with its game
extern "C" {
// the game's own crosshairs on its 2D layer: 0 none; 1 the cabinet's (alone: your own; online: the partner's, your own
// gun has a laser); 2 your own and, online, the partner's too (the flat apps: a mouse has nothing else to show it)
int port_draw_crosshairs = 0;
volatile int arcade_view_cabinet = 0; // quest_xr.c: the cabinet view (its picture is the arcade's: no face layer)
volatile int arcade_screen_scene = 0; // a scene to show on a screen, not round you (build: the game-over scene)
float port_music_gain = 1.0f, port_sfx_gain = 1.0f;
// Player p's bullets (0-6), -1 while that player isn't in the game: what the app's guns go by (the empty gun's
// locked slide and silent laser, the magazine, the reload hints). The game thread keeps these (update_ammo).
static std::atomic<int> g_ammo[2] = {-1, -1};
int host_player_ammo(int p) { return p >= 0 && p < 2 ? g_ammo[p].load() : -1; }
static std::atomic<int> g_playing[2] = {0, 0};
int host_player_playing(int p) { return p >= 0 && p < 2 ? g_playing[p].load() : 0; }
void host_log(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    plat_vlog(PLAT_LOG_INFO, fmt, a);
    va_end(a);
}
// (online play, the net_* calls online.c makes: arcade_net.cpp)
}

namespace rt { extern uint64_t tgpx4_ns; } // (tgp_board.cpp: the TGPx4's time so far)
uint64_t tgpx4_ns_total();

namespace {
uint64_t now_ns();

// ---- scenes: the game thread fills one slot while the VR thread reads another
struct Slot {
    std::vector<VrVert> vv;
    std::vector<M2Vert> mv;  // the world, then (from nv_world) the screen's effects
    uint32_t nv_world = 0;
    VrBatch batch{};
    VrScene sc{};
    M2Scene m{};
    std::vector<uint8_t> luma, xlat;
};
struct Shared {
    std::mutex mu;
    Slot slots[3];
    int latest = -1, reading = -1;
    uint32_t serial = 0;
    std::vector<uint32_t> tex = std::vector<uint32_t>(M2_TEX_WORDS);
    std::vector<uint8_t> dirty = std::vector<uint8_t>(M2_TEX_ROWS, 1), taken = std::vector<uint8_t>(M2_TEX_ROWS);
    bool tex_all = true;
    // the face layer (draw_overlay): RGBA, W x H; its serial moves when it changes
    std::vector<uint32_t> face = std::vector<uint32_t>(size_t(rt::Video::W) * rt::Video::H);
    bool face_any = false;
    uint32_t face_serial = 0;
} g;

void publish_face(const std::vector<uint32_t> &face, bool any) {
    std::lock_guard<std::mutex> lock(g.mu);
    if (!any && !g.face_any) return; // still nothing
    std::copy(face.begin(), face.end(), g.face.begin());
    g.face_any = any;
    g.face_serial++;
}

inline uint16_t le16(const uint8_t *base, uint32_t index) { return uint16_t(base[index * 2] | base[index * 2 + 1] << 8); }

// The rasterizer's gamma (MAME video_start: (i - 64) * 255 / 191, everything under 64 black) with the black level
// Brightness raises, as a cabinet's monitor was turned up (0%: MAME's; 100%: nothing crushed). Only the picture
// (the scenes the shells draw), never the game's state: crossplay is the same. The shells set arcade_brightness
// (m2_gl_brightness); the game thread rebuilds the table when it changes.
uint8_t gamma_lut[256];
extern "C" { volatile int arcade_brightness = 50; }
int gamma_for = -1;
void update_gamma() {
    const int p = std::clamp(int(arcade_brightness), 0, 100);
    if (p == gamma_for) return;
    gamma_for = p;
    const double black = 64.0 * (1.0 - p / 100.0);
    for (int i = 0; i < 256; i++)
        gamma_lut[i] = uint8_t(std::lround(std::max((double(i) - black) * 255.0 / (255.0 - black), 0.0)));
}

// The draw order's step out along the view ray (build), and the screen's effects: the shots' bursts, sparks and
// blood sprays are translucent polygons the arcade puts within kEffectZ (game units) of its camera, where it means
// them to land on whatever is behind them in the picture (the zombie hit). They go out to that surface (a coarse
// depth map of the frame's world, DepthGrid), a little in front of it; with nothing behind, kEffectDistance.
// (Scenery that near the camera is opaque or off the screen: the effects are the translucent ones on it.)
constexpr float kRankStep = 2.5e-5f;
constexpr float kEffectZ = 1.05f;
constexpr float kEffectDistance = 13.6f;
constexpr float kEffectInFront = 0.96f; // of the surface's distance

// The nearest world surface along each 8 x 8 pixel cell of the arcade's picture (1/z, 0: none), from the frame's
// camera-space triangles.
struct DepthGrid {
    static constexpr int kCell = 8, kW = 496 / kCell, kH = 384 / kCell;
    float ooz[kW * kH];
    void clear() { std::fill(std::begin(ooz), std::end(ooz), 0.f); }
    // a triangle in camera space; proj: screen x = cx + fx X / Z, y = cy + fy Y / Z
    void add(const M2Vert &a, const M2Vert &b, const M2Vert &c, float fx, float fy, float cx, float cy) {
        if (a.z <= 0.05f || b.z <= 0.05f || c.z <= 0.05f) return;
        const float ax = cx + fx * a.x / a.z, ay = cy + fy * a.y / a.z, bx = cx + fx * b.x / b.z, by = cy + fy * b.y / b.z;
        const float qx = cx + fx * c.x / c.z, qy = cy + fy * c.y / c.z;
        const float area = (bx - ax) * (qy - ay) - (qx - ax) * (by - ay);
        if (std::fabs(area) < 1e-6f) return;
        const int x0 = std::max(0, int(std::floor(std::min({ax, bx, qx}) / kCell))), x1 = std::min(kW - 1, int(std::max({ax, bx, qx}) / kCell));
        const int y0 = std::max(0, int(std::floor(std::min({ay, by, qy}) / kCell))), y1 = std::min(kH - 1, int(std::max({ay, by, qy}) / kCell));
        for (int gy = y0; gy <= y1; gy++)
            for (int gx = x0; gx <= x1; gx++) {
                const float px = (float(gx) + 0.5f) * kCell, py = (float(gy) + 0.5f) * kCell;
                const float w0 = ((bx - px) * (qy - py) - (qx - px) * (by - py)) / area;
                const float w1 = ((qx - px) * (ay - py) - (ax - px) * (qy - py)) / area;
                const float w2 = 1.f - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                const float o = w0 / a.z + w1 / b.z + w2 / c.z; // 1/z is linear on the screen
                float &cell = ooz[gy * kW + gx];
                cell = std::max(cell, o);
            }
    }
    // the nearest surface's z around a screen point (3 x 3 cells), or 0
    float z_at(float x, float y) const {
        const int gx = int(x / kCell), gy = int(y / kCell);
        float o = 0.f;
        for (int j = gy - 1; j <= gy + 1; j++)
            for (int i = gx - 1; i <= gx + 1; i++)
                if (i >= 0 && i < kW && j >= 0 && j < kH) o = std::max(o, ooz[j * kW + i]);
        return o > 0.f ? 1.f / o : 0.f;
    }
} depth_grid;

// Comfort (the app's Horizon and Turn vignette, d3d_vr.h VrScene up / turn / turn_all; a fade on camera cuts): the
// camera's orientation from the sky, a cylinder of panels round the camera (window 1, about 1,800 units out) whose
// top and bottom are fans: the line from the bottom's centre to the top's is the world's up, and a panel's corner
// (the same every frame) gives the heading.
struct Comfort {
    bool have = false, ever = false;               // the last frame had the sky; some frame had
    float up[3] = {0, 1, 0}, ref[3] = {0, 0, 1}; // the last up and horizontal reference seen, camera space
    int fade = 0;                                // frames of the cut's fade left
} comfort;
constexpr float kCutTurn = 0.44f;  // radians in one frame (25 degrees): a cut, not a turn
constexpr float kSkyNear = 200.f;
constexpr int kScreenHold = 10;  // the sky's points are all further than this (game units; its fans' centres ~300)
constexpr int kCutFade = 9;        // frames the fade takes (from black)

inline void norm3(float *v) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6f) v[0] /= l, v[1] /= l, v[2] /= l;
}
inline float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline void cross3(const float *a, const float *b, float *o) {
    o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
}

// The world's up and a horizontal heading reference (camera space) from the sky, a cylinder of panels round the
// camera (window 1): the line from its bottom fan's centre to its top's, and a panel's corner. False: no sky.
bool sky_frame(const std::vector<rt::GeoPoly> &polys, float *up, float *ref) {
    std::map<std::tuple<int, int, int>, std::pair<int, std::array<float, 3>>> seen; // vertex -> uses, position
    const rt::GeoPoly *first = nullptr;
    int n = 0;
    for (const rt::GeoPoly &q : polys) {
        if (q.window != 1) continue;
        if (!first) first = &q;
        n++;
        for (int k = 0; k < q.num_vertices; k++) {
            const float x = q.v[k].x / q.focus[0], y = q.v[k].y / q.focus[1], z = q.v[k].p[0];
            auto &e = seen[{int(std::lround(x)), int(std::lround(y)), int(std::lround(z))}];
            e.first++, e.second = {x, y, z};
        }
    }
    // (Window 1 holds other things too: the game-over scene's falling character, alone in the dark, a few
    // units away. The sky is a ring of panels with every point hundreds of units out.)
    bool far = n >= 24 && n <= 64;
    for (auto &[k, e] : seen) {
        const auto &q = e.second;
        if (q[0] * q[0] + q[1] * q[1] + q[2] * q[2] < kSkyNear * kSkyNear) { far = false; break; }
    }
    bool ok = false;
    if (far && first) {
        // the two fan centres: the vertices most used
        std::vector<std::pair<int, std::array<float, 3>>> v;
        for (auto &[k, e] : seen) v.push_back(e);
        std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.first > b.first; });
        if (v.size() >= 2 && v[1].first >= 4) {
            const auto &a = v[0].second, &b = v[1].second;
            const bool a_top = a[1] > b[1];
            const auto &top = a_top ? a : b, &bottom = a_top ? b : a;
            up[0] = top[0] - bottom[0], up[1] = top[1] - bottom[1], up[2] = top[2] - bottom[2];
            norm3(up);
            ref[0] = first->v[0].x / first->focus[0], ref[1] = first->v[0].y / first->focus[1], ref[2] = first->v[0].p[0];
            const float d = dot3(ref, up);
            ref[0] -= d * up[0], ref[1] -= d * up[1], ref[2] -= d * up[2];
            norm3(ref);
            ok = up[1] > 0.3f; // (a camera upside down: leave it to the game)
        }
    }
    return ok;
}

// up, turn and the cut fade for the scene being built
void comfort_from_sky(const std::vector<rt::GeoPoly> &polys, VrScene &sc) {
    float turn = 0, turn_all = 0;
    float up[3], ref[3];
    const bool ok = sky_frame(polys, up, ref);
    if (ok && comfort.have) {
        float c[3];
        cross3(comfort.ref, ref, c);
        turn = std::fabs(std::atan2(dot3(c, up), dot3(comfort.ref, ref)));
        // the whole turn: between the frames (ref, up, ref x up) then and now
        float s0[3], s1[3];
        cross3(comfort.ref, comfort.up, s0);
        cross3(ref, up, s1);
        const float tr = dot3(comfort.ref, ref) + dot3(comfort.up, up) + dot3(s0, s1);
        turn_all = std::acos(std::clamp((tr - 1.f) * 0.5f, -1.f, 1.f));
        if (turn_all > kCutTurn) comfort.fade = kCutFade, turn = turn_all = 0; // a cut: fade, no vignette
    }
    if (ok) std::copy(up, up + 3, comfort.up), std::copy(ref, ref + 3, comfort.ref), comfort.ever = true;
    if (comfort.ever) { // (no sky this frame, indoors: the last up, rather than the world snapping by the camera's pitch)
        sc.up[0] = comfort.up[0], sc.up[1] = comfort.up[1], sc.up[2] = comfort.up[2];
        sc.up_ok = 1;
    }
    comfort.have = ok;
    sc.turn = turn, sc.turn_all = turn_all;
    if (comfort.fade > 0) { // black, clearing over kCutFade frames
        const uint32_t a = uint32_t(255 * comfort.fade / kCutFade);
        sc.fade = a << 24;
        comfort.fade--;
    }
}

// The game's projection, kept from the last frame with 3D (the 2D screens use it for the overlay).
struct Projection {
    float fx = 420.0f, fy = -420.0f, cx = 248.0f, cy = 192.0f;
} proj;

// The frame's main camera: the commonest centre and focus among its polygons (kept when it has none)
void choose_projection(const std::vector<rt::GeoPoly> &polys, int windows, int crtc_x, int crtc_y, Projection &pj) {
    std::map<std::tuple<int, int, float, float>, int> count;
    for (const rt::GeoPoly &p : polys)
        if (p.window <= windows) count[{p.center[0], p.center[1], p.focus[0], p.focus[1]}]++;
    if (count.empty()) return;
    const auto best = std::max_element(count.begin(), count.end(), [](auto &a, auto &b) { return a.second < b.second; })->first;
    pj.cx = float(crtc_x + std::get<0>(best));
    pj.cy = float((384 - std::get<1>(best)) + crtc_y);
    pj.fx = std::get<2>(best);
    pj.fy = -std::get<3>(best);
}

// One polygon as camera-space vertices for the main camera (c0m, c1m: its centre; fxm, fym: its focus), with its
// Model 2 state; effect: one of the shots' effects (build places those). 0: nothing to draw.
int poly_vertices(const rt::GeoPoly &poly, const rt::VideoMem &mem, uint32_t rank, int c0m, int c1m, float fxm, float fym,
                  M2Vert *pv, bool &effect) {
    const int renderer = (poly.texheader[0] >> 13) & 3;
    if (renderer == 1) return 0; // translucent solid: the rasterizer draws nothing

    const uint32_t colorbase = (poly.texheader[3] >> 6) & 0x3ff;
    const uint32_t luma = poly.luma >> 2;
    const uint32_t color = le16(mem.palram, colorbase + 0x1000);
    const float r = gamma_lut[le16(mem.colorxlat, 0x0000 / 2 + (((color >> 0) & 0x1f) << 8) + luma) & 0xff] / 255.0f;
    const float gr = gamma_lut[le16(mem.colorxlat, 0x4000 / 2 + (((color >> 5) & 0x1f) << 8) + luma) & 0xff] / 255.0f;
    const float b = gamma_lut[le16(mem.colorxlat, 0x8000 / 2 + (((color >> 10) & 0x1f) << 8) + luma) & 0xff] / 255.0f;

    // texture state as gpu_renderer.cpp decodes it (m2.hlsl PolyIn)
    const uint32_t th0 = poly.texheader[0], th1 = poly.texheader[1], th2 = poly.texheader[2];
    const uint32_t mirx = (th0 >> 8) & 1, miry = (th0 >> 9) & 1;
    const uint32_t wlog = th0 & 7, hlog = (th0 >> 3) & 7;
    const uint32_t max_level = 4 + std::min(wlog, hlog);
    M2Vert mv{};
    mv.info[0] = uint32_t(renderer) | ((th0 >> 15) & 1) << 2 | (((th0 >> 6) & 1) & ~mirx) << 3 |
                 (((th0 >> 7) & 1) & ~miry) << 4 | mirx << 5 | miry << 6 | ((th0 >> 12) & 1) << 7 |
                 ((th0 >> 10) & 3) << 8 | ((th2 >> 12) & 1) << 10 | max_level << 12 | wlog << 17 | hlog << 20;
    mv.info[1] = (32u * (th2 & 0x3f)) | (32u * ((th2 >> 6) & 0x1f)) << 16;
    mv.info[2] = (((th2 >> 13) & 1) * 128) | (((th2 >> 14) & 3) * 128) << 16;
    mv.info[3] = ((th1 & 0xff) << 7) | uint32_t(poly.luma) << 16;
    mv.more[0] = uint32_t(poly.texlod);
    mv.more[1] = color & 0x7fff;
    mv.r = r, mv.g = gr, mv.b = b;
    // The Model 2 has no depth buffer: the first polygon in its order to reach a pixel wins, which is how
    // decals (window frames, shadows, blood on the ground) stay on the surfaces after them. Here each
    // step in that order sits a little further out along its own view ray (m2_gl.c: position x
    // (1 + prio)), enough for the depth buffer to keep the arcade's choice between coplanar polygons;
    // along its own ray a point stays where the picture has it.
    mv.prio = float(rank) * kRankStep;

    // camera space for the main camera: a polygon of another centre keeps its place in the picture
    const float dc0 = float(poly.center[0] - c0m), dc1 = float(poly.center[1] - c1m);
    // The screen's effects (shots' flashes, blood sprays, the bullets' streaks): polygons the arcade puts
    // a hand's width from its camera. In stereo that is between the eyes: they go out to the HUD's
    // distance, still where the picture has them, and are drawn over the world as the arcade does.
    float zmin = 1e30f, zmax = -1e30f;
    for (int k = 0; k < poly.num_vertices; k++) zmin = std::min(zmin, poly.v[k].p[0]), zmax = std::max(zmax, poly.v[k].p[0]);
    effect = renderer == 3 && zmin > 0.f && zmax <= kEffectZ;
    for (int k = 0; k < poly.num_vertices; k++) {
        const rt::GeoVertex &g0 = poly.v[k];
        const float z = g0.p[0];
        pv[k] = mv;
        pv[k].x = (g0.x + dc0 * z) / fxm;
        pv[k].y = (g0.y + dc1 * z) / fym;
        pv[k].z = z;
        pv[k].u = g0.p[1] * 32.0f; // render_one: p[1] / 8 * 256
        pv[k].v = g0.p[2] * 32.0f;
        // the texture level from the distance (the arcade's z, for its own camera's view: behind it
        // that is negative, and to the sides too small)
        pv[k].depth = std::sqrt(pv[k].x * pv[k].x + pv[k].y * pv[k].y + z * z);
        if (effect) { // placed on the world behind it once the world is known (below); its texture level as the arcade's
            pv[k].depth = z;
            pv[k].prio = 0;
        }
    }
    return poly.num_vertices;
}

// One frame's polygons -> slot (camera space, the arcade's draw order).
void build(Slot &s, const rt::Video &video, uint32_t serial) {
    s.vv.clear();
    s.mv.clear();
    const std::vector<rt::GeoPoly> &polys = video.gpu_polys();
    const rt::VideoMem &mem = video.gpu_mem();
    const int windows = video.gpu_windows();
    const int crtc_x = video.crtc_x(), crtc_y = video.crtc_y();
    const bool have_mem = mem.palram && mem.colorxlat && mem.lumaram;

    choose_projection(polys, windows, crtc_x, crtc_y, proj);
    const int c0m = int(proj.cx) - crtc_x, c1m = 384 + crtc_y - int(proj.cy);
    const float fxm = proj.fx, fym = -proj.fy;

    if (have_mem && fxm != 0.f && fym != 0.f) {
        // Raster::render's order: windows from the last down to 0, low z first, newest first within z
        std::vector<size_t> order(polys.size());
        std::iota(order.begin(), order.end(), size_t(0));
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            if (polys[a].window != polys[b].window) return polys[a].window > polys[b].window;
            if (polys[a].z != polys[b].z) return polys[a].z < polys[b].z;
            return a > b;
        });
        std::vector<M2Vert> fx; // the screen's effects (below), in the arcade's order
        uint32_t rank = 0;
        for (size_t i : order) {
            const rt::GeoPoly &poly = polys[i];
            ++rank;
            if (poly.window > windows || poly.num_vertices < 3) continue;
            bool effect = false;
            M2Vert pv[8];
            if (!poly_vertices(poly, mem, rank, c0m, c1m, fxm, fym, pv, effect)) continue;
            for (int k = 1; k + 1 < poly.num_vertices; k++) {
                for (const M2Vert *q : {&pv[0], &pv[k], &pv[k + 1]}) {
                    if (effect) {
                        fx.push_back(*q);
                        continue; // (not in the guns' scene: a shot goes through the blood to what is behind)
                    }
                    s.mv.push_back(*q);
                    s.vv.push_back(VrVert{q->x, q->y, q->z, 0xffffffffu, 0, 0, 0});
                }
            }
        }
        // The effects onto the surfaces behind them in the picture, then after the world (depth tested, not
        // written), in reverse order so the arcade's first wins among them.
        s.nv_world = uint32_t(s.mv.size());
        if (!fx.empty()) {
            depth_grid.clear();
            for (size_t t = 0; t + 2 < s.mv.size(); t += 3)
                depth_grid.add(s.mv[t], s.mv[t + 1], s.mv[t + 2], proj.fx, proj.fy, proj.cx, proj.cy);
            for (size_t t = 0; t + 2 < fx.size(); t += 3) {
                M2Vert *tri = &fx[t];
                const float X = (tri[0].x + tri[1].x + tri[2].x) / 3.f, Y = (tri[0].y + tri[1].y + tri[2].y) / 3.f,
                            Z = (tri[0].z + tri[1].z + tri[2].z) / 3.f;
                const float hit = depth_grid.z_at(proj.cx + proj.fx * X / Z, proj.cy + proj.fy * Y / Z);
                const float to = hit > 0.f ? hit * kEffectInFront : kEffectDistance; // the camera-space z to put it at
                for (int k = 0; k < 3; k++) { // along each vertex's own ray: the same place in the picture
                    const float m = to / tri[k].z;
                    tri[k].x *= m, tri[k].y *= m, tri[k].z = to;
                }
            }
        }
        for (size_t t = fx.size(); t >= 3; t -= 3) s.mv.insert(s.mv.end(), fx.begin() + ptrdiff_t(t - 3), fx.begin() + ptrdiff_t(t));
    } else {
        s.nv_world = uint32_t(s.mv.size());
    }

    s.batch = VrBatch{};
    s.batch.first = 0;
    s.batch.count = uint32_t(s.vv.size());
    s.batch.tex = -1;
    s.batch.zen = 1, s.batch.zwrite = 1, s.batch.zfunc = 4;
    s.sc = VrScene{};
    s.sc.v = s.vv.data(), s.sc.nv = uint32_t(s.vv.size()), s.sc.vcap = s.sc.nv;
    s.sc.b = &s.batch, s.sc.nb = s.vv.empty() ? 0 : 1, s.sc.bcap = 1;
    s.sc.fx = proj.fx, s.sc.fy = proj.fy, s.sc.cx = proj.cx, s.sc.cy = proj.cy;
    s.sc.w = rt::Video::W, s.sc.h = rt::Video::H;
    s.sc.serial = serial;
    comfort_from_sky(polys, s.sc);
    {   // A scene with 3D but none of the world (window 2: every stage, the title, the attract's scenes have it):
        // the game-over scene, a character spinning in the dark as the camera circles it. In VR that turns the
        // world round you; quest_xr.c shows it on a screen ahead instead (arcade_screen_scene), after kScreenHold
        // frames of it and back after as many without (no flicker at its ends).
        bool world = false;
        for (const rt::GeoPoly &p : polys) if (p.window == 2) { world = true; break; }
        const bool screen = !polys.empty() && !world;
        static int run = 0;
        run = screen == bool(arcade_screen_scene) ? 0 : run + 1;
        if (run >= kScreenHold) arcade_screen_scene = screen, run = 0;
    }

    if (have_mem) {
        s.luma.resize(M2_LUMA_ENTRIES);
        for (uint32_t i = 0; i < M2_LUMA_ENTRIES; i++) s.luma[i] = mem.lumaram[i * 4];
        s.xlat.resize(M2_XLAT_ENTRIES);
        for (uint32_t i = 0; i < M2_XLAT_ENTRIES; i++) s.xlat[i] = gamma_lut[le16(mem.colorxlat, i) & 0xff];
    }
    s.m = M2Scene{};
    s.m.v = s.mv.data(), s.m.nv = uint32_t(s.mv.size()), s.m.nv_world = s.nv_world;
    s.m.luma = s.luma.empty() ? nullptr : s.luma.data();
    s.m.xlat = s.xlat.empty() ? nullptr : s.xlat.data();
    s.m.serial = serial;
}

// ---- The cabinet room's courtyard (quest_xr.c, Room: COURTYARD): the mansion's courtyard, kept as a still 3D
// backdrop. It is made once on the headset from the player's own ROM set (it is the game's data, so the app can't
// carry it), on a thread of its own while the game is played: a second game, without sound, played to kCourtyardFrame (two coins and Start, player 1 kept alive, a
// shot every 15 frames, every 7th off the screen: scripts/inputs/hotd_courtyard.txt, the same frame on every
// machine), its world (everything round the camera, as
// VR mode keeps it), texture RAM, luma and colour tables and the sky's up saved in courtyard3.m2w:
//   "M2WORLD1", uint32 vertices, float up[3], M2Vert[vertices], texture RAM (M2_TEX_WORDS dwords), luma
//   (M2_LUMA_ENTRIES bytes), colour translation (M2_XLAT_ENTRIES bytes).
// Frame 4080: in front of the fountain, the mansion behind it, Sophie fallen by the fountain (the gargoyle takes her
// next).
constexpr uint64_t kCourtyardFrame = 4080;
std::string courtyard_path;
std::atomic<const char *> courtyard_ready{nullptr}; // (the VR thread reads it: set once the path is)
void make_courtyard(const std::string &images, const std::string &out) {
    rt::GameLoop cy(images, false);
    cy.board().video().set_external_3d(true, false);
    cy.board().set_vr(true);
    for (uint64_t f; (f = cy.board().frame()) < kCourtyardFrame;) {
        rt::Inputs in;
        in.gun_x = 0x180, in.gun_y = 0x0e9;                                            // the screen's middle
        if ((f >= 600 && f < 610) || (f >= 650 && f < 660)) in.in0 &= uint8_t(~0x01); // two coins
        if (f >= 760 && f < 770) in.in0 &= uint8_t(~0x10);                            // Start
        if (f >= 800) cy.board().write_byte(0x51f290, 3);                             // player 1's lives
        if (f + 2 >= 900) {
            const uint64_t k = (f + 2 - 900) / 15, s = 900 + 15 * k;                  // shot k at frame s
            if (f >= s && f <= s + 4) in.in1 &= uint8_t(~0x01);                      // the trigger
            if (k % 7 == 6 && f + 2 >= s && f <= s + 5) in.gun_x = 0x0ad, in.gun_y = 0x057; // off the screen
        }
        cy.run_frame(in);
    }
    const rt::Video &video = cy.board().video();
    const std::vector<rt::GeoPoly> &polys = video.gpu_polys();
    const rt::VideoMem &mem = video.gpu_mem();
    const int windows = video.gpu_windows();
    if (polys.empty() || !mem.palram || !mem.colorxlat || !mem.lumaram || !mem.tex0 || !mem.tex1)
        throw std::runtime_error("no 3D at the courtyard's frame");
    Projection pj;
    choose_projection(polys, windows, video.crtc_x(), video.crtc_y(), pj);
    const int c0m = int(pj.cx) - video.crtc_x(), c1m = 384 + video.crtc_y() - int(pj.cy);
    std::vector<size_t> order(polys.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (polys[a].window != polys[b].window) return polys[a].window > polys[b].window;
        if (polys[a].z != polys[b].z) return polys[a].z < polys[b].z;
        return a > b;
    });
    std::vector<M2Vert> world;
    uint32_t rank = 0;
    for (size_t i : order) {
        const rt::GeoPoly &poly = polys[i];
        ++rank;
        if (poly.window > windows || poly.num_vertices < 3) continue;
        bool effect = false;
        M2Vert pv[8];
        if (!poly_vertices(poly, mem, rank, c0m, c1m, pj.fx, -pj.fy, pv, effect) || effect) continue;
        for (int k = 1; k + 1 < poly.num_vertices; k++)
            for (const M2Vert *q : {&pv[0], &pv[k], &pv[k + 1]}) world.push_back(*q);
    }
    float up[3] = {0, 1, 0}, ref[3];
    if (!sky_frame(polys, up, ref)) up[0] = 0, up[1] = 1, up[2] = 0;
    const std::string tmp = out + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot write " + tmp);
    const uint32_t n = uint32_t(world.size());
    bool ok = std::fwrite("M2WORLD1", 1, 8, f) == 8 && std::fwrite(&n, 4, 1, f) == 1 && std::fwrite(up, 4, 3, f) == 3 &&
              std::fwrite(world.data(), sizeof(M2Vert), n, f) == n &&
              std::fwrite(mem.tex0, 4, M2_TEX_WORDS / 2, f) == M2_TEX_WORDS / 2 &&
              std::fwrite(mem.tex1, 4, M2_TEX_WORDS / 2, f) == M2_TEX_WORDS / 2;
    std::vector<uint8_t> t(M2_LUMA_ENTRIES);
    for (uint32_t i = 0; i < M2_LUMA_ENTRIES; i++) t[i] = mem.lumaram[i * 4];
    ok = ok && std::fwrite(t.data(), 1, t.size(), f) == t.size();
    t.resize(M2_XLAT_ENTRIES);
    for (uint32_t i = 0; i < M2_XLAT_ENTRIES; i++) t[i] = gamma_lut[le16(mem.colorxlat, i) & 0xff];
    ok = ok && std::fwrite(t.data(), 1, t.size(), f) == t.size();
    ok = std::fclose(f) == 0 && ok;
    if (!ok || std::rename(tmp.c_str(), out.c_str())) throw std::runtime_error("cannot write " + out);
    LOGI("courtyard: %u vertices, up (%.3f %.3f %.3f)", n, up[0], up[1], up[2]);
}

// texture RAM rows that changed (the board's per-page generations) into the shared copy (g.mu held)
std::vector<uint64_t> seen_pages(M2_TEX_ROWS, ~uint64_t(0));
void copy_texture(const rt::VideoMem &mem) {
    if (!mem.tex0 || !mem.tex1 || !mem.tex_page_generation) return;
    for (uint32_t row = 0; row < M2_TEX_ROWS; row++) {
        const uint64_t gen = mem.tex_page_generation[row];
        if (gen == seen_pages[row]) continue;
        seen_pages[row] = gen;
        const uint32_t *src = row < 512 ? mem.tex0 + row * 1024 : mem.tex1 + (row - 512) * 1024;
        std::memcpy(g.tex.data() + size_t(row) * 1024, src, 4096);
        g.dirty[row] = 1;
    }
}

void publish(const rt::Video &video) {
    std::lock_guard<std::mutex> lock(g.mu);
    int slot = 0;
    while (slot == g.latest || slot == g.reading) slot++;
    build(g.slots[slot], video, ++g.serial);
    copy_texture(video.gpu_mem());
    g.latest = slot;
}

// ---- input from the VR thread (host_sdl.c's events)
struct Controls {
    int mx = -1, my = -1;
    bool fire = false, reload = false;
    bool skip = false; // the skip button went down (coins and Start: arcade_net)
} ctl;

void poll_events(bool &quit) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: quit = true; break;
        case SDL_MOUSEMOTION: ctl.mx = e.motion.x, ctl.my = e.motion.y; break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            const bool down = e.type == SDL_MOUSEBUTTONDOWN;
            if (e.button.button == SDL_BUTTON_LEFT) ctl.fire = down;
            else if (e.button.button == SDL_BUTTON_RIGHT) ctl.reload = down;
            if (down) ctl.mx = e.button.x, ctl.my = e.button.y;
            break;
        }
        case SDL_KEYDOWN:
            if (e.key.keysym.scancode == SDL_SCANCODE_SPACE && !e.key.repeat) ctl.skip = true;
            break;
        default: break;
        }
    }
}

// this player's controls for arcade_net::frame (it makes the cabinet's input, alone or online)
arcade_net::Local local_controls() {
    arcade_net::Local l;
    l.fire = ctl.fire;
    l.reload = ctl.reload;
    l.x = ctl.mx, l.y = ctl.my;
    l.on_screen = ctl.mx >= 0 && ctl.mx < rt::Video::W && ctl.my >= 0 && ctl.my < rt::Video::H;
    l.skip = ctl.skip;
    ctl.skip = false;
    return l;
}

// ---- sound: the board's 44.1 kHz stereo floats -> a ring the audio callback drains
struct Ring {
    std::mutex mu;
    std::vector<int16_t> buf = std::vector<int16_t>(44100 * 2); // one second, stereo
    size_t rd = 0, wr = 0, n = 0;
} ring;
std::atomic<float> volume{1.0f};
float sound_peak = 0; // the loudest sample the board made since the last "frame ms" line (game thread)

void SDLCALL audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    int16_t *out = reinterpret_cast<int16_t *>(stream);
    const size_t want = size_t(len) / 2;
    std::lock_guard<std::mutex> lock(ring.mu);
    size_t i = 0;
    for (; i < want && ring.n; i++, ring.n--) {
        out[i] = ring.buf[ring.rd];
        ring.rd = (ring.rd + 1) % ring.buf.size();
    }
    for (; i < want; i++) out[i] = 0;
}

void push_audio(snd::Board &sb) {
    std::vector<float> pcm = sb.take_pcm();
    for (float f : pcm) sound_peak = std::max(sound_peak, std::fabs(f));
    const float gain = volume.load() * (float(quest_volume) / 100.0f) * rt::game().output_gain;
    std::lock_guard<std::mutex> lock(ring.mu);
    for (float f : pcm) {
        const float s = std::clamp(f * gain, -1.0f, 1.0f);
        if (ring.n == ring.buf.size()) { ring.rd = (ring.rd + 1) % ring.buf.size(); ring.n--; } // overrun: drop the oldest
        ring.buf[ring.wr] = int16_t(std::lround(s * 32767.0f));
        ring.wr = (ring.wr + 1) % ring.buf.size();
        ring.n++;
    }
    // keep about 80 ms queued: drop a backlog (the game ran ahead after a pause)
    const size_t keep = size_t(snd::Scsp::kRate * 0.08) * 2;
    if (ring.n > keep * 3) {
        const size_t drop = (ring.n - keep) & ~size_t(1);
        ring.rd = (ring.rd + drop) % ring.buf.size();
        ring.n -= drop;
    }
}

// ---- 2D: the tilemap layers into the overlay (RGB565, D3D_VR_KEY where nothing is drawn)
inline uint16_t to565(uint32_t c) {
    const uint16_t p = uint16_t(((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f));
    return p == D3D_VR_KEY ? uint16_t(D3D_VR_KEY ^ 1) : p;
}
// The front layer's damage marks (the blood slash when a zombie hits you: pure red, above the HUD's strip) go on
// your face instead (the face layer, m2_gl_draw_face); the rest is the overlay. False: a frame not to show, the
// light gun's flash (the cabinet fills the screen white for one frame when you pull the trigger, so the gun can
// see its aim): the overlay keeps the frame before.
constexpr int kHudTop = 336; // the HUD's strip at the bottom (bullets, lives, credits), its red "1P" among them
bool draw_overlay(const rt::Video &video, std::vector<uint16_t> &px, std::vector<uint32_t> &face, bool &face_any) {
    const std::vector<uint32_t> &back = video.background_layer(), &front = video.foreground_layer();
    const bool scene = !video.gpu_polys().empty();
    const size_t n = size_t(rt::Video::W) * rt::Video::H;
    if (front.size() >= n && (front[0] >> 24)) { // the flash: one bright colour all over
        const uint32_t c = front[0];
        const int lum = int(((c >> 16) & 0xff) + ((c >> 8) & 0xff) + (c & 0xff));
        size_t same = 0;
        for (size_t i = 0; i < n; i += 7) same += front[i] == c;
        if (lum > 3 * 0x60 && same * 7 > n * 9 / 10) return false;
    }
    // The slash: pure red in 4 x 4 blocks, joined into shapes; a shape taller than kSlashTall is a slash. A mark is
    // more than its tallest piece (a bite's two halves, a slash its own dark band cuts in two): the shapes within
    // kNear blocks of a slash's box go with it, and every slash grows a block all round (its dark edges), so a mark
    // is never split between your face and the picture (two depths in stereo). Red shapes wholly in the HUD's strip
    // (its "1P") and the rest (the RELOAD box's red letters among them) stay on the overlay; a mark may reach into
    // the strip.
    constexpr int kB = 4, BW = rt::Video::W / kB, BH = rt::Video::H / kB, kHudB = kHudTop / kB, kSlashTall = 160 / kB,
                  kNear = 6;
    static uint8_t red[BW * BH], slash[BW * BH];
    static int stack[BW * BH];
    std::fill(std::begin(red), std::end(red), uint8_t(0));
    std::fill(std::begin(slash), std::end(slash), uint8_t(0));
    // (only over a 3D scene: a hit comes in play; the 2D screens' red, the start's warning and the title among
    // them, stays in the picture)
    if (!arcade_view_cabinet && scene && front.size() >= n)
        for (int y = 0; y < BH * kB; y++)
            for (int x = 0; x < rt::Video::W; x++) {
                const uint32_t f = front[size_t(y) * rt::Video::W + size_t(x)];
                if ((f >> 24) && (f & 0xffff) == 0 && (f & 0xff0000)) red[(y / kB) * BW + x / kB] = 1;
            }
    struct Shape { int x0, y0, x1, y1; std::vector<int> blocks; };
    std::vector<Shape> shapes;
    for (int b0 = 0; b0 < BW * BH; b0++) {
        if (red[b0] != 1) continue;
        Shape sh{BW, BH, -1, -1, {}};
        int sp = 0;
        stack[sp++] = b0;
        red[b0] = 2;
        while (sp) {
            const int b = stack[--sp];
            sh.blocks.push_back(b);
            const int bx = b % BW, by = b / BW;
            sh.x0 = std::min(sh.x0, bx), sh.x1 = std::max(sh.x1, bx), sh.y0 = std::min(sh.y0, by), sh.y1 = std::max(sh.y1, by);
            for (int d = 0; d < 4; d++) {
                const int nx = bx + (d == 0) - (d == 1), ny = by + (d == 2) - (d == 3);
                if (nx < 0 || nx >= BW || ny < 0 || ny >= BH) continue;
                const int nb = ny * BW + nx;
                if (red[nb] == 1) red[nb] = 2, stack[sp++] = nb;
            }
        }
        if (sh.y0 < kHudB) shapes.push_back(std::move(sh));   // (wholly in the HUD's strip: never a mark)
    }
    std::vector<char> in(shapes.size(), 0);
    for (size_t k = 0; k < shapes.size(); k++) in[k] = shapes[k].y1 - shapes[k].y0 + 1 >= kSlashTall;
    for (size_t k = 0; k < shapes.size(); k++) {      // the pieces near a slash
        if (in[k] != 1) continue;                     // (a slash's own box only: no chains of pieces)
        const Shape &a = shapes[k];
        for (size_t j = 0; j < shapes.size(); j++) {
            const Shape &b = shapes[j];
            if (!in[j] && b.x1 >= a.x0 - kNear && b.x0 <= a.x1 + kNear && b.y1 >= a.y0 - kNear && b.y0 <= a.y1 + kNear) in[j] = 2;
        }
    }
    for (size_t k = 0; k < shapes.size(); k++)
        if (in[k])
            for (int b : shapes[k].blocks) {          // each block and the eight round it
                const int bx = b % BW, by = b / BW;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        const int x = bx + dx, y = by + dy;
                        if (x >= 0 && x < BW && y >= 0 && y < BH) slash[y * BW + x] = 1;
                    }
            }
    face_any = false;
    for (size_t i = 0; i < n; i++) {
        const uint32_t f = i < front.size() ? front[i] : 0;
        const int y = int(i / rt::Video::W), x = int(i % rt::Video::W);
        face[i] = 0;
        if ((f >> 24) && slash[(y / kB) * BW + x / kB]) {
            // RGBA bytes, a little see-through
            face[i] = 0xd0000000u | ((f & 0xff) << 16) | (f & 0xff00) | ((f >> 16) & 0xff);
            face_any = true;
            px[i] = D3D_VR_KEY;
        } else if (f >> 24) px[i] = to565(f);
        else if (!scene && i < back.size()) px[i] = to565(back[i]); // a 2D screen: all of it
        else px[i] = D3D_VR_KEY;
    }
    return true;
}

// A gun's crosshair on the overlay (the arcade draws none): online the partner's, so you see where they aim, and
// in the cabinet view your own (port_draw_crosshairs, the app's: the cabinet or online). Player 1 red, 2 blue.
void draw_crosshair(std::vector<uint16_t> &px, uint16_t gx, uint16_t gy, uint16_t colour) {
    const rt::GunRange &gun = rt::game().gun;
    if (gx <= gun.x_min || gx >= gun.x_max || gy <= gun.y_min || gy >= gun.y_max) return; // off the screen
    const int x = int(float(gx - gun.x_min) * float(rt::Video::W) / float(gun.x_max - gun.x_min));
    const int y = int(float(gy - gun.y_min) * float(rt::Video::H) / float(gun.y_max - gun.y_min));
    auto put = [&](int u, int v) {
        if (u >= 0 && u < rt::Video::W && v >= 0 && v < rt::Video::H) px[size_t(v) * rt::Video::W + size_t(u)] = colour;
    };
    for (int d = 3; d <= 9; d++) // four ticks around the aim, two pixels thick
        for (int t = 0; t < 2; t++) put(x + d, y + t), put(x - d, y + t), put(x + t, y + d), put(x + t, y - d);
}
// "SHOOT OUTSIDE OF THE SCREEN !" (orange letters outlined in blue, under the RELOAD sign, when the trigger is pulled
// on an empty gun) says the reload gesture's step instead when one is on (quest_xr.c's arcade_reload_step), in the
// same letters. It is found by its blue outline at its place with little of that blue round it.
extern "C" volatile int arcade_reload_step;
extern "C" void quest_text565(uint16_t *px, int w, int h, int x, int y, int scale, uint16_t col, const char *s);
void draw_shoot_outside(std::vector<uint16_t> &px, const rt::Video &video) {
    const int step = arcade_reload_step;
    if (step <= 0) return;
    const bool online = arcade_net::mode() != arcade_net::Off;
    const int me = online ? arcade_net::local_slot() : 0;
    const std::vector<uint32_t> &front = video.foreground_layer();
    const int W = rt::Video::W, H = rt::Video::H;
    if (front.size() < size_t(W) * H) return;
    // player 1's message: x 34..189, y 217..254 (measured, its outline); player 2's on the right, mirrored
    const int x0 = me ? W - 1 - 192 : 31, x1 = me ? W - 1 - 31 : 192, y0 = 214, y1 = 257;
    auto outline = [&](int x, int y) {
        const uint32_t f = front[size_t(y) * W + size_t(x)];
        return (f >> 24) && (f & 0xffff00) == 0 && (f & 0xff) >= 0x80;
    };
    int in = 0, around = 0;
    for (int y = y0 - 10; y <= y1 + 10; y++)
        for (int x = x0 - 10; x <= x1 + 10; x++) {
            if (x < 0 || x >= W || y < 0 || y >= H) continue;
            const bool inside = x >= x0 && x <= x1 && y >= y0 && y <= y1;
            if (outline(x, y)) (inside ? in : around)++;
        }
    if (in < 300 || around * 8 > in) return;   // not showing, or not it
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) px[size_t(y) * W + size_t(x)] = D3D_VR_KEY;   // (the game shows through)
    static const char *const text[][2] = { { "", "" }, { "DROP THE MAG", "PRESS B" }, { "DROP THE MAG", "PRESS Y" },
                                           { "INSERT A", "MAGAZINE" }, { "RACK THE", "SLIDE" }, { "AIM AT THE", "FLOOR" } };
    const int k = step < 6 ? step : 0;
    const int cx = (x0 + x1) / 2, ty = (y0 + y1) / 2 - 15;
    for (int line = 0; line < 2; line++) {
        const char *t = text[k][line];
        const int n = int(std::strlen(t)), x = cx - n * 6 + 1, y = ty + line * 17;
        for (int dy = -1; dy <= 1; dy++)          // the blue outline, then the orange letters
            for (int dx = -1; dx <= 1; dx++)
                if (dx || dy) quest_text565(px.data(), W, H, x + dx, y + dy, 2, 0x001f, t);
        quest_text565(px.data(), W, H, x, y, 2, 0xfd05, t);
    }
}
void draw_crosshairs(std::vector<uint16_t> &px, const rt::Inputs &in) {
    const bool online = arcade_net::mode() != arcade_net::Off;
    if (!port_draw_crosshairs) return;
    const int me = online ? arcade_net::local_slot() : 0;
    for (int p = 0; p < 2; p++) {
        if (p == me && online && port_draw_crosshairs < 2) continue;   // online, the cabinet's: only the partner's
        if (p != me && !online) continue;                               // alone: your own
        const uint16_t colour = p ? 0x237f : 0xf8a3;
        if (p == me && online) {   // your own, online: where you aim now (the inputs come back a round trip late)
            if (ctl.mx < 0 || ctl.my < 0 || ctl.mx >= rt::Video::W || ctl.my >= rt::Video::H) continue;
            const rt::GunRange &gun = rt::game().gun;
            draw_crosshair(px, uint16_t(gun.x_min + 1 + ctl.mx * (gun.x_max - gun.x_min - 2) / rt::Video::W),
                           uint16_t(gun.y_min + 1 + ctl.my * (gun.y_max - gun.y_min - 2) / rt::Video::H), colour);
            continue;
        }
        if (p == 1 && !in.gun2_x) continue;       // no player 2's gun
        draw_crosshair(px, p ? in.gun2_x : in.gun_x, p ? in.gun2_y : in.gun_y, colour);
    }
}

// The sound board on a thread of its own (GameLoop's sound packets): frame N's sound runs while the board runs
// frame N+1. The overlap would only change the game if the sound board sent bytes back to the i960 (they reach
// the board at a frame's start); The House of the Dead's sends none (0 bytes over a whole game, measured with
// m2gpushot --bench), and online play's checksums would show it if it ever did.
class SoundThread {
public:
    SoundThread() : t_([this] { loop(); }) {}
    ~SoundThread() {
        {
            std::lock_guard<std::mutex> l(mu_);
            quit_ = true;
        }
        cv_.notify_all();
        t_.join();
    }
    void run(rt::GameLoop::SoundPacket &&p) {
        {
            std::lock_guard<std::mutex> l(mu_);
            packet_ = std::move(p);
            busy_ = true;
        }
        cv_.notify_all();
    }
    void wait() {
        std::unique_lock<std::mutex> l(mu_);
        done_.wait(l, [this] { return !busy_; });
    }
    uint64_t take_ns() { const uint64_t n = busy_ns_; busy_ns_ = 0; return n; } // (after wait(): the thread is idle)

private:
    void loop() {
        std::unique_lock<std::mutex> l(mu_);
        for (;;) {
            cv_.wait(l, [this] { return busy_ || quit_; });
            if (!busy_) return;
            rt::GameLoop::SoundPacket p = std::move(packet_);
            l.unlock();
            try {
                const uint64_t t0 = now_ns();
                p.execute();
                busy_ns_ += now_ns() - t0;
            } catch (const std::exception &e) {
                LOGE("sound board: %s", e.what());
            }
            l.lock();
            busy_ = false;
            done_.notify_all();
        }
    }
    std::mutex mu_;
    std::condition_variable cv_, done_;
    rt::GameLoop::SoundPacket packet_;
    bool busy_ = false, quit_ = false;
    uint64_t busy_ns_ = 0;
    std::thread t_; // (last: started once the rest is ready)
};

rt::GameLoop *the_game = nullptr;
uint32_t game_crc() { return the_game->board().state_hash(); } // online play's checksum (arcade_net)

// The players' bullets and lives in work RAM (found with tools/m2probe: RAM at every shot of the scripted games):
// lives at 0x51f290 (player 1) and 0x51f294 (player 2), bullets at 0x51f2c1 and 0x51f38d (each player's block
// 0xcc bytes). A player with no lives left (game over, the continue countdown, the attract) isn't in the game.
void update_ammo(rt::GameLoop &game) {
    rt::M2Board &b = game.board();
    for (int p = 0; p < 2; p++) {
        const uint8_t lives = b.read_byte(p ? 0x0051f294 : 0x0051f290);
        const uint8_t ammo = b.read_byte(p ? 0x0051f38d : 0x0051f2c1);
        g_ammo[p] = lives > 0 && ammo <= 6 ? int(ammo) : -1;
        // In play: a game (0x51f4a0: 0 the title, 1 a game, 2 the attract demo) out of its cut scenes (the phase,
        // 0x51f4b4 / 0x51f4b5 for players 1 / 2: 4 a cut scene, 1..3 play), the player alive. Found by which
        // trigger pulls the game takes (m2probe RAM dumps, docs/vr.md).
        const uint8_t mode = b.read_byte(0x0051f4a0), phase = b.read_byte(p ? 0x0051f4b5 : 0x0051f4b4);
        g_playing[p] = mode == 1 && phase >= 1 && phase <= 3 && lives > 0;
    }
}

// The ROM set's images as the importer lays them out (GameLoop's images folder), kept beside it after the first
// start: unpacking a 7z takes the Quest a while; the images load at once. `stamp` names the set they came from
// (its size and time), so another set makes them again.
const char *const kImageFiles[] = {"program", "main_data", "copro_tables", "copro_data", "polygons", "textures",
                                   "sound_program", "pcm1", "pcm2", "samples"};
std::string set_stamp(const std::string &set) {
    uint64_t size;
    int64_t mtime;
    if (plat_file_info(set.c_str(), &size, &mtime)) return "";
    return set + " " + std::to_string(size) + " " + std::to_string(uint64_t(mtime));
}
bool cache_matches(const std::string &dir, const std::string &stamp) {
    FILE *f = std::fopen((dir + "/stamp.txt").c_str(), "r");
    if (!f) return false;
    char b[1024] = {};
    const size_t n = std::fread(b, 1, sizeof b - 1, f);
    std::fclose(f);
    return std::string(b, n) == stamp;
}
void write_cache(const std::string &dir, const rt::M2Board::Images &img, const std::string &stamp) {
    plat_mkdir(dir.c_str());
    std::remove((dir + "/stamp.txt").c_str());
    const std::vector<uint8_t> *parts[] = {&img.program, &img.main_data, &img.copro_tables, &img.copro_data, &img.polygons,
                                           &img.textures, &img.sound_program, &img.pcm1, &img.pcm2, &img.samples};
    for (size_t i = 0; i < std::size(parts); i++) {
        FILE *f = std::fopen((dir + "/" + kImageFiles[i] + ".bin").c_str(), "wb");
        if (!f) return;
        const bool ok = parts[i]->empty() || std::fwrite(parts[i]->data(), 1, parts[i]->size(), f) == parts[i]->size();
        std::fclose(f);
        if (!ok) return;
    }
    if (!img.eeprom.empty())
        if (FILE *f = std::fopen((dir + "/eeprom.bin").c_str(), "wb")) std::fwrite(img.eeprom.data(), 1, img.eeprom.size(), f), std::fclose(f);
    if (FILE *f = std::fopen((dir + "/stamp.txt").c_str(), "w")) std::fwrite(stamp.data(), 1, stamp.size(), f), std::fclose(f);
}

// A line of text on the overlay while the game isn't drawing yet (loading)
void show_status(SDL_Renderer *ren, const char *line1, const char *line2) {
    SDL_SetRenderDrawColor(ren, 8, 8, 12, 255);
    SDL_RenderClear(ren);
    quest_render_text(40, 160, 2, 0xff60b0ffu, line1);
    if (line2) quest_render_text(40, 190, 1, 0xffd0d0d0u, line2);
    SDL_RenderPresent(ren);
}

bool exists(const std::string &p) { return plat_exists(p.c_str()); }

uint64_t now_ns() { return plat_now_ns(); }

} // namespace

uint64_t tgpx4_ns_total() { return rt::tgpx4_ns; }

// The courtyard's file once it exists (m2_gl.c loads it for the cabinet room), else NULL
extern "C" const char *arcade_courtyard_file(void) { return courtyard_ready.load(std::memory_order_acquire); }

extern "C" const VrScene *d3d_vr_scene(void) {
    std::lock_guard<std::mutex> lock(g.mu);
    if (g.latest < 0) return nullptr;
    g.reading = g.latest;
    return &g.slots[g.reading].sc;
}

extern "C" const M2Scene *m2_vr_scene(void) {
    std::lock_guard<std::mutex> lock(g.mu);
    if (g.reading < 0) return nullptr;
    Slot &s = g.slots[g.reading];
    // the texture rows changed since the last call (the caller uploads them before the next)
    std::swap(g.dirty, g.taken);
    std::fill(g.dirty.begin(), g.dirty.end(), uint8_t(0));
    s.m.tex = g.tex.data();
    s.m.tex_row_dirty = g.taken.data();
    s.m.tex_all = g.tex_all;
    g.tex_all = false;
    s.m.face = g.face.data();
    s.m.face_any = g.face_any;
    s.m.face_serial = g.face_serial;
    return &s.m;
}

// argv: hotdvr GAME_DIR SAVE_DIR (quest_xr.c game_main)
extern "C" int SDL_main(int argc, char **argv) {
    const std::string game_dir = argc > 1 ? argv[1] : ".", save_dir = argc > 2 ? argv[2] : ".";
    update_gamma();
    std::unique_ptr<rt::GameLoop> game;
    // a message in place of the game until the app is closed (returning would end the app at once)
    auto stay_on = [](const std::string &msg) {
        quest_show_message(msg.c_str());
        for (bool quit = false; !quit;) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) quit |= e.type == SDL_QUIT;
            SDL_Delay(50);
        }
        return 0;
    };
    // the overlay first: what is happening while the ROM set loads (a 7z takes the Quest a while the first time)
    SDL_Window *win = SDL_CreateWindow("hotd", 0, 0, rt::Video::W, rt::Video::H, 0);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, 0);
    SDL_RenderSetLogicalSize(ren, rt::Video::W, rt::Video::H);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, rt::Video::W, rt::Video::H);
    std::vector<uint16_t> px(size_t(rt::Video::W) * rt::Video::H);
    std::vector<uint32_t> face_px(size_t(rt::Video::W) * rt::Video::H);
    bool made_images = false; // (this start kept a new set's images)
    try {
        // the ROM set (MAME's hotd, Revision A): a zip or a 7z, the ROMs found by name or CRC; its images kept in
        // game_dir/images after the first start
        std::string set;
        for (const char *n : {"/hotd.zip", "/hotd.7z"})
            if (set.empty() && exists(game_dir + n)) set = game_dir + n;
        const std::string cache = game_dir + "/images", stamp = set.empty() ? "" : set_stamp(set);
        if (!set.empty() && cache_matches(cache, stamp)) {
            show_status(ren, "LOADING THE HOUSE OF THE DEAD", nullptr);
            LOGI("loading the images in %s", cache.c_str());
            game = std::make_unique<rt::GameLoop>(cache);
        } else if (!set.empty()) {
            show_status(ren, "UNPACKING YOUR ROM SET", "THE FIRST START TAKES A MINUTE. THE NEXT ONES ARE QUICK.");
            LOGI("loading %s", set.c_str());
            rt::M2Board::Images img = rt::import_rom_set(set);
            show_status(ren, "LOADING THE HOUSE OF THE DEAD", nullptr);
            write_cache(cache, img, stamp);
            made_images = true;
            game = std::make_unique<rt::GameLoop>(std::move(img));
        } else {
            LOGI("loading the images in %s", game_dir.c_str());
            game = std::make_unique<rt::GameLoop>(game_dir);
        }
    } catch (const std::exception &e) {
        LOGE("cannot start: %s", e.what());
        return stay_on(std::string("The arcade game did not start:\n\n") + e.what() +
                       "\n\nIt needs MAME's hotd set, Revision A (the program ROMs epr-19696a.15 and "
                       "epr-19697a.16), as hotd.zip or hotd.7z in\n" + game_dir + "\n\nPress the menu button to quit.");
    }
    {   // the cabinet room's courtyard, once (the images just kept: a new set's images make it again)
        // (courtyard3: the fountain, with the whole scenery sent (hook_hotd_scenery_cull); the earlier ones, the road
        // outside the gate, are dropped)
        const std::string cy = game_dir + "/courtyard3.m2w", images = game_dir + "/images";
        std::remove((game_dir + "/courtyard2.m2w").c_str());
        std::remove((game_dir + "/courtyard.m2w").c_str());
        if (made_images) std::remove(cy.c_str());
        courtyard_path = cy;
        if (exists(cy)) {
            courtyard_ready.store(courtyard_path.c_str(), std::memory_order_release);
        } else if (exists(images + "/stamp.txt")) {
            // made on a spare core while you play (about 15 s on the Quest 3; the game being played is not touched:
            // two games side by side give the same frames, m2gpushot M2_TWIN=1). The cabinet room is dark till then;
            // the VR thread loads the backdrop once courtyard_ready is set (m2_gl.c load_backdrop).
            std::thread([images] {
                plat_thread_background();   // (below the game's thread and the VR's)
                const auto t0 = std::chrono::steady_clock::now();
                try {
                    make_courtyard(images, courtyard_path);
                    courtyard_ready.store(courtyard_path.c_str(), std::memory_order_release);
                    LOGI("courtyard: made in %.1f s", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
                } catch (const std::exception &e) {
                    LOGE("courtyard: %s", e.what());
                }
            }).detach();
        }
    }
    if (exists(save_dir + "/ioboard_eeprom.bin")) tools::load_nvram(*game, save_dir);
    game->board().video().set_external_3d(true, false);
    game->board().video().set_cpu_layers(true); // the 2D layers here, for the overlay (draw_overlay)
    if (getenv("M2_VR") == nullptr || std::atoi(getenv("M2_VR")) != 0) game->board().set_vr(true);

    SDL_AudioSpec want{}, have{};
    want.freq = int(snd::Scsp::kRate), want.format = AUDIO_S16SYS, want.channels = 2, want.samples = 256, want.callback = audio_cb;
    const SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (dev) SDL_PauseAudioDevice(dev, 0);
    else LOGE("no audio: %s", SDL_GetError());

    const uint64_t frame_ns = uint64_t(1e9 / rt::GameLoop::kFrameHz + 0.5);
    uint64_t next = now_ns();
    bool quit = false;
    std::vector<uint8_t> saved_eeprom(game->board().io().eeprom.begin(), game->board().io().eeprom.end()), saved_backup(game->board().backup_ram().begin(), game->board().backup_ram().end());
    the_game = game.get();
    // The game thread's time per frame by part, logged every 600 frames (hotdarcade: "frame ms"): the measure of
    // the headroom on the headset.
    game->set_profile_clock(now_ns);
    struct { uint64_t frames = 0, board = 0, core = 0, tgpx4 = 0, geo = 0, video = 0, scene = 0, sound = 0, tgpx4_at = 0; } prof;
    SoundThread sound_thread; // (after the game: it goes first, before the sound board its packets use)
    while (!quit) {
        poll_events(quit);
        // this frame's cabinet input: made here (alone, or hosting: and sent), or the host's (a guest waits for it)
        rt::Inputs in;
        if (!arcade_net::frame(local_controls(), in, game_crc)) continue;
        rt::GameLoop::SoundPacket packet; // this frame's sound board work: on the sound thread, during the next frame
        const uint64_t t_board = now_ns();
        try {
            packet = game->run_frame_sound_packet(in);
        } catch (const std::exception &e) {
            LOGE("the game stopped: %s", e.what());
            return stay_on(std::string("The arcade game stopped:\n\n") + e.what() + "\n\nPress the menu button to quit.");
        }
        sound_thread.wait(); // the last frame's sound is done: the sound board is this thread's until run()
        {
            const rt::FrameProfile &fp = game->last_profile();
            prof.board += now_ns() - t_board, prof.core += fp.core(), prof.geo += fp.geometry, prof.video += fp.video;
            prof.sound += sound_thread.take_ns();
        }
        update_ammo(*game);
        const uint64_t t_scene = now_ns();
        const int behind = arcade_net::catching_up(); // a guest with frames to catch up on: 2 many (don't draw)
        const rt::Video &video = game->board().video();
        update_gamma();   // (Brightness changed in the menu)
        if (behind < 2) {
            publish(video);
            bool face_any = false;
            if (draw_overlay(video, px, face_px, face_any)) {
                draw_crosshairs(px, in);
                draw_shoot_outside(px, video);
                SDL_UpdateTexture(tex, nullptr, px.data(), rt::Video::W * 2);
                SDL_RenderCopy(ren, tex, nullptr, nullptr);
                publish_face(face_px, face_any);
            }
        }
        if (game->sound()) {
            if (behind) game->sound()->take_pcm(); // (catching up: no sound of frames gone by)
            else push_audio(*game->sound());
        }
        sound_thread.run(std::move(packet));
        prof.scene += now_ns() - t_scene;
        if (++prof.frames == 600) {
            const uint64_t tg = tgpx4_ns_total();
            const double k = 1e-6 / double(prof.frames);
            LOGI("frame ms: board %.2f (i960 and TGP %.2f, TGPx4 %.2f, geometrizer %.2f, 2D layers %.2f), scene and "
                 "overlay %.2f; sound thread %.2f (peak %.3f)",
                 double(prof.board) * k, double(prof.core) * k, double(tg - prof.tgpx4_at) * k, double(prof.geo) * k,
                 double(prof.video) * k, double(prof.scene) * k, double(prof.sound) * k, double(sound_peak));
            sound_peak = 0;
            prof = {};
            prof.tgpx4_at = tg;
        }
        if (behind < 2) SDL_RenderPresent(ren); // waits while paused (the system menu, the headset off)

        // settings and scores: written when they change (checked every ~5 s)
        if (game->frames() % 300 == 0) {
            const auto &e = game->board().io().eeprom;
            const auto &b = game->board().backup_ram();
            if (!std::equal(e.begin(), e.end(), saved_eeprom.begin(), saved_eeprom.end()) ||
                !std::equal(b.begin(), b.end(), saved_backup.begin(), saved_backup.end())) {
                tools::save_nvram(*game, save_dir);
                saved_eeprom.assign(e.begin(), e.end());
                saved_backup.assign(b.begin(), b.end());
            }
        }

        // the board's frame rate; after a stall (paused), start again from now. A guest goes at the host's pace
        // (its bundles), and as fast as it can while it catches up.
        if (arcade_net::mode() == arcade_net::Guest) continue;
        next += frame_ns;
        const uint64_t t = now_ns();
        if (t > next + 4 * frame_ns) next = t;
        else if (t < next) plat_sleep_until_ns(next);
    }
    if (dev) SDL_CloseAudioDevice(dev);
    return 0;
}

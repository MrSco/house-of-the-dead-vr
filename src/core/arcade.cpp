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
#include "../../tools/common/input_script.h"
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
int port_crosshair_big = 0;   // a TV (src/flat/tvos): the crosshairs larger and outlined in black
volatile int arcade_view_cabinet = 0; // quest_xr.c: the cabinet view (its picture is the arcade's: no face layer)
volatile int arcade_screen_scene = 0; // a scene to show on a screen, not round you (build: the game-over scene)
int arcade_flat = 0;                  // the flat apps (flat_app.c): no cabinet room, so its courtyard isn't made
float port_music_gain = 1.0f, port_sfx_gain = 1.0f;
// Player p's bullets (0-6), -1 while that player isn't in the game: what the app's guns go by (the empty gun's
// locked slide and silent laser, the magazine, the reload hints). The game thread keeps these (update_ammo).
static std::atomic<int> g_ammo[2] = {-1, -1};
int host_player_ammo(int p) { return p >= 0 && p < 2 ? g_ammo[p].load() : -1; }
// The reload gesture's step (quest_xr.c, each frame: 0 none, 1 / 2 drop the magazine, 3 put one in, 4 rack the
// slide, 5 aim at the floor): the RELOAD sign's words and the line said for it (arcade_reload_clip: 0 drop the mag,
// 1 load one, 2 rack the slide, 44.1 kHz mono)
extern volatile int arcade_reload_step;
extern int net_local; // (arcade_net.cpp: this player's slot)
void arcade_reload_clip(int step, const int16_t *pcm, int n);
// Where in the picture (rt::Video's pixels, the last inclusive) the game takes a shot as on the screen: short of its
// edges, where it reloads (rt::GunRange live_*). 0: the whole picture.
int host_gun_live_rect(int *x0, int *y0, int *x1, int *y1) {
    const rt::GunRange &g = rt::game().gun;
    if (!g.present() || !g.live_x_max) return 0;
    const float W = float(rt::Video::W), H = float(rt::Video::H);
    const float sx = W / float(g.x_max - g.x_min), sy = H / float(g.y_max - g.y_min);
    *x0 = int(std::ceil(float(g.live_x_min - g.x_min) * sx)), *x1 = int(std::floor(float(g.live_x_max - g.x_min) * sx)) - 1;
    *y0 = int(std::ceil(float(g.live_y_min - g.y_min) * sy)), *y1 = int(std::floor(float(g.live_y_max - g.y_min) * sy)) - 1;
    return 1;
}
static std::atomic<int> g_playing[2] = {0, 0};
int host_player_playing(int p) { return p >= 0 && p < 2 ? g_playing[p].load() : 0; }
// Player p's place in the game (docs/vr.md, "In play or not"): the byte at 0x51f284, player 2's 0xcc on (0x51f350).
static std::atomic<int> g_state[2] = {0, 0};
int host_player_state(int p) { return p >= 0 && p < 2 ? g_state[p].load() : 0; }
// Player p's shots, reloads and hits so far (update_ammo; quest.h host_player_events): the shells' rumble
static std::atomic<unsigned> g_events[2][HOST_EV_COUNT];
unsigned host_player_events(int p, int ev) {
    return p >= 0 && p < 2 && ev >= 0 && ev < HOST_EV_COUNT ? g_events[p][ev].load(std::memory_order_relaxed) : 0;
}
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
extern rt::GameLoop *the_game; // (below: the game being played)
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
extern "C" { volatile int arcade_brightness = 25; }
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
// The headset's key depth (m2_vr.h M2Vert::key): the arcade camera's z (game units: a third of a metre, about as far
// as a head leans from it) nearer than which key_split gives a polygon m2_gl.c's fixed step instead of its sort plane,
// and the most its sort plane may push a point out along its ray (key / z): a big floor polygon's plane (its far end's
// z, hundreds of units) met by a ray down at the ground near the camera lay hundreds of times as far off, where the
// depth buffer has no steps left between its layers (the path and the grass over it flickered under your feet, the
// two eyes and every move of the head choosing another). Both as m2_gl.c's VS has them.
constexpr float kKeyNear = 3.0f, kKeyStretch = 8.0f;
// The sky's key (m2_vr.h; arcade.cpp Backdrop, m2_gl.c: under -1e5)
constexpr float kSkyKey = -1.0e6f;

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

// Comfort (the app's Horizon and Turn vignette, d3d_vr.h VrScene up / turn / turn_all / move; a fade on camera
// cuts): the game's own camera, its world -> camera matrix in work RAM (GameCamera), every frame, indoors and out;
// for the up, the sky first when it shows (a cylinder of panels round the camera, window 1, about 1,800 units out,
// whose top and bottom are fans: the line from the bottom's centre to the top's is the world's up).
struct Comfort {
    bool have = false, ever = false;               // the last frame had the sky; some frame had
    float up[3] = {0, 1, 0}, ref[3] = {0, 0, 1}; // the last up and horizontal reference seen, camera space
    int fade = 0;                                // frames of the cut's fade left
} comfort;
constexpr float kCutTurn = 0.44f;  // radians in one frame (25 degrees): a cut, not a turn
constexpr float kCutMove = 30.f;   // game units in one frame (3.3 m): a cut, not a move

// The game's camera: its world -> camera matrix at work RAM 0x51ea50, MAME's 3 x 4 as the geometrizer takes it
// (x' = m0 x + m3 y + m6 z + m9 ...; world up +y). Found by searching RAM for a rotation that changes from frame to
// frame: its turn matches the scenery's (2.06 against 2.07 degrees a frame, stage 1), its up the sky's (0, 0.58, 0.81
// looking up at the gargoyle), and it goes on indoors where there is no sky. Read after each frame.
struct GameCamera {
    bool ok = false;
    float r[9]; // rows of R (camera <- world)
    float p[3]; // its position in the world
};
GameCamera read_camera(rt::M2Board &b) {
    GameCamera c;
    float m[12];
    for (int i = 0; i < 12; i++) {
        const uint32_t w = b.read_dword(0x0051ea50 + 4 * uint32_t(i));
        std::memcpy(&m[i], &w, 4);
        if (!std::isfinite(m[i])) return c;
    }
    const float r[9] = {m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]};
    for (int i = 0; i < 3; i++)   // a rotation, or not the camera (the attract's 2D screens, a boot)
        for (int j = 0; j < 3; j++) {
            const float d = r[3 * i] * r[3 * j] + r[3 * i + 1] * r[3 * j + 1] + r[3 * i + 2] * r[3 * j + 2];
            if (std::fabs(d - (i == j ? 1.f : 0.f)) > 0.01f) return c;
        }
    std::copy(r, r + 9, c.r);
    for (int k = 0; k < 3; k++) c.p[k] = -(r[k] * m[9] + r[3 + k] * m[10] + r[6 + k] * m[11]); // -R^T t
    c.ok = true;
    return c;
}
GameCamera game_camera, last_camera; // this frame's (the game thread sets it before publishing), the one before
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
    float turn = 0, turn_all = 0, move = 0;
    float up[3], ref[3];
    const bool ok = sky_frame(polys, up, ref);
    const GameCamera &c = game_camera, &l = last_camera;
    if (c.ok && l.ok) {   // the game's camera: all of its turn (R R'^T's angle), its heading's, its move
        float tr = 0;
        for (int i = 0; i < 9; i++) tr += c.r[i] * l.r[i];
        turn_all = std::acos(std::clamp((tr - 1.f) * 0.5f, -1.f, 1.f));
        // the heading: its forward (R^T z, R's third row) about the world's up (+y)
        const float h1 = std::atan2(c.r[6], c.r[8]), h0 = std::atan2(l.r[6], l.r[8]);
        turn = std::fabs(std::remainder(h1 - h0, 6.2831853f));
        move = std::hypot(c.p[0] - l.p[0], c.p[1] - l.p[1], c.p[2] - l.p[2]);
        if (turn_all > kCutTurn || move > kCutMove) comfort.fade = kCutFade, turn = turn_all = move = 0; // a cut
    } else if (ok && comfort.have) {
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
    else if (c.ok && c.r[4] > 0.3f) { // no sky (indoors): the camera's own (R's +y column)
        comfort.up[0] = c.r[1], comfort.up[1] = c.r[4], comfort.up[2] = c.r[7];
        comfort.ever = true;
    }
    if (comfort.ever) { // (no sky and no camera: the last up, rather than the world snapping by the camera's pitch)
        sc.up[0] = comfort.up[0], sc.up[1] = comfort.up[1], sc.up[2] = comfort.up[2];
        sc.up_ok = 1;
    }
    comfort.have = ok;
    sc.turn = turn, sc.turn_all = turn_all, sc.move = move;
    {   // M2_COMFORT_LOG=N (tests): every Nth frame (1: every one), what comfort measured
        static int on = -1, n = 0;
        if (on < 0) on = getenv("M2_COMFORT_LOG") ? std::max(0, atoi(getenv("M2_COMFORT_LOG"))) : 0;
        if (on && ++n % on == 0)
            LOGI("comfort: frame %llu camera %d sky %d, turn %.0f deg/s (heading %.0f), %.1f m/s, up %.2f %.2f %.2f",
                 (unsigned long long)the_game->frames(), int(c.ok), int(ok),
                 double(turn_all) * 60 * 57.3, double(turn) * 60 * 57.3, double(move) * 60 * 0.11, double(comfort.up[0]),
                 double(comfort.up[1]), double(comfort.up[2]));
    }
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

// The headset's key depth needs each ray from your eye to meet the polygon's sort plane (z = key) ahead of you, and
// not too far past the point (m2_gl.c). One reaching nearer the arcade's camera than kKeyNear (or behind it, as the
// floor at your feet and the sky's dome do) or than key / kKeyStretch (where its plane would push the point out more
// than kKeyStretch times) is cut there: the part beyond keeps its key, the nearer part goes out by m2_gl.c's fixed
// step, key -key (the step: key / kKeyNear, from 1.03 to kKeyStretch: the same as the plane's at the cut, so the two
// parts meet, and a layer over another stays over it). Each part as a fan of triangles to out(a, b, c).
template <class Out> void key_split(const M2Vert *pv, int n, Out out) {
    auto fan = [&](const M2Vert *v, int m) {
        for (int k = 1; k + 1 < m; k++) out(v[0], v[k], v[k + 1]);
    };
    float zmin = 1e30f;
    for (int k = 0; k < n; k++) zmin = std::min(zmin, pv[k].z);
    const float key = pv[0].key, cut = std::max(kKeyNear, key / kKeyStretch);
    if (!(key > 0.f) || zmin >= cut) return fan(pv, n);
    M2Vert front[9], back[9];
    int nf = 0, nb = 0;
    for (int i = 0; i < n; i++) {
        const M2Vert &p = pv[i], &q = pv[(i + 1) % n];
        const bool pin = p.z >= cut, qin = q.z >= cut;
        if (pin) front[nf++] = p;
        else back[nb++] = p;
        if (pin != qin) {
            const float t = (cut - p.z) / (q.z - p.z);
            M2Vert c = p;
            c.x = p.x + t * (q.x - p.x), c.y = p.y + t * (q.y - p.y), c.z = cut;
            c.u = p.u + t * (q.u - p.u), c.v = p.v + t * (q.v - p.v), c.depth = p.depth + t * (q.depth - p.depth);
            front[nf++] = c, back[nb++] = c;
        }
    }
    for (int k = 0; k < nb; k++) back[k].key = -key;
    fan(front, nf);
    fan(back, nb);
}

// The backdrop (m2_vr.h key kSkyKey): the lowest window with polygons when one above it has some too, the sky (window 1
// under the world's 2 outdoors: clouds 1,800 units out, mountains 1,670 in front of them, a lid and a cone under the
// horizon; indoors there is no window 1). The arcade draws that window wholly behind the ones over it, in its own
// order within it, and that order (by each panel's sortz, one corner's z) is right only on its own screen: off to the
// side, as a headset or a wide screen looks, a cloud panel comes over the mountains, a gap in them; by their sortz in
// the depth buffer, clouds covered mountains here and there, panel by panel as the camera moved. The renderer draws it
// where it is, behind the world (m2_gl.c draw_world). The arcade's order puts the window's polygons at the world's end
// (build: windows from the top down), as draw_world takes them.
struct Backdrop {
    int bottom = 1 << 30, top = -1; // the lowest and the highest window with polygons
    Backdrop(const std::vector<rt::GeoPoly> &polys, int windows) {
        for (const rt::GeoPoly &p : polys)
            if (p.window <= windows && p.num_vertices >= 3)
                bottom = std::min(bottom, int(p.window)), top = std::max(top, int(p.window));
    }
    void place(const rt::GeoPoly &poly, M2Vert *pv) const {
        if (int(poly.window) != bottom || bottom >= top) return;
        for (int k = 0; k < poly.num_vertices; k++) pv[k].key = kSkyKey;
    }
};

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
        // the headset's depth: at the z the arcade sorted the polygon by (m2_vr.h key), so that what it draws in
        // front (a figure over the floor or the wall its mesh dips into: the first zombie's feet, the dead lying on
        // the ground, the girl at the mansion's door) stays in front
        // (key_split cuts a polygon coming nearer the arcade's camera than its plane can serve)
        pv[k].key = poly.sortz > 0.f && poly.sortz < 1e9f ? poly.sortz : 0.f;
        if (effect) { // placed on the world behind it once the world is known (below); its texture level as the arcade's
            pv[k].depth = z;
            pv[k].prio = 0;
            pv[k].key = 0;
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
        Backdrop backdrop(polys, windows);
        uint32_t rank = 0;
        for (size_t i : order) {
            const rt::GeoPoly &poly = polys[i];
            ++rank;
            if (poly.window > windows || poly.num_vertices < 3) continue;
            bool effect = false;
            M2Vert pv[8];
            if (!poly_vertices(poly, mem, rank, c0m, c1m, fxm, fym, pv, effect)) continue;
            if (!effect) backdrop.place(poly, pv);
            key_split(pv, poly.num_vertices, [&](const M2Vert &a, const M2Vert &b, const M2Vert &c) {
                for (const M2Vert *q : {&a, &b, &c}) {
                    if (effect) {
                        fx.push_back(*q);
                        continue; // (not in the guns' scene: a shot goes through the blood to what is behind)
                    }
                    s.mv.push_back(*q);
                    s.vv.push_back(VrVert{q->x, q->y, q->z, 0xffffffffu, 0, 0, 0});
                }
            });
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
        // world round you (and you fall onto the camera); quest_xr.c shows it on a screen ahead in the dark instead,
        // the view staying immersive (arcade_screen_scene), after kScreenHold
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
// VR mode keeps it), texture RAM, luma and colour tables and the sky's up saved in courtyard5.m2w:
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
    Backdrop backdrop(polys, windows);
    uint32_t rank = 0;
    for (size_t i : order) {
        const rt::GeoPoly &poly = polys[i];
        ++rank;
        if (poly.window > windows || poly.num_vertices < 3) continue;
        bool effect = false;
        M2Vert pv[8];
        if (!poly_vertices(poly, mem, rank, c0m, c1m, pj.fx, -pj.fy, pv, effect) || effect) continue;
        backdrop.place(poly, pv);
        key_split(pv, poly.num_vertices, [&](const M2Vert &a, const M2Vert &b, const M2Vert &c) {
            world.push_back(a), world.push_back(b), world.push_back(c);
        });
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
};
// this player's gun; ctl2: a second player's here (the flat apps' local player 2: its events are the mouse's `which`
// 1, its Start the 2 key; in from its first event to quest_player2(0)), alone only (online, player 2 is the partner)
Controls ctl, ctl2;
bool ctl2_in = false;

void poll_events(bool &quit) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: quit = true; break;
        case SDL_MOUSEMOTION: {
            Controls &c = e.motion.which == 1 ? (ctl2_in = true, ctl2) : ctl;
            c.mx = e.motion.x, c.my = e.motion.y;
            break;
        }
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            Controls &c = e.button.which == 1 ? (ctl2_in = true, ctl2) : ctl;
            const bool down = e.type == SDL_MOUSEBUTTONDOWN;
            if (e.button.button == SDL_BUTTON_LEFT) c.fire = down;
            else if (e.button.button == SDL_BUTTON_RIGHT) c.reload = down;
            if (down) c.mx = e.button.x, c.my = e.button.y;
            break;
        }
        case SDL_KEYDOWN:
            if (e.key.keysym.scancode == SDL_SCANCODE_SPACE && !e.key.repeat) ctl.skip = true;
            if (e.key.keysym.scancode == SDL_SCANCODE_2 && !e.key.repeat) ctl2_in = true, ctl2.skip = true;
            break;
        case SDL_USEREVENT:
            if (e.user.code == 2) ctl2_in = true;
            if (e.user.code == 3) ctl2_in = false, ctl2 = Controls{};
            break;
        default: break;
        }
    }
}

// a player's controls for arcade_net::frame (it makes the cabinet's input, alone or online)
arcade_net::Local local_controls(Controls &c) {
    arcade_net::Local l;
    l.fire = c.fire;
    l.reload = c.reload;
    l.x = c.mx, l.y = c.my;
    l.on_screen = c.mx >= 0 && c.mx < rt::Video::W && c.my >= 0 && c.my < rt::Video::H;
    l.skip = c.skip;
    c.skip = false;
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
std::atomic<uint32_t> sound_short{0}; // samples the speakers asked for and the ring hadn't, since that line (a stutter)

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
    if (i < want) sound_short.fetch_add(uint32_t(want - i), std::memory_order_relaxed);
    for (; i < want; i++) out[i] = 0;
}

// ---- the reload gesture's own lines for the game's "Shoot outside of the screen!" (said when you pull the trigger
// on an empty gun): "Drop the mag!", "Insert a magazine!", "Rack the slide!" by the step you're at (quest_xr.c sets
// arcade_reload_step; the others: the game's line). Baked in: the line's place in this player's sample ROM holds the
// clip for the step (the rest silence), so the game's own sound program says it, as it says the line. The game and
// its board run as ever (the sample bytes only reach the speakers: the game's RAM is the same, m2probe
// M2_SND_PATCH); online each player bakes their own. Found with the sound log (M2_SCSP_LOG): the command, the
// driver's voice 5, and its samples.
constexpr uint8_t kShootOutside[3] = {0xa0, 0x14, 0x05};
constexpr uint32_t kLineFrom = 0x4b868, kLineTo = 0x548a8; // signed 8-bit, 22050 Hz, 1.67 s
constexpr int kLineFrames = 110;                           // its 1.67 s and the driver's start, in frames
std::mutex clip_mu;
std::vector<uint8_t> reload_clips[3]; // the line's length, 8-bit (arcade_reload_clip)
std::vector<uint8_t> original_line;   // the game's own bytes there
int baked = -1;                       // which is there: -1 the game's, 0..2 a clip
int line_busy = 0;                    // frames the line still plays (not rewritten under it)
// the clip for the gesture's step (arcade_reload_step), -1 the game's own line
int clip_for_step(int step) { return step == 1 || step == 2 ? 0 : step == 3 ? 1 : step == 4 ? 2 : -1; }
uint8_t cmd_seen[3];
int cmd_have = 0;

// the frame's command bytes for the sound board (in order across frames): the line said, it isn't rewritten for a while
void watch_sound_commands(const std::vector<uint8_t> &bytes) {
    for (uint8_t b : bytes) {
        if (b & 0x80) cmd_have = 0;
        if (cmd_have < 3) cmd_seen[cmd_have++] = b;
        if (cmd_have == 3 && std::memcmp(cmd_seen, kShootOutside, 3) == 0) cmd_have = 0, line_busy = kLineFrames;
    }
}
// before the board runs (the sound thread idle: the bytes are this thread's): the clip for the step in the line's
// place, or the game's own back
void bake_line(snd::Board &sb) {
    if (line_busy > 0) { line_busy--; return; }
    std::lock_guard<std::mutex> lock(clip_mu);
    int want = clip_for_step(arcade_reload_step);
    if (want >= 0 && reload_clips[want].empty()) want = -1;
    if (want == baked) return;
    if (original_line.empty()) original_line = sb.sample_bytes(kLineFrom, kLineTo);
    if (original_line.size() != kLineTo - kLineFrom) return; // (a board without the sample ROM)
    sb.patch_samples(kLineFrom, want >= 0 ? reload_clips[want] : original_line);
    baked = want;
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

} // namespace
extern "C" void arcade_reload_clip(int step, const int16_t *pcm, int n) {   // 22050 Hz mono
    if (step < 0 || step > 2) return;
    std::lock_guard<std::mutex> lock(clip_mu);
    std::vector<uint8_t> &c = reload_clips[step];
    c.assign(kLineTo - kLineFrom, 0);  // (silence after it)
    for (int i = 0; i < n && size_t(i) < c.size(); i++) c[size_t(i)] = uint8_t(int8_t(std::clamp((pcm[i] + 128) >> 8, -128, 127)));
    if (n > int(c.size())) LOGE("reload line %d: %d samples, the line has room for %zu", step, n, c.size());
    baked = -2; // (written again)
}
namespace {

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
    auto put = [&](int u, int v, uint16_t c) {
        if (u >= 0 && u < rt::Video::W && v >= 0 && v < rt::Video::H) px[size_t(v) * rt::Video::W + size_t(u)] = c;
    };
    if (port_crosshair_big) { // a TV: longer ticks, three pixels thick, in a black outline, and a dot in the middle
        auto tick = [&](uint16_t c, int grow) {
            for (int d = 4 - grow; d <= 14 + grow; d++)
                for (int t = -1 - grow; t <= 1 + grow; t++)
                    put(x + d, y + t, c), put(x - d, y + t, c), put(x + t, y + d, c), put(x + t, y - d, c);
            for (int v = -1 - grow; v <= 1 + grow; v++)
                for (int u = -1 - grow; u <= 1 + grow; u++) put(x + u, y + v, c);
        };
        tick(0x0000, 1);
        tick(colour, 0);
        return;
    }
    for (int d = 3; d <= 9; d++) // four ticks around the aim, two pixels thick
        for (int t = 0; t < 2; t++)
            put(x + d, y + t, colour), put(x - d, y + t, colour), put(x + t, y + d, colour), put(x + t, y - d, colour);
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
        if (p != me && !online && !(p == 1 && ctl2_in)) continue;      // alone: your own (and a player 2's here)
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

// M2_SHOTS=f1,f2,... (tests, compared with MAME by scripts/compare_mame.py): after drawing each of those frames
// the game waits until the shell has saved its picture (flat.c: shot_<frame>.ppm); M2_INPUT_SCRIPT=path plays a
// scripts/inputs file (alone only) instead of the player's controls, frame for frame as m2run does.
extern "C" volatile uint64_t arcade_shot_serial = 0; // the scene to save, set by the game thread; 0 again once saved
extern "C" volatile uint32_t arcade_shot_frame = 0;
namespace {
std::vector<uint64_t> shot_frames() {
    std::vector<uint64_t> v;
    if (const char *e = getenv("M2_SHOTS"))
        for (const char *p = e; *p;) {
            char *end;
            const unsigned long long f = std::strtoull(p, &end, 10);
            if (end == p) { p++; continue; }
            v.push_back(f);
            p = end;
        }
    return v;
}
} // namespace

rt::GameLoop *the_game = nullptr;

// RESTART (the menu: QACT_RESTART): the game back to its start, as when the cabinet is switched on. The board has no
// reset; the game thread keeps a save state of it soon after power-on (kRestartFrame: the start-up checks done, the
// attract about to begin) and loads it when asked, between frames; the settings and high scores (the I/O board's
// EEPROM, the backup RAM) are written out first and read back after, so they are kept. Not while playing online
// (the other player's game would go on: the menu doesn't offer it then).
constexpr uint64_t kRestartFrame = 240;
std::vector<uint8_t> restart_state;
std::atomic<int> restart_wanted{0};
uint32_t game_crc() { return the_game->board().state_hash(); } // online play's checksum (arcade_net)

// The players' bullets and lives in work RAM (found with tools/m2probe: RAM at every shot of the scripted games, and
// the two-player dumps of docs/vr.md): each player's block 0xcc bytes apart, lives at 0x51f290 / 0x51f35c, bullets at
// 0x51f2c1 / 0x51f38d, the state byte at 0x51f284 / 0x51f350. A player with no lives left (game over, the continue
// countdown, the attract) isn't in the game.
void update_ammo(rt::GameLoop &game) {
    rt::M2Board &b = game.board();
    for (int p = 0; p < 2; p++) {
        const uint8_t lives = b.read_byte(p ? 0x0051f35c : 0x0051f290);
        const uint8_t ammo = b.read_byte(p ? 0x0051f38d : 0x0051f2c1);
        const uint8_t state = b.read_byte(p ? 0x0051f350 : 0x0051f284);
        g_ammo[p] = lives > 0 && ammo <= 6 ? int(ammo) : -1;
        // In play: the player in the game (the state byte: 5, not the continue countdown, the title, the attract
        // demo or a player 2 who never joined) out of its cut scenes (the phase, 0x51f4b4 / 0x51f4b5 for players 1 /
        // 2: 4 a cut scene, 1..3 play), alive. Found by which trigger pulls the game takes (m2probe RAM dumps,
        // docs/vr.md). (0x51f4a0 counts the players in the game: 2 in any two-player game, so it can't say this.)
        const uint8_t phase = b.read_byte(p ? 0x0051f4b5 : 0x0051f4b4);
        const bool playing = state == HOST_P_IN && phase >= 1 && phase <= 3 && lives > 0;
        // the events (host_player_events), from the last game frame to this one, in the game (the attract demo's
        // players are state 9): a bullet less, a full magazine again in play, a life less. Steps of exactly one
        // (a save state loaded online, RESTART, the start of a game or a continue are none of them).
        static uint8_t was_lives[2], was_ammo[2], was_state[2];
        static bool was_playing[2];
        if (was_state[p] == HOST_P_IN && was_lives[p] > 0) {
            if (was_ammo[p] <= 6 && ammo + 1 == was_ammo[p]) g_events[p][HOST_EV_SHOT]++;
            if (was_playing[p] && was_ammo[p] < 6 && ammo == 6 && lives == was_lives[p]) g_events[p][HOST_EV_RELOAD]++;
            if (lives + 1 == was_lives[p]) g_events[p][HOST_EV_HIT]++;
        }
        was_lives[p] = lives, was_ammo[p] = ammo, was_state[p] = state, was_playing[p] = playing;
        g_playing[p] = playing;
        g_state[p] = state;
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
extern "C" void arcade_restart(void) { restart_wanted.store(1); }
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
        // (courtyard7 (6: the near parts' key -1, the sky's -2; 5: the sky by its sortz, 4: M2Vert::key a ratio, 3
        // before it): the fountain, with the whole scenery sent (hook_hotd_scenery_cull); the earlier ones, the road
        // outside the gate, are dropped)
        const std::string cy = game_dir + "/courtyard7.m2w", images = game_dir + "/images";
        std::remove((game_dir + "/courtyard6.m2w").c_str()); // (7: key_split's -key, kSkyKey)
        std::remove((game_dir + "/courtyard5.m2w").c_str());
        std::remove((game_dir + "/courtyard4.m2w").c_str());
        std::remove((game_dir + "/courtyard3.m2w").c_str());
        std::remove((game_dir + "/courtyard2.m2w").c_str());
        std::remove((game_dir + "/courtyard.m2w").c_str());
        if (made_images) std::remove(cy.c_str());
        courtyard_path = cy;
        if (exists(cy)) {
            courtyard_ready.store(courtyard_path.c_str(), std::memory_order_release);
        } else if (arcade_flat) {
            // a screen has no cabinet room: not made (the Apple TV HD's two cores took 42 s over it beside the game)
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
    struct { uint64_t frames = 0, board = 0, core = 0, tgpx4 = 0, geo = 0, video = 0, scene = 0, sound = 0, tgpx4_at = 0, skipped = 0; } prof;
    bool skipped_last = false;
    SoundThread sound_thread; // (after the game: it goes first, before the sound board its packets use)
    const std::vector<uint64_t> shots = shot_frames();
    std::unique_ptr<tools::Script> input_script;
    if (const char *e = getenv("M2_INPUT_SCRIPT"); e && *e) {
        input_script = std::make_unique<tools::Script>();
        input_script->load(e);
        LOGI("input script %s", e);
    }
    while (!quit) {
        poll_events(quit);
        // RESTART: its save state once, then on request (the sound thread idle: the state has its last frame)
        if (restart_state.empty() && game->frames() == kRestartFrame && arcade_net::mode() == arcade_net::Off) {
            sound_thread.wait();
            try {
                game->save_state(restart_state);
                LOGI("restart state: %zu bytes at frame %llu", restart_state.size(), (unsigned long long)game->frames());
            } catch (const std::exception &e) {
                restart_state.clear();
                LOGE("restart state: %s", e.what());
            }
        }
        {   // M2_TEST_RESTART_AT=frame (tests): RESTART pressed once, at that frame
            static long long at = -2;
            if (at == -2) at = getenv("M2_TEST_RESTART_AT") ? std::atoll(getenv("M2_TEST_RESTART_AT")) : -1;
            if (at >= 0 && game->frames() == uint64_t(at)) at = -1, restart_wanted.store(1);
        }
        if (restart_wanted.exchange(0)) {
            if (restart_state.empty() || arcade_net::mode() != arcade_net::Off) {
                LOGE("restart: %s", restart_state.empty() ? "no state yet" : "not online");
            } else {
                sound_thread.wait();
                try {
                    tools::save_nvram(*game, save_dir); // the settings and scores as they are now, kept
                    game->load_state(restart_state.data(), restart_state.size());
                    tools::load_nvram(*game, save_dir);
                    saved_eeprom.assign(game->board().io().eeprom.begin(), game->board().io().eeprom.end());
                    saved_backup.assign(game->board().backup_ram().begin(), game->board().backup_ram().end());
                    game_camera = last_camera = GameCamera{};
                    comfort.fade = kCutFade; // (from black)
                    next = now_ns();
                    LOGI("restarted: frame %llu", (unsigned long long)game->frames());
                } catch (const std::exception &e) {
                    LOGE("restart: %s", e.what());
                }
                continue;
            }
        }
        // a guest joining: the host's game as it is (its save state), loaded instead of running every frame since
        // the host's start; the host's bundles follow from the frame after it
        if (arcade_net::snapshot_waiting()) {
            sound_thread.wait();
            try {
                arcade_net::guest_load(*game);
            } catch (const std::exception &e) {
                LOGE("the host's game did not load: %s", e.what());
                return stay_on(std::string("The host's game did not load:\n\n") + e.what() +
                               "\n\n(Both players need the same version of the app.) Press the menu button to quit.");
            }
            continue;
        }
        // this frame's cabinet input: made here (alone, or hosting: and sent), or the host's (a guest waits for it)
        rt::Inputs in;
        const bool second = ctl2_in && arcade_net::mode() == arcade_net::Off;
        arcade_net::Local local2;
        if (second) local2 = local_controls(ctl2);
        if (!arcade_net::frame(local_controls(ctl), in, game_crc, second ? &local2 : nullptr)) continue;
        if (input_script && arcade_net::mode() == arcade_net::Off) {
            in = input_script->at(game->frames());
            input_script->pokes(game->frames(), game->board());
        }
        rt::GameLoop::SoundPacket packet; // this frame's sound board work: on the sound thread, during the next frame
        // a screen behind the arcade's time (a slow CPU: the Apple TV HD's busiest scenes took 18 ms a frame of 17.4),
        // alone or hosting, or a guest with the host's frames queued, skips drawing a frame, never two in a row: the 2D
        // layers, the scene and the overlay (about 6 ms there). The game runs every frame as ever (the same game for
        // every player), so it and its sound keep up instead of the sound running short.
        bool skip_draw = false;
        const bool late = arcade_net::mode() == arcade_net::Guest ? arcade_net::catching_up() >= 1 : now_ns() > next + frame_ns / 4;
        if (arcade_flat && late && !skipped_last &&
            std::find(shots.begin(), shots.end(), game->frames() + 1) == shots.end()) {
            game->board().skip_next_draw();
            skip_draw = true;
            prof.skipped++;
        }
        skipped_last = skip_draw;
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
        last_camera = game_camera;
        game_camera = read_camera(game->board());
        const uint64_t t_scene = now_ns();
        const int behind = arcade_net::catching_up(); // a guest with frames to catch up on: 2 many (don't draw)
        const rt::Video &video = game->board().video();
        update_gamma();   // (Brightness changed in the menu)
        if (behind < 2 && !skip_draw) {
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
            // (far behind: no sound of frames gone by; on a screen a few behind keep theirs, the ring trims a backlog:
            // a slow guest always a few frames behind had no sound at all)
            if (behind >= 2 || (behind && !arcade_flat)) game->sound()->take_pcm();
            else push_audio(*game->sound());
        }
        if (arcade_net::snapshot_wanted()) {
            // hosting, a guest joined: the game saved as it is after this frame, its sound frame run first (here:
            // the sound thread is idle) so the state has it
            try {
                packet.execute();
                packet = rt::GameLoop::SoundPacket{};
                arcade_net::host_save(*game);
            } catch (const std::exception &e) {
                LOGE("the save state for the guest: %s", e.what());
            }
        }
        watch_sound_commands(packet.bytes());
        if (game->sound()) bake_line(*game->sound()); // (the sound thread idle till run: its bytes are ours)
        sound_thread.run(std::move(packet));
        prof.scene += now_ns() - t_scene;
        if (++prof.frames == 600) {
            const uint64_t tg = tgpx4_ns_total();
            const double k = 1e-6 / double(prof.frames);
            static uint64_t line_at;   // the game's own rate: under the arcade's 57.5 a second, its sound runs short
            const uint64_t t_line = now_ns();
            if (line_at) LOGI("frame ms: %.1f frames a second, %u not drawn (behind the arcade's time)", 600e9 / double(t_line - line_at), unsigned(prof.skipped));
            line_at = t_line;
            LOGI("frame ms: board %.2f (i960 and TGP %.2f, TGPx4 %.2f, geometrizer %.2f, 2D layers %.2f), scene and "
                 "overlay %.2f; sound thread %.2f (peak %.3f, speakers short %.0f ms)",
                 double(prof.board) * k, double(prof.core) * k, double(tg - prof.tgpx4_at) * k, double(prof.geo) * k,
                 double(prof.video) * k, double(prof.scene) * k, double(prof.sound) * k, double(sound_peak),
                 double(sound_short.exchange(0)) * 1000.0 / (2.0 * snd::Scsp::kRate));
            sound_peak = 0;
            prof = {};
            prof.tgpx4_at = tg;
        }
        if (behind < 2) SDL_RenderPresent(ren); // waits while paused (the system menu, the headset off)
        if (std::find(shots.begin(), shots.end(), game->frames()) != shots.end()) {
            {
                std::lock_guard<std::mutex> lock(g.mu);
                arcade_shot_frame = uint32_t(game->frames());
                arcade_shot_serial = g.serial;
            }
            for (int i = 0; i < 2500 && arcade_shot_serial; i++) plat_sleep_until_ns(now_ns() + 2000000);
            if (arcade_shot_serial) LOGE("shot of frame %u: not saved", unsigned(game->frames()));
            arcade_shot_serial = 0;
        }

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

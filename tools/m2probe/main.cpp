// m2probe (diagnostics): the game under an input script, with
//   --ram FIRST,STEP,COUNT DIR   main RAM (0x200000, 128 KB) then work RAM (0x500000, 1 MB) saved at frames
//                                FIRST, FIRST+STEP, ... into DIR/ram_<frame>.bin (finding a game variable)
//   --layers FIRST,STEP,COUNT DIR the 2D layers behind (back_) and in front of (front_) the 3D, raw
//   --watch ADDR                 the frames where the byte at ADDR (hex) changes, and its values
//   --near FIRST,STEP,COUNT      the polygons near the camera (effects): screen box, z, texture state
//   --polys FIRST,STEP,COUNT     at those frames, the geometrizer's polygons by window and camera: how many, their
//                                z range, centre and focus (which are the 3D world, which screen-space overlays)
//   --board-frames               the script's inputs and the frame numbers by the board's frame count, as m2gpushot
//                                and m2run read them (otherwise by this loop's count, which drifts from it)
// The script's poke lines are applied before each frame, as m2gpushot does.
//
//   m2probe IMAGES_DIR FRAMES --inputs SCRIPT [--ram ...] [--polys ...] [--vr] [--board-frames]
#include "runtime/board/game_loop.h"
#include "../common/input_script.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace {
struct Every {
    uint64_t first = 0, step = 1, count = 0;
    bool at(uint64_t f) const { return count && f >= first && (f - first) % step == 0 && (f - first) / step < count; }
};
Every parse(const char *s) {
    Every e;
    unsigned long long a = 0, b = 1, c = 0;
    if (std::sscanf(s, "%llu,%llu,%llu", &a, &b, &c) == 3) e.first = a, e.step = b ? b : 1, e.count = c;
    return e;
}
} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: m2probe IMAGES_DIR FRAMES --inputs SCRIPT [--ram F,S,N DIR] [--polys F,S,N] [--vr]\n");
        return 2;
    }
    const std::string dir = argv[1];
    const uint64_t frames = std::strtoull(argv[2], nullptr, 10);
    std::string inputs, ram_dir;
    Every ram, polys, layers, nearp, sky;
    std::string layer_dir;
    bool vr = false, aim = false, board_frames = false;
    uint32_t watch = 0; // --watch ADDR: print the frames where this byte changes
    int watched = -1;
    for (int i = 3; i < argc; i++) {
        if (!std::strcmp(argv[i], "--inputs") && i + 1 < argc) inputs = argv[++i];
        else if (!std::strcmp(argv[i], "--ram") && i + 2 < argc) ram = parse(argv[++i]), ram_dir = argv[++i];
        else if (!std::strcmp(argv[i], "--polys") && i + 1 < argc) polys = parse(argv[++i]);
        else if (!std::strcmp(argv[i], "--vr")) vr = true;
        else if (!std::strcmp(argv[i], "--board-frames")) board_frames = true; // inputs and frame numbers by the board's frame count, as m2gpushot and m2run
        else if (!std::strcmp(argv[i], "--watch") && i + 1 < argc) watch = uint32_t(std::strtoul(argv[++i], nullptr, 16));
        else if (!std::strcmp(argv[i], "--aim")) aim = true;
        else if (!std::strcmp(argv[i], "--sky") && i + 1 < argc) sky = parse(argv[++i]); // window 1's polygons, camera space // --near also lists small polygons around the screen's centre
        else if (!std::strcmp(argv[i], "--near") && i + 1 < argc) nearp = parse(argv[++i]);
        else if (!std::strcmp(argv[i], "--layers") && i + 2 < argc) layers = parse(argv[++i]), layer_dir = argv[++i];
    }
    tools::Script script;
    if (!inputs.empty()) script.load(inputs);
    rt::GameLoop game(dir);
    game.board().video().set_external_3d(true, false); // as the Quest app: the 2D layers composed on the CPU
    game.board().video().set_cpu_layers(true);
    if (vr) game.board().set_vr(true);
    if (const char *e = std::getenv("M2_SND_PATCH"); e && game.sound()) { // tests: HEXADDR,FILE written over the sample ROM
        unsigned at = 0;
        char path[512] = "";
        if (std::sscanf(e, "%x,%511s", &at, path) == 2)
            if (FILE *f = std::fopen(path, "rb")) {
                std::vector<uint8_t> b;
                for (int c; (c = std::fgetc(f)) != EOF;) b.push_back(uint8_t(c));
                std::fclose(f);
                game.sound()->patch_samples(at, b);
            }
    }
    for (uint64_t f = 0; f < frames; f++) {
        const uint64_t at = board_frames ? game.board().frame() : f;
        script.pokes(at, game.board());
        game.run_frame(script.at(at));
        const uint64_t n = board_frames ? game.board().frame() : f + 1;
        if (watch) {
            const int b = game.board().read_byte(watch);
            if (b != watched) std::printf("frame %llu: %08x = %d\n", (unsigned long long)n, watch, b), watched = b;
        }
        if (ram.at(n)) {
            std::vector<uint8_t> d;
            d.reserve(0x120000);
            for (uint32_t a = 0; a < 0x20000; a++) d.push_back(game.board().read_byte(0x200000 + a));
            for (uint32_t a = 0; a < 0x100000; a++) d.push_back(game.board().read_byte(0x500000 + a));
            char path[512];
            std::snprintf(path, sizeof path, "%s/ram_%05llu.bin", ram_dir.c_str(), (unsigned long long)n);
            if (FILE *o = std::fopen(path, "wb")) std::fwrite(d.data(), 1, d.size(), o), std::fclose(o);
        }
        if (layers.at(n)) { // the 2D layers behind and in front of the 3D, as the Quest app gets them
            for (int k = 0; k < 2; k++) {
                const std::vector<uint32_t> &l = k ? game.board().video().foreground_layer() : game.board().video().background_layer();
                char path[512];
                std::snprintf(path, sizeof path, "%s/%s_%05llu.rgb", layer_dir.c_str(), k ? "front" : "back", (unsigned long long)n);
                if (FILE *o = std::fopen(path, "wb")) std::fwrite(l.data(), 4, l.size(), o), std::fclose(o);
            }
        }
        if (nearp.at(n)) { // each polygon wholly within 5 units of the camera: where on the screen, how big, its state
            const rt::Video &v = game.board().video();
            int shown = 0;
            for (const rt::GeoPoly &q : v.gpu_polys()) {
                float zmin = 1e30f, zmax = -1e30f, x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
                for (int k = 0; k < q.num_vertices; k++) {
                    const float z = q.v[k].p[0];
                    zmin = std::min(zmin, z), zmax = std::max(zmax, z);
                    if (z > 0) {
                        const float sx = float(v.crtc_x() + q.center[0]) + q.v[k].x / z, sy = float(384 - q.center[1] + v.crtc_y()) - q.v[k].y / z;
                        x0 = std::min(x0, sx), x1 = std::max(x1, sx), y0 = std::min(y0, sy), y1 = std::max(y1, sy);
                    }
                }
                // near the camera, or (--aim) a small polygon around the aim (an impact's marker)
                const bool small_at_aim = aim && zmin > 0 && x1 - x0 < 160 && y1 - y0 < 160 && (x0 + x1) / 2 > 248 - 80 &&
                                          (x0 + x1) / 2 < 248 + 80 && (y0 + y1) / 2 > 192 - 80 && (y0 + y1) / 2 < 192 + 80;
                if (!(zmin > 0 && zmax < 5 && x1 > 0 && x0 < 496 && y1 > 0 && y0 < 384) && !small_at_aim) continue;
                if (shown++ < 40)
                    std::printf("  near z %.3f..%.3f screen %4.0f,%4.0f - %4.0f,%4.0f (%4.0fx%4.0f) renderer %d th %04x %04x %04x %04x luma %d win %d\n",
                                zmin, zmax, x0, y0, x1, y1, x1 - x0, y1 - y0, (q.texheader[0] >> 13) & 3, q.texheader[0],
                                q.texheader[1], q.texheader[2], q.texheader[3], q.luma, q.window);
            }
            std::printf("frame %llu: %d near polygons\n", (unsigned long long)n, shown);
        }
        if (sky.at(n)) {
            std::printf("frame %llu: window 1\n", (unsigned long long)n);
            for (const rt::GeoPoly &q : game.board().video().gpu_polys()) {
                if (q.window != 1) continue;
                std::printf("  poly");
                for (int k = 0; k < q.num_vertices; k++)
                    std::printf(" (%.0f %.0f %.0f)", q.v[k].x / q.focus[0], q.v[k].y / q.focus[1], q.v[k].p[0]);
                std::printf("\n");
            }
        }
        if (polys.at(n)) {
            const std::vector<rt::GeoPoly> &p = game.board().video().gpu_polys();
            struct Group { int n = 0; float zmin = 1e30f, zmax = -1e30f; };
            std::map<std::tuple<int, int, int, float, float>, Group> g;
            for (const rt::GeoPoly &q : p) {
                Group &e = g[{q.window, q.center[0], q.center[1], q.focus[0], q.focus[1]}];
                e.n++;
                for (int k = 0; k < q.num_vertices; k++) e.zmin = std::min(e.zmin, q.v[k].p[0]), e.zmax = std::max(e.zmax, q.v[k].p[0]);
            }
            std::printf("frame %llu: %zu polygons\n", (unsigned long long)n, p.size());
            for (auto &[k, e] : g)
                std::printf("  window %d centre %d,%d focus %g,%g: %d polygons, z %g .. %g\n", std::get<0>(k), std::get<1>(k),
                            std::get<2>(k), std::get<3>(k), std::get<4>(k), e.n, e.zmin, e.zmax);
            // the nearest polygons: how near, how many
            int near1 = 0, near5 = 0;
            for (const rt::GeoPoly &q : p) {
                float zmax = -1e30f;
                for (int k = 0; k < q.num_vertices; k++) zmax = std::max(zmax, q.v[k].p[0]);
                if (zmax < 1.f && zmax > 0.f) near1++;
                if (zmax < 5.f && zmax > 0.f) near5++;
            }
            std::printf("  wholly nearer than 1: %d, than 5: %d\n", near1, near5);
        }
    }
    return 0;
}

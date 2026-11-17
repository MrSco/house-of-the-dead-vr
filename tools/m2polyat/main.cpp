// m2polyat: which polygons cover a pixel of a frame, in the rasterizer's order (the first wins), with their sort
// keys: for finding why a renderer shows another polygon there than the arcade (MAME) does.
//
//   m2polyat IMAGES_DIR FRAME X Y [--inputs scripts/inputs/X.txt]
//
// X, Y: the original screen's pixel (496 x 384). The VR scenery hook is on, as in the apps (M2_VR=0: off).
#include "runtime/board/game_loop.h"
#include "../common/input_script.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: m2polyat IMAGES_DIR FRAME X Y [--inputs SCRIPT]\n");
        return 2;
    }
    const uint64_t frames = std::strtoull(argv[2], nullptr, 10);
    const float X = float(std::atof(argv[3])), Y = float(std::atof(argv[4]));
    tools::Script script;
    for (int i = 5; i + 1 < argc; i++)
        if (!std::strcmp(argv[i], "--inputs")) script.load(argv[++i]);
    rt::GameLoop game(argv[1]);
    if (!std::getenv("M2_VR") || std::atoi(std::getenv("M2_VR")) != 0) game.board().set_vr(true);
    for (uint64_t f = 0; f < frames; f++) script.pokes(f, game.board()), game.run_frame(script.at(f));
    const rt::Video &video = game.board().video();
    const std::vector<rt::GeoPoly> &polys = video.gpu_polys();
    std::vector<size_t> order(polys.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (polys[a].window != polys[b].window) return polys[a].window > polys[b].window;
        if (polys[a].z != polys[b].z) return polys[a].z < polys[b].z;
        return a > b;
    });
    std::printf("frame %llu: %zu polygons; those over (%g, %g), first (the winner) first:\n", (unsigned long long)frames,
                polys.size(), X, Y);
    {   // polygons with no usable sort depth (the key leaves them at their real depth)
        int bad = 0, behind = 0, crossing = 0;
        for (const rt::GeoPoly &p : polys) {
            float zmin = 1e30f, zmax = -1e30f;
            for (int i = 0; i < p.num_vertices; i++) zmin = std::min(zmin, p.v[i].p[0]), zmax = std::max(zmax, p.v[i].p[0]);
            if (!(p.sortz > 0.f && p.sortz < 1e9f)) bad++;
            if (zmax <= 0) behind++;
            else if (zmin <= 0) crossing++;
        }
        std::printf("  no sort depth: %d; all behind the camera: %d; crossing its plane: %d\n", bad, behind, crossing);
    }
    if (const char *e = std::getenv("M2_POLYAT_KEYS_BELOW")) {   // every polygon sorted nearer than that, crossing included
        const float below = float(std::atof(e));
        for (size_t rank = 0; rank < order.size(); rank++) {
            const rt::GeoPoly &p = polys[order[rank]];
            if (!(p.sortz < below)) continue;
            float zmin = 1e30f, zmax = -1e30f;
            for (int i = 0; i < p.num_vertices; i++) zmin = std::min(zmin, p.v[i].p[0]), zmax = std::max(zmax, p.v[i].p[0]);
            if (zmin > 0) continue; // (only those crossing the camera's plane or behind it)
            std::printf("  keyed nearer: rank %5zu sortz %8.3f verts z %8.3f..%8.3f window %d tex %04x %04x\n", rank,
                        double(p.sortz), double(zmin), double(zmax), p.window, p.texheader[0], p.texheader[2]);
        }
        return 0;
    }
    int shown = 0;
    for (size_t rank = 0; rank < order.size(); rank++) {
        const rt::GeoPoly &p = polys[order[rank]];
        float px[8], py[8];
        bool front = true;
        for (int i = 0; i < p.num_vertices; i++) {
            const float z = p.v[i].p[0];
            if (z <= 0) front = false;
            px[i] = float(video.crtc_x() + p.center[0]) + p.v[i].x / z;
            py[i] = float((384 - p.center[1]) + video.crtc_y()) - p.v[i].y / z;
        }
        if (!front || p.num_vertices < 3) continue;
        if (std::getenv("M2_POLYAT_NEAR")) {   // (debugging the projection: every polygon nearer than that, its box)
            float zmax = 0, x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
            for (int i = 0; i < p.num_vertices; i++)
                zmax = std::max(zmax, p.v[i].p[0]), x0 = std::min(x0, px[i]), x1 = std::max(x1, px[i]), y0 = std::min(y0, py[i]), y1 = std::max(y1, py[i]);
            if (zmax < float(std::atof(std::getenv("M2_POLYAT_NEAR"))))
                std::printf("  near: rank %zu z<=%.1f box x %.0f..%.0f y %.0f..%.0f tex %04x\n", rank, double(zmax), double(x0), double(x1), double(y0), double(y1), p.texheader[0]);
            continue;
        }
        // inside the convex polygon (either winding)
        int pos = 0, neg = 0;
        for (int i = 0; i < p.num_vertices; i++) {
            const int j = (i + 1) % p.num_vertices;
            const float c = (px[j] - px[i]) * (Y - py[i]) - (py[j] - py[i]) * (X - px[i]);
            if (c > 0) pos++; else if (c < 0) neg++;
        }
        if ((pos && neg) || (!pos && !neg)) continue; // (outside, or a polygon of no area)
        float zmin = 1e30f, zmax = -1e30f;
        for (int i = 0; i < p.num_vertices; i++) zmin = std::min(zmin, p.v[i].p[0]), zmax = std::max(zmax, p.v[i].p[0]);
        std::printf("  rank %5zu  zkey %04x  sortz %9.3f  verts z %8.3f..%8.3f  window %d  texheader %04x %04x %04x %04x\n",
                    rank, p.z, double(p.sortz), double(zmin), double(zmax), p.window, p.texheader[0], p.texheader[1],
                    p.texheader[2], p.texheader[3]);
        if (++shown == 40) break;
    }
    return 0;
}

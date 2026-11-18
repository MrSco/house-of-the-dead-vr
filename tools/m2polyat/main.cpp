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
#include <map>
#include <numeric>
#include <tuple>
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
    // M2_POLYAT_OBJECTS=FIRST: every frame from FIRST on, the models drawn and their matrices (Geo::objects), then out
    const char *objs = std::getenv("M2_POLYAT_OBJECTS");
    const uint64_t objs_from = objs ? std::strtoull(objs, nullptr, 10) : 0;
    // M2_POLYAT_TEXCHECK=1: the hardware renderers' copy of texture RAM (arcade.cpp copy_texture: a 4 KB row copied when
    // its page's generation moves) against texture RAM itself, every frame: the rows that differ (a write its page's
    // generation missed: those renderers would draw stale texels there)
    const bool texcheck = std::getenv("M2_POLYAT_TEXCHECK") != nullptr;
    std::vector<uint32_t> tex_copy(2 * 512 * 1024);
    std::vector<uint64_t> tex_seen(2 * 512, ~uint64_t(0));
    uint64_t stale_frames = 0;
    for (uint64_t f = 0; f < frames; f++) {
        script.pokes(f, game.board()), game.run_frame(script.at(f));
        if (texcheck) {
            const rt::VideoMem &m = game.board().video().gpu_mem();
            int stale = 0, first = -1;
            for (uint32_t row = 0; row < 1024; row++) {
                const uint32_t *src = row < 512 ? m.tex0 + row * 1024 : m.tex1 + (row - 512) * 1024;
                if (m.tex_page_generation[row] != tex_seen[row]) {
                    tex_seen[row] = m.tex_page_generation[row];
                    std::memcpy(tex_copy.data() + size_t(row) * 1024, src, 4096);
                } else if (std::memcmp(tex_copy.data() + size_t(row) * 1024, src, 4096)) {
                    if (first < 0) first = int(row);
                    stale++;
                }
            }
            if (stale) stale_frames++;
            if (stale && (f + 1) % 50 == 0)
                std::printf("frame %llu: %d texture rows stale (first %d)\n", (unsigned long long)(f + 1), stale, first);
            continue;
        }
        if (objs && f + 1 >= objs_from && std::getenv("M2_POLYAT_SKY")) {   // (and =1: the sky's panels kept, window 1)
            int n = 0, lod0 = 1 << 30, lod1 = -(1 << 30);
            for (const rt::GeoPoly &p : game.board().video().gpu_polys())
                if (p.window == 1) {
                    n++;
                    if (p.texheader[0] == 0x60cb) lod0 = std::min(lod0, int(p.texlod)), lod1 = std::max(lod1, int(p.texlod));
                }
            {
                unsigned wins = 0;
                for (const rt::GeoPoly &p : game.board().video().gpu_polys()) wins |= 1u << (p.window & 7);
                std::printf("windows %llu %x\n", (unsigned long long)(f + 1), wins);
            }
            std::printf("sky %llu %d mountains texlod %d..%d\n", (unsigned long long)(f + 1), n, lod0, lod1);
            continue;
        }
        if (objs && f + 1 >= objs_from)
            for (const rt::Geo::Object &o : game.board().geo().objects)
                std::printf("obj %llu %08x %d %g %g %g %g %g %g %g %g %g %g %g %g\n", (unsigned long long)(f + 1), o.address, o.window,
                            o.m[0], o.m[1], o.m[2], o.m[3], o.m[4], o.m[5], o.m[6], o.m[7], o.m[8], o.m[9], o.m[10], o.m[11]);
    }
    if (texcheck) {
        std::printf("texcheck: %llu of %llu frames with stale rows\n", (unsigned long long)stale_frames, (unsigned long long)frames);
        return 0;
    }
    if (objs) return 0;
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
    if (const char *path = std::getenv("M2_POLYAT_SHADOWS")) {
        // the pixels where the arcade's order puts a shadow (solid black, texheader 00c0, colour 0) over a textured
        // polygon nearer the camera there (a figure standing on it): how many, and a mask (PGM) of them
        struct P { float x[8], y[8], iz[8]; int n; float a, b, c; bool ok; };
        std::vector<P> pp(order.size());
        for (size_t r = 0; r < order.size(); r++) {
            const rt::GeoPoly &p = polys[order[r]];
            P &q = pp[r];
            q.n = p.num_vertices, q.ok = q.n >= 3;
            for (int i = 0; i < q.n && q.ok; i++) {
                const float z = p.v[i].p[0];
                if (z <= 0.01f) { q.ok = false; break; }
                q.x[i] = float(video.crtc_x() + p.center[0]) + p.v[i].x / z;
                q.y[i] = float((384 - p.center[1]) + video.crtc_y()) - p.v[i].y / z;
                q.iz[i] = 1.f / z;
            }
            if (!q.ok) continue;   // 1/z over the screen: the plane through the first three points
            const float x1 = q.x[1] - q.x[0], y1 = q.y[1] - q.y[0], z1 = q.iz[1] - q.iz[0];
            const float x2 = q.x[2] - q.x[0], y2 = q.y[2] - q.y[0], z2 = q.iz[2] - q.iz[0];
            const float d = x1 * y2 - x2 * y1;
            if (std::fabs(d) < 1e-6f) { q.ok = false; continue; }
            q.a = (z1 * y2 - z2 * y1) / d, q.b = (x1 * z2 - x2 * z1) / d, q.c = q.iz[0] - q.a * q.x[0] - q.b * q.y[0];
        }
        auto inside = [](const P &q, float X, float Y) {
            int pos = 0, neg = 0;
            for (int i = 0; i < q.n; i++) {
                const int j = (i + 1) % q.n;
                const float c = (q.x[j] - q.x[i]) * (Y - q.y[i]) - (q.y[j] - q.y[i]) * (X - q.x[i]);
                if (c > 0) pos++; else if (c < 0) neg++;
            }
            return !(pos && neg) && (pos || neg);
        };
        std::vector<uint8_t> mask(496 * 384, 0);
        int shadow_px = 0, over = 0;
        for (int y = 0; y < 384; y++)
            for (int x = 0; x < 496; x++) {
                const float X = x + .5f, Y = y + .5f;
                size_t win = SIZE_MAX;
                for (size_t r = 0; r < order.size() && win == SIZE_MAX; r++)
                    if (pp[r].ok && inside(pp[r], X, Y)) win = r;
                if (win == SIZE_MAX) continue;
                const rt::GeoPoly &w = polys[order[win]];
                if (w.texheader[0] != 0x00c0 || w.texheader[3] != 0) continue;
                shadow_px++;
                const float wz = pp[win].a * X + pp[win].b * Y + pp[win].c;
                for (size_t r = win + 1; r < order.size(); r++) {
                    const rt::GeoPoly &p = polys[order[r]];
                    if (!pp[r].ok || p.window != w.window || !((p.texheader[0] >> 14) & 1) || !inside(pp[r], X, Y)) continue;
                    if (pp[r].a * X + pp[r].b * Y + pp[r].c > wz * 1.02f) { over++, mask[size_t(y) * 496 + size_t(x)] = 255; break; }
                }
            }
        std::printf("shadow pixels %d; of them over a nearer textured polygon %d\n", shadow_px, over);
        if (FILE *f = std::fopen(path, "wb")) {
            std::fprintf(f, "P5\n496 384\n255\n");
            std::fwrite(mask.data(), 1, mask.size(), f);
            std::fclose(f);
        }
        return 0;
    }
    if (const char *pre = std::getenv("M2_POLYAT_SKYTEX")) {   // PREFIX: texture RAM (PREFIX.tex) and the sky's polygons
        const rt::VideoMem &m = video.gpu_mem();                    // with their texture state (PREFIX.txt)
        if (FILE *f = std::fopen((std::string(pre) + ".tex").c_str(), "wb")) {
            std::fwrite(m.tex0, 4, 0x80000, f), std::fwrite(m.tex1, 4, 0x80000, f);
            std::fclose(f);
        }
        if (FILE *f = std::fopen((std::string(pre) + ".txt").c_str(), "w")) {
            std::map<std::tuple<int, int, float, float, int>, int> cf;
            for (const rt::GeoPoly &p : polys) cf[{p.center[0], p.center[1], p.focus[0], p.focus[1], int(p.window)}]++;
            for (const auto &[k, n] : cf)
                std::fprintf(f, "# window %d centre %d %d focus %g %g: %d polygons\n", std::get<4>(k), std::get<0>(k), std::get<1>(k),
                             double(std::get<2>(k)), double(std::get<3>(k)), n);
            for (const rt::GeoPoly &p : polys) {
                if (p.window != 1) continue;
                std::fprintf(f, "%04x %04x %04x %04x %d %d %d", p.texheader[0], p.texheader[1], p.texheader[2], p.texheader[3],
                             int(p.texlod), int(p.luma), p.num_vertices);
                std::fprintf(f, " C %d %d F %g %g R", p.center[0], p.center[1], double(p.focus[0]), double(p.focus[1]));
                for (int i = 0; i < p.num_vertices; i++) std::fprintf(f, " %g %g", double(p.v[i].x), double(p.v[i].y));
                for (int i = 0; i < p.num_vertices; i++)
                    std::fprintf(f, " %g %g %g %g %g", double(p.v[i].x / p.focus[0]), double(p.v[i].y / p.focus[1]),
                                 double(p.v[i].p[0]), double(p.v[i].p[1] * 32.0f), double(p.v[i].p[2] * 32.0f));
                std::fprintf(f, "\n");
            }
            std::fclose(f);
        }
        return 0;
    }
    if (const char *path = std::getenv("M2_POLYAT_DUMP")) {   // every polygon in camera space (x right, y up, z ahead)
        if (FILE *f = std::fopen(path, "w")) {
            for (size_t rank = 0; rank < order.size(); rank++) {
                const rt::GeoPoly &p = polys[order[rank]];
                std::fprintf(f, "%zu %d %04x %04x %d", rank, p.window, p.texheader[0], p.texheader[2], p.num_vertices);
                for (int i = 0; i < p.num_vertices; i++) {
                    const float z = p.v[i].p[0];
                    std::fprintf(f, " %g %g %g", double(p.v[i].x / p.focus[0]), double(p.v[i].y / p.focus[1]), double(z));
                }
                std::fprintf(f, " sortz %g\n", double(p.sortz));   // (the z it was sorted by: arcade.cpp's key)
            }
            std::fclose(f);
        }
        return 0;
    }
    if (std::getenv("M2_POLYAT_KINDS")) {   // the frame's polygons by renderer (texheader[0] bits 13-14) and checker
        int count[4][2] = {};
        for (const rt::GeoPoly &p : polys) count[(p.texheader[0] >> 13) & 3][(p.texheader[0] >> 15) & 1]++;
        for (int r = 0; r < 4; r++)
            std::printf("  renderer %d (%s%s): %d, %d of them checkered\n", r, r & 2 ? "textured" : "solid",
                        r & 1 ? ", translucent" : "", count[r][0] + count[r][1], count[r][1]);
        const int want = std::atoi(std::getenv("M2_POLYAT_KINDS"));   // that renderer's polygons, their boxes
        std::vector<uint8_t> mask(496 * 384, 0);   // M2_POLYAT_MASK=file.pgm: where its checkered ones are
        for (size_t rank = 0; rank < order.size(); rank++) {
            const rt::GeoPoly &p = polys[order[rank]];
            if (((p.texheader[0] >> 13) & 3) != want) continue;
            if ((p.texheader[0] >> 15) & 1) {
                float px[8], py[8];
                bool front = p.num_vertices >= 3;
                for (int i = 0; i < p.num_vertices; i++) {
                    const float z = p.v[i].p[0];
                    if (z <= 0) front = false;
                    else px[i] = float(video.crtc_x() + p.center[0]) + p.v[i].x / z, py[i] = float((384 - p.center[1]) + video.crtc_y()) - p.v[i].y / z;
                }
                for (int y = 0; front && y < 384; y++)
                    for (int x = 0; x < 496; x++) {
                        int pos = 0, neg = 0;
                        for (int i = 0; i < p.num_vertices; i++) {
                            const int j = (i + 1) % p.num_vertices;
                            const float c = (px[j] - px[i]) * (y + .5f - py[i]) - (py[j] - py[i]) * (x + .5f - px[i]);
                            if (c > 0) pos++; else if (c < 0) neg++;
                        }
                        if (!(pos && neg)) mask[size_t(y) * 496 + size_t(x)] = 255;
                    }
            }
            float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, zmin = 1e30f;
            for (int i = 0; i < p.num_vertices; i++) {
                const float z = p.v[i].p[0];
                zmin = std::min(zmin, z);
                if (z <= 0) continue;
                const float x = float(video.crtc_x() + p.center[0]) + p.v[i].x / z;
                const float y = float((384 - p.center[1]) + video.crtc_y()) - p.v[i].y / z;
                x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
            }
            std::printf("  rank %5zu box x %4.0f..%4.0f y %4.0f..%4.0f z>=%8.3f window %d luma %3d texheader %04x %04x %04x %04x\n",
                        rank, double(x0), double(x1), double(y0), double(y1), double(zmin), p.window, p.luma,
                        p.texheader[0], p.texheader[1], p.texheader[2], p.texheader[3]);
        }
        if (const char *path = std::getenv("M2_POLYAT_MASK"))
            if (FILE *f = std::fopen(path, "wb")) {
                std::fprintf(f, "P5\n496 384\n255\n");
                std::fwrite(mask.data(), 1, mask.size(), f);
                std::fclose(f);
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

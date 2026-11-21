// m2figure (diagnostics): the models the game draws at a frame, read back from the model ROM in their own space
// (rt::Geo::model): every object's address, window, matrix and polygons, as text. For finding a figure's parts (the
// agents of the attract's intro: src/core/avatar.cpp) and how they hang together.
//
//   m2figure IMAGES_DIR FRAME OUT.txt [--inputs SCRIPT] [--window N] [--tex OUT.bin]
//
// --tex: texture RAM at that frame (both sheets, 0x80000 dwords each), raw.
//
// OUT.txt: "obj INDEX ADDRESS WINDOW TPA THA COUNT MODE m0..m11", then its polygons, "p N TWO DIFFUSE AMBIENT
// TEXHEADER0..3 nx ny nz" and N times "x y z u v". It is the game's data: keep it out of the repository.
#include "runtime/board/game_loop.h"
#include "../common/input_script.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: m2figure IMAGES_DIR FRAME OUT.txt [--inputs SCRIPT] [--window N]\n");
        return 2;
    }
    const uint64_t frames = std::strtoull(argv[2], nullptr, 10);
    tools::Script script;
    int window = -1;
    std::string tex_path;
    for (int i = 4; i + 1 < argc; i++) {
        if (!std::strcmp(argv[i], "--inputs")) script.load(argv[++i]);
        else if (!std::strcmp(argv[i], "--window")) window = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--tex")) tex_path = argv[++i];
    }
    try {
        rt::GameLoop game(argv[1], false);
        game.board().set_vr(true);
        game.board().video().set_external_3d(true, false);
        while (game.board().frame() < frames) {
            script.pokes(game.board().frame(), game.board());
            game.run_frame(script.at(game.board().frame()));
        }
        FILE *f = std::fopen(argv[3], "w");
        if (!f) return 1;
        rt::Geo &geo = game.board().geo();
        int index = 0;
        for (const rt::Geo::Object &o : geo.objects) {
            const int at = index++;
            if (window >= 0 && o.window != window) continue;
            std::fprintf(f, "obj %d %08x %d %08x %08x %u %u", at, o.address, o.window, o.tpa, o.tha, o.count, o.mode);
            for (float m : o.m) std::fprintf(f, " %.9g", double(m));
            std::fprintf(f, "\n");
            std::vector<rt::Geo::ModelPoly> polys;
            if (!geo.model(o, polys)) { std::fprintf(f, "bad\n"); continue; }
            for (const rt::Geo::ModelPoly &p : polys) {
                std::fprintf(f, "p %d %d %g %g %04x %04x %04x %04x %.6g %.6g %.6g", p.n, int(p.two_sided), double(p.diffuse),
                             double(p.ambient), p.texheader[0], p.texheader[1], p.texheader[2], p.texheader[3],
                             double(p.normal[0]), double(p.normal[1]), double(p.normal[2]));
                for (int k = 0; k < p.n; k++)
                    std::fprintf(f, "  %.7g %.7g %.7g %g %g", double(p.v[k][0]), double(p.v[k][1]), double(p.v[k][2]),
                                 double(p.uv[k][0]), double(p.uv[k][1]));
                std::fprintf(f, "\n");
            }
        }
        std::fclose(f);
        if (!tex_path.empty()) {
            const rt::VideoMem &mem = game.board().video().gpu_mem();
            if (FILE *t = std::fopen(tex_path.c_str(), "wb")) {
                std::fwrite(mem.tex0, 4, 0x80000, t), std::fwrite(mem.tex1, 4, 0x80000, t);
                std::fclose(t);
            }
        }
        std::printf("m2figure: frame %llu, %d objects\n", (unsigned long long)game.board().frame(), index);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "m2figure: %s\n", e.what());
        return 1;
    }
    return 0;
}

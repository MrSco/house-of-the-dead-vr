// m2statetest: save states (GameLoop::save_state / load_state) checked. One game runs AT frames from an input
// script and saves; a second game, fresh and run a few frames of its own, loads that state; then both run RUN frames
// more with the same input, and every frame's state checksum, screen and sound must be the same, and so must both
// games' states at the end. With --save FILE the state is written; with --load FILE a state saved elsewhere (another
// platform: the bytes are the same everywhere) is loaded instead, and its checksums compared with --expect FILE
// (a --save run's per-frame list).
//
//   m2statetest IMAGES_DIR --inputs scripts/inputs/hotd_2p.txt --at 3000 --run 1200 [--save FILE]
//   m2statetest IMAGES_DIR --inputs ... --at 3000 --run 1200 --load FILE --expect FILE.crc
//
// The VR scenery hook is on, as in every app (M2_VR=0: off).
#include "runtime/board/game_loop.h"
#include "runtime/board/state.h"
#include "../common/input_script.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {
double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}
std::unique_ptr<rt::GameLoop> make(const std::string &dir) {
    auto g = std::make_unique<rt::GameLoop>(dir);
    if (!std::getenv("M2_VR") || std::atoi(std::getenv("M2_VR")) != 0) g->board().set_vr(true);
    return g;
}
std::vector<float> pcm(rt::GameLoop &g) { return g.sound() ? g.sound()->take_pcm() : std::vector<float>{}; }
} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: m2statetest IMAGES_DIR --inputs SCRIPT --at N --run M [--save FILE | --load FILE --expect FILE]\n");
        return 2;
    }
    const std::string dir = argv[1];
    std::string inputs, save, load, expect;
    uint64_t at = 3000, run = 1200;
    for (int i = 2; i + 1 < argc; i++) {
        if (!std::strcmp(argv[i], "--inputs")) inputs = argv[++i];
        else if (!std::strcmp(argv[i], "--at")) at = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--run")) run = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--save")) save = argv[++i];
        else if (!std::strcmp(argv[i], "--load")) load = argv[++i];
        else if (!std::strcmp(argv[i], "--expect")) expect = argv[++i];
    }
    tools::Script script;
    if (!inputs.empty()) script.load(inputs);
    try {
        std::vector<uint8_t> state;
        std::unique_ptr<rt::GameLoop> a;
        if (load.empty()) {
            a = make(dir);
            for (uint64_t f = 0; f < at; f++) script.pokes(f, a->board()), a->run_frame(script.at(f)), pcm(*a);
            const auto t = std::chrono::steady_clock::now();
            a->save_state(state);
            std::printf("m2statetest: saved at frame %llu: %zu bytes in %.1f ms\n", (unsigned long long)at, state.size(), ms_since(t));
            {   // packed to travel (online play)
                auto t0 = std::chrono::steady_clock::now();
                const std::vector<uint8_t> packed = rt::pack_state(state, a->state_dicts());
                const double pack_ms = ms_since(t0);
                t0 = std::chrono::steady_clock::now();
                const std::vector<uint8_t> back = rt::unpack_state(packed.data(), packed.size(), a->state_dicts());
                std::printf("m2statetest: packed to %zu bytes in %.1f ms, unpacked in %.1f ms%s\n", packed.size(), pack_ms,
                            ms_since(t0), back == state ? "" : ": UNPACKED WRONG");
                if (back != state) return 1;
            }
            if (!save.empty()) {
                FILE *o = std::fopen(save.c_str(), "wb");
                if (!o) throw std::runtime_error("cannot write " + save);
                std::fwrite(state.data(), 1, state.size(), o);
                std::fclose(o);
            }
        } else {
            FILE *f = std::fopen(load.c_str(), "rb");
            if (!f) throw std::runtime_error("cannot open " + load);
            uint8_t buf[65536];
            for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) state.insert(state.end(), buf, buf + n);
            std::fclose(f);
        }
        // the loader: a game of its own first (a few frames in, its own input), then the state
        auto b = make(dir);
        for (uint64_t f = 0; f < 90; f++) b->run_frame(rt::Inputs{}), pcm(*b);
        {
            const auto t = std::chrono::steady_clock::now();
            b->load_state(state.data(), state.size());
            std::printf("m2statetest: loaded in %.1f ms (frame %llu)\n", ms_since(t), (unsigned long long)b->frames());
        }
        // a second save of what was loaded: the same bytes
        {
            std::vector<uint8_t> again;
            b->save_state(again);
            if (again != state) {
                size_t i = 0;
                while (i < again.size() && i < state.size() && again[i] == state[i]) i++;
                std::printf("m2statetest: FAIL: saved again after loading, the state differs from byte %zu (sizes %zu, %zu)\n", i, state.size(), again.size());
                return 1;
            }
        }
        std::vector<uint32_t> want;
        if (!expect.empty()) {
            FILE *f = std::fopen(expect.c_str(), "rb");
            if (!f) throw std::runtime_error("cannot open " + expect);
            uint32_t v;
            while (std::fread(&v, 4, 1, f) == 1) want.push_back(v);
            std::fclose(f);
        }
        FILE *crcs = save.empty() ? nullptr : std::fopen((save + ".crc").c_str(), "wb");
        const uint64_t from = b->frames();
        uint64_t screens_same = 0, sound_same = 0;
        for (uint64_t k = 0; k < run; k++) {
            const uint64_t f = from + k;
            const rt::Inputs in = script.at(f);
            if (a) script.pokes(f, a->board()), a->run_frame(in);
            script.pokes(f, b->board());
            b->run_frame(in);
            const uint32_t hb = b->board().state_hash();
            if (crcs) std::fwrite(&hb, 4, 1, crcs);
            if (a) {
                const uint32_t ha = a->board().state_hash();
                if (ha != hb) {
                    std::printf("m2statetest: FAIL: frame %llu: checksum %08x, loaded %08x\n", (unsigned long long)f, ha, hb);
                    return 1;
                }
                if (a->screen() == b->screen()) screens_same++;
                else if (screens_same + 1 == k + 1 || k < 3)
                    std::printf("m2statetest: frame %llu: the screens differ\n", (unsigned long long)f);
                if (pcm(*a) == pcm(*b)) sound_same++;
            } else if (k < want.size() && want[k] != hb) {
                std::printf("m2statetest: FAIL: frame %llu: checksum %08x, expected %08x\n", (unsigned long long)f, hb, want[k]);
                return 1;
            }
        }
        if (crcs) std::fclose(crcs);
        if (a) {
            std::vector<uint8_t> sa, sb;
            a->save_state(sa);
            b->save_state(sb);
            std::printf("m2statetest: %llu frames after the load: checksums the same, screens the same %llu, sound the same %llu, states at the end %s\n",
                        (unsigned long long)run, (unsigned long long)screens_same, (unsigned long long)sound_same,
                        sa == sb ? "the same" : "DIFFER");
            if (sa != sb || screens_same != run || sound_same != run) return 1;
        } else {
            std::printf("m2statetest: %llu frames after the load, %zu checksums compared: all the same\n",
                        (unsigned long long)run, want.size() < run ? want.size() : size_t(run));
        }
        return 0;
    } catch (const std::exception &e) {
        std::printf("m2statetest: FAIL: %s\n", e.what());
        return 1;
    }
}

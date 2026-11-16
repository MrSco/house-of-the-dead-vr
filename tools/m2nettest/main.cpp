// m2nettest: the arcade game's online play (src/core/arcade_net.cpp) without a network: one run hosts, with
// player 1 and player 2 from an input script (player 2 coming in as a guest's gun does), and records every bundle;
// another run plays those bundles as the guest and compares the state checksums (one every 60 frames) the host put
// in them. Two runs of one machine, or one on the PC and the other on the Quest (both compute the same game).
//
//   m2nettest IMAGES_DIR FRAMES --host BUNDLES --inputs scripts/inputs/hotd_2p.txt
//   m2nettest IMAGES_DIR FRAMES --guest BUNDLES [--shots DIR]
//
// Both runs have the VR scenery hook on, as the apps do (M2_VR=0: off, matching only other runs without it).
//
// The script's coin and start become player 1's skip button (coins and Start, arcade_net's), start2 player 2's;
// gun2x / gun2y / trigger2 player 2's gun.
#include "core/arcade_net.h"
#include "runtime/board/game_loop.h"
#include "../common/input_script.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" void host_log(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    std::vfprintf(stdout, fmt, a);
    va_end(a);
}
extern "C" {
void net_configure(int mode, int slot);
void net_host_join(int slot, const char *name, const char *data_hex);
void net_host_input(int slot, int flags, float wx, float wy, int chr, int key);
}

namespace {
rt::GameLoop *game_ptr;
uint32_t state_crc() { return game_ptr->board().state_hash(); }
float to_px(uint16_t v, uint16_t lo, uint16_t hi, float size) { return float(v - lo) * size / float(hi - lo); }
} // namespace

int main(int argc, char **argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: m2nettest IMAGES_DIR FRAMES --host BUNDLES --inputs SCRIPT | --guest BUNDLES\n");
        return 2;
    }
    const std::string dir = argv[1];
    const uint64_t frames = std::strtoull(argv[2], nullptr, 10);
    std::string host, guest, inputs, shots;
    for (int i = 3; i + 1 < argc; i++) {
        if (!std::strcmp(argv[i], "--host")) host = argv[++i];
        else if (!std::strcmp(argv[i], "--guest")) guest = argv[++i];
        else if (!std::strcmp(argv[i], "--inputs")) inputs = argv[++i];
        else if (!std::strcmp(argv[i], "--shots")) shots = argv[++i]; // a raw frame every 300 (scripts/rgb2png.py)
    }
    tools::Script script;
    if (!inputs.empty()) script.load(inputs);
    if (!guest.empty() && !arcade_net::replay_from(guest)) {
        std::fprintf(stderr, "m2nettest: cannot read %s\n", guest.c_str());
        return 1;
    }
    if (!host.empty()) {
        arcade_net::record_to(host);
        net_configure(1, 0);
        net_host_join(1, "script", "");
    }
    rt::GameLoop game(dir);
    game_ptr = &game;
    // with all the scenery sent (hook_hotd_scenery_cull), as every app runs (docs/platforms.md): it moves the wait
    // loop's spare-time counters, so the checksums only match runs that have it too. M2_VR=0 turns it off.
    if (!std::getenv("M2_VR") || std::atoi(std::getenv("M2_VR")) != 0) game.board().set_vr(true);
    const rt::GunRange &gun = rt::game().gun;
    rt::Inputs prev;
    uint64_t f = 0;
    for (; f < frames; f++) {
        const rt::Inputs s = script.at(f);
        arcade_net::Local local;
        if (!host.empty()) {
            local.fire = !(s.in1 & 0x01);
            local.on_screen = true;
            local.x = int(to_px(s.gun_x, gun.x_min, gun.x_max, 496.f));
            local.y = int(to_px(s.gun_y, gun.y_min, gun.y_max, 384.f));
            local.on_screen = local.x > 0 && local.x < 496 && local.y > 0 && local.y < 384;
            local.skip = !(s.in0 & 0x10) && (prev.in0 & 0x10); // start: player 1's skip button
            const bool fire2 = !(s.in1 & 0x02), skip2 = !(s.in0 & 0x20) && (prev.in0 & 0x20);
            const float x2 = s.gun2_x ? to_px(s.gun2_x, gun.x_min, gun.x_max, 496.f) : -1.f;
            const float y2 = s.gun2_x ? to_px(s.gun2_y, gun.y_min, gun.y_max, 384.f) : -1.f;
            const bool on2 = x2 > 0 && x2 < 496 && y2 > 0 && y2 < 384;
            net_host_input(1, (fire2 ? 1 : 0) | (on2 ? 4 : 0), x2, y2, 0, skip2 ? 0x39 : 0);
        }
        prev = s;
        rt::Inputs in;
        if (!arcade_net::frame(local, in, state_crc)) break;
        game.run_frame(in);
        if (!shots.empty() && (f + 1) % 300 == 0) {
            char path[512];
            std::snprintf(path, sizeof path, "%s/net_%05llu.rgb", shots.c_str(), (unsigned long long)(f + 1));
            if (FILE *o = std::fopen(path, "wb")) {
                std::fwrite(game.screen().data(), 4, game.screen().size(), o);
                std::fclose(o);
            }
        }
    }
    std::printf("m2nettest: %s, %llu frames, %u checksums matched%s\n", host.empty() ? "guest" : "host",
                (unsigned long long)f, arcade_net::checks_ok(), arcade_net::desynced() ? ", DESYNC" : "");
    return arcade_net::desynced() ? 1 : 0;
}

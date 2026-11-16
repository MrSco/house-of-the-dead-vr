// m2netguest: a desktop player 2 for the arcade game's online play, over the real network: it joins a Quest (or
// anything running the arcade app) hosting on the local network, through main's src/net (online.c, the NMN2 client
// and the built-in host's protocol) and arcade_net's lockstep, as the Quest app does. Headless; its gun presses Start
// once it has caught up (--start: that many frames after) and then fires a shot a second at the centre, so the host sees player 2 come in and play.
//
//   m2netguest IMAGES_DIR HOST_ADDRESS FRAMES [--shots DIR] [--start FRAME]
//
// It prints the checksums matched against the host's (arcade_net) and saves a frame every 300 to --shots (raw
// 0xAARRGGBB, 496 x 384). A files folder of its own (online.cfg and the like) is made in the current directory.
#include "core/arcade_net.h"
#include "runtime/board/game_loop.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
int online_boot(const char *files_dir);
void online_start(void);
void online_quit(void);
void host_log(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    std::vfprintf(stdout, fmt, a);
    va_end(a);
    std::fflush(stdout);
}
}

namespace {
rt::GameLoop *game_ptr;
uint32_t state_crc() { return game_ptr->board().state_hash(); }
} // namespace

int main(int argc, char **argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: m2netguest IMAGES_DIR HOST_ADDRESS FRAMES [--shots DIR] [--start FRAME]\n");
        return 2;
    }
    const std::string dir = argv[1], host = argv[2];
    const uint64_t frames = std::strtoull(argv[3], nullptr, 10);
    std::string shots;
    uint64_t start_at = 600;
    for (int i = 4; i + 1 < argc; i++) {
        if (!std::strcmp(argv[i], "--shots")) shots = argv[++i];
        else if (!std::strcmp(argv[i], "--start")) start_at = std::strtoull(argv[++i], nullptr, 10);
    }
    const std::string env = "PORT_ONLINE=join-ip:" + host;
#ifdef _WIN32
    _putenv(env.c_str());
#else
    putenv(const_cast<char *>(env.c_str()));
#endif
    if (!online_boot("m2netguest_files")) {
        std::fprintf(stderr, "m2netguest: online play did not start\n");
        return 1;
    }
    online_start();
    rt::GameLoop game(dir);
    game.board().set_vr(true); // as the headset: all the scenery sent (hook_hotd_scenery_cull), or the checksums differ
    game_ptr = &game;
    uint64_t f = 0, live = 0; // live: frames played since this guest caught up with the host
    for (; f < frames; f++) {
        if (arcade_net::catching_up() == 0) live++;
        arcade_net::Local local;
        local.x = 248, local.y = 192, local.on_screen = true;
        local.skip = live == start_at;                       // player 2's coins and Start
        local.fire = live > start_at + 300 && (f % 60) < 4;  // a shot a second
        rt::Inputs in;
        if (!arcade_net::frame(local, in, state_crc)) break;
        game.run_frame(in);
        game.sound() ? (void)game.sound()->take_pcm() : (void)0;
        if (!shots.empty() && arcade_net::frames() % 300 == 0) {
            char path[512];
            std::snprintf(path, sizeof path, "%s/guest_%05u.rgb", shots.c_str(), arcade_net::frames());
            if (FILE *o = std::fopen(path, "wb")) {
                std::fwrite(game.screen().data(), 4, game.screen().size(), o);
                std::fclose(o);
            }
        }
    }
    std::printf("m2netguest: %llu frames, %u checksums matched%s\n", (unsigned long long)f, arcade_net::checks_ok(),
                arcade_net::desynced() ? ", DESYNC" : "");
    online_quit();
    return arcade_net::desynced() ? 1 : 0;
}

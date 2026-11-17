// Online play for the arcade game (the Quest app's online.c, two players as at the cabinet): a lockstep. Every
// player runs the whole game; only input travels. The host (player 1) makes each frame's cabinet input (both guns,
// both triggers, the coin and Start switches) from its own controls and the guest's latest gun, sends it out as that
// frame's bundle and runs the frame with it; the guest (player 2) runs frame N with bundle N. Every 60th bundle
// carries a checksum of the game's state, which the guest compares. The recompiled game computes the same frames on
// every machine (the same instructions: docs/vr.md), from the same start (fresh settings: online play starts
// each game with an empty save folder).
//
// The bundle has the layout of main's netplay.c (src/netplay.h: frame, checksum, a 64-byte input block, three
// guests' inputs, events), so online.c's transport, history and late joining carry it unchanged; the input block
// holds the cabinet's input (Packed below).
//
// A guest joins with a save state of the host's game (runtime/board/state.h), not by running every frame since its
// start: the host saves one after the frame it is at when the guest comes (host_save), packs it and hands it to
// online.c (net_snapshot_out), which sends it; the guest's online.c passes it in (net_snapshot_in), the guest loads
// it (guest_load) and runs on from the frame after it with the host's bundles.
//
// The C side (online.c) calls the net_* functions netplay.h declares; the game thread calls arcade_net::frame.
#pragma once

#include "games/game.h"

namespace rt { class GameLoop; }

#include <cstdint>
#include <string>

namespace arcade_net {

enum Mode { Off, Host, Guest };

// This player's controls this frame (the gun in the arcade's screen pixels, 496 x 384).
struct Local {
    bool fire = false, reload = false; // the trigger; reload: a shot off the screen
    bool on_screen = false;            // aiming at the picture
    int x = 0, y = 0;
    bool skip = false;                 // the skip button went down this frame: coins and Start for this player
};

Mode mode();
int local_slot();

// The game thread, once per frame: this frame's cabinet input. Off and Host: made here (Host: and sent). Guest: the
// host's for this frame, waiting for it; false when none came (quitting) or the host's save state came (guest_load
// it): don't run the frame. `state_crc` gives
// the game's state checksum (the frames whose bundle carries one).
bool frame(const Local &local, rt::Inputs &out, uint32_t (*state_crc)());
uint32_t frames();     // frames made so far (the next bundle's number - 1)
int catching_up();     // guest: 1 bundles are waiting (don't pace), 2 many (don't draw either)
bool desynced();

// The game thread, between frames, with the sound board idle (no packet running, none pending):
// host: a guest wants the game as it is: saved now (the frame just run), packed on a thread of its own, sent.
bool snapshot_wanted();
void host_save(rt::GameLoop &game);
// guest: the host's save state came (load it before the next frame); guest_load loads it (true), throwing if it
// doesn't load (State::Bad: another build's, a damaged one), when the game is no longer usable.
bool snapshot_waiting();
bool guest_load(rt::GameLoop &game);

// Tests (tools/m2nettest): record every bundle made (host), or play recorded bundles as the guest.
void record_to(const std::string &path);
bool replay_from(const std::string &path);
uint32_t checks_ok();

} // namespace arcade_net

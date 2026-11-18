// The House of the Dead, Revision A (hotd; Model 2C-CRX, MAME's model2c
// machine with its hotd light gun wiring: the 2B-CRX's texture and luma RAM,
// a Fujitsu MB86235 TGPx4 as the copro). Everything the runtime needs to know
// about this game; the board is the runtime's. Seeds: seeds/hotd.txt.
// Bring-up (2026-11-15, the house-of-the-dead-recomp-vr arcade-recomp branch):
// the i960 recompiled, the TGPx4 run by MAME's interpreter (runtime/tgpx4).
#include "games/game.h"
#include "games/hooks.h"
#include "runtime/board/enhance.h"

#include <cstdlib>

namespace rt {

// --- Hooks (seeds/hotd_hooks.txt) ---------------------------------------------

// Settings labels (maincpu 0xa9130): COUNTRY JAPAN / USA / EXPORT; BLOOD COLOR
// RED / GREEN / BLUE / PURPLE. The block is the EEPROM's image (eeprom_read_all
// 0x22950); eeprom_save checksums it when the game writes it back.
void hook_hotd_settings(Cpu &c) {
    auto setting = [](const char *env, int def, int max) {
        const char *e = std::getenv(env);
        const int v = e && *e ? std::atoi(e) : def;
        return v >= 0 && v <= max ? v : def;
    };
    c.bus->write_byte(0x0051f729, uint8_t(setting("HOTD_COUNTRY", 1, 2)));
    c.bus->write_byte(0x0051f72d, uint8_t(setting("HOTD_BLOOD", 0, 3)));
}

// The scenery's view test (hooks.h): g4 (r[16 + 4]) is the TGPx4's answer, "visible" when above 0.
void hook_hotd_scenery_cull(Cpu &c) {
    if (Enhance::all_round) c.m_r[16 + 4] = 0x3f800000u; // 1.0f
}

namespace {

using namespace rom_regions;

// MAME model2.cpp ROM_START(hotd) at mame0289: the i960 program (2 MB, runs
// from ROM), main_data (its last pair mirrored every 1 MB from 0x1800000 to
// 0x1f00000: ROM_COPY), the TGPx4's data ROM, the polygons and textures, and
// the SCSP sound board's 68000 program and samples.
const RomLoad kHotdRoms[] = {
    {"epr-19696a.15", 0x42ADC32E, Program, 0x000000, 0x080000},
    {"epr-19697a.16", 0x1E247CD5, Program, 0x000002, 0x080000},
    {"epr-19694.13", 0xE85CA1A3, Program, 0x100000, 0x080000},
    {"epr-19695.14", 0xCD52B461, Program, 0x100002, 0x080000},
    {"mpr-19704.11", 0xAA80DBB0, MainData, 0x0000000, 0x400000},
    {"mpr-19705.12", 0xF906843B, MainData, 0x0000002, 0x400000},
    {"mpr-19702.9", 0xFC8AA3B7, MainData, 0x0800000, 0x400000},
    {"mpr-19703.10", 0x208D993D, MainData, 0x0800002, 0x400000},
    {"mpr-19700.7", 0x0558CFD3, MainData, 0x1000000, 0x400000},
    {"mpr-19701.8", 0x224A8929, MainData, 0x1000002, 0x400000},
    {"epr-19698.5", 0xE7A7B6EA, MainData, 0x1800000, 0x080000},
    {"epr-19699.6", 0x8160B3D9, MainData, 0x1800002, 0x080000},
    {"epr-19707.29", 0x384FD133, CoproData, 0x000000, 0x080000},
    {"epr-19706.30", 0x1277531C, CoproData, 0x000002, 0x080000},
    {"mpr-19715.17", 0x3FF7DDA7, Polygons, 0x0000000, 0x400000},
    {"mpr-19711.21", 0x080D13F1, Polygons, 0x0000002, 0x400000},
    {"mpr-19714.18", 0x3E55AB49, Polygons, 0x0800000, 0x400000},
    {"mpr-19710.22", 0x80DF1036, Polygons, 0x0800002, 0x400000},
    {"mpr-19713.19", 0x4D092CD3, Polygons, 0x1000000, 0x400000},
    {"mpr-19709.23", 0xD08937BF, Polygons, 0x1000002, 0x400000},
    {"mpr-19712.20", 0x41577943, Polygons, 0x1800000, 0x400000},
    {"mpr-19708.24", 0x5CB790F2, Polygons, 0x1800002, 0x400000},
    {"mpr-19718.27", 0xA9DE5924, Textures, 0x0000000, 0x400000},
    {"mpr-19716.25", 0x45C7DCCE, Textures, 0x0000002, 0x400000},
    {"mpr-19719.28", 0x838F8343, Textures, 0x0800000, 0x400000},
    {"mpr-19717.26", 0x393E440B, Textures, 0x0800002, 0x400000},
    // the sound board: its 68000 program and the SCSP's samples
    {"epr-19720.31", 0xB367D21D, SoundProgram, 0x000000, 0x080000},
    {"mpr-19721.32", 0xF5D8FA9A, Samples, 0x000000, 0x400000},
    {"mpr-19722.34", 0xA56FA539, Samples, 0x400000, 0x400000},
};

// The cabinet's wiring: MAME's INPUT_PORTS(hotd) is vcop2's with its own gun
// ranges: IN0 coin 0x01, coin 2 0x02, Test 0x04, Service 0x08, Start 0x10,
// player 2's Start 0x20; IN1 player 1's trigger 0x01, player 2's 0x02. The
// guns go through the 315-5649's serial channel 2 (IoBoard, Game::gun, Inputs
// gun2_x / gun2_y for player 2), as Virtua Cop 2's.
uint8_t port(unsigned p, const Inputs &in) {
    switch (p) {
    case 1: return uint8_t((in.in0 & 0x3f) | 0xc0);
    case 2: return uint8_t(0xfc | (in.in1 & 0x03));
    default: return 0xff;
    }
}

// One player with the light gun: the trigger, start and coin.
constexpr ControlDef kControls[] = {
    {Control::Trigger, "TRIGGER", PadButton::Cross},
    {Control::Start, "START", PadButton::Start},
    {Control::Coin, "COIN", PadButton::Select},
};
} // namespace

extern const Game hotd_game = {
    .set = "hotd", .title = "The House of the Dead", .label = "Revision A",
    .board = BoardType::Model2c,
    .roms = kHotdRoms, .mirror_from = 0, .tgp = {false, 0, 0, 0},
    .mirrors = {{0x1800000, 0x1f00000}, {0, 0}},
    // The wait for the next frame (0x18c4, called once a frame; found with M2_IP_HIST, 81% of the i960's time
    // while every frame ran to the cap): "ld 0x98000c,g4; addo g0,1,g0; and g4,4,g4; cmpibe g5,g4,0x18d4"
    // loops while the frame bit of the 0x98000c status word is unchanged, counting passes in g0: Virtua Cop's.
    .idle = {{0x18d4, 0x18e7}, {0, 0}},
    .sound = SoundBoardType::Scsp, .native_sound = nullptr,
    .io = {port, nullptr}, .link = false,
    // +14.8 dB: the attract's peaks at -1.2 dBFS (2.5 minutes of attract on the Quest peak -16.2 dBFS at the board;
    // its mean -43 dBFS was too quiet to hear at a middling headset volume)
    .output_gain = 5.5f,
    .control_list = kControls,
    // MAME hotd P1_X/P1_Y PORT_MINMAX: the screen's edges in the gun's units;
    // player 2: MAME's P2_X/P2_Y defaults.
    // (live: the game reloads at x 192 and below, 575 and up, y 100 and below, 370 and up; between, measured with
    // m2probe --watch on the bullets (0x51f2c1), x 196..573 and y 102..362 shoot: the box a step inside those)
    .gun = {173, 596, 87, 380, 0x17b, 0x0e9, 197, 572, 103, 361},
};

} // namespace rt

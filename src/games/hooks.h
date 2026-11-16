// Every game's hooks into its recompiled code. seeds/<set>_hooks.txt lists
// "ADDRESS name" lines (m2recomp --hooks); just before the instruction at a
// hook's address the generated code calls rt::hook_<name>(c). Each hook is
// defined in its game's file in src/games/ and does nothing while the option
// it serves (runtime/board/enhance.h) is off, so the game runs exactly as
// recompiled (rules.md: parity runs that way).
#pragma once

#include "runtime/cpu/cpu.h"

namespace rt {

// Daytona USA (daytona.cpp), 0x17078 (daytona93; 0x17508 in Revision A, the
// same code and RAM), after the game has built its draw list of course
// cells: the count at 0x5016c0 and the cell numbers from 0x5016c1, chosen
// from the 5x5 cells around the car's (r8) in a grid 16 cells wide
// (cell = x + 16 y). Enhance::draw_distance.
void hook_draw_list(Cpu &c);

// The House of the Dead (hotd.cpp), 0xdc20 (apply_settings, Revision A): the
// operator settings the game is about to apply (loaded from the EEPROM into
// 0x51f729: country, difficulty, attract sound, lives, blood colour) are set
// as an operator would in the test menu: country USA and red blood unless
// HOTD_COUNTRY (0 Japan, 1 USA, 2 export) or HOTD_BLOOD (0 red, 1 green,
// 2 blue, 3 purple) say otherwise. A board's EEPROM comes up with Japan.
void hook_hotd_settings(Cpu &c);

// The House of the Dead (hotd.cpp), 0x4b484 (Revision A): the scenery loop
// (0x4b3a4..0x4b63c) asks the TGPx4 (FIFO command 0x3d807b7b: a piece's centre,
// x y z, and its model's bounding radius, +0x18 of 0x2eec390[model]) whether
// the piece's sphere is in the camera's view, and skips the piece when the
// answer (g4, a float) is not above 0. With Enhance::all_round (VR) every
// piece of the area's list is sent: the answer is made 1.0.
void hook_hotd_scenery_cull(Cpu &c);

} // namespace rt

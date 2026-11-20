// The player's hands: "Free FPS-Hand's" by Askasknot (Sketchfab, CC BY 4.0; assets/hands.json), suit sleeves and
// all, converted by tools/vr/hands_import.py. Model space is the controller's aim pose, as for the gun (metres, +Y up,
// -Z forward): at rest each hand holds the gun's grip. The fingers curl on the GPU (24 bones a hand).
#pragma once

enum { QHAND_THUMB, QHAND_INDEX, QHAND_MIDDLE, QHAND_RING, QHAND_PINKY };
int qhand_load(const char *asset);       // 0 if missing or broken (no hands drawn)
// side 0 left, 1 right; curl[QHAND_*] 0 open .. 1 a fist; model: column-major 4x4; tint multiplies the colours
void qhand_draw(const float *view, const float *proj, const float *model, int side, const float curl[5],
                const float *tint);

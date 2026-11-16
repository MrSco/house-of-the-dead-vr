// The cabinet view's arcade cabinet (a model in src/quest/assets: cabinet.tccab, its credit in cabinet.json): drawn
// around the game's screen, which sits where the model's screen is; the guns' cords leave it at its holsters.
// Model space: metres, +Y up, the floor at y = 0, the screen's plane at z = 0, the front towards +Z.
#pragma once

struct AAssetManager;
int qcab_load(struct AAssetManager *am, const char *asset);   // 0 if missing or broken (the plain cabinet then)
// view, projection, model: column-major 4x4
void qcab_draw(const float *view, const float *proj, const float *model);
// the game's picture: centre x, y, z and half width, half height (model space)
void qcab_screen(float out[5]);
// where the guns' cords leave the cabinet: player 1's side (left), player 2's (right) (model space)
void qcab_anchor(int side, float out[3]);
// how far its front (the control panel) stands out from the screen's plane (model space z)
float qcab_front(void);

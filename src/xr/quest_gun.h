// The gun in your hand (immersive and cabinet views): a model in the TCGUN format of the Time Crisis 2 VR project
// (its quest_gun.c, MIT), loaded from the APK's assets. Model space: metres, +Y up, the muzzle towards -Z; the origin
// is the controller's aim pose. The pistol's barrel lies on the aim ray; the cabinet's gun sits as the pistol does in
// the hand, its bigger barrel a little above the ray (qgun_muzzle_y).
#pragma once
#include <math.h>

// Two model slots: 0 your gun (the default), 1 your partner's (online). Every call below works on the slot chosen.
void qgun_use(int slot);
// The gun in the hand: its model scaled by scale (about the aim pose), then moved (metres, gun space), so the hand's
// grip and trigger finger meet the gun's. Drawing and every point below (muzzle, grip, cap, butt) take it.
void qgun_fit(float scale, float dx, float dy, float dz);
int qgun_load(const char *asset);      // 0 if missing or broken (the caller draws boxes)
// view, projection, model: column-major 4x4. recoil, trigger and flash 0..1; tint multiplies the colours.
// whole: the gun as loaded, its grip not cut for a magazine (when no magazine reload is on)
void qgun_draw(const float *view, const float *proj, const float *model, float recoil, float trigger, float flash,
               const float *tint, int whole);
float qgun_muzzle_z(void);                                         // the muzzle's distance ahead of the aim pose (m)
float qgun_muzzle_y(void);                                         // the muzzle's height over the aim ray (m)
int qgun_has_slide(void);                                          // a slide that kicks (else the whole gun kicks)
// The grip's base as a magazine's base plate (a gun without a magazine of its own: the cabinet's): the grip's lowest
// QGUN_CAP_H, cut off the gun (its grip then ends there, open: the magazine well), dark, a yellow band round its top.
// qgun_cap_top: the cut's centre (gun space), which is then the grip's base (qgun_grip_base).
#define QGUN_CAP_H 0.020f
int qgun_cap_top(float out[3]);                                    // 0: no plate (the pistol has its own)
void qgun_draw_cap(const float *view, const float *proj, const float *model);
void qgun_grip_base(float out[3]);                                 // the bottom of the grip, where a magazine goes in (m)
void qgun_butt(float out[3]);                                      // the grip's lowest point (a cord leaves there) (m)

// the slide's fast kick and spring back, from the shot's age
static inline float qgun_recoil(float age_ms) {
    if (age_ms < 0 || age_ms >= 150) return 0;
    if (age_ms < 18) return sinf(age_ms * (1.570796327f / 18.f));
    float t = (age_ms - 18.f) / 132.f;
    return 1 - t * t * (3 - 2 * t);
}
// a short muzzle flash
static inline float qgun_flash(float age_ms) {
    if (age_ms < 0 || age_ms >= 42) return 0;
    float t = 1 - age_ms / 42.f;
    return t * t;
}

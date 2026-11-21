// Your partner's body online (xr_shell.c draw_partner): the game's own agent (m2_vr.h's figures: Rogan for player
// 1, G for player 2), posed from the little a headset knows of its player: where the head is and how it is turned,
// and where each wrist is. The rest is worked out here (inverse kinematics): the chest and hips under the head, the
// arms from the shoulders to the wrists, the legs to feet that stay planted and step after the body.
#pragma once
#include "core/m2_vr.h"

typedef struct {
    float head[16];        // avatar space <- the head (the eyes' midpoint; +y up, -z ahead), column-major, a turn and a move
    int hand[2];           // 0 left, 1 right: this wrist is known (else the arm hangs and the figure's own hand shows)
    float wrist[2][3];
    float stretch;         // an arm too short for its wrist grows to it, by this much at most (1.5; 0 or 1: never)
} AvatarPose;
typedef struct {
    int started;
    float top;             // the neck's height standing (m): it sets the figure's size
    float yaw;             // the body's heading
    float hip[2];          // the hips' place on the floor (x, z; the figure's units and axes)
    struct { float p[2], yaw, from[2], from_yaw, t; int stepping; } foot[2];
} Avatar;

// Avatar space: metres, +y up, the floor at y = 0, right-handed (the player's stand frame: xr_shell.c). dt: seconds
// since the last solve (0: a new pose, nothing smoothed). part[M2_FIGURE_*]: avatar space <- that part's model
// (column-major 4x4, for m2_gl_figure_part after the stand frame's own matrix); draw[]: which to draw (a hand only
// where the pose has no wrist; the waist never: m2_gl_figure_waist draws it from the pelvis's and the chest's).
void avatar_solve(Avatar *a, const AvatarPose *in, float dt, float part[M2_FIGURE_PARTS][16], int draw[M2_FIGURE_PARTS]);

// The figure's own hand on an arm whose wrist is known, turned as the player's hand is rather than as the forearm.
// aim: avatar space <- that hand's aim pose (a controller's: -z ahead, +y up; a turn and a move). gun: the hand
// with the game's pistol (M2_FIGURE_GUN_HAND: the barrel along the aim), else the open hand (M2_FIGURE_HAND_L or
// _R: the fingers ahead, the thumb up, the palm inwards).
// avatar_wrist: where that hand's wrist is (avatar space, metres): the pose's wrist[] for avatar_solve.
// avatar_hand: part is that arm's hand as avatar_solve left it (M2_FIGURE_HAND_L or _R: at the forearm's end);
// it is turned in place. A gun hand's is then drawn as M2_FIGURE_GUN_HAND (a left one's comes out mirrored).
void avatar_wrist(const float *aim, int right, int gun, float out[3]);
// The game's pistol in a gun hand so placed, in the aim pose's space (metres; x = 0): the middle of its grip's base,
// and its muzzle's distance ahead (on the aim's ray)
#define AVATAR_GUN_GRIP_Y (-0.112f)
#define AVATAR_GUN_GRIP_Z 0.072f
#define AVATAR_GUN_GRIP_SLANT 0.234f    // radians: the base rises towards the back (+z) by that
#define AVATAR_GUN_MUZZLE_Z 0.103f
void avatar_hand(float part[16], const float *aim, int right, int gun);

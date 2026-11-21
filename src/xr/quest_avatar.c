// Your partner's body online (quest_avatar.h): the game's agent posed by inverse kinematics.
//
// The skeleton is the game's own, found in its attract intro (tools/m2figure: every part's matrix over 180 frames;
// the child's origin in its parent's space never moves, and both agents have the same joints). Each part is a
// rigid model whose origin is its joint; lengths in the game's units (about 9 cm):
//
//   pelvis                      x down, y forward, z to the figure's left; its origin at the waist
//     flaps (two)               at (1.748, 1.297, 0): the coat's front panels, as the pelvis
//     thigh                     at (3.627, -0.026, +0.726 left / -0.722 right); x down the bone, z the knee's hinge
//       shin                    at (4.527, 0, 0)
//         foot                  at (4.755, 0, 0); x down, y forward (the ankle 0.56 over the floor)
//           toes                at (0.346, 1.374, +0.153 / -0.153)
//     chest                     at (-0.017, 0, 0); x UP, y back, z left
//       coat (Rogan)            the chest's own matrix
//       head                    at (3.800, -0.428, 0); x forward, y left, z down; the eyes about 0.9 ahead and 1.0 up
//       upper arm               at (1.966, -0.157, +2.187 left / -2.195 right); x down the bone, z the elbow's hinge
//         forearm               at (3.093, 0, 0)
//           hand                at (2.629, 0, 0); the fingers along x
//
// A knee bends its shin back (a turn about z by a negative angle), an elbow its forearm forward (positive).
// The game's space is left-handed (x right, y up, z ahead: its camera's), and so is the work here ("figure space":
// the avatar space with z negated, in the game's units); cross() is the usual formula, for which a part's axes obey
// x cross y = z there, as the game's matrices do (determinant +1).
#include "quest_avatar.h"
#include <math.h>
#include <string.h>

typedef struct { float x, y, z; } V;
static V v(float x, float y, float z) { V r = { x, y, z }; return r; }
static V add(V a, V b) { return v(a.x + b.x, a.y + b.y, a.z + b.z); }
static V sub(V a, V b) { return v(a.x - b.x, a.y - b.y, a.z - b.z); }
static V mul(V a, float k) { return v(a.x * k, a.y * k, a.z * k); }
static float dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V cross(V a, V b) { return v(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static float len(V a) { return sqrtf(dot(a, a)); }
static V norm_or(V a, V fallback) { const float l = len(a); return l > 1e-5f ? mul(a, 1 / l) : fallback; }
static V perp(V a, V axis, V fallback) { return norm_or(sub(a, mul(axis, dot(a, axis))), fallback); }   // a without its part along axis (a unit)
static float wrap(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }
static float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

static const V UP = { 0, 1, 0 };
static V heading(float yaw) { return v(sinf(yaw), 0, cosf(yaw)); }         // forward
static V left_of(float yaw) { return v(-cosf(yaw), 0, sinf(yaw)); }       // (up x forward is the right)

typedef struct { V x, y, z, t; } Frame;                                    // a part: its axes and origin, figure space
static Frame frame_xz(V x, V z_hint, V t) {                                // x kept, z as near the hint as it can be
    Frame f;
    f.x = x; f.z = perp(z_hint, x, v(1, 0, 0)); f.y = cross(f.z, f.x); f.t = t;
    return f;
}
static V at(const Frame *f, float x, float y, float z) {
    return add(f->t, add(mul(f->x, x), add(mul(f->y, y), mul(f->z, z))));
}
// figure space -> avatar space: the scale, z negated
static void put(float *m, const Frame *f, float s) {
    const V c[4] = { f->x, f->y, f->z, f->t };
    for (int i = 0; i < 4; i++) { m[i * 4] = c[i].x * s; m[i * 4 + 1] = c[i].y * s; m[i * 4 + 2] = -c[i].z * s; m[i * 4 + 3] = i == 3 ? 1.0f : 0.0f; }
}

#define THIGH 4.527f
#define SHIN 4.755f
#define ANKLE 0.56f                       // over the floor, standing
#define HIP_DROP 3.627f
#define SPINE_X 3.800f
#define SPINE_Y (-0.428f)
#define UPPER 3.093f
#define FORE 2.629f
#define COAT_BACK 0.10f                   // radians
#define EYE_AHEAD 0.9f
#define EYE_UP 1.0f
// the neck's height with the legs straight (the figure's units): the ankle, the leg, the hips to the waist, the
// spine to the neck (17.3; the eyes one more)
#define NECK (ANKLE + SHIN + THIGH + HIP_DROP + 0.017f + SPINE_X)
// (the least is an adult of average height, not a short one: a player who is seated when the figure first shows has
// nothing to say how tall they stand, and a small figure's arms came up well short of a grown player's hands)
#define UNIT_MIN 0.088f                   // metres a unit: eyes at 1.61 m ...
#define UNIT_MAX 0.110f                   // ... to 2.01 m
#define STEP_FAR 2.4f                     // a foot steps once it is this far from under its hip (units) ...
#define STEP_TURN 0.7f                    // ... or turned this far from the body (radians)
#define STEP_TIME 0.22f                   // seconds

// Two bones from a to the target (cut to what they reach), bent towards pole: the middle joint and the end
static void two_bone(V a, V target, float l1, float l2, V pole, V *mid, V *end, V *bend) {
    V d = sub(target, a);
    float L = len(d);
    const V dir = norm_or(d, v(0, -1, 0));
    L = clampf(L, fabsf(l1 - l2) + 0.3f, (l1 + l2) * 0.999f);
    const float c = clampf((l1 * l1 + L * L - l2 * l2) / (2 * l1 * L), -1, 1), sn = sqrtf(1 - c * c);
    const V e = perp(pole, dir, perp(v(0, 0, 1), dir, v(1, 0, 0)));
    *mid = add(a, add(mul(dir, l1 * c), mul(e, l1 * sn)));
    *end = add(a, mul(dir, L));
    *bend = e;
}

void avatar_solve(Avatar *a, const AvatarPose *in, float dt, float part[M2_FIGURE_PARTS][16], int draw[M2_FIGURE_PARTS]) {
    Frame P[M2_FIGURE_PARTS];
    for (int i = 0; i < M2_FIGURE_PARTS; i++) draw[i] = 1;
    if (!a->started || dt <= 0) dt = 0;
    const float *h = in->head;

    // the head's axes (figure space: the avatar's with z negated)
    const V hl = v(-h[0], -h[1], h[2]), hu = v(h[4], h[5], -h[6]), hf = v(-h[8], -h[9], h[10]);
    // the size: by the neck's height when standing (the eyes' less what the head's own turn gives them, so that
    // looking down or up changes nothing), followed up at once and down slowly (a player who sits, or a short one);
    // a figure smaller than its least stands with its feet under the floor rather than crouching for good
    const float neck_m = h[13] - 0.09f * (hf.y * EYE_AHEAD + hu.y * EYE_UP);
    if (!a->started) a->top = neck_m > 1.1f ? neck_m : 1.1f;
    if (neck_m > a->top) a->top = neck_m;
    else a->top += (neck_m - a->top) * (dt > 0 ? 1 - expf(-dt / 120.0f) : 0);   // (minutes: a crouch is not a seat)
    const float s = clampf(a->top / NECK, UNIT_MIN, UNIT_MAX);
    const float floor_y = a->top < UNIT_MIN * NECK ? (a->top - UNIT_MIN * NECK) / s : 0;

    // the head (in units)
    const V eye = v(h[12] / s, h[13] / s, -h[14] / s);
    V neck = sub(eye, add(mul(hf, EYE_AHEAD), mul(hu, EYE_UP)));
    // (never higher than the figure stands: a neck its legs can't reach up to comes down to them, the head with it,
    // rather than the feet leaving the floor)
    neck.y = fminf(neck.y, floor_y + NECK);
    P[M2_FIGURE_HEAD].x = hf; P[M2_FIGURE_HEAD].y = hl; P[M2_FIGURE_HEAD].z = mul(hu, -1); P[M2_FIGURE_HEAD].t = neck;

    // the wrists
    V wrist[2];
    int nh = 0;
    V hands = v(0, 0, 0);
    for (int k = 0; k < 2; k++) {
        wrist[k] = v(in->wrist[k][0] / s, in->wrist[k][1] / s, -in->wrist[k][2] / s);
        if (in->hand[k]) { hands = add(hands, wrist[k]); nh++; }
    }

    // the body's heading: the head's, drawn half way to where the hands are held out; followed, not snapped to
    // (looking straight up or down: the heading it had)
    const V hfh = hypotf(hf.x, hf.z) > 0.3f ? norm_or(v(hf.x, 0, hf.z), heading(a->yaw)) : heading(a->yaw);
    V want = hfh;
    if (nh) {
        V d = sub(mul(hands, 1.0f / nh), neck);
        d.y = 0;
        if (len(d) > 1.5f) want = norm_or(add(hfh, norm_or(d, hfh)), hfh);
    }
    const float head_yaw = atan2f(hfh.x, hfh.z), want_yaw = atan2f(want.x, want.z);
    if (!a->started) a->yaw = want_yaw;
    else a->yaw = wrap(a->yaw + wrap(want_yaw - a->yaw) * (1 - expf(-dt * 5)));
    const V fwd = heading(a->yaw), lft = left_of(a->yaw);

    // the hips: under the neck, a little behind it, following with a lag (a quick lean bends the spine, a step moves
    // the body); never further from the neck than the spine can lean
    {
        const float tx = neck.x - fwd.x * 0.3f, tz = neck.z - fwd.z * 0.3f, k = a->started && dt > 0 ? 1 - expf(-dt * 6) : 1;
        if (!a->started) { a->hip[0] = tx; a->hip[1] = tz; }
        a->hip[0] += (tx - a->hip[0]) * k; a->hip[1] += (tz - a->hip[1]) * k;
        const float dx = a->hip[0] - neck.x, dz = a->hip[1] - neck.z, d = sqrtf(dx * dx + dz * dz), most = 2.2f;
        if (d > most) { a->hip[0] = neck.x + dx * most / d; a->hip[1] = neck.z + dz * most / d; }
    }
    const float spine = sqrtf(SPINE_X * SPINE_X + SPINE_Y * SPINE_Y);
    V waist;
    {
        const float dx = a->hip[0] - neck.x, dz = a->hip[1] - neck.z;
        waist = v(a->hip[0], neck.y - sqrtf(spine * spine - dx * dx - dz * dz), a->hip[1]);
    }
    {   // the chest: its x up the spine to the neck (the neck sits SPINE_Y off that axis), turned a little with the head
        const V u = norm_or(sub(neck, waist), UP);
        const float cyaw = a->yaw + 0.35f * wrap(head_yaw - a->yaw);
        Frame c = frame_xz(u, left_of(cyaw), waist);
        const float th = atanf(-SPINE_Y / SPINE_X), ct = cosf(th), st = sinf(th);
        const V x = add(mul(c.x, ct), mul(c.y, st)), y = add(mul(c.x, -st), mul(c.y, ct));
        c.x = x; c.y = y;
        P[M2_FIGURE_CHEST] = c;
        // the coat hangs from the chest (its model is one frame of the game's flapping coat, in the chest's space):
        // tilted back a little about the waist, clear of the seat of the trousers, which it otherwise cuts through
        P[M2_FIGURE_COAT] = c;
        P[M2_FIGURE_COAT].x = add(mul(c.x, cosf(COAT_BACK)), mul(c.y, -sinf(COAT_BACK)));
        P[M2_FIGURE_COAT].y = add(mul(c.x, sinf(COAT_BACK)), mul(c.y, cosf(COAT_BACK)));
    }
    {   // the pelvis: upright, leaning a third of the way with the spine
        const V down = mul(norm_or(add(mul(UP, 0.65f), mul(P[M2_FIGURE_CHEST].x, 0.35f)), UP), -1);
        Frame p = frame_xz(down, lft, waist);
        p.t = add(waist, mul(down, 0.017f));
        P[M2_FIGURE_PELVIS] = p;
        P[M2_FIGURE_FLAP_L] = P[M2_FIGURE_FLAP_R] = p;
        P[M2_FIGURE_FLAP_L].t = P[M2_FIGURE_FLAP_R].t = at(&p, 1.748f, 1.297f, 0);
    }

    // the legs: each foot planted, stepping after its hip when the body has moved or turned away (one at a time)
    for (int k = 0; k < 2; k++) {
        const float sg = k ? -1.0f : 1.0f;
        const int thigh = k ? M2_FIGURE_THIGH_R : M2_FIGURE_THIGH_L;
        const V hip = at(&P[M2_FIGURE_PELVIS], HIP_DROP, -0.026f, k ? -0.722f : 0.726f);
        const float ix = a->hip[0] + lft.x * sg * 1.05f + fwd.x * 0.1f, iz = a->hip[1] + lft.z * sg * 1.05f + fwd.z * 0.1f;
        const float iyaw = a->yaw - sg * 0.14f;   // (toes a little out)
        float *p = a->foot[k].p;
        const float far = hypotf(ix - p[0], iz - p[1]);
        if (!a->started || far > 9.0f) { p[0] = ix; p[1] = iz; a->foot[k].yaw = iyaw; a->foot[k].stepping = 0; }
        else if (!a->foot[k].stepping && !a->foot[1 - k].stepping &&
                 (far > STEP_FAR || fabsf(wrap(iyaw - a->foot[k].yaw)) > STEP_TURN)) {
            a->foot[k].stepping = 1; a->foot[k].t = 0;
            a->foot[k].from[0] = p[0]; a->foot[k].from[1] = p[1]; a->foot[k].from_yaw = a->foot[k].yaw;
        }
        float lift = 0;
        if (a->foot[k].stepping) {
            a->foot[k].t += dt / STEP_TIME;
            float t = a->foot[k].t;
            if (t >= 1) { t = 1; a->foot[k].stepping = 0; }
            const float e = t * t * (3 - 2 * t);
            p[0] = a->foot[k].from[0] + (ix - a->foot[k].from[0]) * e;
            p[1] = a->foot[k].from[1] + (iz - a->foot[k].from[1]) * e;
            a->foot[k].yaw = a->foot[k].from_yaw + wrap(iyaw - a->foot[k].from_yaw) * e;
            lift = sinf(3.14159265f * t) * 0.9f;
        }
        const V toe_way = heading(a->foot[k].yaw);
        V knee, ankle, bend;
        two_bone(hip, v(p[0], floor_y + ANKLE + lift, p[1]), THIGH, SHIN, add(toe_way, fwd), &knee, &ankle, &bend);
        const V dir = norm_or(sub(ankle, hip), v(0, -1, 0)), hinge = cross(dir, bend);
        P[thigh] = frame_xz(norm_or(sub(knee, hip), dir), hinge, hip);
        P[thigh + 1] = frame_xz(norm_or(sub(ankle, knee), dir), hinge, knee);
        Frame f;
        f.x = v(0, -1, 0); f.y = toe_way; f.z = cross(f.x, f.y); f.t = ankle;
        P[thigh + 2] = f;
        P[thigh + 3] = f;
        P[thigh + 3].t = at(&f, 0.346f, 1.374f, sg * 0.153f);
    }

    // the arms: shoulder to wrist, the elbow down, out and back; no wrist: hanging, with the figure's own hand
    for (int k = 0; k < 2; k++) {
        const float sg = k ? -1.0f : 1.0f;
        const int arm = k ? M2_FIGURE_ARM_R : M2_FIGURE_ARM_L;
        const V sh = at(&P[M2_FIGURE_CHEST], 1.966f, -0.157f, k ? -2.195f : 2.187f);
        V target = wrist[k];
        if (!in->hand[k]) target = add(sh, add(mul(UP, -(UPPER + FORE) * 0.97f), add(mul(fwd, 0.9f), mul(lft, sg * 0.5f))));
        // (a player's arm longer than the figure's: both bones grow to the wrist, so the hand stays on the arm)
        float grow = 1;
        if (in->hand[k] && in->stretch > 1) grow = clampf(len(sub(target, sh)) / ((UPPER + FORE) * 0.985f), 1, in->stretch);
        V elbow, end, bend;
        two_bone(sh, target, UPPER * grow, FORE * grow, add(mul(UP, -1.0f), add(mul(lft, sg * 0.7f), mul(fwd, -0.6f))), &elbow, &end, &bend);
        const V dir = norm_or(sub(end, sh), v(0, -1, 0)), hinge = cross(bend, dir);
        P[arm] = frame_xz(norm_or(sub(elbow, sh), dir), hinge, sh);
        P[arm + 1] = frame_xz(norm_or(sub(end, elbow), dir), hinge, elbow);
        P[arm + 2] = P[arm + 1];
        P[arm + 2].t = end;
        P[arm].x = mul(P[arm].x, grow); P[arm + 1].x = mul(P[arm + 1].x, grow);
        draw[arm + 2] = !in->hand[k];
    }
    a->started = 1;
    P[M2_FIGURE_WAIST] = P[M2_FIGURE_PELVIS];   // (drawn from the pelvis's and the chest's: m2_gl_figure_waist)
    draw[M2_FIGURE_WAIST] = 0;
    P[M2_FIGURE_GUN_HAND] = P[M2_FIGURE_HAND_R];   // (drawn by avatar_hand's matrix, when the player wants it)
    draw[M2_FIGURE_GUN_HAND] = 0;
    for (int i = 0; i < M2_FIGURE_PARTS; i++) put(part[i], &P[i], s);
}

// The figure's own hands on a tracked arm. The game's models (tools/m2figure, the intro's frame 2300):
//   the gun hand (0x8e7f31, a right hand's): the wrist at the origin, the fist from there to y = -1.4, the pistol's
//     barrel along -y at x = -0.375, z = +0.7 (its muzzle at y = -2.26), its top towards +z;
//   the open hand (a left one; the right is its mirror in z): the fingers along +x, the thumb towards +y, the palm
//     towards -z (+z mirrored).
// 0.09 m a unit here (the figure's own size is not known before it is solved: 0.088 to 0.11).
#define GUN_SIDE 0.034f                   // the barrel beside the wrist (m) ...
#define GUN_OVER 0.063f                   // ... and over it
#define GUN_BACK 0.100f                   // the wrist behind the aim pose (where the app's hand model has its own)
#define OPEN_WRIST_X 0.028f               // the open hand's wrist: the app's hand model's (xr_shell.c HAND_WRIST)
#define OPEN_WRIST_Y (-0.086f)
#define OPEN_WRIST_Z 0.101f
void avatar_wrist(const float *aim, int right, int gun, float out[3]) {
    const float sx = right ? 1.0f : -1.0f;
    const float p[3] = { gun ? sx * GUN_SIDE : sx * OPEN_WRIST_X, gun ? -GUN_OVER : OPEN_WRIST_Y, gun ? GUN_BACK : OPEN_WRIST_Z };
    for (int k = 0; k < 3; k++) out[k] = aim[k] * p[0] + aim[4 + k] * p[1] + aim[8 + k] * p[2] + aim[12 + k];
}
void avatar_hand(float part[16], const float *aim, int right, int gun) {
    const float s = sqrtf(part[0] * part[0] + part[1] * part[1] + part[2] * part[2]), sx = right ? 1.0f : -1.0f;
    for (int k = 0; k < 3; k++) {
        const float X = aim[k] * s, Y = aim[4 + k] * s, Z = aim[8 + k] * s;
        if (gun) { part[k] = sx * X; part[4 + k] = Z; part[8 + k] = Y; }   // x across, -y the barrel (ahead), z up
        else { part[k] = -Z; part[4 + k] = Y; part[8 + k] = -X; }          // x the fingers (ahead), y the thumb (up)
    }
}

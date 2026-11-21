// The partner's figure (src/xr/quest_avatar.c): whatever pose comes in, the parts must stay the game's skeleton
// (every part a turn, one scale and the z flip; every joint where its parent has it), the head and the wrists where
// the pose has them, the feet on the floor.
extern "C" {
#include "xr/quest_avatar.h"
}

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
int failures = 0;
void check(bool ok, const char *what, int pose, int frame, double got) {
    if (ok) return;
    std::printf("FAIL pose %d frame %d: %s (%g)\n", pose, frame, what, got);
    failures++;
}
struct P3 { float x, y, z; };
P3 at(const float *m, float x, float y, float z) {
    return {m[0] * x + m[4] * y + m[8] * z + m[12], m[1] * x + m[5] * y + m[9] * z + m[13], m[2] * x + m[6] * y + m[10] * z + m[14]};
}
float dist(P3 a, P3 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }
P3 origin(const float *m) { return {m[12], m[13], m[14]}; }

// a head at (x, y, z) turned by yaw (about +y) and pitch
void head(AvatarPose &p, float x, float y, float z, float yaw, float pitch) {
    const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    const float h[16] = {cy, 0, sy, 0, -sy * sp, cp, cy * sp, 0, -sy * cp, -sp, cy * cp, 0, x, y, z, 1};
    std::memcpy(p.head, h, sizeof h);
}
} // namespace

int main() {
    // the joints (quest_avatar.c's table): child, parent, the child's origin in the parent's space
    static const struct { int child, parent; float x, y, z; } joints[] = {
        {M2_FIGURE_FLAP_L, M2_FIGURE_PELVIS, 1.748f, 1.297f, 0},     {M2_FIGURE_FLAP_R, M2_FIGURE_PELVIS, 1.748f, 1.297f, 0},
        {M2_FIGURE_THIGH_L, M2_FIGURE_PELVIS, 3.627f, -0.026f, 0.726f}, {M2_FIGURE_THIGH_R, M2_FIGURE_PELVIS, 3.627f, -0.026f, -0.722f},
        {M2_FIGURE_SHIN_L, M2_FIGURE_THIGH_L, 4.527f, 0, 0},         {M2_FIGURE_SHIN_R, M2_FIGURE_THIGH_R, 4.527f, 0, 0},
        {M2_FIGURE_FOOT_L, M2_FIGURE_SHIN_L, 4.755f, 0, 0},          {M2_FIGURE_FOOT_R, M2_FIGURE_SHIN_R, 4.755f, 0, 0},
        {M2_FIGURE_TOE_L, M2_FIGURE_FOOT_L, 0.346f, 1.374f, 0.153f}, {M2_FIGURE_TOE_R, M2_FIGURE_FOOT_R, 0.346f, 1.374f, -0.153f},
        {M2_FIGURE_CHEST, M2_FIGURE_PELVIS, -0.017f, 0, 0},          {M2_FIGURE_HEAD, M2_FIGURE_CHEST, 3.800f, -0.428f, 0},
        {M2_FIGURE_ARM_L, M2_FIGURE_CHEST, 1.966f, -0.157f, 2.187f}, {M2_FIGURE_ARM_R, M2_FIGURE_CHEST, 1.966f, -0.157f, -2.195f},
        {M2_FIGURE_FORE_L, M2_FIGURE_ARM_L, 3.093f, 0, 0},           {M2_FIGURE_FORE_R, M2_FIGURE_ARM_R, 3.093f, 0, 0},
        {M2_FIGURE_HAND_L, M2_FIGURE_FORE_L, 2.629f, 0, 0},          {M2_FIGURE_HAND_R, M2_FIGURE_FORE_R, 2.629f, 0, 0},
    };
    for (int pose = 0; pose < 6; pose++) {
        Avatar av{};
        for (int frame = 0; frame < 400; frame++) {
            const float t = float(frame) / 72.0f;
            AvatarPose p{};
            float hx = 0, hy = 1.66f, hz = 0, yaw = 0, pitch = 0;
            if (pose == 1) { // aiming, both hands
                p.hand[0] = p.hand[1] = 1;
                p.wrist[1][0] = 0.10f, p.wrist[1][1] = 1.36f, p.wrist[1][2] = -0.34f;
                p.wrist[0][0] = 0.04f, p.wrist[0][1] = 1.33f, p.wrist[0][2] = -0.36f;
            } else if (pose == 2) { // crouching after a second, a hand up, the head turned
                hy = t < 1 ? 1.66f : 1.05f, yaw = 0.6f, pitch = -0.2f;
                p.hand[1] = 1, p.wrist[1][0] = 0.3f, p.wrist[1][1] = hy - 0.3f, p.wrist[1][2] = -0.22f;
            } else if (pose == 3) { // walking about and turning right round
                hx = 0.8f * std::sin(t * 1.5f), hz = 0.5f * std::cos(t), yaw = t * 1.3f;
            } else if (pose == 4) { // looking straight down, then up
                pitch = t < 2 ? -1.5707f : 1.5707f;
            } else if (pose == 5) { // a wrist out of reach, the other on the body
                p.hand[0] = p.hand[1] = 1;
                p.wrist[1][0] = 2.0f, p.wrist[1][1] = 2.5f, p.wrist[1][2] = -1.0f;
                p.wrist[0][0] = 0.0f, p.wrist[0][1] = 1.2f, p.wrist[0][2] = 0.0f;
            }
            head(p, hx, hy, hz, yaw, pitch);
            float part[M2_FIGURE_PARTS][16];
            int draw[M2_FIGURE_PARTS];
            avatar_solve(&av, &p, frame ? 1.0f / 72.0f : 0.0f, part, draw);
            // every part: three axes of one length at right angles, mirrored (the game's space is left-handed)
            const float s = std::sqrt(part[0][0] * part[0][0] + part[0][1] * part[0][1] + part[0][2] * part[0][2]);
            check(s > 0.07f && s < 0.12f, "the figure's scale", pose, frame, s);
            for (int i = 0; i < M2_FIGURE_PARTS; i++) {
                const float *m = part[i];
                for (int a = 0; a < 3; a++)
                    for (int b = a; b < 3; b++) {
                        const float d = m[a * 4] * m[b * 4] + m[a * 4 + 1] * m[b * 4 + 1] + m[a * 4 + 2] * m[b * 4 + 2];
                        check(std::fabs(d - (a == b ? s * s : 0)) < 1e-4f, "a part's axes", pose, frame, d);
                    }
                const float det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[4] * (m[1] * m[10] - m[2] * m[9]) + m[8] * (m[1] * m[6] - m[2] * m[5]);
                check(det < 0, "a part's handedness", pose, frame, det);
                for (int k = 0; k < 16; k++) check(std::isfinite(m[k]), "a part's matrix", pose, frame, m[k]);
            }
            for (const auto &j : joints)
                check(dist(origin(part[j.child]), at(part[j.parent], j.x, j.y, j.z)) < 2e-3f, "a joint", pose, frame,
                      dist(origin(part[j.child]), at(part[j.parent], j.x, j.y, j.z)));
            // the eyes where the head is (the head's model: 0.9 ahead of the neck, 1.0 up, which is its -z)
            // (looking straight down or up from full height puts the neck over where the figure's can be: it comes down)
            if (pose != 4) check(dist(at(part[M2_FIGURE_HEAD], 0.9f, 0, -1.0f), P3{hx, hy, hz}) < 2e-3f, "the eyes", pose, frame, dist(at(part[M2_FIGURE_HEAD], 0.9f, 0, -1.0f), P3{hx, hy, hz}));
            // the wrists where the pose has them, when they can be reached
            if (pose == 1 || pose == 2)
                for (int h = 0; h < 2; h++)
                    if (p.hand[h])
                        check(dist(origin(part[h ? M2_FIGURE_HAND_R : M2_FIGURE_HAND_L]), P3{p.wrist[h][0], p.wrist[h][1], p.wrist[h][2]}) < 5e-3f,
                              "a wrist", pose, frame, dist(origin(part[h ? M2_FIGURE_HAND_R : M2_FIGURE_HAND_L]), P3{p.wrist[h][0], p.wrist[h][1], p.wrist[h][2]}));
            for (int h = 0; h < 2; h++) check(draw[h ? M2_FIGURE_HAND_R : M2_FIGURE_HAND_L] == !p.hand[h], "a hand drawn", pose, frame, 0);
            // the feet on the floor, but for a step's lift (the ankle 0.56 units over it)
            for (int f = 0; f < 2; f++) {
                const float y = part[f ? M2_FIGURE_FOOT_R : M2_FIGURE_FOOT_L][13] / s;
                check(y > 0.55f && y < 1.5f, "a foot's height", pose, frame, y);
            }
            // standing straight: the knees barely bent (the hips over the ankles by nearly the legs' length)
            if (pose == 0 && frame > 10) {
                const float leg = dist(origin(part[M2_FIGURE_THIGH_L]), origin(part[M2_FIGURE_FOOT_L])) / s;
                check(leg > 9.0f, "a standing leg", pose, frame, leg);
            }
        }
    }
    // a head seen too high for a while (a headset lifted to be put on, 15 cm), then the player standing as they do:
    // the figure stands straight, not with its knees bent under a height it was never
    {
        Avatar av{};
        float part[M2_FIGURE_PARTS][16];
        int draw[M2_FIGURE_PARTS];
        for (int frame = 0; frame < 72 * 6; frame++) {
            AvatarPose p{};
            head(p, 0, frame < 72 * 2 ? 1.81f : 1.66f, 0, 0, 0);
            avatar_solve(&av, &p, frame ? 1.0f / 72.0f : 0.0f, part, draw);
        }
        const float s = std::sqrt(part[0][0] * part[0][0] + part[0][1] * part[0][1] + part[0][2] * part[0][2]);
        const float leg = dist(origin(part[M2_FIGURE_THIGH_L]), origin(part[M2_FIGURE_FOOT_L])) / s;
        check(leg > 9.0f, "a standing leg after a head seen too high", 6, 0, leg);
        const float y = part[M2_FIGURE_FOOT_L][13] / s;
        check(y > 0.55f && y < 0.6f, "its foot on the floor", 6, 0, y);
    }
    // the figure's own hands on a tracked arm (avatar_wrist, avatar_hand): the hand stays where the solver put it,
    // at the figure's scale; the gun hand's barrel (its model's -y, 0.375 to its -x and 0.7 up its z) lies on the
    // aim's ray, whichever hand holds it; the open hand's fingers (+x) point ahead, its thumb (+y) up
    for (int right = 0; right < 2; right++)
        for (int gun = 0; gun < 2; gun++) {
            const float a = 0.4f, c = std::cos(a), sn = std::sin(a), u = 0.09f;
            const float aim[16] = {c, 0, -sn, 0, 0, 1, 0, 0, sn, 0, c, 0, 0.2f, 1.3f, -0.4f, 1};   // turned about y
            float w[3], m[16] = {0, u, 0, 0, u, 0, 0, 0, 0, 0, u, 0, 0, 0, 0, 1};
            avatar_wrist(aim, right, gun, w);
            m[12] = w[0], m[13] = w[1], m[14] = w[2];
            avatar_hand(m, aim, right, gun);
            check(dist(origin(m), P3{w[0], w[1], w[2]}) < 1e-6f, "an own hand's place", right, gun, 0);
            for (int x = 0; x < 3; x++)
                for (int y = x; y < 3; y++) {
                    const float d = m[x * 4] * m[y * 4] + m[x * 4 + 1] * m[y * 4 + 1] + m[x * 4 + 2] * m[y * 4 + 2];
                    check(std::fabs(d - (x == y ? u * u : 0)) < 1e-6f, "an own hand's axes", right, gun, d);
                }
            const P3 o = origin(aim), ahead = {-aim[8], -aim[9], -aim[10]}, up = {aim[4], aim[5], aim[6]};
            if (gun) {   // two points of the barrel's line, both on the ray; the muzzle ahead of the wrist
                float along[2];
                for (int k = 0; k < 2; k++) {
                    const P3 b = at(m, -0.375f, k ? -2.0f : 0.0f, 0.7f);
                    along[k] = (b.x - o.x) * ahead.x + (b.y - o.y) * ahead.y + (b.z - o.z) * ahead.z;
                    const P3 on = {o.x + ahead.x * along[k], o.y + ahead.y * along[k], o.z + ahead.z * along[k]};
                    check(dist(b, on) < 2e-3f, "the gun's barrel on the aim", right, gun, dist(b, on));
                }
                check(along[1] > along[0] + 0.15f, "the gun's barrel ahead", right, gun, along[1] - along[0]);
            } else {
                check(dist(at(m, 1, 0, 0), P3{w[0] + ahead.x * u, w[1] + ahead.y * u, w[2] + ahead.z * u}) < 1e-5f, "an open hand's fingers", right, gun, 0);
                check(dist(at(m, 0, 1, 0), P3{w[0] + up.x * u, w[1] + up.y * u, w[2] + up.z * u}) < 1e-5f, "an open hand's thumb", right, gun, 0);
            }
        }
    if (failures) return 1;
    std::printf("avatar: ok\n");
    return 0;
}

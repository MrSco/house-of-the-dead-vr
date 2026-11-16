#!/usr/bin/env python3
"""The light gun's trigger, made rather than copied: a curved blade with rounded edges, its front (the finger's
side) concave, its tip curling forward, its top tucked into the frame (it slides 3 mm back when pulled: quest_gun.c).

    python tools/cabinet/gun_trigger.py GUN.tcgun...     # replace each TCGUN002 gun's trigger (part 1) in place

gun_build.py makes its guns' triggers with make_trigger. Gun space: metres, +Y up, the muzzle towards -Z.
"""
import math, struct, sys
import numpy as np

# the blade (metres): its height from the frame to the tip, its width across the gun, its thickness at the top and
# at the tip, how far its middle bows back (the finger's hollow) and how far its tip curls forward
HEIGHT, WIDTH = 0.0215, 0.0072
THICK_TOP, THICK_TIP = 0.0070, 0.0050
BOW, CURL = 0.0016, 0.0042
RINGS, AROUND = 40, 32


def make_trigger(top_z, top_y, x_mid=0.0):
    """the trigger with the middle of its top at (x_mid, top_y, top_z): positions, normals (per vertex), triangles"""
    ts = np.linspace(0, 1, RINGS)
    # the centre line in (z, y): down from the top, bowing back, the tip curling forward
    cz = top_z + BOW * np.sin(np.pi * ts) - CURL * ts ** 3
    cy = top_y - HEIGHT * ts
    dz, dy = np.gradient(cz), np.gradient(cy)
    ln = np.hypot(dz, dy)
    tz, ty = dz / ln, dy / ln
    nz, ny = ty, -tz                                      # across the blade in the side view (towards the back)
    thick = THICK_TOP + (THICK_TIP - THICK_TOP) * ts
    # the tip rounded off: the last rings shrink along a quarter circle
    k_end = 6
    shrink = np.ones(RINGS)
    for i in range(k_end):
        a = (i + 1) / (k_end + 1)
        shrink[RINGS - k_end + i] = math.sqrt(max(0.0, 1 - a * a))
    P = []
    for i in range(RINGS):
        for j in range(AROUND):
            ang = 2 * math.pi * j / AROUND
            c, s = math.cos(ang), math.sin(ang)
            # a rounded rectangle (superellipse): x across the gun, n through the blade
            ex, en = 2 / 4.0, 2 / 4.0
            ux = math.copysign(abs(c) ** ex, c) * WIDTH / 2 * shrink[i]
            un = math.copysign(abs(s) ** en, s) * thick[i] / 2 * shrink[i]
            P.append((x_mid + ux, cy[i] + un * ny[i], cz[i] + un * nz[i]))
    P.append((x_mid, cy[0], cz[0]))                       # the top's centre (inside the frame)
    P.append((x_mid, cy[-1] + 0.0008 * ty[-1], cz[-1] + 0.0008 * tz[-1]))   # the tip's point
    P = np.array(P)
    T = []
    for i in range(RINGS - 1):
        for j in range(AROUND):
            a, b = i * AROUND + j, i * AROUND + (j + 1) % AROUND
            c2, d2 = a + AROUND, b + AROUND
            T += [(a, c2, b), (b, c2, d2)]
    top, tip = RINGS * AROUND, RINGS * AROUND + 1
    for j in range(AROUND):
        T.append((top, j, (j + 1) % AROUND))
        a, b = (RINGS - 1) * AROUND + j, (RINGS - 1) * AROUND + (j + 1) % AROUND
        T.append((tip, b, a))
    T = np.array(T)
    # outward, whatever the winding came out as: each face against the centre line's nearest point
    cen = np.stack([np.full(RINGS, x_mid), cy, cz], 1)
    for k, (a, b, c3) in enumerate(T):
        n = np.cross(P[b] - P[a], P[c3] - P[a])
        m = (P[a] + P[b] + P[c3]) / 3
        near = cen[np.argmin(((cen - m) ** 2).sum(1))]
        if np.dot(n, m - near) < 0: T[k] = (a, c3, b)
    N = np.zeros_like(P)
    for a, b, c3 in T:
        n = np.cross(P[b] - P[a], P[c3] - P[a])
        N[a] += n; N[b] += n; N[c3] += n
    N /= np.linalg.norm(N, axis=1, keepdims=True) + 1e-12
    return P, N, T


def replace_in(path):
    """a TCGUN002 gun's trigger (part 1) replaced: the new one's top where the old one's was, its colour kept"""
    d = open(path, 'rb').read()
    assert d[:8] == b'TCGUN002', path
    nv, ni = struct.unpack_from('<II', d, 8)
    tip = d[16:28]
    V = np.frombuffer(d, np.float32, nv * 10, 28).reshape(nv, 10).astype(np.float64)
    I = np.frombuffer(d, np.uint32, ni, 28 + nv * 40).reshape(-1, 3).astype(np.int64)
    trig = V[:, 9] == 1
    if not trig.any(): sys.exit('%s has no trigger' % path)
    colour = V[trig, 6:9].mean(0)
    old = V[trig, :3]
    x_mid = (old[:, 0].min() + old[:, 0].max()) / 2
    top_y = old[:, 1].max() + 0.0015                      # (into the frame a little)
    top_z = (old[:, 2].min() + old[:, 2].max()) / 2
    keep = ~trig
    new_index = -np.ones(nv, np.int64); new_index[keep] = np.arange(keep.sum())
    body_tris = I[keep[I].all(1)]
    BV = V[keep]
    P, N, T = make_trigger(top_z, top_y, x_mid)
    out = [b'TCGUN002', struct.pack('<II', len(BV) + len(P), (len(body_tris) + len(T)) * 3), tip]
    out.append(BV.astype(np.float32).tobytes())
    TV = np.zeros((len(P), 10), np.float32)
    TV[:, :3], TV[:, 3:6], TV[:, 6:9], TV[:, 9] = P, N, colour, 1
    out.append(TV.tobytes())
    out.append(new_index[body_tris].astype(np.uint32).tobytes())
    out.append((T + len(BV)).astype(np.uint32).tobytes())
    open(path, 'wb').write(b''.join(out))
    print('%s: trigger replaced (%d -> %d vertices), top at y %.4f z %.4f' % (path, trig.sum(), len(P), top_y, top_z))


if __name__ == '__main__':
    if len(sys.argv) < 2: sys.exit(__doc__)
    for p in sys.argv[1:]: replace_in(p)

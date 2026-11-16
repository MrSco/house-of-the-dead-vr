#!/usr/bin/env python3
"""Converts the VR hands (Sketchfab "Free FPS-Hand's" by Askasknot, CC BY 4.0) into src/quest/assets/hands.tchand.

    python tools/vr/hands_import.py free_fps-hands.glb [--preview out.png]

The model (a glTF binary, downloaded from Sketchfab) has both arms to the shoulder, each with a 24-bone skeleton:
Bone (shoulder), Bone.001, Bone.002 (forearm), five metacarpals from the wrist (Bone.003 thumb, .007 index, .008
middle, .009 ring, .010 pinky) and three bones down each finger. Armature.000 is the right arm, Armature.001 the left.
Its third material is an outline shell 3 mm outside the skin (a toon trick); it is left out.

What this writes, per hand (0 left, 1 right): the hand and the sleeve up to SLEEVE_M behind the wrist, in metres, in
the space of the controller's aim pose (as the gun model: +Y up, -Z forward), placed holding the gun's grip; the
skeleton at that rest pose; for every finger bone the axis and angle it turns through from open to a fist (taken from
the model's own animation, where the hand makes a fist at 1.48 s), so the game curls each finger by any amount. One
texture: the skin (left half) and the sleeve (right half).

TCHAND01 layout (little endian): "TCHAND01", u32 bones, u32 hands, u32 tex_w, u32 tex_h; per hand: u32 vertices,
u32 indices, per bone { i32 parent, f32 rest_local[16] (column major, in the parent's space, the root's in model
space), f32 inverse_rest_world[16], f32 axis[3] (the bone's space), f32 fist_angle (radians), i32 finger (0 thumb ..
4 pinky, -1 none), i32 joint (0 metacarpal .. 3) }, per vertex 16 f32 { position[3], normal[3], uv[2], joints[4],
weights[4] }, u32 indices; then the texture, RGBA8, rows from the top.
"""
import argparse, io, json, os, struct, sys
import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))   # the repository
OUT = os.path.join(ROOT, 'src', 'quest', 'assets', 'hands.tchand')
CREDIT = os.path.join(ROOT, 'src', 'quest', 'assets', 'hands.json')
SCALE = 0.25          # metres per model unit (wrist to middle fingertip: 0.19 m)
SLEEVE_M = 0.11       # the sleeve kept behind the wrist
FIST_T = 1.48         # the animation's fist
TEX = (1024, 512)
FINGERS = {  # metacarpal and the three finger bones
    0: ['Bone.003', 'Bone.004', 'Bone.005', 'Bone.006'],
    1: ['Bone.007', 'Bone.011', 'Bone.012', 'Bone.013'],
    2: ['Bone.008', 'Bone.014', 'Bone.015', 'Bone.016'],
    3: ['Bone.009', 'Bone.017', 'Bone.018', 'Bone.019'],
    4: ['Bone.010', 'Bone.020', 'Bone.021', 'Bone.022'],
}
# where each hand sits on the gun's grip (aim space, metres; tuned by eye with --preview): the middle of the four
# knuckles (index..pinky), and the turn about the grip's axis
PLACE = {'knuckles': (-0.024, -0.075, 0.0), 'grip_up': (0.0, 1.0, 0.22), 'yaw_deg': 0.0}

CT = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}
NC = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}


class GLB:
    def __init__(self, path):
        b = open(path, 'rb').read()
        if b[:4] != b'glTF': raise SystemExit('%s: not a glTF binary' % path)
        n = struct.unpack('<I', b[12:16])[0]
        self.j = json.loads(b[20:20 + n])
        o = 20 + n
        bl = struct.unpack('<I', b[o:o + 4])[0]
        self.bin = b[o + 8:o + 8 + bl]

    def acc(self, i):
        a = self.j['accessors'][i]; bv = self.j['bufferViews'][a['bufferView']]
        dt = np.dtype(CT[a['componentType']]); nc = NC[a['type']]
        off = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
        stride = bv.get('byteStride', dt.itemsize * nc)
        raw = np.frombuffer(self.bin, np.uint8, count=stride * (a['count'] - 1) + dt.itemsize * nc, offset=off)
        out = np.lib.stride_tricks.as_strided(raw, (a['count'], dt.itemsize * nc), (stride, 1)).copy().view(dt).reshape(a['count'], nc)
        if a.get('normalized'): out = out.astype(np.float32) / np.iinfo(dt).max
        return out

    def image(self, i):
        bv = self.j['bufferViews'][self.j['images'][i]['bufferView']]
        o = bv.get('byteOffset', 0)
        return Image.open(io.BytesIO(self.bin[o:o + bv['byteLength']]))


def quat_mat(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def trs(nd):
    if 'matrix' in nd: return np.array(nd['matrix'], float).reshape(4, 4).T
    m = np.eye(4)
    m[:3, :3] = quat_mat(nd.get('rotation', [0, 0, 0, 1])) * np.array(nd.get('scale', [1, 1, 1]))
    m[:3, 3] = nd.get('translation', [0, 0, 0])
    return m


def axis_angle(R):
    ang = np.arccos(np.clip((np.trace(R) - 1) / 2, -1, 1))
    if ang < 1e-5: return np.array([1.0, 0, 0]), 0.0
    ax = np.array([R[2, 1] - R[1, 2], R[0, 2] - R[2, 0], R[1, 0] - R[0, 1]])
    return ax / np.linalg.norm(ax), float(ang)


def rot(axis, ang):
    axis = np.asarray(axis, float); axis = axis / np.linalg.norm(axis)
    K = np.array([[0, -axis[2], axis[1]], [axis[2], 0, -axis[0]], [-axis[1], axis[0], 0]])
    return np.eye(3) + np.sin(ang) * K + (1 - np.cos(ang)) * K @ K


class Model:
    def __init__(self, path):
        self.g = g = GLB(path); j = g.j
        self.names = {n.get('name'): i for i, n in enumerate(j['nodes'])}
        self.parent = {}
        for i, n in enumerate(j['nodes']):
            for c in n.get('children', []): self.parent[c] = i

    def local_rest(self, i): return trs(self.g.j['nodes'][i])

    def anim_local(self, t):
        g = self.g; a = g.j['animations'][0]; L = {}
        for c in a['channels']:
            s = a['samplers'][c['sampler']]; T = g.acc(s['input'])[:, 0]; V = g.acc(s['output'])
            k = int(np.clip(np.searchsorted(T, t), 0, len(T) - 1))
            n = c['target']['node']
            L.setdefault(n, dict(g.j['nodes'][n]))[c['target']['path']] = V[k].tolist()
        return {n: trs(nd) for n, nd in L.items()}

    def world(self, local=None):
        j = self.g.j; W = {}
        def rec(i, P):
            W[i] = P @ (local[i] if local and i in local else self.local_rest(i))
            for c in j['nodes'][i].get('children', []): rec(c, W[i])
        for r in j['scenes'][j.get('scene', 0)]['nodes']: rec(r, np.eye(4))
        return W


def build_hand(m, side):
    """side 0: left (Armature.001), 1: right (Armature.000)"""
    j = m.g.j; arm = '001' if side == 0 else '000'
    skin_i = 1 if side == 0 else 0
    sk = j['skins'][skin_i]; joints = sk['joints']
    W = m.world(); Wf = m.world(m.anim_local(FIST_T))
    bone = lambda b: m.names[b + '_Armature.' + arm]
    P = lambda M, b: M[bone(b)][:3, 3]
    # the hand's frame at rest: f wrist -> middle knuckle, u pinky -> index knuckle, palm across
    knuck = np.mean([P(W, FINGERS[k][1]) for k in (1, 2, 3, 4)], axis=0)
    f = P(W, 'Bone.014') - P(W, 'Bone.008'); f /= np.linalg.norm(f)
    u = P(W, 'Bone.011') - P(W, 'Bone.020'); u -= f * u.dot(f); u /= np.linalg.norm(u)
    r = np.cross(f, u)
    H = np.stack([r, u, -f], 1)         # hand frame columns: x, y, z (fingers along -z)
    # the target frame on the grip: up along the grip, fingers forward (-Z), turned about the grip by yaw
    gu = np.array(PLACE['grip_up'], float); gu /= np.linalg.norm(gu)
    gf = np.array([0, 0, -1.0]); gf -= gu * gf.dot(gu); gf /= np.linalg.norm(gf)
    G = np.stack([np.cross(gf, gu), gu, -gf], 1)
    G = rot(gu, np.radians(PLACE['yaw_deg'] * (1 if side == 0 else -1))) @ G
    R = G @ H.T
    kn = np.array(PLACE['knuckles'], float) * np.array([1 if side == 0 else -1, 1, 1])
    place = np.eye(4); place[:3, :3] = R * SCALE; place[:3, 3] = kn - (R * SCALE) @ knuck
    # bones: rest world in model space, locals, inverse rest, curl axes
    jidx = {n: k for k, n in enumerate(joints)}
    bones = []
    finger_of = {}
    for fi, chain in FINGERS.items():
        for ji, b in enumerate(chain): finger_of[bone(b)] = (fi, ji)
    for k, n in enumerate(joints):
        pn = m.parent.get(n); parent = jidx.get(pn, -1)
        Wr = place @ W[n]
        local = (place @ W[pn] if parent >= 0 else np.eye(4))
        local = np.linalg.inv(local) @ Wr
        ax, ang = np.array([1.0, 0, 0]), 0.0
        fi, ji = finger_of.get(n, (-1, -1))
        if fi >= 0:
            Lr = m.local_rest(n)[:3, :3]; Lf = (np.linalg.inv(Wf[pn]) @ Wf[n])[:3, :3] if pn in Wf else Lr
            # the bone's own turn from rest to fist, in its rest space (scale removed)
            Rr = Lr / np.linalg.norm(Lr[:, 0]); Rf = Lf / np.linalg.norm(Lf[:, 0])
            ax, ang = axis_angle(Rr.T @ Rf)
        bones.append(dict(parent=parent, local=local, rest_world=Wr, inv=np.linalg.inv(Wr), axis=ax, angle=ang, finger=fi, joint=ji))
    # skinned rest vertices (materials 0 skin, 1 sleeve), into model space
    ibm = m.g.acc(sk['inverseBindMatrices']).reshape(-1, 4, 4).transpose(0, 2, 1)
    Jm = np.stack([place @ W[n] @ ibm[k] for k, n in enumerate(joints)])
    wrist = place @ np.r_[P(W, 'Bone.008'), 1]
    fwd_model = (R @ f)
    verts, idx = [], []
    mesh_nodes = [n for n, nd in enumerate(j['nodes']) if nd.get('skin') == skin_i and 'mesh' in nd]
    for n in mesh_nodes:
        for p in j['meshes'][j['nodes'][n]['mesh']]['primitives']:
            mat = p.get('material')
            if mat not in (0, 1): continue
            A = p['attributes']
            pos = m.g.acc(A['POSITION']).astype(float); nrm = m.g.acc(A['NORMAL']).astype(float)
            uv = m.g.acc(A['TEXCOORD_0']).astype(float); jo = m.g.acc(A['JOINTS_0']).astype(int); w = m.g.acc(A['WEIGHTS_0']).astype(float)
            w = w / w.sum(1, keepdims=True)
            M = np.einsum('vk,vkij->vij', w, Jm[jo])
            q = np.einsum('vij,vj->vi', M, np.c_[pos, np.ones(len(pos))])[:, :3]
            nn = np.einsum('vij,vj->vi', M[:, :3, :3], nrm); nn /= np.linalg.norm(nn, axis=1, keepdims=True) + 1e-12
            uv2 = np.c_[uv[:, 0] * 0.5 + (0.5 if mat == 1 else 0.0), uv[:, 1]]
            back = -(q - wrist[:3]) @ fwd_model          # how far behind the wrist
            tri = m.g.acc(p['indices'])[:, 0].reshape(-1, 3)
            tri = tri[(back[tri] <= SLEEVE_M).all(1)]
            used = np.unique(tri); remap = -np.ones(len(q), int); remap[used] = np.arange(len(used)) + sum(len(v) for v in verts)
            verts.append(np.c_[q[used], nn[used], uv2[used], jo[used], w[used]])
            idx.append(remap[tri])
    V = np.vstack(verts).astype(np.float32); I = np.vstack(idx).astype(np.uint32).ravel()
    return dict(bones=bones, V=V, I=I)


def texture(m):
    skin = m.g.image(0).convert('RGB').resize((TEX[0] // 2, TEX[1]), Image.LANCZOS)
    sleeve = m.g.image(3).convert('RGB').resize((TEX[0] // 2, TEX[1]), Image.LANCZOS)
    t = Image.new('RGBA', TEX); t.paste(skin, (0, 0)); t.paste(sleeve, (TEX[0] // 2, 0))
    return t


def pose(hand, curl):
    """model-space world matrices with finger k curled by curl[k] (0 open .. 1 fist)"""
    Wc = []
    for b in hand['bones']:
        L = b['local'].copy()
        if b['finger'] >= 0: L[:3, :3] = L[:3, :3] @ rot(b['axis'], b['angle'] * curl[b['finger']])
        Wc.append((Wc[b['parent']] if b['parent'] >= 0 else np.eye(4)) @ L)
    return Wc


def skin(hand, curl):
    Wc = pose(hand, curl)
    S = np.stack([Wc[k] @ b['inv'] for k, b in enumerate(hand['bones'])])
    V = hand['V']; jo = V[:, 8:12].astype(int); w = V[:, 12:16]
    M = np.einsum('vk,vkij->vij', w, S[jo])
    return np.einsum('vij,vj->vi', M, np.c_[V[:, :3], np.ones(len(V))])[:, :3]


def write(hands, tex):
    out = [b'TCHAND01', struct.pack('<4I', len(hands[0]['bones']), len(hands), *tex.size)]
    for h in hands:
        out.append(struct.pack('<2I', len(h['V']), len(h['I'])))
        for b in h['bones']:
            out.append(struct.pack('<i', b['parent']))
            out.append(np.asarray(b['local'], np.float32).T.tobytes())
            out.append(np.asarray(b['inv'], np.float32).T.tobytes())
            out.append(struct.pack('<4f2i', *b['axis'], b['angle'], b['finger'], b['joint']))
        out.append(h['V'].astype(np.float32).tobytes())
        out.append(h['I'].astype(np.uint32).tobytes())
    out.append(tex.tobytes())
    open(OUT, 'wb').write(b''.join(out))
    json.dump({'model': "Free FPS-Hand's", 'author': 'Askasknot', 'license': 'CC BY 4.0',
               'url': 'https://sketchfab.com/3d-models/free-fps-hands-2111009606fa4a07b355413ffa3c74f9',
               'changes': 'forearms cut behind the wrist, outline shell removed, posed on the gun grip, textures packed',
               'format': 'TCHAND01 (tools/vr/hands_import.py)', 'hands': ['left', 'right'],
               'vertices': [len(h['V']) for h in hands], 'triangles': [len(h['I']) // 3 for h in hands]},
              open(CREDIT, 'w'), indent=2)


def load_gun():
    b = open(os.path.join(ROOT, 'src', 'quest', 'assets', 'player-gun.tcgun'), 'rb').read()
    nv, ni = struct.unpack('<II', b[8:16])
    V = np.frombuffer(b, np.float32, nv * 10, 28).reshape(nv, 10)
    I = np.frombuffer(b, np.uint32, ni, 28 + nv * 40).reshape(-1, 3)
    return V, I


def preview(hands, tex, path):
    from PIL import ImageDraw
    gv, gi = load_gun()
    T = np.asarray(tex.convert('RGB'), float) / 255
    views = {'side (from the left)': np.array([[0, 0, -1], [0, 1, 0], [1, 0, 0]], float),
             'front': np.array([[-1, 0, 0], [0, 1, 0], [0, 0, -1]], float),
             'top': np.array([[1, 0, 0], [0, 0, -1], [0, 1, 0]], float),
             'player': rot([1, 0, 0], -0.5) @ rot([0, 1, 0], 0.5)}
    curls = [(0.1, 0.1, 0.1, 0.1, 0.1), (0.85, 0.85, 0.9, 0.9, 0.9), (0.85, 0.45, 0.9, 0.9, 0.9)]
    size = 360; img = Image.new('RGB', (size * len(views), size * len(curls) * 2), 'white'); d = ImageDraw.Draw(img)
    for hi, h in enumerate(hands):
        for ci, c in enumerate(curls):
            P = skin(h, c)
            for vi, (vn, Rv) in enumerate(views.items()):
                tris = []
                Q = P @ Rv.T
                cols = T[np.clip(((h['V'][:, 7]) * (T.shape[0] - 1)).astype(int), 0, T.shape[0] - 1), np.clip((h['V'][:, 6] * (T.shape[1] - 1)).astype(int), 0, T.shape[1] - 1)]
                for t in h['I'].reshape(-1, 3): tris.append((Q[t, 2].mean(), Q[t], cols[t].mean(0)))
                G = gv[:, :3] @ Rv.T
                for t in gi: tris.append((G[t, 2].mean(), G[t], gv[t, 6:9].mean(0)))
                tris.sort(key=lambda x: x[0])
                ox, oy = vi * size, (hi * len(curls) + ci) * size
                sc = size / 0.36
                for z, Tq, col in tris:
                    n = np.cross(Tq[1] - Tq[0], Tq[2] - Tq[0]); n /= np.linalg.norm(n) + 1e-12
                    s = 0.45 + 0.55 * abs(n[2])
                    d.polygon([(ox + size / 2 + p[0] * sc, oy + size * 0.42 - p[1] * sc) for p in Tq], fill=tuple(int(255 * min(1, x * s)) for x in col))
                d.text((ox + 4, oy + 4), '%s %s curl %s' % (['left', 'right'][hi], vn, c[1:3]), fill='black')
    img.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('glb'); ap.add_argument('--preview')
    a = ap.parse_args()
    m = Model(a.glb)
    hands = [build_hand(m, 0), build_hand(m, 1)]
    tex = texture(m)
    for s, h in zip(('left', 'right'), hands):
        print('%s: %d vertices, %d triangles; fist angles %s' % (s, len(h['V']), len(h['I']) // 3,
              ' '.join('%.0f' % np.degrees(b['angle']) for b in h['bones'] if b['finger'] >= 0)))
    if a.preview: preview(hands, tex, a.preview)
    else: write(hands, tex); print('wrote', OUT)


if __name__ == '__main__':
    main()

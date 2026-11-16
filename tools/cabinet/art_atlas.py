#!/usr/bin/env python3
"""Sharper artwork for the cabinet model (export.py --art): its marquee and side art from flat scans.

    python art_atlas.py MODEL_TEXTURES_DIR ART_DIR OUT_DIR [--check]

The model's own artwork is a few hundred pixels in its one 1024 texture (lambert6_baseColor.jpeg). This makes a 2048
texture: the model's 1024 one in the top-left quarter, and each new piece of art in a region of its own. Every piece
is lined up with the model's old art by image alignment (OpenCV's ECC, a homography, tried in the eight ways the old
art may be turned or mirrored in the texture), so the faces that showed the old art show the new art in the same
place. Where the new art has no pixels (outside a cut-to-shape side panel), the old texture shows through, scaled up.

ART_DIR holds (the names this script looks for; any one may be missing):
  marquee.*      the marquee, flat (e.g. LaunchBox's 3840x1363 capture)
  panel.*        the control panel under the screen (speakers, instruction card, 1P / 2P buttons), flat
  side-left.*, side-right.*   the side art, flat, cut to the panel's shape (alpha outside)
Writes OUT_DIR/atlas_base.png, atlas_emissive.png (2048, rows from the top) and uvmap.json: per piece the model's old
texture rectangle (pixels of the 1024 texture, rows from the top), which faces (see export.py) and the 3x3 matrix
taking old texture pixels to new atlas pixels. --check also writes a picture of each alignment.
"""
import glob, json, os, sys
import numpy as np
import cv2
from PIL import Image

OLD = 1024           # the model's texture
NEW = 2048           # the atlas
# The pieces: the old art's rectangle in the model's texture (x0, y0, x1, y1; rows from the top), the region of the
# atlas it gets, and which faces carry it (export.py: 'side-' then the side's sign of x; 'marquee')
PIECES = [
    # The marquee and the instruction card fill their panels: the art's own outline (corners: top left, top right,
    # bottom right, bottom left) drawn onto the panel's (Blender's world space, the faces that carried the old art:
    # export.py), its clear edges filled from the art; none of the model's old texture shows there.
    {'name': 'marquee', 'files': ['marquee'], 'old': (95, 556, 183, 736), 'region': (0, 1024, 2048, 1544),
     'faces': 'marquee', 'emissive': True,
     'fill': {'axes': (0, 2), 'u': (-0.9239, 0.9241), 'v': (4.5261, 4.0178),     # x across, z down the panel
              'face': [(-0.9239, 4.5261), (0.9241, 4.5261), (0.9241, 4.0178), (-0.9239, 4.0178)],
              'art': [(29, 97), (2142, 97), (2059, 617), (112, 617)]}},       # (a slight keystone: squared up)
    # the whole control panel (its speakers, the instruction card, the 1P / 2P buttons): one picture over the panel, a
    # strip bent down at its front (a slope, a rounded edge, a front face), unrolled: x across, the distance down the
    # strip's cross-section ('profile', (y, z) from its back edge) the other way. The picture's green card goes on the
    # card (a raised piece on the slope); past the picture, its frame's colour; the front face takes the last row.
    # (old: the strip, the card, the left and right speaker boxes)
    {'name': 'panel', 'files': ['panel'], 'region': (0, 1544, 2048, 2048),
     'old': [(598, 2, 697, 182), (21, 5, 67, 99), (797, 213, 826, 258), (644, 183, 675, 232)],
     'faces': 'panel', 'emissive': True, 'border': 'frame',
     'fill': {'axes': (0, 'profile'), 'u': (-0.9239, 0.9241), 'v': (0.0, 0.77),
              'profile': [(-0.924, 2.173), (-1.464, 1.691), (-1.479, 1.671), (-1.484, 1.648), (-1.484, 1.407)],
              'face3d': [(-0.4684, -1.0032, 2.1207), (0.4689, -1.0032, 2.1207), (0.3603, -1.3604, 1.8115),
                         (-0.3602, -1.3604, 1.8115)],                       # the card piece's corners
              'art': [(510, 78), (1661, 78), (1507, 638), (664, 638)]}},      # the picture's green card
    {'name': 'side-left', 'files': ['side-left'], 'region': (1024, 0, 1536, 1024), 'faces': 'side-', 'side': -1},
    {'name': 'side-right', 'files': ['side-right'], 'region': (1536, 0, 2048, 1024), 'faces': 'side+', 'side': 1},
]
# The side art is projected straight onto the flat side panels (the model's old side art sat askew on them, and the
# panels' outline is not the real cabinet's, so neither can place it): the panel (Blender's y, z: the cabinet's front
# at -y) drawn into the region, the art across the panel's top (its back edge to the top's front edge), its aspect
# kept; its clear corners filled from its own edges, black below it (as the real cabinet's sides).
PANEL = {'y_front': -1.555, 'y_back': 0.785, 'z_bottom': -0.152, 'z_top': 4.586, 'art_y_front': -1.351}
SIDE_BLACK = (0.015, 0.015, 0.017)


def load(path):
    im = Image.open(path)
    return np.asarray(im.convert('RGBA')).astype(np.float32) / 255.0


def find(art_dir, stem):
    for f in sorted(glob.glob(os.path.join(art_dir, stem + '.*'))):
        if f.lower().endswith(('.png', '.jpg', '.jpeg', '.webp')): return f
    return None


def gray(rgba, size=None):
    g = cv2.cvtColor((rgba[:, :, :3] * 255).astype(np.uint8), cv2.COLOR_RGB2GRAY).astype(np.float32) / 255.0
    if size: g = cv2.resize(g, size, interpolation=cv2.INTER_AREA)
    return g


def dihedral(w, h, k):
    """the 8 ways a w x h picture lies in a unit square: corners of the art (0,0),(1,0),(1,1),(0,1) -> crop corners"""
    corners = [(0, 0), (1, 0), (1, 1), (0, 1)]
    order = corners[k % 4:] + corners[:k % 4]
    if k >= 4: order = order[::-1]
    return np.float32([(x * w, y * h) for x, y in order])


def align_features(old_crop, art):
    """the homography old crop pixels -> art pixels by SIFT features (the art also tried mirrored: the model's
    texture may hold it so), or None; its quality: the RANSAC inliers' share"""
    up = 4
    crop = cv2.resize((gray(old_crop) * 255).astype(np.uint8), (old_crop.shape[1] * up, old_crop.shape[0] * up),
                      interpolation=cv2.INTER_CUBIC)
    ah, aw = art.shape[:2]
    s = 1000 / ah
    a = (gray(art, (int(aw * s), 1000)) * 255).astype(np.uint8)
    sift = cv2.SIFT_create()
    kc, dc = sift.detectAndCompute(crop, None)
    best = None
    for flip in (0, 1):
        b = cv2.flip(a, 1) if flip else a
        ka, da = sift.detectAndCompute(b, None)
        if dc is None or da is None: continue
        m = cv2.BFMatcher().knnMatch(dc, da, k=2)
        good = [q[0] for q in m if len(q) == 2 and q[0].distance < 0.8 * q[1].distance]
        if len(good) < 12: continue
        src = np.float32([kc[g.queryIdx].pt for g in good]); dst = np.float32([ka[g.trainIdx].pt for g in good])
        H, inl = cv2.findHomography(src, dst, cv2.RANSAC, 6.0)
        if H is None: continue
        n = int(inl.sum())
        if best is None or n > best[0]:
            F = np.array([[-1, 0, b.shape[1] - 1], [0, 1, 0], [0, 0, 1]], float) if flip else np.eye(3)
            # old crop pixels -> upscaled crop -> (mirrored) small art -> small art -> full art
            M = np.diag([1 / s, 1 / s, 1]) @ F @ H @ np.diag([up, up, 1])
            best = (n, M / M[2, 2], len(good), flip)
    if best is None or best[0] < 30: return None
    n, M, ng, flip = best
    return n / ng, M, 8 + flip


def art_to_art(a, b):
    """the homography a's pixels -> b's (two versions of the same artwork) by SIFT features, or None"""
    def small(img):
        s = 1400 / max(img.shape[:2])
        return (gray(img, (int(img.shape[1] * s), int(img.shape[0] * s))) * 255).astype(np.uint8), s
    ga, sa = small(a)
    gb, sb = small(b)
    sift = cv2.SIFT_create(8000)
    ka, da = sift.detectAndCompute(ga, None)
    kb, db = sift.detectAndCompute(gb, None)
    if da is None or db is None: return None
    m = cv2.BFMatcher().knnMatch(da, db, k=2)
    good = [q[0] for q in m if len(q) == 2 and q[0].distance < 0.75 * q[1].distance]
    if len(good) < 20: return None
    src = np.float32([ka[g.queryIdx].pt for g in good]); dst = np.float32([kb[g.trainIdx].pt for g in good])
    H, inl = cv2.findHomography(src, dst, cv2.RANSAC, 4.0)
    if H is None or int(inl.sum()) < 20: return None
    print('  art to art: %d of %d matches agree' % (int(inl.sum()), len(good)))
    return np.diag([1 / sb, 1 / sb, 1]) @ H @ np.diag([sa, sa, 1])


def align(old_crop, art):
    """the homography old crop pixels -> art pixels, and its quality: by features when they are found, else by ECC
    tried in 8 orientations"""
    r = align_features(old_crop, art)
    if r: return r
    ch, cw = old_crop.shape[:2]
    ah, aw = art.shape[:2]
    # the art at about the crop's scale (its long side as the crop's) for the search
    s = max(cw, ch) / max(aw, ah)
    small = gray(art, (max(8, int(aw * s)), max(8, int(ah * s))))
    mask = (cv2.resize(art[:, :, 3], small.shape[::-1]) > 0.5).astype(np.uint8)
    tmpl = cv2.GaussianBlur(gray(old_crop), (5, 5), 0)
    small = cv2.GaussianBlur(small, (5, 5), 0)
    best = None
    sh, sw = small.shape
    for k in range(8):
        # start: the crop's corners on the small art's corners (the k-th way round)
        src = np.float32([(0, 0), (cw, 0), (cw, ch), (0, ch)])
        dst = dihedral(sw, sh, k)
        H0 = cv2.getPerspectiveTransform(src, dst).astype(np.float32)
        try:
            cc, H = cv2.findTransformECC(tmpl, small, H0.copy(), cv2.MOTION_HOMOGRAPHY,
                                         (cv2.TERM_CRITERIA_EPS | cv2.TERM_CRITERIA_COUNT, 300, 1e-6), None, 5)
        except cv2.error:
            continue
        if best is None or cc > best[0]: best = (cc, H, k)
    if best is None: return None
    cc, H, k = best
    # small art pixels -> full art pixels
    S = np.diag([1 / s, 1 / s, 1]).astype(np.float64)
    return cc, S @ H.astype(np.float64), k


def unroll(profile, y, z):
    """the distance along a panel's cross-section (a polyline of (y, z) corners) to the point on it nearest (y, z)"""
    best, at, run = None, 0.0, 0.0
    for (ay, az), (by, bz) in zip(profile, profile[1:]):
        dy, dz = by - ay, bz - az
        ln = (dy * dy + dz * dz) ** 0.5
        t = max(0.0, min(ln, ((y - ay) * dy + (z - az) * dz) / ln))
        py, pz = ay + dy * t / ln, az + dz * t / ln
        d = (y - py) ** 2 + (z - pz) ** 2
        if best is None or d < best: best, at = d, run + t
        run += ln
    return at


def fill_piece(p, f, atlas, atlas_e):
    """an art drawn onto its panel: its outline onto the panel's, the region linear in the panel's two axes"""
    art = load(f)
    ah, aw = art.shape[:2]
    fl = p['fill']
    rx0, ry0, rx1, ry1 = p['region']
    rw, rh = rx1 - rx0, ry1 - ry0
    (u0, u1), (v0, v1) = fl['u'], fl['v']
    to_reg = lambda u, v: ((u - u0) / (u1 - u0) * rw, (v - v0) / (v1 - v0) * rh)
    face = fl.get('face') or [(x, unroll(fl['profile'], y, z)) for x, y, z in fl['face3d']]
    dst = np.float32([to_reg(u, v) for u, v in face])
    Hw = cv2.getPerspectiveTransform(np.float32(fl['art']), dst)
    # the art's clear parts filled from its edges first (so nothing of the old texture is needed round it)
    small = cv2.resize(art, (max(8, aw // 4), max(8, ah // 4)), interpolation=cv2.INTER_AREA)
    hole = (small[:, :, 3] < 0.5).astype(np.uint8)
    filled = cv2.inpaint((np.clip(small[:, :, :3], 0, 1) * 255).astype(np.uint8), hole, 9, cv2.INPAINT_TELEA)
    filled = cv2.resize(filled.astype(np.float32) / 255, (aw, ah), interpolation=cv2.INTER_CUBIC)
    a = art[:, :, 3:4]
    full = art[:, :, :3] * a + filled * (1 - a)
    if p.get('border') == 'frame':      # past the picture: its frame's colour (the median of its outer edge)
        ring = np.concatenate([full[:6].reshape(-1, 3), full[-6:].reshape(-1, 3), full[:, :6].reshape(-1, 3),
                               full[:, -6:].reshape(-1, 3)])
        bc = [float(c) for c in np.median(ring, 0)]
        reg = cv2.warpPerspective(full, Hw, (rw, rh), flags=cv2.INTER_AREA, borderMode=cv2.BORDER_CONSTANT,
                                  borderValue=bc)
    else:
        reg = cv2.warpPerspective(full, Hw, (rw, rh), flags=cv2.INTER_AREA, borderMode=cv2.BORDER_REPLICATE)
    out = np.dstack([reg, np.ones((rh, rw), np.float32)])
    atlas[ry0:ry1, rx0:rx1] = out
    if p.get('emissive'): atlas_e[ry0:ry1, rx0:rx1] = out
    ax0, ax1 = fl['axes']
    P = [[rw / (u1 - u0), 0, rx0 - u0 * rw / (u1 - u0)], [0, rh / (v1 - v0), ry0 - v0 * rh / (v1 - v0)]]
    print('%s: %s drawn onto its panel (%d x %d texels)' % (p['name'], os.path.basename(f), rw, rh))
    olds = p['old'] if isinstance(p['old'], list) else [p['old']]
    out = {'name': p['name'], 'faces': p['faces'], 'old': [list(r) for r in olds], 'planar': P, 'axes': [ax0, ax1],
           'art': os.path.basename(f)}
    if 'profile' in fl:
        out['profile'] = [list(q) for q in fl['profile']]
        out['vclamp'] = [ry0 + 0.5, ry1 - 0.5]      # (past the region's ends: its first or last row)
    return out


def side_piece(p, f, atlas):
    """a side's art projected onto its panel: the region drawn, and the affine (Blender y, z, 1) -> atlas pixels"""
    art = load(f)
    ah, aw = art.shape[:2]
    rx0, ry0, rx1, ry1 = p['region']
    rw, rh = rx1 - rx0, ry1 - ry0
    yf, yb, zb, zt = PANEL['y_front'], PANEL['y_back'], PANEL['z_bottom'], PANEL['z_top']
    s = min(rw / (yb - yf), rh / (zt - zb))                       # region pixels per unit
    # region x: from the outside the left side (-1) has the front on the right, the right side on the left
    def to_reg(y, z):
        x = (yb - y) * s if p['side'] < 0 else (y - yf) * s
        return x, (zt - z) * s
    # the art: its width across the top (art_y_front .. y_back), its top at the panel's top
    ax0, ay0 = to_reg(PANEL['art_y_front'] if p['side'] < 0 else yb, zt)
    ax1, _ = to_reg(yb if p['side'] < 0 else PANEL['art_y_front'], zt)
    x_lo, x_hi = min(ax0, ax1), max(ax0, ax1)
    sc = (x_hi - x_lo) / aw
    A = np.array([[sc, 0, x_lo], [0, sc, ay0], [0, 0, 1]])        # art pixels -> region pixels
    # the art's clear parts filled from its edges (inpainted small, then scaled back)
    small = cv2.resize(art, (aw // 4, ah // 4), interpolation=cv2.INTER_AREA)
    hole = (small[:, :, 3] < 0.5).astype(np.uint8)
    rgb8 = (np.clip(small[:, :, :3], 0, 1) * 255).astype(np.uint8)
    filled = cv2.inpaint(rgb8, hole, 9, cv2.INPAINT_TELEA).astype(np.float32) / 255
    filled = cv2.resize(filled, (aw, ah), interpolation=cv2.INTER_CUBIC)
    a = art[:, :, 3:4]
    full = art[:, :, :3] * a + filled * (1 - a)
    reg = np.zeros((rh, rw, 4), np.float32); reg[:, :, :3] = SIDE_BLACK; reg[:, :, 3] = 1
    warped = cv2.warpAffine(np.dstack([full, np.ones((ah, aw), np.float32)]), A[:2], (rw, rh),
                            flags=cv2.INTER_AREA if sc < 1 else cv2.INTER_CUBIC, borderMode=cv2.BORDER_CONSTANT,
                            borderValue=(0, 0, 0, 0))
    m = np.clip(warped[:, :, 3:4], 0, 1)
    reg[:, :, :3] = warped[:, :, :3] * m + reg[:, :, :3] * (1 - m)
    atlas[ry0:ry1, rx0:rx1] = reg
    # Blender (y, z, 1) -> atlas pixels
    sx = -s if p['side'] < 0 else s
    ox = rx0 + (yb * s if p['side'] < 0 else -yf * s)
    P = [[sx, 0, ox], [0, -s, ry0 + zt * s]]
    print('%s: %s projected onto the panel, %d x %d pixels of art' % (p['name'], os.path.basename(f), int(x_hi - x_lo), int(ah * sc)))
    return {'name': p['name'], 'faces': p['faces'], 'planar': P, 'art': os.path.basename(f)}


def main():
    tex_dir, art_dir, out = sys.argv[1], sys.argv[2], sys.argv[3]
    check = '--check' in sys.argv
    os.makedirs(out, exist_ok=True)
    base = load(os.path.join(tex_dir, 'lambert6_baseColor.jpeg'))
    emis = load(os.path.join(tex_dir, 'lambert6_emissive.jpeg'))
    base = cv2.resize(base, (OLD, OLD), interpolation=cv2.INTER_AREA)
    emis = cv2.resize(emis, (OLD, OLD), interpolation=cv2.INTER_AREA)
    atlas = np.zeros((NEW, NEW, 4), np.float32); atlas[:, :, 3] = 1
    atlas_e = np.zeros((NEW, NEW, 4), np.float32); atlas_e[:, :, 3] = 1
    atlas[:OLD, :OLD] = base
    atlas_e[:OLD, :OLD] = emis
    pieces = []
    for p in PIECES:
        if 'fill' in p:
            f = find(art_dir, p['files'][0])
            if not f:
                print(p['name'], ': no art, kept as it was'); continue
            pieces.append(fill_piece(p, f, atlas, atlas_e))
            continue
        if 'side' in p:
            f = find(art_dir, p['files'][0])
            if not f:
                print(p['name'], ': no art, kept as it was'); continue
            pieces.append(side_piece(p, f, atlas))
            continue
        files = [find(art_dir, f) for f in p['files']]
        files = [f for f in files if f]
        if not files:
            print(p['name'], ': no art, kept as it was'); continue
        x0, y0, x1, y1 = p['old']
        crop = base[y0:y1, x0:x1]
        best = None
        if 'corners' in p:   # placed by hand: crop pixels -> art pixels
            f = files[0]
            art = load(f)
            src = np.float32([[ox - x0, oy - y0] for (ox, oy), _ in p['corners']])
            dst = np.float32([a for _, a in p['corners']])
            best = (1.0, cv2.getPerspectiveTransform(src, dst).astype(np.float64), -1, f, art)
        ref = find(art_dir, p['ref']) if 'ref' in p else None
        if ref and not best:   # the model's art -> the reference version -> the new one
            f = files[0]
            art, refart = load(f), load(ref)
            r = align(crop, refart)
            if r and 'fit' in p:   # the reference's frame -> the new art's outline
                rh, rw = refart.shape[:2]
                H2 = cv2.getPerspectiveTransform(np.float32([(0, 0), (rw, 0), (rw, rh), (0, rh)]),
                                                 np.float32(p['fit'])).astype(np.float64)
            else:
                H2 = art_to_art(refart, art) if r else None
            if r and H2 is not None: best = (r[0], H2 @ r[1], r[2], f, art)
        for f in [] if best else (files[:1] if p['name'] == 'marquee' else files):
            art = load(f)
            r = align(crop, art)
            if r and (best is None or r[0] > best[0]): best = (r[0], r[1], r[2], f, art)
        if best is None:
            print(p['name'], ': could not be lined up'); continue
        cc, H, k, f, art = best
        print('%s: %s, correlation %.3f, orientation %d' % (p['name'], os.path.basename(f), cc, k))
        # the region: everything the old art's faces show (the old rectangle, seen in the art's frame) scaled into it,
        # aspect kept, centred; the new art over the old texture, which shows where the art is clear or ends
        rx0, ry0, rx1, ry1 = p['region']
        rw, rh = rx1 - rx0, ry1 - ry0
        ah, aw = art.shape[:2]
        q = cv2.perspectiveTransform(np.float32([[[0, 0]], [[x1 - x0, 0]], [[x1 - x0, y1 - y0]], [[0, y1 - y0]]]), H)[:, 0]
        bx0, by0 = q.min(0) - 2
        bx1, by1 = q.max(0) + 2
        sc = min(rw / (bx1 - bx0), rh / (by1 - by0))
        ox = rx0 + (rw - (bx1 - bx0) * sc) / 2 - bx0 * sc
        oy = ry0 + (rh - (by1 - by0) * sc) / 2 - by0 * sc
        A = np.array([[sc, 0, ox], [0, sc, oy], [0, 0, 1]])            # art pixels -> atlas pixels
        R = np.array([[1, 0, -rx0], [0, 1, -ry0], [0, 0, 1]])        # atlas pixels -> region pixels
        T = np.array([[1, 0, -x0], [0, 1, -y0], [0, 0, 1]])           # old texture pixels -> crop pixels
        M = A @ H @ T                                                  # old texture pixels -> atlas pixels
        M /= M[2, 2]
        # behind: the old texture seen through M's inverse, for the region's pixels
        behind = cv2.warpPerspective(base, R @ M, (rw, rh), flags=cv2.INTER_CUBIC, borderMode=cv2.BORDER_REPLICATE)
        # the art in the region's pixels (A then into the region), clear outside it
        front = cv2.warpPerspective(art, R @ A, (rw, rh), flags=cv2.INTER_AREA if sc < 1 else cv2.INTER_CUBIC,
                                    borderMode=cv2.BORDER_CONSTANT, borderValue=(0, 0, 0, 0))
        a = np.clip(front[:, :, 3:4], 0, 1)
        reg = behind.copy()
        reg[:, :, :3] = front[:, :, :3] * a + reg[:, :, :3] * (1 - a)
        reg[:, :, 3] = 1
        atlas[ry0:ry1, rx0:rx1] = reg
        if p['emissive']:
            atlas_e[ry0:ry1, rx0:rx1] = reg
        pieces.append({'name': p['name'], 'old': [x0, y0, x1, y1], 'faces': p['faces'], 'm': M.tolist(),
                       'art': os.path.basename(f), 'correlation': float(cc)})
        if check:
            # the old crop next to the atlas sampled where M puts each of its pixels (they should look alike)
            up = 3
            c_old = cv2.resize(crop, ((x1 - x0) * up, (y1 - y0) * up), interpolation=cv2.INTER_NEAREST)
            # sample the atlas at M(old pixel) for every old pixel (upsampled)
            ys, xs = np.mgrid[0:(y1 - y0) * up, 0:(x1 - x0) * up].astype(np.float32)
            ox_, oy_ = x0 + (xs + 0.5) / up, y0 + (ys + 0.5) / up
            d = M[2, 0] * ox_ + M[2, 1] * oy_ + M[2, 2]
            mx = ((M[0, 0] * ox_ + M[0, 1] * oy_ + M[0, 2]) / d).astype(np.float32)
            my = ((M[1, 0] * ox_ + M[1, 1] * oy_ + M[1, 2]) / d).astype(np.float32)
            c_new = cv2.remap(atlas, mx, my, cv2.INTER_LINEAR)
            pic = np.concatenate([c_old, np.ones((c_old.shape[0], 8, 4), np.float32), c_new], 1)
            Image.fromarray((np.clip(pic, 0, 1) * 255).astype(np.uint8)).convert('RGB').save(os.path.join(out, 'check_' + p['name'] + '.png'))
    Image.fromarray((np.clip(atlas, 0, 1) * 255).astype(np.uint8)).convert('RGB').save(os.path.join(out, 'atlas_base.png'))
    Image.fromarray((np.clip(atlas_e, 0, 1) * 255).astype(np.uint8)).convert('RGB').save(os.path.join(out, 'atlas_emissive.png'))
    json.dump({'old': OLD, 'new': NEW, 'pieces': pieces}, open(os.path.join(out, 'uvmap.json'), 'w'), indent=1)
    print('wrote', out)


if __name__ == '__main__':
    main()

# Blender (headless): the House of the Dead light gun (a model made from a photo of the real gun, hotd-lightgun.glb)
# into the Quest app's TCGUN002 guns, red (player 1) and blue (player 2).
#   blender -b --factory-startup --python gun_build.py -- GUN.glb BLUE_PHOTO RED_PHOTO OUTDIR
#
# The model (Y is its thickness, Z up, the muzzle towards -X) is a soup of ~1700 surface pieces, 700k triangles, its
# colour in a 4096 texture. Here:
#   - its hammer, which floats a few millimetres behind the slide, goes forward onto it;
#   - a slot through the frame behind the cylinder, with stray slivers in it (the "hook"), is plugged;
#   - the pieces become one closed surface (voxel remesh), cut down to TARGET_TRIS triangles;
#   - the colour is taken from the texture into the vertices (nearest point of the original surface);
#   - it has no trigger (the guard is empty): one is made (gun_trigger.py: a curved blade) and goes in;
#   - placed in the hand as export.py places the cabinet gun: its trigger on the pistol's, so the hands close round
#     its grip; 26 cm long; the muzzle (the barrel's front) is the file's tip.
# The blue gun: the same, its colour moved from the model's red to the blue of a photo of a real blue gun. The photo's
# light is taken out with a photo of a real red gun in the same light: the blue is brightened (in linear light) as
# much as the model's red is brighter than that photo's red. Each vertex keeps its brightness relative to the body.
import bpy, bmesh, sys, struct, math, mathutils
import numpy as np
from mathutils.bvhtree import BVHTree

argv = sys.argv[sys.argv.index('--') + 1:]
glb, blue_photo, red_photo, out = argv[:4]
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gun_trigger import make_trigger   # noqa: E402

GUN_LEN = 0.26                     # muzzle to back (metres)
PISTOL_TRIGGER = (-0.0415, -0.040)  # the pistol's trigger (y, z): where the hands' fingers close (export.py)
import os
TARGET_TRIS = int(os.environ.get('GUN_TRIS', 150000))   # (fewer leaves dents in its flat panels)
MODE = os.environ.get('GUN_MODE', 'weld')   # weld: the pieces joined where they touch; remesh: one voxel surface;
                                            # none: the pieces as they are
VOXEL = 0.0022                     # remesh size, model units (the model is 0.95 long: about 0.6 mm)
WELD = 0.0006                      # pieces' edges closer than this join (model units)
# the model's flaws (model units, found by ray picks and vertex histograms)
HAMMER = {'x_min': 0.359, 'z_min': 0.17, 'shift_x': -0.011}
SLOT_PLUG = ((0.158, 0.004, 0.077), (0.200, 0.090, 0.093))   # a box: min, max

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=glb)
src = [o for o in bpy.context.scene.objects if o.type == 'MESH'][0]
bpy.context.view_layer.objects.active = src
bpy.ops.object.select_all(action='DESELECT'); src.select_set(True)
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

# the original surface and its colour, kept for the colour transfer
me = src.data
me.calc_loop_triangles()
co = np.array([v.co[:] for v in me.vertices], np.float64)
tri_v = np.array([t.vertices[:] for t in me.loop_triangles], np.int64)
tri_l = np.array([t.loops[:] for t in me.loop_triangles], np.int64)
uvs = np.zeros(len(me.loops) * 2, np.float32); me.uv_layers.active.data.foreach_get('uv', uvs); uvs = uvs.reshape(-1, 2)
timg = None
for n in me.materials[0].node_tree.nodes:
    if n.type == 'TEX_IMAGE' and any(l.to_socket.name == 'Base Color' for l in n.outputs[0].links):
        timg = n.image
TS = 2048
ti = timg.copy(); ti.scale(TS, TS)
tex = np.array(ti.pixels[:], np.float32).reshape(TS, TS, 4)[:, :, :3]      # rows from v = 0
bvh_src = BVHTree.FromPolygons([tuple(map(float, c)) for c in co], [tuple(map(int, t)) for t in tri_v], all_triangles=True)
print('SOURCE vertices %d triangles %d texture %s' % (len(co), len(tri_v), tuple(timg.size)))

# a working copy: the hammer forward, the slot plugged
work = src.copy(); work.data = src.data.copy(); bpy.context.collection.objects.link(work)
bm = bmesh.new(); bm.from_mesh(work.data)
moved = 0
for v in bm.verts:
    if v.co.x > HAMMER['x_min'] and v.co.z > HAMMER['z_min']:
        v.co.x += HAMMER['shift_x']; moved += 1
(a0, a1) = SLOT_PLUG
bmesh.ops.create_cube(bm, size=1.0, matrix=mathutils.Matrix.Translation(((a0[0] + a1[0]) / 2, (a0[1] + a1[1]) / 2, (a0[2] + a1[2]) / 2)) @
                      mathutils.Matrix.Diagonal((a1[0] - a0[0], a1[1] - a0[1], a1[2] - a0[2], 1)))
bm.to_mesh(work.data); bm.free()
print('HAMMER moved %d vertices by %.3f' % (moved, HAMMER['shift_x']))

# one closed surface, then fewer triangles
bpy.context.view_layer.objects.active = work
bpy.ops.object.select_all(action='DESELECT'); work.select_set(True)
if MODE == 'remesh':
    work.data.remesh_voxel_size = VOXEL
    work.data.use_remesh_fix_poles = True
    bpy.ops.object.voxel_remesh()
    print('REMESH faces %d' % len(work.data.polygons))
elif MODE == 'weld':
    bm = bmesh.new(); bm.from_mesh(work.data)
    n0 = len(bm.verts)
    bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=WELD)
    bm.to_mesh(work.data); bm.free()
    print('WELD vertices %d -> %d' % (n0, len(work.data.vertices)))
# keep the largest part (bits the remesh left loose)
bm = bmesh.new(); bm.from_mesh(work.data); bm.verts.ensure_lookup_table()
seen, parts = set(), []
for v in bm.verts:
    if v.index in seen: continue
    st, comp = [v], []
    seen.add(v.index)
    while st:
        w = st.pop(); comp.append(w)
        for e in w.link_edges:
            x = e.other_vert(w)
            if x.index not in seen: seen.add(x.index); st.append(x)
    parts.append(comp)
parts.sort(key=len, reverse=True)
for comp in parts[1:]:
    if MODE == 'remesh' or len(comp) < 8:      # the welded soup keeps its pieces (the plug's 8), but not crumbs
        bmesh.ops.delete(bm, geom=comp, context='VERTS')
bm.to_mesh(work.data); bm.free()
print('PARTS %d, kept the largest (%d vertices)' % (len(parts), len(parts[0])))
# flat areas merged first (the gun is mostly flat panels: cutting a noisy flat surface down leaves dents), then
# collapsed to the budget, then split at sharp edges so the panels shade flat
FLAT_DEG = float(os.environ.get('GUN_FLAT_DEG', 0))
if FLAT_DEG > 0:
    mod = work.modifiers.new('flat', 'DECIMATE'); mod.decimate_type = 'DISSOLVE'
    mod.angle_limit = math.radians(FLAT_DEG)
    bpy.ops.object.modifier_apply(modifier='flat')
    bm = bmesh.new(); bm.from_mesh(work.data); bmesh.ops.triangulate(bm, faces=bm.faces[:]); bm.to_mesh(work.data); bm.free()
    print('FLAT merged: %d triangles' % len(work.data.polygons))
mod = work.modifiers.new('dec', 'DECIMATE'); mod.decimate_type = 'COLLAPSE'
mod.ratio = min(1.0, TARGET_TRIS / max(1, sum(len(p.vertices) - 2 for p in work.data.polygons)))
bpy.ops.object.modifier_apply(modifier='dec')
bm = bmesh.new(); bm.from_mesh(work.data); bmesh.ops.triangulate(bm, faces=bm.faces[:]); bm.to_mesh(work.data); bm.free()
mod = work.modifiers.new('split', 'EDGE_SPLIT'); mod.split_angle = math.radians(35); mod.use_edge_sharp = False
bpy.ops.object.modifier_apply(modifier='split')
work.data.calc_loop_triangles()
print('DECIMATED triangles %d, vertices %d (split at 35 degrees)' % (len(work.data.loop_triangles), len(work.data.vertices)))


def sample_colour(p):
    """the texture's colour at the original surface's nearest point to p"""
    loc, nrm, fi, dist = bvh_src.find_nearest(p)
    if fi is None: return (0.5, 0.5, 0.5)
    a, b, c = (mathutils.Vector(co[i]) for i in tri_v[fi])
    w = mathutils.geometry.barycentric_transform(loc, a, b, c, mathutils.Vector((1, 0, 0)), mathutils.Vector((0, 1, 0)), mathutils.Vector((0, 0, 1)))
    uv = w.x * uvs[tri_l[fi][0]] + w.y * uvs[tri_l[fi][1]] + w.z * uvs[tri_l[fi][2]]
    x = int((uv[0] % 1.0) * (TS - 1)); y = int((uv[1] % 1.0) * (TS - 1))
    # a few texels round it (the texture is noisy at this scale)
    pat = tex[max(0, y - 1):y + 2, max(0, x - 1):x + 2].reshape(-1, 3)
    return tuple(pat.mean(0))


# the vertices of the cut-down surface: position, normal, colour (the hammer's part samples where it came from)
wm = work.data
wm.calc_loop_triangles()
cols = []
for v in wm.vertices:
    p = v.co.copy()
    if p.x > HAMMER['x_min'] + HAMMER['shift_x'] - 0.002 and p.z > HAMMER['z_min']:
        p.x -= HAMMER['shift_x']
    cols.append(sample_colour(p))
cols = np.array(cols, np.float32)
print('COLOURS sampled, median %s' % np.median(cols, 0).round(3))

# the normals: the original surface's round each vertex (the cut-down surface wobbles a little; lit by the original's
# normals it shades as the original does), those facing the vertex's own side of a sharp edge only
NRM_R = 0.004
nrms = []
for v in wm.vertices:
    p = v.co.copy()
    if p.x > HAMMER['x_min'] + HAMMER['shift_x'] - 0.002 and p.z > HAMMER['z_min']:
        p.x -= HAMMER['shift_x']
    own = v.normal
    acc = mathutils.Vector((0, 0, 0))
    for loc, n, fi, dist in bvh_src.find_nearest_range(p, NRM_R):
        if n.dot(own) > 0.6: acc += n * (1 - dist / NRM_R)
    nrms.append(acc.normalized() if acc.length > 1e-6 else own.copy())
print('NORMALS from the original surface')

# ---- into the gun's space: model (x, y, z) -> gun (y, z, x) (muzzle -X -> -Z, up Z -> Y), scaled, placed
P = np.array([v.co[:] for v in wm.vertices], np.float64)
N = np.array([n[:] for n in nrms], np.float64)
I = np.array([t.vertices[:] for t in wm.loop_triangles], np.int64)
length = P[:, 0].max() - P[:, 0].min()
gs = GUN_LEN / length
G = np.stack([P[:, 1], P[:, 2], P[:, 0]], 1) * gs
GN = np.stack([N[:, 1], N[:, 2], N[:, 0]], 1)

# the trigger guard's opening, seen from the side (gun z, y): the enclosed hole of the silhouette
res = 0.0015
zmin, zmax = G[:, 2].min(), G[:, 2].max(); ymin, ymax = G[:, 1].min(), G[:, 1].max()
W, H = int((zmax - zmin) / res) + 3, int((ymax - ymin) / res) + 3
sil = np.zeros((H, W), bool)
for t in I:
    pts = G[t][:, [2, 1]]
    lo = np.floor((pts.min(0) - [zmin, ymin]) / res).astype(int); hi = np.ceil((pts.max(0) - [zmin, ymin]) / res).astype(int)
    gz, gy = np.meshgrid(np.arange(lo[0], hi[0] + 1), np.arange(lo[1], hi[1] + 1))
    qx = zmin + gz * res; qy = ymin + gy * res
    (ax, ay), (bx, by), (cx, cy) = pts
    den = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
    if abs(den) < 1e-12: continue
    w0 = ((by - cy) * (qx - cx) + (cx - bx) * (qy - cy)) / den
    w1 = ((cy - ay) * (qx - cx) + (ax - cx) * (qy - cy)) / den
    m = (w0 >= -0.05) & (w1 >= -0.05) & (1 - w0 - w1 >= -0.05)
    sil[gy[m], gz[m]] = True
# flood the outside from the border; what stays empty is enclosed
outside = np.zeros_like(sil)
stack = [(0, x) for x in range(W)] + [(H - 1, x) for x in range(W)] + [(y, 0) for y in range(H)] + [(y, W - 1) for y in range(H)]
while stack:
    y, x = stack.pop()
    if y < 0 or y >= H or x < 0 or x >= W or outside[y, x] or sil[y, x]: continue
    outside[y, x] = True
    stack += [(y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)]
hole = ~sil & ~outside
hy, hz = np.nonzero(hole)
# the biggest enclosed region (the guard), not a screw hole
lab = -np.ones_like(hy)
regions = []
hole_set = {(int(a), int(b)) for a, b in zip(hy, hz)}
seenh = set()
for a, b in zip(hy, hz):
    if (a, b) in seenh: continue
    st, reg = [(int(a), int(b))], []
    seenh.add((int(a), int(b)))
    while st:
        p = st.pop(); reg.append(p)
        for q in ((p[0] + 1, p[1]), (p[0] - 1, p[1]), (p[0], p[1] + 1), (p[0], p[1] - 1)):
            if q in hole_set and q not in seenh: seenh.add(q); st.append(q)
    regions.append(reg)
regions.sort(key=len, reverse=True)
guard = np.array(regions[0])
g_y = ymin + guard[:, 0] * res; g_z = zmin + guard[:, 1] * res
print('GUARD opening: z %.4f..%.4f  y %.4f..%.4f (%d cells)' % (g_z.min(), g_z.max(), g_y.min(), g_y.max(), len(guard)))

# the trigger (gun_trigger.py), into the opening: its top tucked into the frame over the opening, a third of the way
# back from the opening's front (plus half a trigger), centred across
g_front = g_z.min(); g_back = g_z.max()
x_mid = (G[:, 0].min() + G[:, 0].max()) / 2
top_y, top_z = g_y.max() + 0.0045, g_front + 0.30 * (g_back - g_front) + 0.012
TP, TN, TT = make_trigger(top_z, top_y, x_mid)
tri_t = [tuple(int(i) for i in t) for t in TT]
remap = {k: k for k in range(len(TP))}
print('TRIGGER %d vertices, top %.4f, middle %.4f' % (len(TP), top_y, top_z))

# placed in the hand: the trigger's middle (where the finger rests) on the pistol's
tc = np.array([x_mid, top_y - 0.0107, top_z])
off = np.array([-x_mid, PISTOL_TRIGGER[0] - tc[1], PISTOL_TRIGGER[1] - tc[2]])
G += off; TP += off
# the muzzle: the front-most centimetre's centre
front = G[:, 2].min()
mz = G[G[:, 2] < front + 0.004]
tip = (float(mz[:, 0].mean()), float(mz[:, 1].mean()), float(front))
print('PLACED: bounds %s %s, tip %s' % (G.min(0).round(3), G.max(0).round(3), np.round(tip, 4)))

# ---- colours: the body's red, and the blue from the photo
body = np.median(cols, 0)
def photo_body(path, kind):
    """a gun photo's lit body colour (sRGB): the strongly coloured pixels round the 70th percentile of brightness"""
    img = bpy.data.images.load(path)
    flat = np.array(img.pixels[:], np.float32).reshape(-1, img.channels)[:, :3]
    mx = flat.max(1); mn = flat.min(1)
    sat = (mx - mn) / (mx + 1e-6)
    if kind == 'blue': m = (flat[:, 2] > flat[:, 0] * 1.4) & (flat[:, 2] > flat[:, 1] * 1.05)
    else: m = (flat[:, 0] > flat[:, 1] * 1.6) & (flat[:, 0] > flat[:, 2] * 1.6)
    b = flat[m & (sat > 0.35) & (mx > 0.25)]
    L = b.max(1)
    return np.median(b[(L >= np.percentile(L, 65)) & (L <= np.percentile(L, 75))], 0)


def lin(c): return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
def srgb(c): return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(np.clip(c, 0, None), 1 / 2.4) - 0.055)


p_blue, p_red = photo_body(blue_photo, 'blue'), photo_body(red_photo, 'red')
gain = lin(body).max() / lin(p_red).max()
blue = srgb(np.clip(lin(p_blue) * gain, 0, 1))
print('BLUE: the photo %s, its red %s, the model red %s: x%.2f -> %s' % (p_blue.round(3), p_red.round(3), body.round(3), gain, blue.round(3)))


def lum(c): return 0.2126 * c[..., 0] + 0.7152 * c[..., 1] + 0.0722 * c[..., 2]


def recolour(c, target):
    """each vertex's colour as the target, its brightness relative to the body's kept"""
    k = (lum(c) / max(lum(body), 1e-4))[:, None]
    return np.clip(target[None, :] * k, 0, 1)


def write(path, colours, trig_colour):
    with open(path, 'wb') as f:
        f.write(b'TCGUN002')
        f.write(struct.pack('<II', len(G) + len(TP), (len(I) + len(tri_t)) * 3))
        f.write(struct.pack('<3f', *tip))
        for p, n, c in zip(G, GN, colours):
            f.write(struct.pack('<10f', *p, *n, *c, 0.0))
        for p, n in zip(TP, TN):
            f.write(struct.pack('<10f', *p, *n, *trig_colour, 1.0))
        f.write(struct.pack('<%dI' % (len(I) * 3), *I.flatten()))
        f.write(struct.pack('<%dI' % (len(tri_t) * 3), *[remap[int(i)] + len(G) for t in tri_t for i in t]))
    print('WROTE %s: %d vertices, %d triangles' % (path, len(G) + len(TP), len(I) + len(tri_t)))


red_cols = np.clip(cols, 0, 1)
blue_cols = recolour(cols, blue)
write(out + '/arcade-gun.tcgun', red_cols, tuple(body * 0.85))
write(out + '/arcade-gun-blue.tcgun', blue_cols, tuple(blue * 0.85))
np.savez(out + '/gun_preview.npz', G=G, I=I, TP=TP, TI=np.array([[remap[int(i)] for i in t] for t in tri_t]), red=red_cols, blue=blue_cols)

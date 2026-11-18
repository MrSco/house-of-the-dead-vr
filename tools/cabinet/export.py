# Blender (headless): Songti's "The House of the Dead Arcade cabinet" (CC-BY 4.0) into the Quest app's formats.
#   blender -b --factory-startup --python export.py -- GLTF OUTDIR [CHECKDIR] [--art=ATLASDIR]
#
# --art: art_atlas.py's output (sharper marquee and side art): the texture becomes its 2048 atlas, the model's own
# texture in its top-left quarter, and the faces that carried the old art take their new places (uvmap.json).
#
# cabinet.tccab ("TCCAB001", little-endian):
#   magic[8], uint32 vertices, uint32 indices, uint32 texture size (square, base colour then emissive),
#   float screen[5]: centre x, y, z and half width, half height (the game's 4:3 picture),
#   float anchor[6]: where the guns' cords leave the cabinet (the control panel's cord ports; the model's own cords
#   are left out, the app draws them), the left one then the right,
#   then per vertex position[3], normal[3], uv[2] (floats), uint32 indices, RGBA8 base colour, RGBA8 emissive
#   (rows from v = 0). Space: metres, +Y up, the floor at y = 0, the screen's plane at z = 0, the cabinet's front
#   towards +Z (the player).
# arcade-gun.tcgun, arcade-gun-blue.tcgun (TCGUN002): player 1's red gun and player 2's blue; metres, +Y up, the
#   muzzle towards -Z, the barrel parallel to the Z axis. Placed in the hand as the pistol (player-gun.tcgun) is:
#   its trigger where the pistol's is, so the hands (hands.tchand, posed on the pistol) close round its grip; the
#   barrel's line then runs above the origin, and the muzzle (the file's tip) says where. Per vertex position,
#   normal, colour (from the texture), part (1: the trigger).
import bpy, bmesh, sys, struct, math, mathutils

argv = sys.argv[sys.argv.index('--') + 1:]
art_dir = next((a.split('=', 1)[1] for a in argv if a.startswith('--art=')), None)
argv = [a for a in argv if not a.startswith('--art=')]
gltf, out = argv[0], argv[1]
check = argv[2] if len(argv) > 2 else None
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=gltf)
scene = bpy.context.scene
for o in scene.objects:
    o.animation_data_clear()
bpy.context.view_layer.update()
cab = scene.objects['Mesh_lambert6_0']
gun = scene.objects['group9_lambert4_0']   # the red gun (right holster)

CAB_SCALE = 0.44            # the cabinet 2.09 m tall, its gun 28 cm long (the real one's size)
SCREEN_Y_B = -0.99          # the screen's plane, Blender y (the recess behind the bezel)
SCREEN_Z_B = 3.018          # its centre's height, Blender z (the opening's middle)
SCREEN_W_B, SCREEN_H_B = 1.70, 1.275   # 4:3 inside the opening (1.85 x 1.57)
lo_z = min((cab.matrix_world @ mathutils.Vector(c)).z for c in cab.bound_box)


def cab_pos(p):             # Blender -> app: x, up = z, towards the player = -y
    return ((p.x) * CAB_SCALE, (p.z - lo_z) * CAB_SCALE, -(p.y - SCREEN_Y_B) * CAB_SCALE)


def cab_nrm(n):
    return (n.x, n.z, -n.y)


def mesh_tris(o, posf, nrmf, skip=frozenset(), uvf=None):
    """(vertices: [(pos, nrm, uv)], indices) from o's evaluated mesh, world space, split at uv/normal seams; the
    triangles of the vertices in skip left out; uvf(centre, normal, uvs, corners) -> uvs: a triangle's new uvs
    (Blender's world space)."""
    me = o.data
    me.calc_loop_triangles()
    mw = o.matrix_world
    nm = mw.to_3x3().inverted().transposed()
    uv = me.uv_layers.active.data
    verts, index, idx = [], {}, []
    for t in me.loop_triangles:
        if t.vertices[0] in skip: continue
        tuvs = [tuple(uv[li].uv) for li in t.loops]
        if uvf:
            ws = [mw @ me.vertices[v].co for v in t.vertices]
            c = (ws[0] + ws[1] + ws[2]) / 3
            fn = (ws[1] - ws[0]).cross(ws[2] - ws[0]).normalized()
            tuvs = uvf(c, fn, tuvs, ws)
        for k, li in enumerate(t.loops):
            l = me.loops[li]
            p = posf(mw @ me.vertices[l.vertex_index].co)
            n = nrmf((nm @ me.corner_normals[li].vector).normalized())
            u = tuvs[k]
            key = (tuple(round(c, 6) for c in p), tuple(round(c, 4) for c in n), tuple(round(c, 6) for c in u))
            if key not in index:
                index[key] = len(verts)
                verts.append((p, n, u, me.vertices[l.vertex_index].index))
            idx.append(index[key])
    return verts, idx


def texture(o, kind, size):
    for n in o.active_material.node_tree.nodes:
        if n.type == 'TEX_IMAGE' and kind in n.image.name:
            img = n.image.copy()
            img.scale(size, size)
            px = img.pixels[:]
            return bytes(max(0, min(255, int(round(c * 255)))) for c in px), n.image
    return bytes([0, 0, 0, 255]) * size * size, None


def components(o):
    """o's mesh's connected parts: lists of vertex indices"""
    bm = bmesh.new(); bm.from_mesh(o.data); bm.verts.ensure_lookup_table()
    seen, parts = set(), []
    for v in bm.verts:
        if v.index in seen: continue
        st, comp = [v], []
        seen.add(v.index)
        while st:
            w = st.pop(); comp.append(w.index)
            for e in w.link_edges:
                x = e.other_vert(w)
                if x.index not in seen: seen.add(x.index); st.append(x)
        parts.append(comp)
    bm.free()
    return parts


# ---- the cabinet
TEX = 1024
base, _ = texture(cab, 'baseColor', TEX)
emis, _ = texture(cab, 'emissive', TEX)
# its guns' coiled cords (the app draws its own, to the gun in your hand): the strongly coloured (red, blue) parts
# between the control panel's cord ports and the holsters
uv_of = {}
for l in cab.data.loops: uv_of[l.vertex_index] = cab.data.uv_layers.active.data[l.index].uv
cmw = cab.matrix_world
cord_verts, ports = set(), []
for comp in components(cab):
    ps = [cmw @ cab.data.vertices[i].co for i in comp]
    cs = []
    for i in comp:
        u = uv_of[i]
        x = int((u[0] % 1.0) * (TEX - 1)); y = int((u[1] % 1.0) * (TEX - 1))
        k = (y * TEX + x) * 4
        cs.append([base[k + j] / 255.0 for j in range(3)])
    c = [sum(q[j] for q in cs) / len(cs) for j in range(3)]
    in_cords = (min(abs(q.x) for q in ps) > 0.35 and max(abs(q.x) for q in ps) < 0.82 and
                min(q.z for q in ps) > 1.0 and max(q.z for q in ps) < 1.95 and min(q.y for q in ps) > -2.25)
    # (and a dimmer scrap of one floating clear of the cabinet's front, y -1.56: a red sliver by the right holster)
    if in_cords and ((max(c) - min(c) > 0.25 and max(q.y for q in ps) < -1.2) or
                     (max(c) - min(c) > 0.15 and max(q.y for q in ps) < -1.6)):
        cord_verts.update(comp)
        ports.extend(ps)
# the sharper art (--art): the atlas for the texture, the art's faces' uvs moved to their new places
uv_new = None
if art_dir:
    import json
    amap = json.load(open(art_dir + '/uvmap.json'))
    OLDT, NEWT = amap['old'], amap['new']

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

    def on_faces(kind, c, fn):
        """the faces that carried a piece of art (Blender's world space)"""
        if kind == 'marquee': return c.z > 4.0 and fn.y < -0.5
        if kind == 'card': return abs(c.x) < 0.4 and 1.8 < c.z < 2.15 and fn.z > 0.5   # the card, facing up and out
        if kind == 'panel': return abs(c.x) < 1.0 and 1.3 < c.z < 2.25 and -1.6 < c.y < -0.85   # (and the rectangles)
        if kind == 'side-': return c.x < -1.0 and fn.x < -0.9
        if kind == 'side+': return c.x > 1.0 and fn.x > 0.9
        return False

    def uv_new(c, fn, uvs, ws):
        olds = [(u * OLDT, (1 - v) * OLDT) for u, v in uvs]      # the old texture's pixels, rows from the top
        for pc in amap['pieces']:
            if not on_faces(pc['faces'], c, fn): continue
            # the control panel's front face (upright, over the holsters): one dark grey texel, the cabinet front's own
            # (30, 30, 31; atlas pixel (256, 20), rows from the top: a plain 5 x 5 patch of the model's texture, no
            # glow). The picture stops at the rounded edge: its last row ran down the face, and blurred (the face
            # seen at a slant) with the atlas's other edge, smeared red and black. One texel: no blur at any distance.
            if pc['faces'] == 'panel' and fn.y < -0.9 and c.z < 1.655 and abs(c.y + 1.484) < 0.006:   # (not the
                # holsters' mounting plates in front of it)
                return [((256 + 0.5) / NEWT, 1 - (20 + 0.5) / NEWT)] * 3
            if 'planar' in pc:                                     # projected: two axes -> atlas pixels
                if 'old' in pc:                                    # (only the faces that carried the old art)
                    rects = pc['old'] if isinstance(pc['old'][0], list) else [pc['old']]
                    if not any(all(x0 - 1 <= x <= x1 + 1 and y0 - 1 <= y <= y1 + 1 for x, y in olds)
                               for x0, y0, x1, y1 in rects): continue
                P = pc['planar']
                a0, a1 = pc.get('axes', (1, 2))
                res = []
                for w in ws:
                    q0 = w[a0] if a0 != 'profile' else unroll(pc['profile'], w[1], w[2])
                    q1 = w[a1] if a1 != 'profile' else unroll(pc['profile'], w[1], w[2])
                    X = P[0][0] * q0 + P[0][1] * q1 + P[0][2]
                    Y = P[1][0] * q0 + P[1][1] * q1 + P[1][2]
                    if 'vclamp' in pc: Y = min(max(Y, pc['vclamp'][0]), pc['vclamp'][1])
                    res.append((X / NEWT, 1 - Y / NEWT))
                return res
            x0, y0, x1, y1 = pc['old']
            if not all(x0 - 1 <= x <= x1 + 1 and y0 - 1 <= y <= y1 + 1 for x, y in olds): continue
            m = pc['m']
            res = []
            for x, y in olds:
                w = m[2][0] * x + m[2][1] * y + m[2][2]
                X = (m[0][0] * x + m[0][1] * y + m[0][2]) / w
                Y = (m[1][0] * x + m[1][1] * y + m[1][2]) / w
                res.append((X / NEWT, 1 - Y / NEWT))
            return res
        return [(x / NEWT, 1 - y / NEWT) for x, y in olds]       # the rest: the old texture, the top-left quarter

    def image_bytes(path):
        """a picture file's RGBA8 bytes, rows from v = 0 (Blender's order), and its size"""
        img = bpy.data.images.load(path)
        px = img.pixels[:]
        return bytes(max(0, min(255, int(round(c * 255)))) for c in px), img.size[0]

    base, TEX = image_bytes(art_dir + '/atlas_base.png')
    emis, _ = image_bytes(art_dir + '/atlas_emissive.png')
    print('ART atlas %d from %s: %s' % (TEX, art_dir, ', '.join('%s (%s)' % (q['name'], q['art']) for q in amap['pieces'])))
verts, idx = mesh_tris(cab, cab_pos, cab_nrm, frozenset(cord_verts), uv_new)
scr = (0.0, (SCREEN_Z_B - lo_z) * CAB_SCALE, 0.004, SCREEN_W_B / 2 * CAB_SCALE, SCREEN_H_B / 2 * CAB_SCALE)
# the cords leave the cabinet where the model's did: their ends nearest the cabinet (the panel's ports)
anchors = []
for side in (-1, 1):                                   # left (the blue gun's), right (the red's)
    ps = [q for q in ports if q.x * side > 0]
    top = max(q.y for q in ps)
    end = [q for q in ps if q.y > top - 0.03]
    p = mathutils.Vector((sum(q.x for q in end), sum(q.y for q in end), sum(q.z for q in end))) / len(end)
    anchors.extend(cab_pos(p))
print('CORDS left out: %d vertices' % len(cord_verts))
with open(out + '/cabinet.tccab', 'wb') as f:
    f.write(b'TCCAB001')
    f.write(struct.pack('<III', len(verts), len(idx), TEX))
    f.write(struct.pack('<5f', *scr))
    f.write(struct.pack('<6f', *anchors))
    for p, n, u, _ in verts:
        f.write(struct.pack('<8f', *p, *n, *u))
    f.write(struct.pack('<%dI' % len(idx), *idx))
    f.write(base)
    f.write(emis)
lo = [min(v[0][k] for v in verts) for k in range(3)]
hi = [max(v[0][k] for v in verts) for k in range(3)]
print('CABINET vertices %d triangles %d bounds %s %s screen %s anchors %s' % (
    len(verts), len(idx) // 3, [round(v, 3) for v in lo], [round(v, 3) for v in hi], [round(v, 3) for v in scr],
    [round(v, 3) for v in anchors]))

# ---- the guns: player 1's red (right holster) and player 2's blue (left)
# The model's guns stand in their holsters muzzle down: the barrel runs down Blender's -Z, the grip out towards -Y.
# So (x, y, z) -> (x, y, z) about a point, scaled: the muzzle to -Z, the grip down, no turn.
GUN_LEN = 0.26                     # muzzle to back (the real gun's size)
PISTOL_TRIGGER = (-0.0415, -0.040)  # the pistol's trigger (y, z): where the hands' fingers close (player-gun.tcgun)


def export_gun(gun, fname):
    mw = gun.matrix_world
    gpts = [mw @ v.co for v in gun.data.vertices]
    lo_z, hi_z = min(p.z for p in gpts), max(p.z for p in gpts)
    gs = GUN_LEN / (hi_z - lo_z)
    muzzle = [p for p in gpts if p.z < lo_z + 0.02]
    mx = sum(p.x for p in muzzle) / len(muzzle)
    my = sum(p.y for p in muzzle) / len(muzzle)
    # the trigger: the loose part inside the trigger guard (found by its place: centre near y -1.81, z 1.60, about
    # 0.13 tall, 105 vertices)
    trigger = set()
    for comp in components(gun):
        ps = [mw @ gun.data.vertices[i].co for i in comp]
        c = mathutils.Vector((sum(p.x for p in ps), sum(p.y for p in ps), sum(p.z for p in ps))) / len(ps)
        ext = max(p.z for p in ps) - min(p.z for p in ps)
        if abs(c.y - (-1.809)) < 0.03 and abs(c.z - 1.602) < 0.04 and ext < 0.16 and len(comp) > 40:
            trigger.update(comp)
    tps = [mw @ gun.data.vertices[i].co for i in trigger]
    ty = sum(p.y for p in tps) / len(tps)
    tz = sum(p.z for p in tps) / len(tps)
    oy = ty - PISTOL_TRIGGER[0] / gs            # the origin: the trigger lands on the pistol's
    oz = tz - PISTOL_TRIGGER[1] / gs
    tip = (0.0, (my - oy) * gs, (lo_z - oz) * gs)
    print(fname, 'GUN scale %.4f trigger verts %d barrel y %.4f tip z %.4f' % (gs, len(trigger), tip[1], tip[2]))

    def gun_pos(p):
        return ((p.x - mx) * gs, (p.y - oy) * gs, (p.z - oz) * gs)

    def gun_nrm(n):
        return (n.x, n.y, n.z)

    gv, gi = mesh_tris(gun, gun_pos, gun_nrm)
    gtex, gimg = texture(gun, 'baseColor', 256)
    GT = 256

    def colour_at(u):
        x = int((u[0] % 1.0) * (GT - 1)); y = int((u[1] % 1.0) * (GT - 1))
        k = (y * GT + x) * 4
        return tuple(gtex[k + j] / 255.0 for j in range(3))

    with open(out + '/' + fname, 'wb') as f:
        f.write(b'TCGUN002')
        f.write(struct.pack('<II', len(gv), len(gi)))
        f.write(struct.pack('<3f', *tip))
        for p, n, u, vi in gv:
            f.write(struct.pack('<10f', *p, *n, *colour_at(u), 1.0 if vi in trigger else 0.0))
        f.write(struct.pack('<%dI' % len(gi), *gi))
    glo = [min(v[0][k] for v in gv) for k in range(3)]
    ghi = [max(v[0][k] for v in gv) for k in range(3)]
    print(fname, 'GUNOUT vertices %d triangles %d bounds %s %s' % (len(gv), len(gi) // 3,
          [round(v, 3) for v in glo], [round(v, 3) for v in ghi]))


export_gun(gun, 'arcade-gun.tcgun')
export_gun(scene.objects['group8_lambert4_0'], 'arcade-gun-blue.tcgun')

# ---- a check render: the cabinet lit, the screen's rectangle drawn on it
if check:
    for o in list(scene.objects):
        if o.type == 'MESH' and o.name not in ('Mesh_lambert6_0', 'group8_lambert4_0', 'group9_lambert4_0'):
            bpy.data.objects.remove(o, do_unlink=True)
    w, h = SCREEN_W_B / 2, SCREEN_H_B / 2
    me = bpy.data.meshes.new('screen')
    me.from_pydata([(-w, SCREEN_Y_B - 0.006, SCREEN_Z_B - h), (w, SCREEN_Y_B - 0.006, SCREEN_Z_B - h),
                    (w, SCREEN_Y_B - 0.006, SCREEN_Z_B + h), (-w, SCREEN_Y_B - 0.006, SCREEN_Z_B + h)], [], [(0, 1, 2, 3)])
    so = bpy.data.objects.new('screen', me); scene.collection.objects.link(so)
    mat = bpy.data.materials.new('scr'); mat.diffuse_color = (0.1, 0.9, 0.2, 1); me.materials.append(mat)
    scene.render.engine = 'BLENDER_WORKBENCH'
    scene.display.shading.light = 'STUDIO'
    scene.display.shading.color_type = 'TEXTURE'
    scene.render.resolution_x, scene.render.resolution_y = 800, 1000
    cd = bpy.data.cameras.new('c'); cam = bpy.data.objects.new('c', cd); scene.collection.objects.link(cam); scene.camera = cam
    for name, (az, el) in {'check_front': (0, 8), 'check_34': (40, 18)}.items():
        a, e = math.radians(az), math.radians(el)
        target = mathutils.Vector((0, -1.0, 2.2))
        cam.location = target + mathutils.Vector((math.sin(a) * math.cos(e), -math.cos(a) * math.cos(e), math.sin(e))) * 9
        cam.rotation_euler = (target - cam.location).to_track_quat('-Z', 'Y').to_euler()
        scene.render.filepath = check + '/' + name + '.png'
        bpy.ops.render.render(write_still=True)

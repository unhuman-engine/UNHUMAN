"""Procedural card fir: a bark trunk, whorls of drooping limbs, and layered spray cards cut from
Poly Haven's fir_tree_01 twig atlas. run after lib.py; call make_fir(name, seed, height, detail)."""

# Spray boxes in the twig atlas, image space (u0, u1, v_top, v_bottom); stems sit at v_bottom.
SPRAYS = [
    (0.297, 0.656, 0.391, 0.789),  # large, five-fingered
    (0.621, 0.969, 0.441, 0.844),  # large
    (0.641, 0.949, 0.031, 0.383),  # medium
    (0.176, 0.441, 0.035, 0.324),  # medium
    (0.480, 0.645, 0.250, 0.414),  # small
]
FIR_TEX = os.path.join(PH, "fir_tree_01", "textures")


def fir_materials():
    twig = pbr_material("Fir_Needles", os.path.join(FIR_TEX, "fir_tree_01_twig_diff_2k_rgba.png"),
                        os.path.join(FIR_TEX, "fir_tree_01_twig_nor_gl_2k.jpg"),
                        os.path.join(FIR_TEX, "fir_tree_01_twig_arm_2k.jpg"), alpha_clip=True, double=True)
    bark = pbr_material("Fir_Bark", os.path.join(FIR_TEX, "fir_tree_01_bark_diff_2k.jpg"),
                        os.path.join(FIR_TEX, "fir_tree_01_bark_nor_gl_2k.jpg"),
                        os.path.join(FIR_TEX, "fir_tree_01_bark_arm_2k.jpg"))
    return twig, bark


def _frame(d):
    """Orthonormal (side, up) for a direction d."""
    d = d.normalized()
    side = d.cross(Vector((0, 0, 1)))
    if side.length < 1e-4:
        side = Vector((1, 0, 0))
    side.normalize()
    up = side.cross(d).normalized()
    return side, up


def make_fir(name, seed=1, height=16.0, detail=1.0):
    rnd = random.Random(seed)
    twig, bark = fir_materials()
    bm = bmesh.new()
    uvl = bm.loops.layers.uv.new("UVMap")
    normals = {}  # vert -> custom normal

    def quad(vs, uvs, mat, nrm=None):
        bv = [bm.verts.new(v) for v in vs]
        f = bm.faces.new(bv)
        f.material_index = mat
        for l, uv in zip(f.loops, uvs):
            l[uvl].uv = uv
        if nrm is not None:
            for v, n in zip(bv, nrm):
                normals[v] = n
        return f

    def tube(p0, p1, r0, r1, sides, mat, vrep=1.0):
        d = (p1 - p0)
        side, up = _frame(d)
        ring = lambda p, r: [p + (side * math.cos(a) + up * math.sin(a)) * r
                             for a in [2 * math.pi * i / sides for i in range(sides + 1)]]
        a, b = ring(p0, r0), ring(p1, r1)
        L = d.length
        for i in range(sides):
            u0, u1 = i / sides, (i + 1) / sides
            quad([a[i], a[i + 1], b[i + 1], b[i]], [(u0, 0), (u1, 0), (u1, L * vrep), (u0, L * vrep)], mat)

    H = height
    base_r = 0.012 * H + 0.08
    crown_base = H * rnd.uniform(0.10, 0.18)
    # Trunk in segments, with a slight lean.
    lean = Vector((rnd.uniform(-1, 1), rnd.uniform(-1, 1), 0)) * 0.015
    seg = 8
    pts = [Vector((0, 0, 0)) + lean * (H * i / seg) ** 1.3 + Vector((0, 0, H * i / seg)) for i in range(seg + 1)]
    pts[0].z = -0.4
    for i in range(seg):
        r0 = base_r * (1 - i / seg) ** 0.9 + 0.015
        r1 = base_r * (1 - (i + 1) / seg) ** 0.9 + 0.01
        tube(pts[i], pts[i + 1], r0 * (1.25 if i == 0 else 1.0), r1, 10, 1, vrep=0.5)

    def trunk_at(z):
        t = max(0.0, min(1.0, z / H))
        return lean * max(z, 0.0) ** 1.3 + Vector((0, 0, z))

    Lmax = H * rnd.uniform(0.20, 0.25)
    spacing = 0.42 / detail ** 0.5
    z = crown_base
    whorl = 0
    while z < H - 0.25:
        t = (z - crown_base) / (H - crown_base)
        n_br = rnd.randint(5, 7)
        a0 = rnd.uniform(0, 2 * math.pi)
        for k in range(n_br):
            az = a0 + 2 * math.pi * k / n_br + rnd.uniform(-0.25, 0.25)
            L = (Lmax * (1 - t) ** 0.85 + 0.35) * rnd.uniform(0.8, 1.1)
            if t < 0.15:
                L *= 0.85 + t
            pitch = math.radians(-28 + 45 * t + rnd.uniform(-8, 8))
            droop = math.radians(-18 * (1 - t))
            root = trunk_at(z)
            dir0 = Vector((math.cos(az) * math.cos(pitch), math.sin(az) * math.cos(pitch), math.sin(pitch)))
            # Limb: 3 segments bending down.
            nseg = 3
            p = root.copy()
            limb = [p.copy()]
            d = dir0.copy()
            for s in range(nseg):
                d = (d + Vector((0, 0, math.sin(droop) / nseg))).normalized()
                p = p + d * (L / nseg)
                limb.append(p.copy())
            lr = 0.012 + 0.025 * L / Lmax
            for s in range(nseg):
                tube(limb[s], limb[s + 1], lr * (1 - s / nseg) + 0.006, lr * (1 - (s + 1) / nseg) + 0.004, 4, 1, vrep=1.0)
            # Sprays along the limb.
            step = 0.15 / detail
            s = 0.18 * L
            while s <= L:
                f = s / L
                seg_i = min(nseg - 1, int(f * nseg))
                fl = f * nseg - seg_i
                pos = limb[seg_i].lerp(limb[seg_i + 1], fl)
                bdir = (limb[seg_i + 1] - limb[seg_i]).normalized()
                for sidef in (-1, 1, 0) if rnd.random() < 0.7 else (-1, 1):
                    yaw = math.radians(rnd.uniform(35, 75)) * sidef
                    side, up = _frame(bdir)
                    sd = (bdir * math.cos(yaw) + side * math.sin(yaw)).normalized()
                    sd.z += rnd.uniform(-0.15, 0.25)
                    sd.normalize()
                    box = SPRAYS[rnd.choice([0, 0, 1, 1, 2, 3, 4] if f > 0.3 else [2, 3, 4, 4])]
                    size = rnd.uniform(0.45, 0.7) * (0.75 + 0.5 * (1 - t))
                    aspect = (box[1] - box[0]) / (box[3] - box[2])
                    w = size * aspect * 0.5
                    sside, sup = _frame(sd)
                    roll = rnd.uniform(-0.35, 0.35)
                    sside = (sside * math.cos(roll) + sup * math.sin(roll)).normalized()
                    sup = sside.cross(sd).normalized()
                    fold = 0.12 * size  # V-fold along the spray axis
                    base = pos - sd * 0.04
                    tip = base + sd * size
                    mid0, mid1 = base + sup * fold, tip + sup * fold
                    u0, u1, vt, vb = box
                    um = (u0 + u1) / 2
                    # Blender UV v is up: image v -> 1 - v.
                    uv = lambda uu, vv: (uu, 1 - vv)
                    # Canopy normal: away from the trunk axis and up.
                    out = (pos - trunk_at(pos.z))
                    out.z = 0
                    cn = (out.normalized() * 0.8 + Vector((0, 0, 0.75))).normalized() if out.length > 1e-3 else Vector((0, 0, 1))
                    l0, l1 = base - sside * w, tip - sside * w
                    r0, r1 = base + sside * w, tip + sside * w
                    quad([l0, mid0, mid1, l1], [uv(u0, vb), uv(um, vb), uv(um, vt), uv(u0, vt)], 0, [cn] * 4)
                    quad([mid0, r0, r1, mid1], [uv(um, vb), uv(u1, vb), uv(u1, vt), uv(um, vt)], 0, [cn] * 4)
                s += step * rnd.uniform(0.8, 1.2)
        # Top: a few upright sprays.
        z += spacing * rnd.uniform(0.85, 1.15) * (0.75 + 0.5 * (1 - t))
        whorl += 1
    # Leader: upright sprays round the top of the trunk, so no tree ends in a bare spike.
    zz = H - 3.2
    while zz < H - 0.3:
        f = (zz - (H - 3.2)) / 3.2
        for k in range(3):
            az = rnd.uniform(0, 2 * math.pi)
            d = Vector((math.cos(az) * 0.8, math.sin(az) * 0.8, 1.1 + f)).normalized()
            sside, sup = _frame(d)
            u0, u1, vt, vb = SPRAYS[rnd.choice([2, 3, 4])]
            size = rnd.uniform(0.45, 0.65) * (1.2 - 0.5 * f)
            w = size * 0.42
            base = trunk_at(zz)
            tip = base + d * size
            quad([base - sside * w, base + sside * w, tip + sside * w, tip - sside * w],
                 [(u0, 1 - vb), (u1, 1 - vb), (u1, 1 - vt), (u0, 1 - vt)], 0,
                 [(d + Vector((0, 0, 0.5))).normalized()] * 4)
        zz += 0.28
    for k in range(5):
        az = 2 * math.pi * k / 5
        top = trunk_at(H)
        d = Vector((math.cos(az) * 0.35, math.sin(az) * 0.35, 1)).normalized()
        sside, sup = _frame(d)
        box = SPRAYS[2]
        size = 0.7
        u0, u1, vt, vb = box
        w = size * 0.4
        base = top - Vector((0, 0, 0.6))
        tip = base + d * size
        quad([base - sside * w, base + sside * w, tip + sside * w, tip - sside * w],
             [(u0, 1 - vb), (u1, 1 - vb), (u1, 1 - vt), (u0, 1 - vt)], 0, [Vector((0, 0, 1))] * 4)

    me = bpy.data.meshes.get(name)
    if me:
        bpy.data.meshes.remove(me)
    me = bpy.data.meshes.new(name)
    vlist = list(bm.verts)
    nlist = [normals.get(v) for v in vlist]
    bm.normal_update()
    nlist = [n if n is not None else v.normal.copy() for v, n in zip(vlist, nlist)]
    bm.to_mesh(me)
    bm.free()
    me.materials.append(twig)
    me.materials.append(bark)
    for p in me.polygons:
        p.use_smooth = True
    # Custom normals per vertex (cards keep the canopy normal; bark keeps its own).
    me.normals_split_custom_set_from_vertices([tuple(n) for n in nlist])
    return me

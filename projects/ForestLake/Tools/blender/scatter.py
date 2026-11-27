"""Step 2: the fir library (card trees from firgen.py) and every placement: forest, rocks, wood, ferns, grass."""
import json

CAM = Vector((-6.0, 8.0))
LOOK = Vector((8.0, 60.0))
VIEW = (LOOK - CAM).normalized()

lib = collection("Library")
bpy.context.view_layer.layer_collection.children["Library"].exclude = True

# Remove earlier tests and placements.
for cname in ("Forest", "Props", "Plants"):
    c = bpy.data.collections.get(cname)
    if c:
        for o in list(c.objects):
            bpy.data.objects.remove(o)
for o in [o for o in lib.objects]:
    bpy.data.objects.remove(o)
for m in [m for m in bpy.data.meshes if m.users == 0]:
    bpy.data.meshes.remove(m)


def centre_mesh(o):
    """Bakes the object's transform into its mesh and moves the origin to the bottom centre."""
    me = o.data.copy()
    me.transform(o.matrix_world)
    xs = [v.co.x for v in me.vertices]; ys = [v.co.y for v in me.vertices]; zs = [v.co.z for v in me.vertices]
    me.transform(Matrix.Translation((-(min(xs) + max(xs)) / 2, -(min(ys) + max(ys)) / 2, -min(zs))))
    o.parent = None
    o.matrix_world = Matrix.Identity(4)
    o.data = me
    return o


def load_asset(name, swap_alpha=False):
    objs = import_gltf(name, lib)
    mats = ph_materials(name) if swap_alpha else {}
    meshes = []
    for o in objs:
        if o.type != 'MESH':
            continue
        if swap_alpha:
            for s in o.material_slots:
                b = s.material.name.split(".")[0] if s.material else ""
                if b in mats:
                    s.material = mats[b]
        meshes.append(centre_mesh(o))
    for o in objs:
        if o.type != 'MESH':
            bpy.data.objects.remove(o)
    for i, o in enumerate(meshes):
        o.name = f"{name}_{i}"
    return meshes


A = {}
for name, alpha in [("rock_moss_set_01", False), ("rock_moss_set_02", False), ("boulder_01", False),
                    ("rock_face_01", False), ("rock_face_02", False), ("mountainside", False),
                    ("tree_stump_01", False), ("tree_stump_02", False), ("dead_tree_trunk", False),
                    ("pine_roots", False), ("dry_branches_medium_01", False),
                    ("fern_02", True), ("grass_medium_01", True), ("grass_medium_02", True),
                    ("shrub_02", True)]:
    A[name] = load_asset(name, alpha)

# Fir library.
NEAR = []
for i, (h, seed) in enumerate([(15, 11), (18, 12), (21, 13), (24, 14), (19, 15)]):
    me = make_fir(f"FirNear{i}", seed=seed, height=h, detail=1.15)
    o = bpy.data.objects.new(f"FirNear{i}", me); lib.objects.link(o); NEAR.append(o)
FAR = []
for i, (h, seed) in enumerate([(14, 21), (17, 22), (20, 23), (23, 24)]):
    me = make_fir(f"FirFar{i}", seed=seed, height=h, detail=0.32)
    o = bpy.data.objects.new(f"FirFar{i}", me); lib.objects.link(o); FAR.append(o)
SAPLING = []
for i, (h, seed) in enumerate([(3.5, 31), (5.0, 32), (6.5, 33)]):
    me = make_fir(f"FirSapling{i}", seed=seed, height=h, detail=1.2)
    o = bpy.data.objects.new(f"FirSapling{i}", me); lib.objects.link(o); SAPLING.append(o)


def place(src, coll, x, y, sink=0.0, rot=None, scale=1.0, tilt=0.0, rnd=random):
    o = bpy.data.objects.new(src.name + "_i", src.data)
    coll.objects.link(o)
    o.location = (x, y, height(x, y) - sink)
    o.rotation_euler = Euler((rnd.uniform(-tilt, tilt), rnd.uniform(-tilt, tilt),
                              rnd.uniform(0, 2 * math.pi) if rot is None else rot))
    o.scale = (scale, scale, scale)
    return o


def slope(x, y):
    e = 1.0
    gx = (height(x + e, y) - height(x - e, y)) / (2 * e)
    gy = (height(x, y + e) - height(x, y - e)) / (2 * e)
    return math.degrees(math.atan(math.hypot(gx, gy)))


def in_view(x, y, margin_deg=55):
    d = Vector((x, y)) - CAM
    if d.length < 1e-3:
        return True
    return math.degrees(VIEW.angle(d.normalized())) < margin_deg


forest = collection("Forest")
props = collection("Props")
plants = collection("Plants")
R = random.Random(42)

# ── Trees ──────────────────────────────────────────────────────────────
# Hero trees that frame the shot.
for x, y, k, s in [(-15.5, 27.5, 2, 1.0), (-19, 19, 1, 1.0),
                   (15.0, 21.5, 2, 1.0), (19.5, 27.0, 3, 0.95), (23, 19, 4, 1.0)]:
    place(NEAR[k], forest, x, y, sink=0.2, scale=s, rnd=R)
counts = {"near": 6, "far": 0, "sapling": 0}
cands = []
step = 5.0
y = -60.0
while y < 420:
    x = -320.0
    while x < 320:
        px, py = x + R.uniform(-2.0, 2.0), y + R.uniform(-2.0, 2.0)
        x += step
        dcam = (Vector((px, py)) - CAM).length
        if lake_d(px, py) < 1.12 or dcam < 14 or dcam > 260:
            continue
        if py < LAKE_C.y and abs(px) < 11 and dcam < 30:
            continue
        view = in_view(px, py)
        if not view and dcam > 110:
            continue
        h = height(px, py)
        if h > 75 or h < 0.5 or slope(px, py) > 34:
            continue
        clump = noise.noise(Vector((px * 0.025, py * 0.025, 7.7)), noise_basis='PERLIN_NEW')
        density = 0.8 if view else 0.12
        if clump < -0.25:
            density *= 0.15
        if R.random() < density:
            cands.append((px, py, dcam, view))
    y += step
near_c = [c for c in cands if c[2] < 75 and c[3]]
far_c = [c for c in cands if not (c[2] < 75 and c[3])]
R.shuffle(far_c)
far_c = far_c[:460]
for px, py, dcam, view in near_c:
    place(R.choice(NEAR), forest, px, py, sink=0.2, scale=R.uniform(0.8, 1.15), rnd=R)
    counts["near"] += 1
    if R.random() < 0.4:
        place(R.choice(SAPLING), forest, px + R.uniform(-2.5, 2.5), py + R.uniform(-2.5, 2.5),
              scale=R.uniform(0.8, 1.2), rnd=R)
        counts["sapling"] += 1
for px, py, dcam, view in far_c:
    place(R.choice(FAR), forest, px, py, sink=0.2, scale=R.uniform(0.8, 1.2), rnd=R)
    counts["far"] += 1

# ── Rocks and wood on the near shore ───────────────────────────────────
rocks = A["rock_moss_set_01"] + A["rock_moss_set_02"]
for i in range(26):
    # Along the near shoreline, some half in the water.
    a = math.radians(R.uniform(205, 335))
    rr = R.uniform(0.93, 1.08)
    px = LAKE_C.x + math.cos(a) * LAKE_R.x * rr
    py = LAKE_C.y + math.sin(a) * LAKE_R.y * rr
    place(R.choice(rocks), props, px, py, sink=R.uniform(0.1, 0.5), scale=R.uniform(0.6, 1.6), tilt=0.25, rnd=R)
for x, y, s in [(-1.5, 17.0, 1.0), (6.0, 15.5, 0.8), (-9.0, 13.0, 0.7)]:
    place(A["boulder_01"][0], props, x, y, sink=0.25, scale=s, tilt=0.2, rnd=R)
# A fallen trunk reaching into the lake.
o = place(A["dead_tree_trunk"][0], props, 3.5, 18.0, sink=0.1, rot=math.radians(70), scale=1.6, rnd=R)
o.rotation_euler.x = math.radians(-4)
place(A["tree_stump_01"][0], props, -9.0, 15.0, sink=0.15, scale=1.0, rnd=R)
place(A["tree_stump_02"][0], props, 10.5, 13.0, sink=0.15, scale=0.9, rnd=R)
place(A["pine_roots"][0], props, -11.5, 20.0, sink=0.05, scale=1.2, rnd=R)
place(A["pine_roots"][1], props, 15.0, 20.5, sink=0.05, scale=1.2, rnd=R)
# Cliffs on the far shore and the right.
for x, y, s, r in [(-48, 66, 2.8, 20)]:
    place(A["mountainside"][0], props, x, y, sink=3.0, rot=math.radians(r), scale=s, rnd=R)
for x, y, s in [(30, 26, 1.4), (-28, 36, 1.8), (12, 74, 2.2)]:
    place(R.choice(A["rock_face_01"] + A["rock_face_02"]), props, x, y, sink=0.6, scale=s, tilt=0.15, rnd=R)
for i in range(10):
    px, py = R.uniform(-14, 14), R.uniform(9, 17)
    place(R.choice(A["dry_branches_medium_01"]), props, px, py, sink=0.02, scale=R.uniform(1, 1.6), tilt=0.1, rnd=R)

# ── Ground cover in front of the camera ────────────────────────────────
grass = A["grass_medium_01"] + A["grass_medium_02"]
n_grass = n_fern = n_shrub = 0
for i in range(14000):
    px, py = R.uniform(-45, 45), R.uniform(2, 60)
    if not in_view(px, py, 50) or lake_d(px, py) < 1.0 or height(px, py) < 0.12:
        continue
    dcam = (Vector((px, py)) - CAM).length
    if dcam < 2.0:
        continue
    keep = 1.0 if dcam < 15 else (0.5 if dcam < 30 else 0.2)
    if R.random() > keep:
        continue
    pick = R.random()
    if pick < 0.62:
        place(R.choice(grass), plants, px, py, sink=0.02, scale=R.uniform(1.0, 1.8), rnd=R); n_grass += 1
    elif pick < 0.92:
        place(R.choice(A["fern_02"]), plants, px, py, sink=0.02, scale=R.uniform(0.8, 1.5), rnd=R); n_fern += 1
    elif dcam > 8:
        place(R.choice(A["shrub_02"]), plants, px, py, sink=0.05, scale=R.uniform(0.6, 1.0), rnd=R); n_shrub += 1


def tris(coll):
    t = 0
    for o in coll.objects:
        o.data.calc_loop_triangles()
        t += len(o.data.loop_triangles)
    return t


print(json.dumps({"trees": counts, "grass": n_grass, "fern": n_fern, "shrub": n_shrub,
                  "forest_tris": tris(forest), "props_tris": tris(props), "plants_tris": tris(plants),
                  "forest_objs": len(forest.objects), "plants_objs": len(plants.objects)}))

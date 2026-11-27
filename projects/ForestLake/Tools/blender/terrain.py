"""Step 1: the terrain mesh (lake basin, shores, hills, distant mountains) with four Poly Haven ground materials."""
# Fresh scene.
for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
for c in list(bpy.data.collections):
    bpy.data.collections.remove(c)
for m in list(bpy.data.meshes):
    bpy.data.meshes.remove(m)

coll = collection("Terrain")
N = 360
HALF = 850.0
K = 4.6
CX, CY = 0.0, 30.0  # grid centre: densest around the camera and the lake

bm = bmesh.new()
verts = []
for j in range(N + 1):
    y = CY + warp(j / N * 2 - 1, HALF, K)
    row = []
    for i in range(N + 1):
        x = CX + warp(i / N * 2 - 1, HALF, K)
        row.append(bm.verts.new((x, y, height(x, y))))
    verts.append(row)
for j in range(N):
    for i in range(N):
        bm.faces.new((verts[j][i], verts[j][i + 1], verts[j + 1][i + 1], verts[j + 1][i]))
bm.normal_update()

uv = bm.loops.layers.uv.new("UVMap")
TILE = 3.0
for f in bm.faces:
    for l in f.loops:
        co = l.vert.co
        l[uv].uv = (co.x / TILE, co.y / TILE)

mats = [ground_set("forest_ground_04"), ground_set("rock_pitted_mossy"),
        ground_set("brown_mud_leaves_01"), ground_set("forest_leaves_02")]
for f in bm.faces:
    c = f.calc_center_median()
    slope = math.degrees(math.acos(max(-1, min(1, f.normal.z))))
    patch = noise.noise(Vector((c.x * 0.07, c.y * 0.07, 3.3)), noise_basis='PERLIN_NEW')
    if slope > 30 or c.z > 60:
        f.material_index = 1
    elif c.z < 0.45:
        f.material_index = 2
    elif patch > 0.25:
        f.material_index = 3
    else:
        f.material_index = 0
    f.smooth = True

me = bpy.data.meshes.new("Terrain")
bm.to_mesh(me)
bm.free()
for m in mats:
    me.materials.append(m)
ob = bpy.data.objects.new("Terrain", me)
coll.objects.link(ob)
print("terrain faces", len(me.polygons))

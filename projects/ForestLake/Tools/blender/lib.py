"""Shared helpers for the ForestLake scene build, run inside Blender by build.py (which sets PH and OUT)."""
import bpy, bmesh, math, os, random
from mathutils import Vector, noise, Matrix, Euler


LAKE_C = Vector((0.0, 42.0))
LAKE_R = Vector((42.0, 24.0))


def smoothstep(a, b, x):
    t = max(0.0, min(1.0, (x - a) / (b - a)))
    return t * t * (3 - 2 * t)


def lake_d(x, y):
    return math.hypot((x - LAKE_C.x) / LAKE_R.x, (y - LAKE_C.y) / LAKE_R.y)


def height(x, y):
    n = noise.fractal(Vector((x * 0.018, y * 0.018, 0.3)), 1.0, 2.0, 5, noise_basis='PERLIN_NEW')
    det = noise.fractal(Vector((x * 0.2, y * 0.2, 1.7)), 0.5, 2.0, 3, noise_basis='PERLIN_NEW')
    # Wobble the shoreline so it is not an ellipse.
    wob = noise.noise(Vector((x * 0.05, y * 0.05, 9.1)), noise_basis='PERLIN_NEW') * 0.18
    d = lake_d(x, y) + wob
    shore = smoothstep(0.72, 1.18, d)
    ground = 0.75 + n * 0.5 + det * 0.12 + 1.4 * smoothstep(1.15, 2.2, d)
    back = max(0.0, y - (LAKE_C.y + LAKE_R.y + 6))
    side = max(0.0, abs(x) - (LAKE_R.x + 10))
    hills = (min(back, 90) * 0.13 + min(side, 90) * 0.10) * (0.75 + 0.5 * (n * 0.5 + 0.5))
    r = math.hypot(x - LAKE_C.x, (y - LAKE_C.y) * 0.8)
    ridge = 0.5 + 0.5 * noise.fractal(Vector((x * 0.003, y * 0.003, 5.0)), 0.55, 2.0, 4, noise_basis='PERLIN_NEW')
    mountains = smoothstep(240, 560, r) * (30 + 130 * max(ridge, 0.0) ** 1.6)
    # Keep the slope behind the camera low and open.
    if y < 0:
        hills += max(0.0, -y - 25) * 0.15
    land = ground + hills + mountains
    bottom = -3.0 + n * 0.5
    return bottom + (land - bottom) * shore


def warp(u, half, k):
    """Maps u in [-1,1] to [-half, half], dense near 0."""
    return half * math.sinh(k * u) / math.sinh(k)


def tex(path, colorspace='sRGB'):
    img = bpy.data.images.load(path, check_existing=True)
    img.colorspace_settings.name = colorspace
    return img


def pbr_material(name, diff, nor=None, arm=None, alpha_clip=False, double=False):
    """A glTF-exportable Principled material: base colour (+alpha), normal, ARM -> occlusion/rough/metal."""
    m = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    out = nt.nodes.new('ShaderNodeOutputMaterial')
    bsdf = nt.nodes.new('ShaderNodeBsdfPrincipled')
    nt.links.new(bsdf.outputs['BSDF'], out.inputs['Surface'])
    d = nt.nodes.new('ShaderNodeTexImage'); d.image = tex(diff)
    nt.links.new(d.outputs['Color'], bsdf.inputs['Base Color'])
    if alpha_clip:
        nt.links.new(d.outputs['Alpha'], bsdf.inputs['Alpha'])
    if nor:
        n = nt.nodes.new('ShaderNodeTexImage'); n.image = tex(nor, 'Non-Color')
        nm = nt.nodes.new('ShaderNodeNormalMap')
        nt.links.new(n.outputs['Color'], nm.inputs['Color'])
        nt.links.new(nm.outputs['Normal'], bsdf.inputs['Normal'])
    if arm:
        a = nt.nodes.new('ShaderNodeTexImage'); a.image = tex(arm, 'Non-Color')
        sep = nt.nodes.new('ShaderNodeSeparateColor')
        nt.links.new(a.outputs['Color'], sep.inputs['Color'])
        nt.links.new(sep.outputs['Green'], bsdf.inputs['Roughness'])
        nt.links.new(sep.outputs['Blue'], bsdf.inputs['Metallic'])
    else:
        bsdf.inputs['Roughness'].default_value = 0.9
    m.use_backface_culling = not double
    return m


def ground_set(name):
    base = os.path.join(PH, name, name)
    diff = base + "_diff_2k.jpg"
    if not os.path.exists(diff):
        diff = base + "_diffuse_2k.jpg"
    return pbr_material(name, diff, base + "_nor_gl_2k.jpg", base + "_arm_2k.jpg")


def collection(name, parent=None):
    c = bpy.data.collections.get(name)
    if c is None:
        c = bpy.data.collections.new(name)
        (parent or bpy.context.scene.collection).children.link(c)
    return c


def import_gltf(name, into):
    """Imports a Poly Haven model into collection `into`; returns its mesh objects."""
    path = os.path.join(PH, name, name + "_2k.gltf")
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=path)
    new = [o for o in bpy.data.objects if o not in before]
    for o in new:
        for c in list(o.users_collection):
            c.objects.unlink(o)
        into.objects.link(o)
    return new


def ray_ground(x, y):
    return height(x, y)


def ph_materials(asset):
    """Builds this module's materials for every material of a Poly Haven glTF, by name, from its own textures."""
    import json
    d = os.path.join(PH, asset)
    g = json.load(open(os.path.join(d, asset + "_2k.gltf")))
    uri = lambda ti: os.path.join(d, g["images"][g["textures"][ti]["source"]]["uri"])
    out = {}
    for m in g["materials"]:
        pbr = m.get("pbrMetallicRoughness", {})
        if "baseColorTexture" not in pbr:
            continue
        diff = uri(pbr["baseColorTexture"]["index"])
        rgba = diff.replace(".jpg", "_rgba.png")
        clip = os.path.exists(rgba)
        nor = uri(m["normalTexture"]["index"]) if "normalTexture" in m else None
        arm = uri(pbr["metallicRoughnessTexture"]["index"]) if "metallicRoughnessTexture" in pbr else None
        out[m["name"]] = pbr_material("PH_" + m["name"], rgba if clip else diff, nor, arm, alpha_clip=clip,
                                      double=m.get("doubleSided", False) or clip)
    return out


def import_glb(path, into, asset):
    """Imports a (thinned) glb and swaps its materials for the textured ones of `asset`."""
    mats = ph_materials(asset)
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=path)
    new = [o for o in bpy.data.objects if o not in before]
    for o in new:
        for c in list(o.users_collection):
            c.objects.unlink(o)
        into.objects.link(o)
        if o.type == 'MESH':
            for slot in o.material_slots:
                base = slot.material.name.split(".")[0] if slot.material else ""
                if base in mats:
                    slot.material = mats[base]
    return [o for o in new if o.type == 'MESH']

"""Step 3: one glTF per collection into Assets/Models, foliage set to alpha cutout with translucency."""
import json, sys
os.makedirs(OUT, exist_ok=True)
FOLIAGE_HINTS = ("rgba",)


def patch(path):
    g = json.load(open(path))
    used = set(g.get("extensionsUsed", []))
    for m in g.get("materials", []):
        pbr = m.get("pbrMetallicRoughness", {})
        bc = pbr.get("baseColorTexture")
        if not bc:
            continue
        img = g["images"][g["textures"][bc["index"]]["source"]]
        if any(h in img.get("uri", "") for h in FOLIAGE_HINTS):
            m["alphaMode"] = "MASK"
            m["alphaCutoff"] = 0.45
            m["doubleSided"] = True
            m.setdefault("extensions", {})["KHR_materials_diffuse_transmission"] = {"diffuseTransmissionFactor": 0.35}
            used.add("KHR_materials_diffuse_transmission")
        else:
            m.pop("alphaMode", None)
    if used:
        g["extensionsUsed"] = sorted(used)
    json.dump(g, open(path, "w"), indent=1)


for cname in ("Terrain", "Forest", "Props", "Plants"):
    coll = bpy.data.collections[cname]
    for o in bpy.context.view_layer.objects:
        o.select_set(o.name in coll.objects)
    path = os.path.join(OUT, cname + ".gltf")
    bpy.ops.export_scene.gltf(filepath=path, use_selection=True, export_format='GLTF_SEPARATE',
                              export_texture_dir="textures", export_image_format='AUTO',
                              export_yup=True, export_apply=False, export_cameras=False, export_lights=False)
    patch(path)
    print("EXPORTED", path, flush=True)

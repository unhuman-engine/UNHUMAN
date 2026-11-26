#!/usr/bin/env python3
"""Downloads the Poly Haven assets the ForestLake build uses into projects/ForestLake/.polyhaven.

    python3 projects/ForestLake/Tools/fetch_assets.py

Everything comes from Poly Haven (https://polyhaven.com), CC0. About 150 MB
at 2k. Poly Haven's glTF downloads carry leaf colour as JPEG, with the cutout
in a separate alpha map: those pairs are merged into an RGBA PNG next to the
diffuse, which is what the Blender build and the engine read. Needs Pillow.
Files already present are kept, so a rerun only fetches what is missing.
"""

import io
import json
import os
import sys
import urllib.request

from PIL import Image

RES = "2k"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".polyhaven")
HEADERS = {"User-Agent": "uhe-forestlake-fetch"}

# Models used whole: glTF plus its textures.
MODELS = ["rock_moss_set_01", "rock_moss_set_02", "boulder_01", "rock_face_01", "rock_face_02", "mountainside",
          "tree_stump_01", "tree_stump_02", "dead_tree_trunk", "pine_roots", "dry_branches_medium_01",
          "fern_02", "grass_medium_01", "grass_medium_02", "shrub_02"]
# Ground texture sets: diffuse, normal, ARM.
GROUNDS = ["forest_ground_04", "rock_pitted_mossy", "brown_mud_leaves_01", "forest_leaves_02"]
# Only the maps of fir_tree_01 the card firs are cut from (its mesh is 478 MB and unused).
FIR_MAPS = ["twig_diff", "twig_alpha", "twig_nor_gl", "twig_arm", "bark_diff", "bark_nor_gl", "bark_arm"]


def get(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=HEADERS), timeout=600) as r:
        return r.read()


def save(url, path):
    if os.path.exists(path):
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    data = get(url)
    with open(path + ".part", "wb") as f:
        f.write(data)
    os.replace(path + ".part", path)


def files(asset):
    return json.loads(get(f"https://api.polyhaven.com/files/{asset}"))


def merge_alpha(diffuse, alpha_url):
    """Writes <diffuse>_rgba.png: the diffuse colour with the alpha map as its alpha."""
    out = diffuse.replace(".jpg", "_rgba.png")
    if os.path.exists(out):
        return
    alpha = Image.open(io.BytesIO(get(alpha_url))).convert("L")
    rgb = Image.open(diffuse).convert("RGB")
    if alpha.size != rgb.size:
        alpha = alpha.resize(rgb.size)
    rgb.putalpha(alpha)
    rgb.save(out)


def fetch_model(asset):
    info = files(asset)
    gltf = info["gltf"][RES]["gltf"]
    root = os.path.join(OUT, asset)
    save(gltf["url"], os.path.join(root, os.path.basename(gltf["url"])))
    for rel, entry in gltf["include"].items():
        save(entry["url"], os.path.join(root, rel))
    for key in info:
        if "alpha" not in key.lower():
            continue
        part = key.lower().replace("alpha", "").strip("_")  # "twig", "twigs" or "" (one map)
        entry = info[key][RES]
        textures = os.path.join(root, "textures")
        for name in os.listdir(textures):
            if "_diff_" in name and name.endswith(".jpg") and (part == "" or f"_{part}_diff" in name):
                merge_alpha(os.path.join(textures, name), (entry.get("png") or entry["jpg"])["url"])


def fetch_ground(asset):
    info = files(asset)
    for key in ("Diffuse", "nor_gl", "arm"):
        entry = info[key][RES]
        url = (entry.get("jpg") or entry["png"])["url"]
        save(url, os.path.join(OUT, asset, os.path.basename(url)))


def fetch_fir():
    info = files("fir_tree_01")
    textures = os.path.join(OUT, "fir_tree_01", "textures")
    for key in FIR_MAPS:
        if key == "twig_alpha":
            continue
        url = info[key][RES]["jpg"]["url"]
        save(url, os.path.join(textures, os.path.basename(url)))
    alpha = info["twig_alpha"][RES]
    merge_alpha(os.path.join(textures, f"fir_tree_01_twig_diff_{RES}.jpg"), (alpha.get("png") or alpha["jpg"])["url"])


def main():
    for asset in MODELS:
        fetch_model(asset)
        print("model", asset, flush=True)
    for asset in GROUNDS:
        fetch_ground(asset)
        print("ground", asset, flush=True)
    fetch_fir()
    print("fir maps", flush=True)
    print("done:", os.path.normpath(OUT))
    return 0


if __name__ == "__main__":
    sys.exit(main())

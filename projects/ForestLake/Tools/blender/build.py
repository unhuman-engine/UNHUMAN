"""Builds the ForestLake models in Blender and writes them to ../../Assets/Models.

    python3 projects/ForestLake/Tools/fetch_assets.py          # Poly Haven downloads, once
    blender -b --python projects/ForestLake/Tools/blender/build.py

Runs headless: the scene is about nine million triangles, more than an
interactive Blender wants to draw. Each step runs in one shared namespace:
lib.py (height field, materials, importers), firgen.py (card firs),
terrain.py, scatter.py, export.py. Placement is seeded, so a rebuild gives
the same scene. The built scene is saved to .build/ForestLake.blend for inspection.
"""
import os

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.normpath(os.path.join(HERE, "..", ".."))
namespace = {
    "__name__": "forestlake",
    "PH": os.path.join(PROJECT, ".polyhaven"),
    "OUT": os.path.join(PROJECT, "Assets", "Models"),
}
bpy.ops.wm.read_factory_settings(use_empty=True)
for step in ("lib.py", "firgen.py", "terrain.py", "scatter.py", "export.py"):
    print("STEP", step, flush=True)
    with open(os.path.join(HERE, step)) as f:
        exec(compile(f.read(), step, "exec"), namespace)
os.makedirs(os.path.join(PROJECT, ".build"), exist_ok=True)
bpy.ops.wm.save_as_mainfile(filepath=os.path.join(PROJECT, ".build", "ForestLake.blend"))

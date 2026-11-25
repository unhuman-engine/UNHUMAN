#!/usr/bin/env python3
"""Writes projects/PondDemo/Assets/Scenes/Pond.unhuman.

    python3 projects/PondDemo/Tools/generate_pond_scene.py

Every placement samples the same ground_height the meshes were built from, so
trees, rocks, reeds and the character stand on the surface. Edit the scene in
the editor freely afterwards; rerunning this overwrites it.
"""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from generate_pond_assets import WATER_LEVEL, ground_height  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Assets", "Scenes", "Pond.unhuman")

next_id = [1000]
lines = []


def vec(values):
    return "[" + ", ".join(f"{v:.4g}" for v in values) + "]"


def entity(tag, translation, rotation=(0.0, 0.0, 0.0), scale=(1.0, 1.0, 1.0), components=()):
    next_id[0] += 1
    lines.extend([
        f"  - Entity: {next_id[0]}",
        "    TagComponent:",
        f"      Tag: {tag}",
        "    TransformComponent:",
        f"      Translation: {vec(translation)}",
        f"      Rotation: {vec(rotation)}",
        f"      Scale: {vec(scale)}",
    ])
    for name, fields in components:
        lines.append(f"    {name}:")
        for key, value in fields.items():
            if isinstance(value, bool):
                value = "true" if value else "false"
            elif isinstance(value, (tuple, list)):
                value = vec(value)
            lines.append(f"      {key}: {value}")


def model(path):
    return ("Model3DComponent", {"ModelPath": path})


def foliage(bend, height):
    return ("FoliageComponent", {"Bend": bend, "Height": height})


def on_ground(x, z, sink=0.0):
    return (x, ground_height(x, z) - sink, z)


def facing(from_xz, to_xz):
    """Yaw that turns a model's +Z toward a point."""
    return math.atan2(to_xz[0] - from_xz[0], to_xz[1] - from_xz[1])


def main():
    # ── terrain, grass and water ──
    entity("Ground", (0, 0, 0), components=[model("Models/Ground.gltf")])
    for i in range(4):
        entity(f"Grass {i + 1}", (0, 0, 0), components=[model(f"Models/Grass{i}.gltf"), foliage(1.0, 0.6)])
    entity("Pond", (0, WATER_LEVEL, 0), components=[("WaterComponent", {
        "Size": (19, 14), "Resolution": 128,
        "ShallowColor": (0.28, 0.62, 0.52), "DeepColor": (0.02, 0.09, 0.10),
        "Clarity": 1.8, "WaveHeight": 0.012, "WaveLength": 1.4, "WaveSpeed": 0.6, "Choppiness": 0.2,
        "Direction": 0, "FollowWind": True, "Foam": 0.35})])

    # ── shore ──
    for i, (x, z, s) in enumerate([(-7.6, 3.2, 1.0), (5.8, -4.6, 1.1), (-3.0, -5.9, 0.9), (2.6, 5.7, 0.8),
                                   (-8.4, -1.6, 0.7)]):
        entity(f"Reeds {i + 1}", on_ground(x, z, 0.05), (0, i * 1.3, 0), (s, s, s),
               [model("Models/Reeds.gltf"), foliage(1.3, 1.6)])
    for i, (x, z, s, yaw) in enumerate([(9.4, -1.3, 0.9, 0.4), (-9.0, -2.6, 0.6, 1.9), (-2.2, 6.9, 0.5, 2.7),
                                        (3.6, -6.6, 0.4, 0.9), (10.4, 2.4, 0.35, 1.3)]):
        entity(f"Rock {i + 1}", on_ground(x, z, s * 0.2), (0, yaw, 0), (s, s, s), [model("Models/Rock.gltf")])

    # ── trees ──
    for i, (x, z, s) in enumerate([(-14, -8, 1.2), (-17.5, 4, 1.4), (12, -13, 1.1), (16.5, 8.5, 1.3),
                                   (-5, -16.5, 1.0), (4, 17, 1.25), (-11, 13, 0.9), (19, -3, 1.0)]):
        entity(f"Pine {i + 1}", on_ground(x, z, 0.05), (0, i * 0.9, 0), (s, s, s),
               [model("Models/Pine.gltf"), foliage(0.12, 5.0)])

    # ── character on the bank, looking over the water ──
    spot = (7.9, 4.6)
    entity("Lynae", on_ground(*spot), (0, facing(spot, (0.0, 0.0)), 0),
           components=[model("model/lynae_from_wuthering_waves.glb")])

    # ── light, wind and the play camera ──
    entity("Sun", (0, 10, 0), (-0.55, 2.3, 0), components=[("DirectionalLightComponent", {
        "Color": (1.0, 0.93, 0.82), "Intensity": 1.7, "CastShadows": True})])
    entity("Wind", (0, 2, 0), (0, 0.7, 0), components=[("WindComponent", {
        "Strength": 0.7, "GustStrength": 0.9, "GustFrequency": 0.22})])
    camera_pos = (15.5, 3.2, 12.5)
    entity("Camera", camera_pos, (-0.16, math.atan2(camera_pos[0], camera_pos[2]), 0),
           components=[("CameraComponent", {
               "ProjectionType": 0, "PerspectiveFOV": 0.8, "PerspectiveNear": 0.1, "PerspectiveFar": 500,
               "OrthographicSize": 10, "OrthographicNear": -1, "OrthographicFar": 1,
               "Primary": True, "FixedAspectRatio": False})])

    header = [
        "Scene: Pond",
        "Environment:",
        "  Environment:",
        "    SkyColor: [0.56, 0.71, 0.9]",
        "    GroundColor: [0.3, 0.28, 0.2]",
        "    AmbientIntensity: 0.38",
        "    Exposure: 1.1",
        "  Shadows:",
        "    Enabled: true",
        "    CascadeCount: 4",
        "    Distance: 55",
        "    SplitLambda: 0.8",
        "    Resolution: 2048",
        "    DepthBias: 0.5",
        "    NormalBias: 1.5",
        "    Softness: 1.2",
        "    ShowCascades: false",
        "  SSAO:",
        "    Enabled: true",
        "    Radius: 0.6",
        "    Intensity: 1.2",
        "    Bias: 0.025",
        "  Bloom:",
        "    Enabled: true",
        "    Intensity: 0.04",
        "    FilterRadius: 1",
        "  Wind:",
        "    Direction: [1, 0.3]",
        "    Strength: 0.5",
        "    GustStrength: 0.4",
        "    GustFrequency: 0.25",
        "Entities:",
    ]
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w") as f:
        f.write("\n".join(header + lines) + "\n")
    print("wrote", os.path.normpath(OUT), f"({next_id[0] - 1000} entities)")


if __name__ == "__main__":
    main()

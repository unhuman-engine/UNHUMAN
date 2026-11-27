#!/usr/bin/env python3
"""Writes projects/ForestLake: the project file and Assets/Scenes/ForestLake.unhuman.

    python3 projects/ForestLake/Tools/generate_forest_lake_scene.py

The scene: a mountain lake in a fir forest at golden hour. The models in
Assets/Models are built in Blender by Tools/blender/ (see Assets/README.md):
the terrain, card fir trees made from Poly Haven's fir atlas, and Poly Haven
rocks, wood, ferns and grass. This script places them with the sun, the
procedural sky, the lake's water surface and a camera on the near shore.

Blender is Z-up and the engine Y-up: a Blender point (x, y, z) is (x, z, -y) here.
"""

import os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")

# Lake centre and radii in Blender's ground plane (Tools/blender/lib.py).
LAKE_C = (0.0, 42.0)
LAKE_R = (42.0, 24.0)

lines = []
next_id = [7000]


def vec(values):
    return "[" + ", ".join(f"{v:.4g}" for v in values) + "]"


def entity(tag, translation=(0, 0, 0), rotation=(0, 0, 0), components=()):
    next_id[0] += 1
    lines.extend([f"  - Entity: {next_id[0]}", "    TagComponent:", f"      Tag: {tag}", "    TransformComponent:",
                  f"      Translation: {vec(translation)}", f"      Rotation: {vec(rotation)}",
                  "      Scale: [1, 1, 1]"])
    for name, fields in components:
        lines.append(f"    {name}:")
        for key, value in fields.items():
            if isinstance(value, bool):
                value = "true" if value else "false"
            elif isinstance(value, (tuple, list)):
                value = vec(value)
            elif isinstance(value, str):
                value = '"' + value + '"'
            lines.append(f"      {key}: {value}")
    lines.extend(["    RelationshipComponent:", "      Parent: 0", "      Children: []"])


def model(tag, path):
    entity(tag, components=[("Model3DComponent", {"ModelPath": path})])


def main():
    lines.extend([
        "Scene: ForestLake",
        "Environment:",
        "  Environment:",
        "    SkyColor: [0.62, 0.7, 0.85]",
        "    GroundColor: [0.24, 0.22, 0.17]",
        "    AmbientIntensity: 0.32",
        "    Exposure: 1.2",
        "  Sky:",
        "    Mode: Procedural",
        "    Intensity: 1",
        "    ZenithColor: [0.22, 0.45, 0.82]",
        "    HorizonColor: [0.98, 0.87, 0.72]",
        "    GroundColor: [0.25, 0.23, 0.2]",
        "    SunSize: 2",
        "    SunBrightness: 40",
        "    AmbientFromSky: true",
        "  Shadows:",
        "    Enabled: true",
        "    CascadeCount: 4",
        "    Distance: 140",
        "    SplitLambda: 0.8",
        "    Resolution: 2048",
        "    DepthBias: 0.6",
        "    NormalBias: 1.5",
        "    Softness: 1.2",
        "    ShowCascades: false",
        "  SSAO:",
        "    Enabled: true",
        "    Radius: 0.6",
        "    Intensity: 1.1",
        "    Bias: 0.025",
        "  Bloom:",
        "    Enabled: true",
        "    Intensity: 0.05",
        "    FilterRadius: 1",
        "  Wind:",
        "    Direction: [1, 0.2]",
        "    Strength: 0.25",
        "    GustStrength: 0.2",
        "    GustFrequency: 0.2",
        "Entities:",
    ])
    model("Terrain", "Models/Terrain.gltf")
    model("Forest", "Models/Forest.gltf")
    model("Rocks and Wood", "Models/Props.gltf")
    model("Ferns and Grass", "Models/Plants.gltf")
    # Low golden sun from the front left: rim light on the far shore, long shadows towards the camera.
    entity("Sun", (0, 40, 0), (-0.16, -2.53, 0), [("DirectionalLightComponent",
                                                   {"Color": (1.0, 0.8, 0.58), "Intensity": 2.4,
                                                    "CastShadows": True})])
    entity("Lake", (LAKE_C[0], 0, -LAKE_C[1]), components=[("WaterComponent", {
        "Size": (LAKE_R[0] * 2.7, LAKE_R[1] * 2.9), "Resolution": 256,
        "ShallowColor": (0.16, 0.36, 0.34), "DeepColor": (0.02, 0.07, 0.09), "Clarity": 2.5,
        "WaveHeight": 0.015, "WaveLength": 2.5, "WaveSpeed": 0.5, "Choppiness": 0.1, "Direction": 20,
        "FollowWind": True, "Foam": 0.15})])
    # Eye on a rise of the near shore, looking down across the lake.
    entity("Camera", (-6, 5.2, -6), (-0.07, -0.263, 0), [("CameraComponent", {
        "ProjectionType": 0, "PerspectiveFOV": 0.95, "PerspectiveNear": 0.1, "PerspectiveFar": 1500,
        "OrthographicSize": 10, "OrthographicNear": -1, "OrthographicFar": 1, "Primary": True,
        "FixedAspectRatio": False})])

    os.makedirs(os.path.join(ROOT, "Assets", "Scenes"), exist_ok=True)
    with open(os.path.join(ROOT, "Assets", "Scenes", "ForestLake.unhuman"), "w") as f:
        f.write("\n".join(lines) + "\n")
    with open(os.path.join(ROOT, "Project.uheproject"), "w") as f:
        f.write("\n".join([
            "Project:",
            "  Name: ForestLake",
            '  EngineVersion: "0.1"',
            '  StartScene: "Assets/Scenes/ForestLake.unhuman"',
            "  Window:",
            "    Title: Forest Lake",
            "    Width: 1600",
            "    Height: 900",
            "    Mode: Windowed",
            "    VSync: true",
        ]) + "\n")
    print("wrote", os.path.normpath(ROOT))


if __name__ == "__main__":
    main()

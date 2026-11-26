#!/usr/bin/env python3
"""Writes projects/WorldDemo/Assets: a scene and a script showing the sky,
terrain and game UI together.

    python3 projects/WorldDemo/Tools/generate_world_scene.py

The scene: a generated terrain under the procedural sky at a low sun, a few
boxes that fall onto it in Play (the terrain is a heightfield collider), and
a HUD - a panel with live readouts and a battery bar, a throttle slider and a
reset button. Scripts/Hud.lua drives the HUD and answers the button's
OnClick. Edit the scene in the editor freely; rerunning this overwrites it.
"""

import base64
import json
import os
import struct

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Assets")

next_id = [5000]
lines = []


def vec(values):
    return "[" + ", ".join(f"{v:.4g}" for v in values) + "]"


def entity(tag, translation=(0, 0, 0), rotation=(0, 0, 0), scale=(1, 1, 1), components=(), parent=0, uid=None):
    next_id[0] += 1
    uid = uid or next_id[0]
    lines.extend([
        f"  - Entity: {uid}",
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
            elif isinstance(value, str):
                value = '"' + value.replace('"', '\\"') + '"'
            lines.append(f"      {key}: {value}")
    return uid


def relation(uid, parent, children):
    """Appends the RelationshipComponent of the entity just written."""
    lines.append("    RelationshipComponent:")
    lines.append(f"      Parent: {parent}")
    if children:
        lines.append("      Children:")
        lines.extend(f"        - {c}" for c in children)
    else:
        lines.append("      Children: []")


def rect(anchor_min, anchor_max, pivot, position, size, order=0):
    return ("UIRectComponent", {"AnchorMin": anchor_min, "AnchorMax": anchor_max, "Pivot": pivot,
                                "Position": position, "Size": size, "Order": order, "Visible": True})


def image(color, radius=8, border=0.0, border_color=(1, 1, 1, 0.2)):
    return ("UIImageComponent", {"Color": color, "TexturePath": "", "CornerRadius": radius,
                                 "BorderWidth": border, "BorderColor": border_color})


def text(value, size, align=1, color=(1, 1, 1, 1)):
    return ("UITextComponent", {"Text": value, "FontSize": size, "Color": color, "Align": align, "FontPath": ""})


def bar(value, fill, interactable=False):
    return ("UIProgressBarComponent", {"Value": value, "FillColor": fill, "BackgroundColor": (0.08, 0.09, 0.12, 0.9),
                                       "CornerRadius": 5, "Interactable": interactable})


def write_crate(path):
    """A unit cube with normals and a wood colour, as glTF with an embedded buffer."""
    faces = [((1, 0, 0), (0, 0, -1), (0, 1, 0)), ((-1, 0, 0), (0, 0, 1), (0, 1, 0)),
             ((0, 1, 0), (1, 0, 0), (0, 0, -1)), ((0, -1, 0), (1, 0, 0), (0, 0, 1)),
             ((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((0, 0, -1), (-1, 0, 0), (0, 1, 0))]
    positions, normals, indices = [], [], []
    for n, u, v in faces:
        base = len(positions)
        for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            positions.append(tuple(0.5 * n[i] + 0.5 * su * u[i] + 0.5 * sv * v[i] for i in range(3)))
            normals.append(n)
        indices += [base, base + 1, base + 2, base, base + 2, base + 3]
    blob = b"".join(struct.pack("<3f", *p) for p in positions) + b"".join(struct.pack("<3f", *n) for n in normals)
    index_offset = len(blob)
    blob += b"".join(struct.pack("<H", i) for i in indices)
    gltf = {
        "asset": {"version": "2.0", "generator": "WorldDemo"},
        "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0, "name": "Crate"}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0}]}],
        "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [0.55, 0.36, 0.18, 1], "metallicFactor": 0,
                                                "roughnessFactor": 0.8}}],
        "buffers": [{"byteLength": len(blob),
                     "uri": "data:application/octet-stream;base64," + base64.b64encode(blob).decode()}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 288},
                        {"buffer": 0, "byteOffset": 288, "byteLength": 288},
                        {"buffer": 0, "byteOffset": index_offset, "byteLength": len(indices) * 2}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 24, "type": "VEC3",
                       "min": [-0.5, -0.5, -0.5], "max": [0.5, 0.5, 0.5]},
                      {"bufferView": 1, "componentType": 5126, "count": 24, "type": "VEC3"},
                      {"bufferView": 2, "componentType": 5123, "count": len(indices), "type": "SCALAR"}],
    }
    with open(path, "w") as f:
        json.dump(gltf, f)


def main():
    # ── World ──
    uid = entity("Terrain", (0, -4, 0), components=[("TerrainComponent", {
        "HeightmapPath": "", "Size": 160, "Height": 16, "Resolution": 160, "Seed": 7, "FeatureSize": 45,
        "Octaves": 6, "Roughness": 0.5, "SandLevel": 0.05, "SnowLevel": 0.8, "RockSlope": 32, "Collider": True})])
    relation(uid, 0, [])
    for i, (x, z) in enumerate([(-3, 2), (0, 0), (3, -2), (1, 4)]):
        uid = entity(f"Crate {i + 1}", (x, 14 + i * 2, z), (0.3 * i, 0.5 * i, 0), components=[
            ("RigidBody3DComponent", {"BodyType": 2, "Mass": 1.0, "LinearDamping": 0.05, "AngularDamping": 0.05,
                                      "IsSensor": False}),
            ("BoxCollider3DComponent", {"Offset": (0, 0, 0), "HalfExtent": (0.5, 0.5, 0.5), "Friction": 0.6,
                                        "Restitution": 0.1}),
            ("Model3DComponent", {"ModelPath": "Models/Crate.gltf"})])
        relation(uid, 0, [])
    uid = entity("Sun", (0, 20, 0), (-0.35, 2.5, 0), components=[("DirectionalLightComponent", {
        "Color": (1.0, 0.86, 0.7), "Intensity": 1.8, "CastShadows": True})])
    relation(uid, 0, [])
    uid = entity("Camera", (14, 18, 46), (-0.3, 0.29, 0), components=[("CameraComponent", {
        "ProjectionType": 0, "PerspectiveFOV": 0.9, "PerspectiveNear": 0.1, "PerspectiveFar": 1000,
        "OrthographicSize": 10, "OrthographicNear": -1, "OrthographicFar": 1, "Primary": True,
        "FixedAspectRatio": False})])
    relation(uid, 0, [])

    # ── HUD ──
    panel = 9100
    title, speed, battery_label, battery = 9101, 9102, 9103, 9104
    entity("HUD", components=[rect((0, 0), (0, 0), (0, 0), (20, 20), (300, 128)),
                              image((0.05, 0.07, 0.1, 0.8), 12, 1.0)], uid=panel)
    relation(panel, 0, [title, speed, battery_label, battery])
    entity("Title", components=[rect((0, 0), (1, 0), (0.5, 0), (0, 12), (-32, 26)), text("WorldDemo", 22, 0)],
           uid=title)
    relation(title, panel, [])
    entity("Speed", components=[rect((0, 0), (1, 0), (0.5, 0), (0, 46), (-32, 22)),
                                text("Throttle 40%", 17, 0, (0.75, 0.85, 1, 1))], uid=speed)
    relation(speed, panel, [])
    entity("BatteryLabel", components=[rect((0, 0), (1, 0), (0.5, 0), (0, 76), (-32, 18)),
                                       text("Battery", 15, 0, (0.7, 0.75, 0.8, 1))], uid=battery_label)
    relation(battery_label, panel, [])
    entity("Battery", components=[rect((0, 1), (1, 1), (0.5, 1), (0, -14), (-32, 10)),
                                  bar(1.0, (0.35, 0.85, 0.45, 1))], uid=battery)
    relation(battery, panel, [])

    uid = entity("Throttle", components=[rect((0.5, 1), (0.5, 1), (0.5, 1), (0, -36), (360, 14)),
                                         bar(0.4, (0.3, 0.65, 1, 1), True)])
    relation(uid, 0, [])
    uid = entity("Reset", components=[
        rect((1, 1), (1, 1), (1, 1), (-20, -20), (150, 46)), image((0.85, 0.35, 0.2, 0.95), 10),
        ("UIButtonComponent", {"NormalColor": (0.85, 0.35, 0.2, 0.95), "HoverColor": (0.95, 0.45, 0.3, 1),
                               "PressedColor": (0.6, 0.25, 0.15, 1), "Interactable": True}),
        text("Recharge", 20), ("ScriptComponent", {"ScriptPath": "Scripts/Hud.lua", "Enabled": True})])
    relation(uid, 0, [])

    header = [
        "Scene: World",
        "SceneFormat: 2",
        "Environment:",
        "  Environment:",
        "    SkyColor: [0.56, 0.71, 0.9]",
        "    GroundColor: [0.3, 0.28, 0.2]",
        "    AmbientIntensity: 0.35",
        "    Exposure: 1.1",
        "  Sky:",
        "    Mode: Procedural",
        "    Intensity: 1",
        "    SunSize: 2",
        "    SunBrightness: 40",
        "    AmbientFromSky: true",
        "  Shadows:",
        "    Enabled: true",
        "    CascadeCount: 4",
        "    Distance: 120",
        "Entities:",
    ]
    os.makedirs(os.path.join(ROOT, "Scenes"), exist_ok=True)
    os.makedirs(os.path.join(ROOT, "Scripts"), exist_ok=True)
    os.makedirs(os.path.join(ROOT, "Models"), exist_ok=True)
    write_crate(os.path.join(ROOT, "Models", "Crate.gltf"))
    with open(os.path.join(ROOT, "Scenes", "World.unhuman"), "w") as f:
        f.write("\n".join(header + lines) + "\n")
    with open(os.path.join(ROOT, "Scripts", "Hud.lua"), "w") as f:
        f.write(HUD_SCRIPT)
    print("wrote", os.path.normpath(ROOT))


HUD_SCRIPT = """-- WorldDemo HUD: the throttle drains the battery, the button recharges it.
-- Attached to the Reset button; finds the rest of the HUD by name.

function OnCreate(self)
    self.battery = 1.0
    self.throttle = Find("Throttle")
    self.speed = Find("Speed")
    self.bar = Find("Battery")
end

function OnUpdate(self, dt)
    local throttle = self.throttle and self.throttle:GetValue() or 0
    self.battery = math.max(0, self.battery - throttle * dt * 0.05)
    if self.bar then self.bar:SetValue(self.battery) end
    if self.speed then
        self.speed:SetText(string.format("Throttle %d%%   %.1f m/s", math.floor(throttle * 100), throttle * 12))
    end
end

function OnClick(self)
    self.battery = 1.0
    Log.info("recharged (" .. self:GetClicks() .. " times)")
end
"""

if __name__ == "__main__":
    main()

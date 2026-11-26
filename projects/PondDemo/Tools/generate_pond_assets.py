#!/usr/bin/env python3
"""Generates the PondDemo meshes as glTF 2.0 (vertex-coloured, no textures).

    python3 projects/PondDemo/Tools/generate_pond_assets.py

Writes into projects/PondDemo/Assets/Models/. Deterministic (fixed seeds), so
the committed files only change when this script does. Standard library only.

The ground's shape is defined once here (ground_height) and the grass, reeds
and the scene's placements all sample it, so blades stand on the surface and
the pond's rim sits just above the water entity (y = WATER_LEVEL).
"""

import base64
import json
import math
import os
import random
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "Assets", "Models")

GROUND_SIZE = 64.0          # metres per side
GROUND_CELLS = 128          # grid cells per side
POND_CENTRE = (0.0, 0.0)
POND_RADII = (8.0, 5.5)     # ellipse half axes (x, z) of the waterline
POND_DEPTH = 1.6            # deepest point below the rim
WATER_LEVEL = -0.12         # the Water entity's height in the scene


# ── shape ────────────────────────────────────────────────────────────────────

def pond_distance(x, z):
    """Elliptical distance from the pond centre: 1 at the waterline."""
    dx = (x - POND_CENTRE[0]) / POND_RADII[0]
    dz = (z - POND_CENTRE[1]) / POND_RADII[1]
    # A little wobble so the shore is not a perfect ellipse.
    angle = math.atan2(dz, dx)
    wobble = 1.0 + 0.08 * math.sin(angle * 3.0 + 0.7) + 0.05 * math.sin(angle * 5.0 + 2.1)
    return math.sqrt(dx * dx + dz * dz) / wobble


def smoothstep(e0, e1, x):
    t = min(max((x - e0) / (e1 - e0), 0.0), 1.0)
    return t * t * (3.0 - 2.0 * t)


def ground_height(x, z):
    """Gentle rolling meadow with the pond basin cut into it."""
    meadow = (0.18 * math.sin(x * 0.21 + 0.5) * math.cos(z * 0.17 - 0.3)
              + 0.10 * math.sin(x * 0.53 + z * 0.37)
              + 0.35 * smoothstep(18.0, 32.0, math.hypot(x, z)))  # rises toward the edges
    d = pond_distance(x, z)
    # Bank: from the meadow down to just under the waterline, then the basin.
    basin = -POND_DEPTH * (1.0 - smoothstep(0.0, 1.0, d) ** 1.5)
    rim = smoothstep(1.0, 1.6, d)  # 0 in the pond, 1 on the meadow
    # The meadow sits 0.2 m up, so its dips never reach the water level.
    return (meadow + 0.2) * rim + basin * (1.0 - smoothstep(0.85, 1.25, d)) + 0.02


def ground_normal(x, z, e=0.05):
    hx = ground_height(x + e, z) - ground_height(x - e, z)
    hz = ground_height(x, z + e) - ground_height(x, z - e)
    n = (-hx, 2.0 * e, -hz)
    length = math.sqrt(sum(c * c for c in n))
    return tuple(c / length for c in n)


def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(len(a)))


def ground_colour(x, z, h, rng_value):
    """Linear vertex colours: meadow green, sand at the bank, mud under water."""
    grass = mix((0.10, 0.24, 0.05), (0.18, 0.33, 0.07), rng_value)
    sand = (0.42, 0.36, 0.22)
    mud = (0.13, 0.11, 0.07)
    d = pond_distance(x, z)
    colour = mix(sand, grass, smoothstep(1.05, 1.45, d))
    colour = mix(mud, colour, smoothstep(WATER_LEVEL - 0.35, WATER_LEVEL + 0.05, h))
    return colour + (1.0,)


# ── glTF writing ─────────────────────────────────────────────────────────────

class Mesh:
    def __init__(self):
        self.positions = []
        self.normals = []
        self.colours = []
        self.uvs = []
        self.indices = []

    def vertex(self, p, n, c, uv=(0.0, 0.0)):
        self.positions.append(p)
        self.normals.append(n)
        self.colours.append(tuple(c) + (1.0,) * (4 - len(c)))  # RGB or RGBA
        self.uvs.append(uv)
        return len(self.positions) - 1


def write_gltf(name, primitives, double_sided=False, roughness=0.9):
    """primitives: list of Mesh; one material, vertex colours carry the look."""
    blob = bytearray()
    views, accessors, prims = [], [], []

    def add_view(data, target):
        while len(blob) % 4:
            blob.append(0)
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data), "target": target})
        blob.extend(data)
        return len(views) - 1

    for mesh in primitives:
        count = len(mesh.positions)
        pos = b"".join(struct.pack("<3f", *p) for p in mesh.positions)
        nrm = b"".join(struct.pack("<3f", *n) for n in mesh.normals)
        # Colours as normalized bytes and 16-bit indices where they fit: a
        # quarter and a half of the float/uint32 size, and the loader reads both.
        col = b"".join(struct.pack("<4B", *(max(0, min(255, round(v * 255))) for v in c)) for c in mesh.colours)
        wide = count > 65535
        idx = b"".join(struct.pack("<I" if wide else "<H", i) for i in mesh.indices)
        lo = [min(p[i] for p in mesh.positions) for i in range(3)]
        hi = [max(p[i] for p in mesh.positions) for i in range(3)]
        base = len(accessors)
        accessors += [
            {"bufferView": add_view(pos, 34962), "componentType": 5126, "count": count, "type": "VEC3",
             "min": lo, "max": hi},
            {"bufferView": add_view(nrm, 34962), "componentType": 5126, "count": count, "type": "VEC3"},
            {"bufferView": add_view(col, 34962), "componentType": 5121, "normalized": True, "count": count,
             "type": "VEC4"},
            {"bufferView": add_view(idx, 34963), "componentType": 5125 if wide else 5123,
             "count": len(mesh.indices), "type": "SCALAR"},
        ]
        prims.append({"attributes": {"POSITION": base, "NORMAL": base + 1, "COLOR_0": base + 2},
                      "indices": base + 3, "material": 0})

    gltf = {
        "asset": {"version": "2.0", "generator": "PondDemo generate_pond_assets.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": name}],
        "meshes": [{"name": name, "primitives": prims}],
        "materials": [{"name": name, "doubleSided": double_sided,
                       "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1], "metallicFactor": 0.0,
                                                "roughnessFactor": roughness}}],
        "buffers": [{"byteLength": len(blob), "uri": name + ".bin"}],
        "bufferViews": views,
        "accessors": accessors,
    }
    with open(os.path.join(OUT, name + ".bin"), "wb") as f:
        f.write(blob)
    with open(os.path.join(OUT, name + ".gltf"), "w") as f:
        json.dump(gltf, f, indent=1)
    print(f"  {name}.gltf: {sum(len(m.positions) for m in primitives)} vertices, {len(blob) // 1024} KiB")


# ── meshes ───────────────────────────────────────────────────────────────────

def make_ground():
    rng = random.Random(1)
    mesh = Mesh()
    n = GROUND_CELLS
    step = GROUND_SIZE / n
    for j in range(n + 1):
        for i in range(n + 1):
            x = -GROUND_SIZE / 2 + i * step
            z = -GROUND_SIZE / 2 + j * step
            h = ground_height(x, z)
            mesh.vertex((x, h, z), ground_normal(x, z), ground_colour(x, z, h, rng.random()), (x / 4, z / 4))
    for j in range(n):
        for i in range(n):
            a = j * (n + 1) + i
            b, c, d = a + 1, a + n + 1, a + n + 2
            mesh.indices += [a, c, b, b, c, d]  # counter-clockwise seen from above
    write_gltf("Ground", [mesh], roughness=0.95)


def blade(mesh, rng, x, z, height, width, base_colour, tip_colour, lean=0.25):
    """One grass blade: two triangles tapering to a tip that leans a little."""
    h0 = ground_height(x, z)
    angle = rng.uniform(0.0, math.tau)
    side = (math.cos(angle) * width * 0.5, math.sin(angle) * width * 0.5)
    bend = rng.uniform(-lean, lean), rng.uniform(-lean, lean)
    # Normals lean toward up: blades lit like a soft carpet instead of flat cards.
    face = (-side[1], 0.0, side[0])
    length = math.hypot(face[0], face[2]) or 1.0
    normal = (face[0] / length * 0.35, 0.94, face[2] / length * 0.35)
    mid = 0.55
    a = mesh.vertex((x - side[0], h0, z - side[1]), normal, base_colour)
    b = mesh.vertex((x + side[0], h0, z + side[1]), normal, base_colour)
    c = mesh.vertex((x - side[0] * 0.6 + bend[0] * height * mid, h0 + height * mid,
                     z - side[1] * 0.6 + bend[1] * height * mid), normal, mix(base_colour, tip_colour, mid))
    d = mesh.vertex((x + side[0] * 0.6 + bend[0] * height * mid, h0 + height * mid,
                     z + side[1] * 0.6 + bend[1] * height * mid), normal, mix(base_colour, tip_colour, mid))
    e = mesh.vertex((x + bend[0] * height, h0 + height, z + bend[1] * height), normal, tip_colour)
    mesh.indices += [a, b, c, b, d, c, c, d, e]


def make_grass_patches():
    """Four quadrant patches, so each sways with its own phase."""
    for qi, (sx, sz) in enumerate([(-1, -1), (1, -1), (-1, 1), (1, 1)]):
        rng = random.Random(10 + qi)
        mesh = Mesh()
        placed = 0
        while placed < 2700:
            x = sx * rng.uniform(0.0, 26.0)
            z = sz * rng.uniform(0.0, 26.0)
            d = pond_distance(x, z)
            r = math.hypot(x, z)
            if d < 1.45 or r > 27.0:
                continue
            # Thinner toward the shore and the far edge, in clumps elsewhere.
            density = smoothstep(1.45, 2.2, d) * (1.0 - smoothstep(20.0, 27.0, r))
            clump = 0.6 + 0.4 * math.sin(x * 0.9) * math.cos(z * 0.8)
            if rng.random() > density * clump:
                continue
            # A tuft: three blades around one root, so the field reads dense.
            v = rng.random()
            for _ in range(3):
                bx, bz = x + rng.uniform(-0.07, 0.07), z + rng.uniform(-0.07, 0.07)
                height = rng.uniform(0.25, 0.6) * (0.8 + 0.4 * clump)
                base = mix((0.05, 0.13, 0.02), (0.08, 0.17, 0.03), v)
                tip = mix((0.30, 0.50, 0.09), (0.55, 0.62, 0.18), v * rng.random())
                blade(mesh, rng, bx, bz, height, rng.uniform(0.04, 0.08), base, tip)
            placed += 1
        write_gltf(f"Grass{qi}", [mesh], double_sided=True, roughness=0.75)


def make_reeds():
    """Tall reeds and cattails for the shore; placed in clumps by the scene."""
    rng = random.Random(30)
    mesh = Mesh()
    for _ in range(70):
        r = rng.uniform(0.0, 0.9)
        a = rng.uniform(0.0, math.tau)
        x, z = math.cos(a) * r, math.sin(a) * r
        height = rng.uniform(0.9, 1.7)
        width = rng.uniform(0.03, 0.05)
        angle = rng.uniform(0.0, math.tau)
        side = (math.cos(angle) * width, math.sin(angle) * width)
        lean = (rng.uniform(-0.12, 0.12), rng.uniform(-0.12, 0.12))
        normal = (0.0, 0.8, 0.6)
        base, tip = (0.06, 0.12, 0.03), (0.36, 0.44, 0.14)
        p0 = mesh.vertex((x - side[0], 0.0, z - side[1]), normal, base)
        p1 = mesh.vertex((x + side[0], 0.0, z + side[1]), normal, base)
        p2 = mesh.vertex((x + lean[0] * height, height, z + lean[1] * height), normal, tip)
        mesh.indices += [p0, p1, p2]
        if rng.random() < 0.3:
            # A cattail head: a small brown box near the top.
            cx, cy, cz = x + lean[0] * height * 0.85, height * 0.85, z + lean[1] * height * 0.85
            box(mesh, (cx, cy, cz), (0.035, 0.12, 0.035), (0.20, 0.10, 0.04))
    write_gltf("Reeds", [mesh], double_sided=True, roughness=0.8)


def box(mesh, centre, half, colour):
    cx, cy, cz = centre
    hx, hy, hz = half
    faces = [((1, 0, 0), [(1, -1, -1), (1, 1, -1), (1, 1, 1), (1, -1, 1)]),
             ((-1, 0, 0), [(-1, -1, 1), (-1, 1, 1), (-1, 1, -1), (-1, -1, -1)]),
             ((0, 1, 0), [(-1, 1, -1), (-1, 1, 1), (1, 1, 1), (1, 1, -1)]),
             ((0, -1, 0), [(-1, -1, 1), (-1, -1, -1), (1, -1, -1), (1, -1, 1)]),
             ((0, 0, 1), [(1, -1, 1), (1, 1, 1), (-1, 1, 1), (-1, -1, 1)]),
             ((0, 0, -1), [(-1, -1, -1), (-1, 1, -1), (1, 1, -1), (1, -1, -1)])]
    for normal, corners in faces:
        ids = [mesh.vertex((cx + sx * hx, cy + sy * hy, cz + sz * hz), normal, colour + (1.0,))
               for sx, sy, sz in corners]
        mesh.indices += [ids[0], ids[2], ids[1], ids[0], ids[3], ids[2]]


def cone(mesh, base_y, top_y, radius, sides, colour_base, colour_top, rng, jitter=0.0):
    ring = []
    for k in range(sides):
        a = k / sides * math.tau
        r = radius * (1.0 + rng.uniform(-jitter, jitter))
        ring.append((math.cos(a) * r, base_y, math.sin(a) * r))
    slope = radius / (top_y - base_y)
    for k in range(sides):
        p0, p1 = ring[k], ring[(k + 1) % sides]
        a = (k + 0.5) / sides * math.tau
        normal = (math.cos(a), slope, math.sin(a))
        length = math.sqrt(sum(c * c for c in normal))
        normal = tuple(c / length for c in normal)
        i0 = mesh.vertex(p0, normal, colour_base)
        i1 = mesh.vertex(p1, normal, colour_base)
        i2 = mesh.vertex((0.0, top_y, 0.0), normal, colour_top)
        mesh.indices += [i0, i2, i1]
    # Underside, so a cone seen from below is closed.
    centre = mesh.vertex((0.0, base_y, 0.0), (0.0, -1.0, 0.0), colour_base)
    for k in range(sides):
        i0 = mesh.vertex(ring[k], (0.0, -1.0, 0.0), colour_base)
        i1 = mesh.vertex(ring[(k + 1) % sides], (0.0, -1.0, 0.0), colour_base)
        mesh.indices += [centre, i0, i1]


def cylinder(mesh, radius, height, sides, colour):
    for k in range(sides):
        a0 = k / sides * math.tau
        a1 = (k + 1) / sides * math.tau
        n0 = (math.cos(a0), 0.0, math.sin(a0))
        n1 = (math.cos(a1), 0.0, math.sin(a1))
        b0 = mesh.vertex((n0[0] * radius, 0.0, n0[2] * radius), n0, colour)
        b1 = mesh.vertex((n1[0] * radius, 0.0, n1[2] * radius), n1, colour)
        t0 = mesh.vertex((n0[0] * radius * 0.7, height, n0[2] * radius * 0.7), n0, colour)
        t1 = mesh.vertex((n1[0] * radius * 0.7, height, n1[2] * radius * 0.7), n1, colour)
        mesh.indices += [b0, t0, b1, b1, t0, t1]


def make_pine():
    rng = random.Random(40)
    mesh = Mesh()
    cylinder(mesh, 0.18, 1.6, 7, (0.16, 0.09, 0.05, 1.0))
    levels = [(1.0, 3.0, 1.5), (2.0, 4.0, 1.15), (3.0, 4.9, 0.8)]
    for base, top, radius in levels:
        cone(mesh, base, top, radius, 9, (0.03, 0.10, 0.04, 1.0), (0.08, 0.20, 0.07, 1.0), rng, jitter=0.12)
    write_gltf("Pine", [mesh], roughness=0.85)


def make_rock():
    """A subdivided octahedron pushed about with noise: a rounded boulder."""
    rng = random.Random(50)
    verts = [(1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)]
    tris = [(0, 2, 4), (4, 2, 1), (1, 2, 5), (5, 2, 0), (4, 3, 0), (1, 3, 4), (5, 3, 1), (0, 3, 5)]
    for _ in range(3):
        cache, new_tris = {}, []

        def midpoint(a, b):
            key = (min(a, b), max(a, b))
            if key not in cache:
                p = [(verts[a][i] + verts[b][i]) / 2 for i in range(3)]
                length = math.sqrt(sum(c * c for c in p))
                verts.append(tuple(c / length for c in p))
                cache[key] = len(verts) - 1
            return cache[key]

        for a, b, c in tris:
            ab, bc, ca = midpoint(a, b), midpoint(b, c), midpoint(c, a)
            new_tris += [(a, ab, ca), (b, bc, ab), (c, ca, bc), (ab, bc, ca)]
        tris = new_tris
    bumps = [(rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(0.1, 0.25)) for _ in range(9)]
    shaped = []
    for v in verts:
        r = 1.0 + sum(s * math.exp(-((v[0] - bx) ** 2 + (v[1] - by) ** 2 + (v[2] - bz) ** 2) * 3.0)
                      for bx, by, bz, s in bumps)
        shaped.append((v[0] * r, v[1] * r * 0.65, v[2] * r))
    mesh = Mesh()
    # Flat-shaded facets read as stone; colour darkens toward the bottom.
    for a, b, c in tris:
        pa, pb, pc = shaped[a], shaped[b], shaped[c]
        u = [pb[i] - pa[i] for i in range(3)]
        w = [pc[i] - pa[i] for i in range(3)]
        n = (u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0])
        length = math.sqrt(sum(x * x for x in n)) or 1.0
        n = tuple(x / length for x in n)
        if sum(n[i] * (pa[i] + pb[i] + pc[i]) for i in range(3)) < 0:
            n = tuple(-x for x in n)
            pb, pc = pc, pb
        shade = 0.25 + 0.08 * rng.random()
        moss = max(n[1], 0.0) ** 3 * 0.6
        colour = mix((shade, shade * 0.97, shade * 0.9), (0.12, 0.2, 0.06), moss) + (1.0,)
        ids = [mesh.vertex(p, n, colour) for p in (pa, pb, pc)]
        mesh.indices += ids
    write_gltf("Rock", [mesh], roughness=0.8)


def main():
    os.makedirs(OUT, exist_ok=True)
    print("PondDemo assets ->", os.path.normpath(OUT))
    make_ground()
    make_grass_patches()
    make_reeds()
    make_pine()
    make_rock()
    # Heights the scene needs for placements, printed for reference.
    for name, (x, z) in {"character": (6.4, 3.4), "reeds A": (-7.6, 3.2), "reeds B": (5.8, -4.6),
                         "reeds C": (-3.0, -5.9)}.items():
        print(f"  ground at {name} ({x}, {z}): {ground_height(x, z):.3f}")


if __name__ == "__main__":
    main()

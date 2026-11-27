#!/usr/bin/env python3
"""Writes THIRD_PARTY_NOTICES.txt: the license of every library a package ships.

    python script/third_party_notices.py --out dist/THIRD_PARTY_NOTICES.txt [--check]

The text comes from the vendored sources, never from a copy kept here: a
library's LICENSE file when it has one, otherwise the notice in its header
(stb, miniaudio, Lua and the RenderDoc API header carry it inline). --check
only verifies that every source is found, which is what the build test does.
Uses only the standard library, like package.py which calls it.
"""

import argparse
import os
import sys

FILE_NAME = "THIRD_PARTY_NOTICES.txt"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VENDOR = os.path.join("UHE", "vendor")


def v(*parts):
    return os.path.join(VENDOR, *parts)


# (name, where it is used, source). A source is a file path; (header, first
# line of the notice, last line of the notice) for a notice inside code; or
# {"note", "file", "needs"}: a standard license text with a note naming the
# holder, for a library whose vendored copy ships without one.
LIBRARIES = [
    ("basis_universal", "KTX2/Basis texture encoding and transcoding", v("basis_universal", "LICENSE")),
    ("Zstandard (in basis_universal)", "cooked file and KTX2 compression",
     v("basis_universal", "LICENSES", "BSD-3-clause.txt")),
    ("Box2D", "2D physics", v("box2d", "LICENSE")),
    ("EnTT", "entity component system", v("entt", "LICENSE.txt")),
    ("fastgltf", "glTF import", v("fastgltf", "LICENSE.md")),
    # fastgltf downloads simdjson's single header, which carries no license text.
    ("simdjson (in fastgltf)", "glTF import",
     {"note": "simdjson is dual-licensed under Apache-2.0 or MIT; it is used here under Apache-2.0.\n"
              "Copyright 2018-2025 The simdjson authors.\n\n",
      "file": v("basis_universal", "LICENSES", "Apache-2.0.txt"),
      "needs": v("fastgltf", "deps", "simdjson", "simdjson.h")}),
    ("GLFW", "windows and input", v("GLFW", "LICENSE.md")),
    ("GLM", "math", v("glm", "copying.txt")),
    ("Dear ImGui", "editor and debug UI", v("imgui", "LICENSE.txt")),
    ("ImGuizmo", "editor gizmos", v("imGuizmo", "LICENSE")),
    ("Jolt Physics", "3D physics", v("jolt", "LICENSE")),
    ("Lua", "scripting", (v("lua", "src", "lua.h"), "Copyright (C) 1994-", "SOFTWARE.")),
    ("sol2", "Lua bindings", v("sol2", "LICENSE.txt")),
    ("meshoptimizer", "mesh optimization and decoding", v("meshoptimizer", "LICENSE.md")),
    ("miniaudio", "audio", (v("miniaudio", "miniaudio.h"), "This software is available as a choice", "SOFTWARE.")),
    ("msdf-atlas-gen", "font atlases", v("msdf-atlas-gen", "LICENSE.txt")),
    ("msdfgen", "font atlases", v("msdf-atlas-gen", "msdfgen", "LICENSE.txt")),
    ("RenderDoc in-application API", "GPU capture", (v("renderdoc", "renderdoc_app.h"), "The MIT License", "THE SOFTWARE.")),
    ("Slang", "shader compiler", v("bin", "windows" if os.name == "nt" else "linux", "slang", "LICENSE")),
    ("spdlog", "logging", v("spdlog", "LICENSE")),
    ("stb_image", "image decoding", (v("stb_image", "stb_image.h"), "This software is available under 2 licenses",
                                     "SOFTWARE.")),
    ("Tracy", "profiler", v("tracy", "LICENSE")),
    ("volk", "Vulkan loader", v("volk", "LICENSE.md")),
    ("Vulkan Memory Allocator", "GPU memory", v("VulkanMemoryAllocator", "LICENSE.txt")),
    ("yaml-cpp", "scene and project files", v("yaml-cpp", "LICENSE")),
]


def _strip_comment(line):
    """A notice line without the comment decoration around it."""
    text = line.rstrip("\n")
    for prefix in ("** ", "* ", " * ", "**", "*"):
        if text.lstrip().startswith(prefix.strip()) and text.strip() not in ("*/", "/*"):
            text = text.lstrip()[len(prefix.strip()):]
            if text.startswith(" "):
                text = text[1:]
            break
    return text.rstrip()


def notice_from_header(path, first, last):
    """The block of a header from the line holding `first` to the next line holding `last`."""
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.readlines()
    # The last occurrence: stb and miniaudio mention the license near the top too.
    starts = [i for i, line in enumerate(lines) if first in line]
    if not starts:
        return None
    start = starts[-1]
    for end in range(start, len(lines)):
        if last in lines[end]:
            return "\n".join(_strip_comment(line) for line in lines[start:end + 1]).strip() + "\n"
    return None


def read_notice(source):
    if isinstance(source, dict):
        if not os.path.isfile(os.path.join(ROOT, source["needs"])):
            return None
        text = read_notice(source["file"])
        return source["note"] + text if text else None
    if isinstance(source, tuple):
        path, first, last = source
        full = os.path.join(ROOT, path)
        return notice_from_header(full, first, last) if os.path.isfile(full) else None
    full = os.path.join(ROOT, source)
    if not os.path.isfile(full):
        return None
    with open(full, encoding="utf-8", errors="replace") as f:
        return f.read().strip() + "\n"


def build():
    """The notices text, or exits naming every library whose license was not found."""
    parts = ["Third-party software in UNHUMAN Engine\n",
             "=" * 38 + "\n\n",
             "This package contains the following libraries. Each is used under the\n"
             "license reproduced below.\n\n"]
    missing = []
    for name, use, source in LIBRARIES:
        text = read_notice(source)
        if text is None:
            where = source[0] if isinstance(source, tuple) else source["needs"] if isinstance(source, dict) else source
            missing.append(f"{name} ({where})")
            continue
        parts.append("-" * 78 + "\n")
        parts.append(f"{name} ({use})\n")
        parts.append("-" * 78 + "\n\n")
        parts.append(text + "\n")
    if missing:
        sys.exit("third_party_notices: no license found for " + ", ".join(missing))
    return "".join(parts)


def write(out_path):
    text = build()
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return len(LIBRARIES)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out", help="where to write THIRD_PARTY_NOTICES.txt")
    parser.add_argument("--check", action="store_true", help="only check every license is found")
    args = parser.parse_args()
    if args.check:
        build()
        print(f"third_party_notices: {len(LIBRARIES)} licenses found")
        return
    if not args.out:
        parser.error("--out is required unless --check")
    print(f"third_party_notices: {write(args.out)} licenses -> {args.out}")


if __name__ == "__main__":
    main()

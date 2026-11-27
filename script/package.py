#!/usr/bin/env python3
"""Packages a build into the folders a user downloads.

    python script/package.py --bin bin/Release-linux-x64 --out dist [--name linux-x64] [--zip]

Writes, under --out:
  UHE-Editor-<name>/   the editor, the game runtime and the export tool with
                       every shared library they load, the engine assets and
                       the example projects. File > Export Game works from it.
  PondDemo-<name>/     the PondDemo project exported as a game (uhe_export).

Both carry THIRD_PARTY_NOTICES.txt, the licenses of the libraries they ship
(script/third_party_notices.py). The editor's copy sits beside UHE_RUNTIME, so
a game exported from the editor bundle gets it too (GameExporter).

The build folder also holds unit tests, static libraries, the Sandbox and
CMake leftovers; none of those are copied. On Linux the executables and
libraries are stripped of their symbols when `strip` is installed. --zip also writes a .zip of each
folder next to it. Uses only the standard library, so it runs the same on the
Linux and Windows CI runners.
"""

import argparse
import os
import shutil
import subprocess
import sys

import third_party_notices

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# What the editor bundle runs: the editor, the runtime its Export Game copies,
# and the command-line exporter and shader compiler.
EXECUTABLES = ["UHE_EDITOR", "UHE_RUNTIME", "uhe_export", "uhe_shaderc"]
# Example projects shipped beside the editor.
PROJECTS = ["PondDemo", "WorldDemo", "GameSnake", "SimulationDrone"]
# The game exported from the build.
DEMO = "PondDemo"
# Project assets are not in git (.gitignore: projects/*/Assets/*). A project
# that generates its own (Tools/generate_*.py, run in name order) gets them
# generated when its start scene is missing.


def exe(name):
    return name + ".exe" if os.name == "nt" else name


def is_shared_library(name):
    return name.endswith(".dll") or name.endswith(".so") or ".so." in name


def copy_editor(bin_dir, dest):
    os.makedirs(dest)
    for name in EXECUTABLES:
        source = os.path.join(bin_dir, exe(name))
        if not os.path.isfile(source):
            sys.exit(f"package: '{source}' is missing; build the {name} target")
        shutil.copy2(source, dest)
    # Every shared library, Slang's compile-time modules included: the editor
    # compiles shaders (hot reload), unlike an exported game.
    for name in sorted(os.listdir(bin_dir)):
        path = os.path.join(bin_dir, name)
        if os.path.isfile(path) and is_shared_library(name):
            shutil.copy2(path, dest, follow_symlinks=True)
        elif os.path.isdir(path) and name.startswith("slang-standard-module"):
            shutil.copytree(path, os.path.join(dest, name))
    shutil.copytree(os.path.join(ROOT, "UHE_EDITOR", "assets"), os.path.join(dest, "assets"))
    for project in PROJECTS:
        source = os.path.join(ROOT, "projects", project)
        if os.path.isdir(source):
            shutil.copytree(source, os.path.join(dest, "projects", project),
                            ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    for name in ["LICENSE", "LICENSE.md", "README.md"]:
        if os.path.isfile(os.path.join(ROOT, name)):
            shutil.copy2(os.path.join(ROOT, name), dest)
    third_party_notices.write(os.path.join(dest, third_party_notices.FILE_NAME))


def start_scene(project):
    """The StartScene the manifest names, relative to the project; None without one."""
    with open(os.path.join(project, "Project.uheproject")) as manifest:
        for line in manifest:
            key, _, value = line.strip().partition(":")
            if key == "StartScene":
                return value.strip().strip('"') or None
    return None


def generate_demo(project):
    scene = start_scene(project)
    tools = os.path.join(project, "Tools")
    if scene is None or os.path.isfile(os.path.join(project, scene)) or not os.path.isdir(tools):
        return
    for script in sorted(f for f in os.listdir(tools) if f.startswith("generate_") and f.endswith(".py")):
        print(f"package: generating {os.path.basename(project)} assets ({script})")
        subprocess.run([sys.executable, os.path.join(tools, script)], check=True)


def export_demo(bin_dir, dest):
    generate_demo(os.path.join(ROOT, "projects", DEMO))
    subprocess.run([os.path.join(bin_dir, exe("uhe_export")),
                    "--project", os.path.join(ROOT, "projects", DEMO),
                    "--out", dest,
                    "--runtime-dir", bin_dir,
                    "--engine-assets", os.path.join(ROOT, "UHE_EDITOR", "assets")], check=True)


def strip_binaries(folder):
    """Strips symbols from every ELF file in a package (Linux; a no-op elsewhere)."""
    strip = shutil.which("strip")
    if os.name == "nt" or strip is None:
        return
    for base, _, files in os.walk(folder):
        for name in files:
            path = os.path.join(base, name)
            with open(path, "rb") as f:
                if f.read(4) != b"\x7fELF":
                    continue
            subprocess.run([strip, "--strip-unneeded", path], check=True)


def size_mb(folder):
    total = 0
    for base, _, files in os.walk(folder):
        total += sum(os.path.getsize(os.path.join(base, f)) for f in files)
    return total / (1024 * 1024)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--bin", required=True, help="the build's output folder, e.g. bin/Release-linux-x64")
    parser.add_argument("--out", required=True, help="where the packages are written; must not exist yet")
    parser.add_argument("--name", default="linux-x64" if os.name != "nt" else "windows-x64",
                        help="platform suffix of the package names")
    parser.add_argument("--zip", action="store_true", help="also write a .zip of each package")
    args = parser.parse_args()

    bin_dir = os.path.abspath(args.bin)
    out = os.path.abspath(args.out)
    if os.path.exists(out):
        sys.exit(f"package: '{out}' already exists; choose a new folder")
    os.makedirs(out)

    packages = [os.path.join(out, f"UHE-Editor-{args.name}"), os.path.join(out, f"{DEMO}-{args.name}")]
    # First, so the editor's copy of the example projects includes their generated assets.
    for project in PROJECTS:
        if os.path.isdir(os.path.join(ROOT, "projects", project)):
            generate_demo(os.path.join(ROOT, "projects", project))
    copy_editor(bin_dir, packages[0])
    export_demo(bin_dir, packages[1])
    third_party_notices.write(os.path.join(packages[1], third_party_notices.FILE_NAME))
    for package in packages:
        strip_binaries(package)
        print(f"package: {os.path.relpath(package, out)} ({size_mb(package):.0f} MB)")
        if args.zip:
            archive = shutil.make_archive(package, "zip", root_dir=out, base_dir=os.path.basename(package))
            print(f"package: {os.path.basename(archive)} ({os.path.getsize(archive) / (1024 * 1024):.0f} MB)")


if __name__ == "__main__":
    main()

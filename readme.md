<div align="center">
  <h1>Unhuman Engine (UHE)</h1>
  <p>A modern, high-performance C++ robotics simulation engine powered by Vulkan and Slang.</p>

---

## Showcase

<div align="center">
  <!-- Place an image in the demo/ folder named 'editor_showcase.png' -->
  <img src="demo/image.png" alt="Unhuman Environment Editor" width="80%">
  <br>
  <em>Unhuman Environment & Simulation Editor</em>
  <br><br>
  <!-- YouTube Demo Video -->
  <a href="https://youtu.be/CHDIU61auYo">
    <img src="https://img.youtube.com/vi/CHDIU61auYo/maxresdefault.jpg" alt="Robotics Simulation Demo" width="80%">
  </a>
  <br>
  <em>Real-time Kinematics & Physics Simulation Demo</em>
</div>

**Unhuman Engine (UHE)** is a lightweight, highly extensible C++ simulation engine designed for real-time robotics applications, synthetic data generation, and embodied AI. It utilizes a modern **Vulkan RHI** backend, compiles compute and rendering shaders via **Slang**, and features a fully integrated ImGui editor for environment authoring and robot introspection.

## Key Features
- **Modern Graphics Backend**: Fully abstracted Render Hardware Interface (RHI) running on **Vulkan**, optimized for high-fidelity sensor rendering (RGB, Depth, Segmentation).
- **Slang Shader Compiler**: Next-generation shading language support for accelerating robot perception algorithms and compute workloads via SPIR-V.
- **Multi-Threading**: High-performance Job System and Task Graph architecture for parallel execution of complex multi-agent simulations.
- **Text & HUD Rendering**: Crisp MSDF text rendering for on-screen telemetry, spatial UI, and diagnostic overlays.
- **UHE Editor**: A robust, dockable ImGui-based editor (`UHE_EDITOR`) for scene inspection, kinematic profiling, and asset management.
- **Entity Component System**: A fast, data-driven scene system (`entt`) supporting native script components, massive robot swarms, and deterministic serialization.
- **2D & 3D Physics Dynamics**: Integrated rigid-body physics handling with `Box2D` and `Jolt` (upcoming) for accurate locomotion, grasping, and collision simulation.
- **AAA Dependency Management**: No bloated submodules. The engine fetches precompiled binaries (Slang, GLFW) automatically for rapid environment deployment.
- **Cross-Platform Tooling**: Generates Ninja builds for Linux (ROS compatibility environments) and Visual Studio 2022 solutions for Windows with a single click.
## Recent Updates

- **Core Job System**: Implemented a robust JobSystem and TaskGraph for multi-threading.
- **Text Rendering**: Added comprehensive MSDF text rendering capabilities within the Vulkan backend.
- **Vulkan Enhancements**: Setup Vulkan sync2 fallback and implemented improved texture fallback safety.
- **Engine Stability & Fixes**: Fixed MSVC linker issues, UTF-8 decoding mismatches, and asset loading for standalone distribution.
- **Vulkan Refactor & AMD Stability**: Resolved texture dynamic indexing validation issues on AMD/RADV drivers by manually binding texture slots in Slang.
- **Color Accuracy & Aesthetics**: Restored pure black ImGui docking themes and fixed sRGB gamma-correction bugs that were washing out linear textures.
- **Improved UI Workflows**: Added full Drag-and-Drop payload support for textures directly into the 3D Viewport and Scene Hierarchy component inspectors.

## Architecture Overview

The repository is logically split to ensure the core engine remains separate from the application logic:

- `UHE/` — The core engine, platform abstraction (Windows/Linux), Vulkan RHI, and vendor libraries.
- `UHE_EDITOR/` — The standalone editor application built for scene authoring and robot introspection.
- `sandbox/` — A lightweight testing application for running isolated control logic and environments.
- `script/Setup.py` — The automated dependency fetcher that pulls heavy OS-specific binaries (like Slang) into `UHE/vendor/bin/`.
- `docs/` — Design docs, architecture notes and the work order. Start at [`docs/README.md`](docs/README.md); the roadmap is [`docs/ROADMAP.md`](docs/ROADMAP.md).

## Documentation

- [`IDEA.md`](IDEA.md) — what the engine is, why it exists, and its hard constraints.
- [`docs/ROADMAP.md`](docs/ROADMAP.md) — milestones, issue map and the open decision register.
- [`docs/architecture/`](docs/architecture/) — sync/tiers, render graph, job system, assets & audio, CI/CD.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — style, workflow and commit conventions.

## Getting Started

> [!NOTE]
> **Hardware Compatibility:** Because the engine relies on a custom Vulkan RHI, it may not compile or run on all machines (especially older hardware or unsupported drivers). If you are unable to compile the project, please check out the **video preview** in the Showcase section above and stay tuned for future precompiled releases and updates!

UHE uses a fully automated bootstrap system. You do not need to manually configure CMake or download binaries.

### Prerequisites
- A C++20 compatible compiler (GCC, Clang, or MSVC)
- CMake 3.16+
- Python 3 (Used for fetching precompiled binaries)
- Vulkan SDK

### Windows Installation
1. Clone the repository recursively (to fetch the lightweight submodules like `glm` and `spdlog`):
   ```cmd
   git clone --recursive https://github.com/unhuman-engine/UNHUMAN.git
   cd unhuman
   ```
2. Run the generation script. This will download the Windows binaries for Slang and generate a `.sln` file:
   ```cmd
   GenerateProject.bat
   ```
3. Open `build/UHE.sln` in Visual Studio 2022 and hit **Build**.

### Linux Installation
1. Clone the repository recursively:
   ```bash
   git clone --recursive https://github.com/unhuman-engine/UNHUMAN.git
   cd unhuman
   ```
2. Run the generation script. This will download the Linux binaries for Slang and generate `build.ninja`:
   ```bash
   ./GenerateProject.sh
   ```
3. Compile the engine using CMake:
   ```bash
   cmake --build build -j$(nproc)
   ```

*Note: The built executables (`UHE_EDITOR`, `Sandbox`) will be placed in the `bin/` directory.*

## Contributing

Contributions are always welcome! If you want to contribute to the engine:
1. Ensure your code complies with the project's `.clang-format` and `.clang-tidy` rules. (If using Neovim/VS Code with `clangd`, this will be automatic).
2. Avoid adding heavy binaries directly to the repository. If you need a new dependency (like Assimp), add its download URL to the `DEPENDENCIES` dictionary in `script/Setup.py`.

## 📄 License

Unhuman Engine is licensed under the **MIT License**. See the [LICENSE](LICENSE) file for more details.

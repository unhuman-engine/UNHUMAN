<!-- UHE docs — index: README.md · roadmap: ROADMAP.md -->
# Building Unhuman Engine

> [!NOTE]
> The engine needs a Vulkan 1.3 driver. Older hardware or unsupported drivers may not run it.

The bootstrap scripts fetch the precompiled binaries (Slang, GLFW) and configure CMake for you.

## Prerequisites

- A C++20 compiler (GCC, Clang or MSVC)
- CMake 3.16+
- Python 3
- Vulkan SDK

## Windows

```cmd
git clone --recursive https://github.com/unhuman-engine/UNHUMAN.git
cd UNHUMAN
GenerateProject.bat
```

Open `build/UHE.sln` in Visual Studio 2022 and build.

## Linux

```bash
git clone --recursive https://github.com/unhuman-engine/UNHUMAN.git
cd UNHUMAN
./GenerateProject.sh
cmake --build build -j4
```

The executables (`UHE_EDITOR`, `Sandbox`) land in `bin/`.

## Dependencies

Do not commit heavy binaries. A new dependency is a download URL in the `DEPENDENCIES` dictionary of `script/Setup.py`.

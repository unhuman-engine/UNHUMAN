<!-- UHE docs — index: ../README.md · roadmap: ../ROADMAP.md -->

> **Status:** `IN PROGRESS`. Build, sanitizer and headless-render workflows run,
> but there are no in-tree unit tests or CTest target yet, which is the central
> point of this document. Related issues:
> [#9](https://github.com/unhuman-engine/UNHUMAN/issues/9),
> [#10](https://github.com/unhuman-engine/UNHUMAN/issues/10).
> **Code lands in:** `.github/workflows/`, `.github/actions/`, `ci/`,
> `CMakePresets.json`.
> **Snapshot:** re-verify action SHAs and SDK or NDK pins when implementing.

# CI/CD

Plan for turning the current four workflows into a tiered, cacheable,
multi-platform pipeline with complete sanitizer coverage.

## Summary

The hard parts of sanitizer wiring already exist, namely the CMake toggle and the
presets, along with a headless smoke pattern that is better than most. What is
missing is structure:

1. Tiers, so a pull request gets fast feedback and main or nightly gets the
   expensive runs.
2. Reuse, so setup is not copy-pasted into four files.
3. CTest and a runtime test binary, so tests are real and composable.
4. A matrix, so compilers, sanitizers and platforms are data rather than
   duplicated jobs.
5. Correctness depth: validation-layer-strict runs, golden images, and a
   real-GPU runner, since software Lavapipe cannot catch driver or sync bugs.
6. Release on tags and a nightly soak.

The highest-value work first is concurrency, caching, a format gate and CTest,
then a UBSan and Clang matrix with static analysis, then Android and macOS, then
GPU runners.

## Principles

```
Tier 0  Fast gates        (<2 min, every push)   format, cmake configure, actionlint
Tier 1  Build and unit    (5-15 min, every PR)   compiler and build-type matrix, ctest -L unit
Tier 2  Deep correctness  (15-45 min, PR/main)   asan, asan+ubsan, tsan, msan, clang-tidy
Tier 3  Platform matrix   (10-40 min, main)      windows, macos, android compile
Tier 4  GPU correctness   (self-hosted, main)    validation-strict, golden images, real ICDs
Tier 5  Release, nightly  (tags, cron)           packages, releases, long soak
```

Rules that keep it manageable: fail fast and cheap, so format and configure run
before a thirty-minute sanitizer build; one build per configuration, cached with
ccache or sccache, so the matrix is affordable; everything reproducible locally
through `ci/` scripts that the workflows wrap; data over duplication, using the
matrix for compiler, sanitizer and platform and reusable workflows for setup; and
least privilege, with read-only permissions by default and actions pinned by SHA.

## Current state and gaps

| Workflow | Does | Gaps |
|---|---|---|
| `build-linux.yml` | Release build, artifact | No tests, patches vendor in CI, no cache |
| `build-windows.yml` | Release build, artifact | Never runs anything, vcpkg not cached |
| `test-renderer.yml` | ASAN editor, treats `timeout` exit 124 as pass | Fragile pass criterion, deprecated `UHE_ENABLE_ASAN`, predates the presets |
| `sanitizers.yml` | ASan and TSan sandbox smoke via Lavapipe | TSan suppresses all of `libvulkan`, which can hide real races; no UBSan or MSan; no shared matrix |

Systemic gaps: no `enable_testing`, `add_test` or CTest anywhere, with
`UHE/src/UHE/Test/*` as empty stubs, and the job-system tests stand alone rather
than running in CI; no caching of ccache, the Vulkan SDK or vcpkg; no
`concurrency` block, so superseded runs keep consuming minutes; no format or
static-analysis gate; vendor CMake patched by rewriting files in CI, which means
the in-repo vendor configuration is wrong for Linux; Windows and macOS never run
tests; no Android or macOS workflows; Lavapipe only, so there is no real driver
coverage for Sync2, dynamic rendering or sync bugs; and actions are not pinned
by SHA, with no Dependabot.

## Building blocks

### Composite actions

Extract the repeated setup into one place:

```
.github/actions/setup-linux/action.yml     # apt deps, Vulkan SDK, Slang, ccache
.github/actions/setup-windows/action.yml   # Vulkan SDK, vcpkg, MSVC
.github/actions/setup-android/action.yml   # NDK and CMake toolchain
```

A reusable workflow that configures, builds and runs tests takes `os`,
`compiler`, `sanitizer` and `build_type` as inputs:

```yaml
on:
  workflow_call:
    inputs:
      os: { type: string, required: true }
      compiler: { type: string, default: "" }
      sanitizer: { type: string, default: "" }
      build_type: { type: string, default: Debug }
jobs:
  build-test:
    runs-on: ${{ inputs.os }}
    steps:
      - uses: actions/checkout@<sha>
        with: { submodules: recursive, persist-credentials: false }
      - uses: ./.github/actions/setup-linux
      - run: cmake --preset ci -DCMAKE_BUILD_TYPE=${{ inputs.build_type }} -DUHE_SANITIZER=${{ inputs.sanitizer }}
      - run: cmake --build --preset ci --parallel
      - run: ctest --preset ci --output-on-failure
```

### Caching

- ccache for GCC and Clang, sccache for MSVC and clang-cl, set through
  `CMAKE_CXX_COMPILER_LAUNCHER` with `actions/cache` on the cache directory,
  keyed on the compiler and a hash of the CMake files and sources. This is what
  makes a ten-configuration matrix affordable.
- Cache the `script/Setup.py` downloads, such as Slang, keyed on version.
- Cache the Vulkan SDK install prefix and skip reinstalling on a hit.
- Cache the vcpkg binary cache on Windows.
- Do not cache CMake or Ninja build directories across runners; ccache is the
  lever.

### Local parity

```
ci/
  setup-linux.sh
  setup-windows.ps1
  configure.sh
  test.sh          # ctest --output-on-failure
  sanitize.sh      # one sanitizer preset
  coverage.sh
  run.sh           # setup, configure, build, test
```

Workflows call these scripts and contributors run the same thing locally, which
avoids CI-only failures.

### Concurrency, permissions and pinning

```yaml
permissions:
  contents: read
concurrency:
  group: ${{ github.workflow }}-${{ github.ref }}
  cancel-in-progress: true
```

Pin every action to a full commit SHA, add a weekly Dependabot configuration for
GitHub Actions, and run actionlint as a Tier-0 job to validate the workflow YAML
itself.

## Test infrastructure

The largest missing piece is real tests.

Add a `UHE_TESTS` option and a test executable per subsystem, or one runner using
doctest or Catch2, with doctest being the smallest to adopt:

```cmake
option(UHE_TESTS "Build tests" ON)
if(UHE_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
```

Register tests with labels so CI can select them:

```cmake
add_executable(uhe_tests_jobs tests/jobs.cpp src/UHE/Jobsystem/Jobsystem.cpp)
target_link_libraries(uhe_tests_jobs PRIVATE doctest UHE)
add_test(NAME jobs.stress COMMAND uhe_tests_jobs --test-case=stress)
set_tests_properties(jobs.stress PROPERTIES LABELS "unit;thread")
```

Then `ctest -L unit` runs the fast tests without a GPU, and `ctest -L gpu` runs
the ones that need a Vulkan ICD and a display.

The standalone job-system test binary described in the jobsystem document, with
its ten-thousand-job counter test, ParallelFor, DAG stress and soak runs, is the
most valuable test suite and needs no GPU, which makes it ideal for TSan and
MSan. It should be committed under `tests/` and run in CI.

GPU tests set `VK_ICD_FILENAMES` through the test property so CTest can run them
headlessly, keep the readiness-gate idea from `sanitizers.yml` as a `gpu.smoke`
test, and use labels such as `unit`, `gpu`, `slow` and `soak`.

A `ci` preset avoids hand-writing `-B build -G Ninja` in every workflow:

```json
{ "name": "ci", "inherits": "base", "binaryDir": "${sourceDir}/build_ci" }
```

## Sanitizers

The CMake plumbing exists, so run it as a matrix with each sanitizer in its own
preset directory and the right test set.

| Sanitizer | Toolchain | Runs | Notes |
|---|---|---|---|
| ASan | GCC or Clang | unit and GPU smoke | `detect_leaks=1`, `detect_stack_use_after_return=1` |
| ASan+UBSan | Clang | unit and GPU smoke | cheap combination, halts on error |
| UBSan | GCC or Clang | unit and GPU smoke | `print_stacktrace=1` |
| LSan | Clang | unit | usually redundant with ASan |
| TSan | Clang or GCC | unit only | needs `vm.mmap_rnd_bits=28`, see below |
| MSan | Clang with libc++ | unit only, narrow | all linked code must be instrumented |

### TSan and Vulkan

The current TSan job suppresses `called_from_lib:libvulkan`, which can hide real
races that merely have a stack frame in the driver. The fix is to run TSan on a
headless test binary with no Vulkan dependency, covering the job system, the task
graph, the asset manager and containers. That gives real race detection. If a GPU
test is ever needed under TSan, the suppression should match specific known
driver symbols rather than the whole library.

### MSan

MSan is only practical with Clang and libc++ across the whole tree, since every
linked object including vendor code must be instrumented or it reports false
positives. Run it on a small unit binary of pure engine code first, not
repository-wide.

### Lavapipe coverage

Lavapipe is useful for functional smoke tests, validation-layer errors, leaks
under ASan and headless determinism. It does not cover Sync2 driver semantics,
queue family behaviour, timeline semaphore edge cases or vendor-specific hazards.
A real-GPU runner is needed for that; until then, Lavapipe should not be treated
as driver correctness.

## Static analysis and formatting

Tier 0, fast:

```yaml
- name: clang-format
  run: find UHE UHEGAME sandbox -name '*.cpp' -o -name '*.h' | xargs clang-format --dry-run --Werror
- name: actionlint
  uses: raven-actions/actionlint@<sha>
```

Tier 2, progressive: clang-tidy through `CMAKE_CXX_CLANG_TIDY` on a baseline,
either only changed files or a `.clang-tidy` that starts with `bugprone-*`,
`performance-*` and `clang-analyzer-*` and grows, rather than running
repository-wide from the start. cmake-lint catches CMake drift. include-what-you-use,
cppcheck and CodeQL are optional, with CodeQL free for public repositories and a
reasonable security gate. Compiler warnings-as-errors in a dedicated
configuration finds drift without breaking normal builds.

## Platform matrix

| Platform | Build | Tests | Notes |
|---|---|---|---|
| Linux GCC | yes | unit and GPU with Lavapipe | primary |
| Linux Clang | yes | unit | different diagnostics and sanitizers |
| Windows MSVC | yes | unit | currently builds only; add ctest |
| Windows clang-cl | optional | unit | portability |
| macOS | yes | unit | Metal later; at least compile-check |
| Android | compile only | none | the Vulkan 1.1 fallback target |

The Android compile gate uses the SDK setup action and the NDK toolchain file to
build without a device. It directly protects the legacy Sync fallback path,
because it forces the Vulkan 1.1 assumptions to compile. Device or emulator smoke
tests can come later through Gradle instrumented tests or `adb` on a self-hosted
runner.

macOS on `macos-latest` with Homebrew dependencies and `ctest -L unit` is a cheap
portability check even without targeting the platform, since libc++, Clang and
`std::filesystem` differ.

## GPU correctness

This is where a Vulkan engine's real bugs live.

1. Run the smoke with the validation layer and synchronization validation
   enabled, and fail the job if any validation error line appears. This alone
   finds the hardcoded-`oldLayout` class of bugs.
2. GPU-assisted validation catches out-of-bounds descriptors on a real GPU.
3. Golden images. Render a fixed scene with a fixed seed and camera and AA off,
   capture the framebuffer, and compare against a baseline with a tolerance
   metric such as SSIM or FLIP rather than exact pixels. Store baselines keyed by
   GPU, or compare only on the software driver for cross-machine stability.
4. A real GPU runner. Hosted runners have no usable GPU, so this means a
   self-hosted machine, a cloud GPU runner, or a nightly-only workflow. A matrix
   across at least two vendors is the goal.
5. Swapchain and resize stress, scripting window resizes and minimize and restore
   to exercise the recreate paths.

## Release and distribution

Trigger on tags, build Release for Linux, Windows and optionally macOS, run the
tests, then create a GitHub release with the artifacts. Stamp the build with
`git describe` through a `UHE_BUILD_VERSION` definition, print it at startup and
include it in the artifact name. Keep the branch artifact upload as well.

Because the project vendors many libraries, generate a third-party license
bundle or SBOM and ship `THIRD_PARTY_LICENSES`. Pin the Vulkan SDK and NDK
versions and record them in the artifact for reproducibility.

## Nightly and soak

A scheduled workflow runs what is too slow for pull requests: a long GPU smoke
measured in minutes, extended TSan and MSan unit runs, a job-system thread-count
sweep over one, two, four and eight threads, file descriptor and handle leak
checks, and later fuzzing of asset loaders through libFuzzer.

## Proposed workflow layout

```
.github/
  actions/
    setup-linux/action.yml
    setup-windows/action.yml
    setup-android/action.yml
  workflows/
    ci.yml               # orchestrator for PR and push
    _build-test.yml      # reusable: checkout, setup, configure, build, ctest
    _sanitizers.yml      # reusable: asan, ubsan, tsan matrix
    format.yml           # clang-format, actionlint, cmake-lint
    static-analysis.yml  # clang-tidy baseline, warnings-as-errors, CodeQL
    platform.yml         # windows, macos, android matrix
    gpu.yml              # self-hosted: validation-strict and golden images
    nightly.yml          # scheduled soak, fuzz, sweeps
    release.yml          # tags: packages, release, licenses
```

## Rollout order

Now, a few hours of work with high value:

1. Add `concurrency` and `permissions` to all workflows, pin actions by SHA, add
   Dependabot and actionlint.
2. Add caching for ccache or sccache, the Slang download, the Vulkan SDK and
   vcpkg.
3. Add clang-format and warnings-as-errors jobs.
4. Add `UHE_TESTS`, CTest and a `ci` preset, and move the job-system tests
   in-tree.
5. Fix the vendor CMake files in-tree and delete the CI rewrite hack.

Next, days:

6. Add UBSan and an ASan plus UBSan matrix entry, and convert `test-renderer.yml`
   to the preset and CTest world.
7. Rework TSan to run the headless, Vulkan-free unit binary.
8. Add Linux Clang and Windows ctest to the matrix, and a macOS build.

Then, a week or more:

9. Android compile gate.
10. Validation-strict GPU job that fails on errors, plus a golden-image harness.
11. Self-hosted GPU runner and a vendor matrix.
12. Release workflow on tags with a third-party license bundle.
13. Nightly soak and fuzzing.

## Fixes to the existing workflows

`test-renderer.yml` treats a `timeout` exit of 124 as success, which accepts any
ten-second survival including a wedged or error-spamming process. It should use
the readiness-gate pattern from `sanitizers.yml` and CTest labels instead. It
also uses the deprecated `UHE_ENABLE_ASAN` and should move to the `asan` preset
with `UHE_SANITIZER`.

All workflows rewrite `UHE/vendor/imgui/CMakeLists.txt` and edit imguizmo with
`sed`. Those fixes belong in the repository so CI is a pure checkout.

The TSan suppression covers the whole Vulkan loader and should be narrowed, with
Vulkan-free unit tests providing the signal. Windows CI should run the unit
tests, which need no device, covering the job system, asset manager and scene
serialization. The Linux workflow builds Release only and should at least also
configure Debug so assertion-enabled code is compiled somewhere. The platform
matrix should use `fail-fast: false` so one OS failure does not cancel the
others. Artifact uploads should set `if-no-files-found: error` and include the
version from `git describe`.

## Open questions

- Whether a self-hosted GPU runner is available, which decides whether GPU
  correctness is feasible or stays Lavapipe-only.
- Test framework choice: doctest for minimalism or Catch2 for better matchers.
  Pick one and standardize rather than keeping hand-rolled mains.
- clang-tidy appetite: changed-files baseline or repository-wide with
  suppressions.
- Android ambition: compile-only gate now, or emulator and device smoke soon,
  which would also need a self-hosted runner.
- Whether MSan is worth the libc++ cost before the core is stable.
- Whether to run coverage on one job for trend visibility before the unit suite
  is meaningful.
- Golden-image policy: per-GPU baselines or driver-agnostic tolerance only.

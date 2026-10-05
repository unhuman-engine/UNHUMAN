cmake_minimum_required(VERSION 3.16)
project(UHE)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# Make RPATH/RUNPATH relative to the executable using $ORIGIN
set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set(CMAKE_INSTALL_RPATH "$ORIGIN")


add_compile_definitions(ENTT_STANDARD_CPP)
add_compile_definitions(VULKAN_HPP_NO_STRUCT_CONSTRUCTORS)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Setup submodules automatically

option(VG_PROFILE "Enable Tracy profiling" ON)

# Build + register the no-GPU unit tests under CTest (ctest -L unit).
# See tests/CMakeLists.txt; CI rollout in docs/architecture/ci-cd.md §"Test infrastructure".
option(UHE_TESTS "Build UHE unit tests and register them with CTest" ON)

# ---------------------------------------------------------------------------
# Sanitizer support (GCC / Clang) - one flag to pick them all:
#
#   -DUHE_SANITIZER=address            AddressSanitizer   (heap/stack/use-after-free)
#   -DUHE_SANITIZER=thread             ThreadSanitizer    (data races / deadlocks)
#   -DUHE_SANITIZER=undefined          UndefinedBehaviorSanitizer
#   -DUHE_SANITIZER=leak               LeakSanitizer
#   -DUHE_SANITIZER=memory             MemorySanitizer    (Clang only, needs instrumented libc++)
#   -DUHE_SANITIZER=address,undefined  combos are allowed, EXCEPT tsan/msan
#                                      which must always run alone
#   -DUHE_SANITIZER=none               (default) no sanitizer
#
# Use a SEPARATE build directory per sanitizer (incremental rebuilds of a reused
# dir would mix instrumented and non-instrumented objects). Easiest way:
#
#   cmake --preset tsan          &&  cmake --build --preset tsan -j
#   cmake --preset asan          &&  cmake --build --preset asan -j
#   cmake --preset ubsan         &&  cmake --build --preset ubsan -j
#   cmake --preset msan          &&  cmake --build --preset msan -j
#
# ...or use ONE toggleable dir and switch sanitizers whenever you like:
#
#   cmake --preset sanitizer -DUHE_SANITIZER=address   (first time; also: thread,
#       undefined, leak, memory, address,undefined or none)
#   cmake --build  --preset sanitizer -j
#
#   cmake --preset sanitizer -DUHE_SANITIZER=thread    # same dir -> rebuilds all
#   cmake --preset sanitizer -DUHE_SANITIZER=none      # back to a normal build
#
# The build detects the sanitizer change and deletes stale objects for you
# (Ninja does NOT recompile when flags change, so this is required for
# correctness). Binaries still land in bin/<cfg>-<sys>-x64-<sanitizers>/.
#
# Sanitized binaries land in bin/<cfg>-<sys>-x64-<sanitizers>/ so they never
# overwrite a normal build.
#
# Handy runtime settings:
#   ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1
#   TSAN_OPTIONS=second_deadlock_stack=1        (Vulkan ICDs may cause false
#       positives - put a 'called_from_lib:libvulkan' style suppressions file
#       in TSAN_OPTIONS=suppressions=tsan.supp if the driver misbehaves)
#   UBSAN_OPTIONS=print_stacktrace=1            (errors abort via
#       -fno-sanitize-recover=undefined, which is enabled automatically)
#
# NOTE: everything built by this CMake tree gets instrumented (engine, editor,
# sandbox, vendor libs) - that is required for TSan/MSan correctness anyway.
# ---------------------------------------------------------------------------

# Kept for backward compatibility; mapped onto UHE_SANITIZER below.
option(UHE_ENABLE_ASAN "Enable AddressSanitizer (deprecated: use UHE_SANITIZER=address)" OFF)
option(UHE_ENABLE_MSAN "Enable MemorySanitizer (deprecated: use UHE_SANITIZER=memory)" OFF)

set(UHE_SANITIZER "" CACHE STRING "Sanitizer(s): address, thread, undefined, leak, memory, combos with commas, or none")
set_property(CACHE UHE_SANITIZER PROPERTY STRINGS "" none address thread undefined leak memory "address,undefined")

if(UHE_ENABLE_ASAN AND NOT UHE_SANITIZER)
    set(UHE_SANITIZER "address")
    message(STATUS "UHE_ENABLE_ASAN is deprecated - use -DUHE_SANITIZER=address")
endif()
if(UHE_ENABLE_MSAN AND NOT UHE_SANITIZER)
    set(UHE_SANITIZER "memory")
    message(STATUS "UHE_ENABLE_MSAN is deprecated - use -DUHE_SANITIZER=memory")
endif()

set(UHE_SANITIZER_TAG "")
if(UHE_SANITIZER AND NOT UHE_SANITIZER STREQUAL "none")
    string(REPLACE "," ";" UHE_SANITIZER_LIST "${UHE_SANITIZER}")
    list(TRANSFORM UHE_SANITIZER_LIST TOLOWER)
    list(REMOVE_DUPLICATES UHE_SANITIZER_LIST)

    foreach(san IN LISTS UHE_SANITIZER_LIST)
        if(NOT san MATCHES "^(address|thread|undefined|leak|memory)$")
            message(FATAL_ERROR "Unknown sanitizer '${san}'. Valid: address, thread, undefined, leak, memory (or 'none').")
        endif()
    endforeach()

    list(LENGTH UHE_SANITIZER_LIST UHE_SANITIZER_COUNT)
    list(FIND UHE_SANITIZER_LIST "thread" UHE_HAS_TSAN)
    list(FIND UHE_SANITIZER_LIST "memory" UHE_HAS_MSAN)
    list(FIND UHE_SANITIZER_LIST "undefined" UHE_HAS_UBSAN)

    if((UHE_HAS_TSAN GREATER -1 OR UHE_HAS_MSAN GREATER -1) AND NOT UHE_SANITIZER_COUNT EQUAL 1)
        message(FATAL_ERROR "ThreadSanitizer and MemorySanitizer must be the ONLY sanitizer (got: ${UHE_SANITIZER_LIST}).")
    endif()

    if(MSVC)
        message(FATAL_ERROR "UHE_SANITIZER is only wired up for GCC/Clang toolchains.")
    endif()
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "UHE_SANITIZER requires GCC or Clang (found: ${CMAKE_CXX_COMPILER_ID}).")
    endif()
    if(UHE_HAS_MSAN GREATER -1 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(FATAL_ERROR "MemorySanitizer requires Clang (upstream).")
    endif()

    list(JOIN UHE_SANITIZER_LIST "," UHE_SANITIZER_FLAG)

    # -g: sanitized builds are useless without symbols (cheap, codegen-neutral).
    # -fno-omit-frame-pointer: readable stacks (also helps Tracy/profilers).
    add_compile_options(-fsanitize=${UHE_SANITIZER_FLAG} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${UHE_SANITIZER_FLAG})

    if(UHE_HAS_MSAN GREATER -1)
        add_compile_options(-fPIE)
        add_link_options(-pie)
    endif()

    if(UHE_HAS_UBSAN GREATER -1)
        # Abort on the first UB found - actionable instead of a log spam you ignore.
        add_compile_options(-fno-sanitize-recover=undefined)
    endif()

    set(UHE_SANITIZER_TAG "-${UHE_SANITIZER_FLAG}")
    set(UHE_SANITIZER_EFFECTIVE "${UHE_SANITIZER_FLAG}")
    message(STATUS "Sanitizers enabled: ${UHE_SANITIZER_LIST}")
else()
    set(UHE_SANITIZER_EFFECTIVE "")
endif()

# Clean-slate guard for the shared toggle dir: switching the sanitizer set in the
# same build dir must not leave behind objects built with the old flags (Ninja
# does not recompile when compile/link options change, and link archives/libs
# would silently keep stale members). Delete every object and depfile when the
# set changes; CMake then relinks everything from fresh objects.
#
# NOTE: Ninja also detects changed compile commands and recompiles by itself,
# but this guard additionally catches stale objects of targets/sources that no
# longer exist (renames, disabled targets) - cheap insurance.
if(NOT DEFINED UHE_SANITIZER_LAST)
    set(UHE_SANITIZER_LAST "__UNINITIALIZED__" CACHE INTERNAL "Sanitizer set of the last configure in this dir")
endif()
if(NOT UHE_SANITIZER_LAST STREQUAL "${UHE_SANITIZER_EFFECTIVE}")
    if(NOT UHE_SANITIZER_LAST STREQUAL "__UNINITIALIZED__")
        file(GLOB_RECURSE UHE_STALE_OBJECTS
            "${CMAKE_BINARY_DIR}/CMakeFiles/*.o"
            "${CMAKE_BINARY_DIR}/CMakeFiles/*.d")
        if(UHE_STALE_OBJECTS)
            file(REMOVE ${UHE_STALE_OBJECTS})
            message(STATUS "Sanitizer set changed (${UHE_SANITIZER_LAST} -> ${UHE_SANITIZER_EFFECTIVE}) - stale objects removed, everything rebuilds")
        endif()
    endif()
    set(UHE_SANITIZER_LAST "${UHE_SANITIZER_EFFECTIVE}" CACHE INTERNAL "Sanitizer set of the last configure in this dir" FORCE)
endif()

set(ARCH x64)
string(TOLOWER ${CMAKE_SYSTEM_NAME} SYS_NAME)
if(CMAKE_BUILD_TYPE)
  set(OUTPUT_DIR "${CMAKE_BUILD_TYPE}-${SYS_NAME}-${ARCH}")
else()
  set(OUTPUT_DIR "Debug-${SYS_NAME}-${ARCH}")
endif()

# Keep sanitized builds in their own output dir (never mixes with normal bins)
string(APPEND OUTPUT_DIR "${UHE_SANITIZER_TAG}")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/${OUTPUT_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/${OUTPUT_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/${OUTPUT_DIR})

foreach(CONFIG ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER ${CONFIG} CONFIG_UPPER)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_SOURCE_DIR}/bin/${CONFIG}-${SYS_NAME}-${ARCH}${UHE_SANITIZER_TAG})
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_SOURCE_DIR}/bin/${CONFIG}-${SYS_NAME}-${ARCH}${UHE_SANITIZER_TAG})
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_SOURCE_DIR}/bin/${CONFIG}-${SYS_NAME}-${ARCH}${UHE_SANITIZER_TAG})
endforeach()

# Tests are added AFTER the sanitizer flags and CMAKE_*_OUTPUT_DIRECTORY setup
# above so test targets inherit sanitizer instrumentation and the output layout.
if(UHE_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()


function(auto_group target)
  get_target_property(files ${target} SOURCES)
  foreach(file ${files})
    file(RELATIVE_PATH rel ${CMAKE_SOURCE_DIR} ${file})
    get_filename_component(dir ${rel} PATH)
    string(REPLACE "/" "\\" group ${dir})
    source_group("${group}" FILES ${file})
  endforeach()
endfunction()

# all libs
add_subdirectory(UHE/vendor/GLFW)

# ImGui is built here instead of add_subdirectory(UHE/vendor/imgui): the vendored
# fork's CMakeLists.txt defines IMGUI_API=__declspec(dllexport) unconditionally,
# which breaks GCC/Linux builds
add_library(ImGui OBJECT
    ${CMAKE_CURRENT_SOURCE_DIR}/UHE/vendor/imgui/imgui.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/UHE/vendor/imgui/imgui_draw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/UHE/vendor/imgui/imgui_tables.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/UHE/vendor/imgui/imgui_widgets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/UHE/vendor/imgui/imgui_demo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/UHE/vendor/imgui/misc/cpp/imgui_stdlib.cpp
)
if(MSVC)
    target_compile_definitions(ImGui PRIVATE IMGUI_API=__declspec\(dllexport\))
endif()
target_include_directories(ImGui PUBLIC UHE/vendor/imgui)

add_subdirectory(UHE/vendor/imGuizmo)
add_subdirectory(UHE/vendor/volk)
add_subdirectory(UHE/vendor/VulkanMemoryAllocator)
add_subdirectory(UHE/vendor/yaml-cpp)
add_subdirectory(UHE/vendor/box2d)
add_subdirectory(UHE/vendor/fastgltf)

# Meshoptimizer (https://github.com/zeux/meshoptimizer) - MIT, no dependencies.
# Upstream also builds a demo/ and a gltfpack executable; we only want the
# library. MESHOPT_INSTALL is forced off because the install() rules would
# otherwise pull into `make install` a vendored dep the engine never installs.
set(MESHOPT_BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_GLTFPACK OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(MESHOPT_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(UHE/vendor/meshoptimizer EXCLUDE_FROM_ALL)

# Basis Universal (https://github.com/BinomialLLC/basis_universal) - Apache 2.0.
# KTX2/Basis transcoding for KHR_texture_basisu (issues #29 Tier 4, #42).
# Only the transcoder goes into the runtime: the file's own CMakeLists builds
# the encoder CLI and examples, which the engine never needs at runtime. The
# TEST harness additionally compiles the encoder to generate real KTX2
# fixtures (see tests/gltf). Zstd is in because KTX2 UASTC files are commonly
# zstd-supercompressed and refusing them would look like a loader bug.
set(BASISU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/UHE/vendor/basis_universal")
add_library(basisu_transcoder STATIC
    "${BASISU_DIR}/transcoder/basisu_transcoder.cpp"
    "${BASISU_DIR}/zstd/zstd.c"
)
target_include_directories(basisu_transcoder PUBLIC
    "${BASISU_DIR}/transcoder"
    "${BASISU_DIR}/zstd"
)
# Upstream notes the codec must be built with strict aliasing off (GCC/Clang).
if(NOT MSVC)
    target_compile_options(basisu_transcoder PRIVATE -fno-strict-aliasing)
endif()
target_compile_definitions(basisu_transcoder PUBLIC
    BASISD_SUPPORT_KTX2=1
    BASISD_SUPPORT_KTX2_ZSTD=1
    # Upstream 2026-04 note: globally disable the dEQP Android ASTC
    # decompressor and route to basisu's internal decoder, which lives in the
    # transcoder TU (guarded by BASISU_ASTC_HELPERS_IMPLEMENTATION).
    BASISU_DISABLE_ANDROID_ASTC_DECOMP=1
)

# Test-only encoder for the KTX2 round-trip harness: encodes real .ktx2
# fixtures at run time so the decode assertions run against genuine files
# rather than hand-packed bytes. EXCLUDE_FROM_ALL keeps it out of normal
# builds; only tests/gltf links it. SSE is off so the lib needs no special
# compile flags (encoding speed is irrelevant for a 32x32 fixture).
add_library(basisu_encoder_testlib EXCLUDE_FROM_ALL STATIC
    "${BASISU_DIR}/encoder/basisu_backend.cpp"
    "${BASISU_DIR}/encoder/basisu_basis_file.cpp"
    "${BASISU_DIR}/encoder/basisu_comp.cpp"
    "${BASISU_DIR}/encoder/basisu_enc.cpp"
    "${BASISU_DIR}/encoder/basisu_etc.cpp"
    "${BASISU_DIR}/encoder/basisu_frontend.cpp"
    "${BASISU_DIR}/encoder/basisu_gpu_texture.cpp"
    "${BASISU_DIR}/encoder/basisu_pvrtc1_4.cpp"
    "${BASISU_DIR}/encoder/basisu_resampler.cpp"
    "${BASISU_DIR}/encoder/basisu_resample_filters.cpp"
    "${BASISU_DIR}/encoder/basisu_ssim.cpp"
    "${BASISU_DIR}/encoder/basisu_uastc_enc.cpp"
    "${BASISU_DIR}/encoder/basisu_bc7e_scalar.cpp"
    "${BASISU_DIR}/encoder/basisu_dds_export.cpp"
    "${BASISU_DIR}/encoder/basisu_bc7enc.cpp"
    "${BASISU_DIR}/encoder/jpgd.cpp"
    "${BASISU_DIR}/encoder/basisu_kernels_sse.cpp"
    "${BASISU_DIR}/encoder/basisu_bc15_spmd.cpp"
    "${BASISU_DIR}/encoder/basisu_bc15_spmd_sse.cpp"
    "${BASISU_DIR}/encoder/basisu_opencl.cpp"
    "${BASISU_DIR}/encoder/pvpngreader.cpp"
    "${BASISU_DIR}/encoder/basisu_tinyexr.cpp"
    # The compressor references these even for LDR-only encodes, so the
    # upstream list is followed rather than trimmed further.
    "${BASISU_DIR}/encoder/basisu_uastc_hdr_4x4_enc.cpp"
    "${BASISU_DIR}/encoder/basisu_astc_hdr_6x6_enc.cpp"
    "${BASISU_DIR}/encoder/basisu_astc_hdr_common.cpp"
    "${BASISU_DIR}/encoder/basisu_astc_ldr_common.cpp"
    "${BASISU_DIR}/encoder/basisu_astc_ldr_encode.cpp"
    "${BASISU_DIR}/encoder/basisu_astc_ldr_fencode.cpp"
    "${BASISU_DIR}/encoder/basisu_xbc7_encode.cpp"
)
target_include_directories(basisu_encoder_testlib PUBLIC
    "${BASISU_DIR}/transcoder"
    "${BASISU_DIR}/encoder"
)
if(NOT MSVC)
    target_compile_options(basisu_encoder_testlib PRIVATE -fno-strict-aliasing)
endif()
# zstd for UASTC supercompression resolves from basisu_transcoder at link time.
target_compile_definitions(basisu_encoder_testlib PUBLIC
    BASISU_SUPPORT_SSE=0
    BASISU_SUPPORT_KTX2=1
    BASISU_SUPPORT_KTX2_ZSTD=1
)

# MSDF text rendering (msdf-atlas-gen + msdfgen)
set(MSDF_ATLAS_USE_VCPKG OFF        CACHE BOOL "" FORCE)
set(MSDF_ATLAS_USE_SKIA  OFF        CACHE BOOL "" FORCE)
set(MSDF_ATLAS_BUILD_STANDALONE OFF CACHE BOOL "" FORCE)
set(MSDF_ATLAS_NO_ARTERY_FONT ON    CACHE BOOL "" FORCE)
set(MSDF_ATLAS_DYNAMIC_RUNTIME ON   CACHE BOOL "" FORCE)
add_subdirectory(UHE/vendor/msdf-atlas-gen)

# Disable Jolt Physics tests and viewers to drastically reduce compile times
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
set(USE_SSE4_2 ON CACHE BOOL "" FORCE) # Fast SIMD physics
set(USE_AVX2 OFF CACHE BOOL "" FORCE)  # Keep off for broader compatibility
set(USE_AVX512 OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE) # Force Jolt to use /MD instead of /MT
add_subdirectory(UHE/vendor/jolt/Build)
#Tracy Profiler
set(TRACY_STATIC ON CACHE BOOL "" FORCE)
set(TRACY_ENABLE ${VG_PROFILE} CACHE BOOL "" FORCE)
add_subdirectory(UHE/vendor/tracy)

#main engine core
add_subdirectory(UHE)
#sandbox
add_subdirectory(sandbox)
add_subdirectory(UHE_EDITOR)
add_subdirectory(UHEGAME)


get_property(all_targets DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY BUILDSYSTEM_TARGETS)
foreach(t ${all_targets})
  auto_group(${t})
endforeach()

# Add 'make format' target
add_custom_target(format
    COMMAND find ${CMAKE_SOURCE_DIR}/UHE/src ${CMAKE_SOURCE_DIR}/UHE_EDITOR/src -type f \\( -name "*.cpp" -o -name "*.h" \\) -exec clang-format -i {} +
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Formatting source files with clang-format"
)

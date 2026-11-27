# Build-time tools.
#
# `uhe_shaderc` compiles the Slang sources to `.spv` next to them, so the engine
# reads bytes at startup instead of running the compiler (see
# UHE/src/UHE/Renderer/ShaderCache.h). It links the same SlangCompiler the
# engine's hot-reload path uses, so an artifact and a runtime compile cannot
# disagree about a session setting.

set(UHE_SHADER_DIR "${CMAKE_SOURCE_DIR}/UHE_EDITOR/assets/shaders")

# The sources the engine loads, and therefore the ones a build must compile. A
# new shader is added here next to the call site that loads it; a runtime that
# asks for an unlisted one still works -- ShaderCache compiles it and writes
# the artifact -- it is just slower the first time.
set(UHE_SHADER_SOURCES
    "${UHE_SHADER_DIR}/Basic3D.slang"
    "${UHE_SHADER_DIR}/Bloom.slang"
    "${UHE_SHADER_DIR}/Grid.slang"
    "${UHE_SHADER_DIR}/ImGuiSrgb.slang"
    "${UHE_SHADER_DIR}/RHISelfTest.slang"
    "${UHE_SHADER_DIR}/Sky.slang"
    "${UHE_SHADER_DIR}/SSAO.slang"
    "${UHE_SHADER_DIR}/Texture.slang"
    "${UHE_SHADER_DIR}/Text.slang"
    "${UHE_SHADER_DIR}/Tonemap.slang"
    "${UHE_SHADER_DIR}/UI.slang"
    "${UHE_SHADER_DIR}/Water.slang"
)

set(UHE_SHADER_DEPENDS ${UHE_SHADER_SOURCES} "${UHE_SHADER_DIR}/variants.txt")

add_executable(uhe_shaderc
    shaderc/main.cpp
    ${CMAKE_SOURCE_DIR}/UHE/src/UHE/Renderer/SlangCompiler.cpp
    ${CMAKE_SOURCE_DIR}/UHE/src/UHE/Renderer/ShaderCache.cpp
    ${CMAKE_SOURCE_DIR}/UHE/src/UHE/Renderer/ShaderReflection.cpp
    # UHE::Log's statics without Log.cpp's ImGui sink tree: the same stub the
    # standalone test harnesses link (tests/shader/CMakeLists.txt does too).
    ${CMAKE_SOURCE_DIR}/tests/rendergraph/log_stub.cpp
)

target_include_directories(uhe_shaderc PRIVATE
    "${CMAKE_SOURCE_DIR}/UHE/src"
    "${CMAKE_SOURCE_DIR}/UHE/vendor/spdlog/include"
    "${CMAKE_SOURCE_DIR}/UHE/vendor/glm"
    "${CMAKE_SOURCE_DIR}/UHE/vendor/slang/include"
    "${CMAKE_SOURCE_DIR}/UHE/vendor/tracy/public"
)

target_compile_definitions(uhe_shaderc PRIVATE GLM_FORCE_DEPTH_ZERO_TO_ONE)
if(WIN32)
    target_compile_definitions(uhe_shaderc PRIVATE UHE_PLATFORM_WINDOWS)
elseif(UNIX)
    target_compile_definitions(uhe_shaderc PRIVATE UHE_PLATFORM_LINUX)
endif()

# spdlog's bundled fmt static_asserts on MSVC without /utf-8.
if(MSVC)
    target_compile_options(uhe_shaderc PRIVATE /utf-8)
endif()

# The slang package target is registered by UHE/CMakeLists.txt, which is
# processed before this directory; find it here too so the tool can be built on
# its own.
if(NOT TARGET slang::slang)
    if(WIN32)
        set(SLANG_DIR "${CMAKE_SOURCE_DIR}/UHE/vendor/bin/windows/slang")
    else()
        set(SLANG_DIR "${CMAKE_SOURCE_DIR}/UHE/vendor/bin/linux/slang")
    endif()
    find_package(slang REQUIRED CONFIG PATHS "${SLANG_DIR}" NO_DEFAULT_PATH)
endif()
target_link_libraries(uhe_shaderc PRIVATE slang::slang)
# Runs during the build, so the Slang runtime must be in place first
# (UHE/CMakeLists.txt, uhe_slang_runtime).
if(TARGET uhe_slang_runtime)
    add_dependencies(uhe_shaderc uhe_slang_runtime)
endif()

# Runs on every build that touched a shader (or the variant list), and compiles
# nothing when it did not: up-to-dateness is decided per source and variant by
# the tool itself, so this stays cheap on a no-op build.
set(UHE_SHADERC_COMMAND uhe_shaderc --shaders "${UHE_SHADER_DIR}")
foreach(UHE_SHADER_SOURCE ${UHE_SHADER_SOURCES})
    list(APPEND UHE_SHADERC_COMMAND --source "${UHE_SHADER_SOURCE}")
endforeach()

add_custom_target(uhe_shaders ALL
    COMMAND ${UHE_SHADERC_COMMAND}
    DEPENDS uhe_shaderc ${UHE_SHADER_DEPENDS}
    COMMENT "Compiling Slang shaders to .spv"
    VERBATIM
)

# Golden-image comparison for CI (.github/workflows/test-renderer.yml): reads two
# PNGs with the vendored stb, writes a diff image, exits non-zero on a mismatch.
add_executable(uhe_imgdiff imgdiff/main.cpp)
target_include_directories(uhe_imgdiff PRIVATE "${CMAKE_SOURCE_DIR}/UHE/vendor/stb_image")
if(MSVC)
    target_compile_options(uhe_imgdiff PRIVATE /utf-8)
endif()
# Texture cook (issue #45): a cooked model's images to KTX2. A static library
# so the encoder reaches only what cooks: the editor, uhe_export and uhe_cook.
add_library(uhe_texcook STATIC texcook/TextureCook.cpp texcook/TextureCook.h)
target_include_directories(uhe_texcook PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/texcook"
                                       PRIVATE "${CMAKE_SOURCE_DIR}/UHE/vendor/stb_image")
target_link_libraries(uhe_texcook PUBLIC basisu_encoder basisu_transcoder)
target_compile_features(uhe_texcook PUBLIC cxx_std_20)
set_target_properties(uhe_texcook PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(uhe_texcook PRIVATE /utf-8)
else()
    # Decoding and copying 2K images at -O0 is seconds per image.
    target_compile_options(uhe_texcook PRIVATE $<$<CONFIG:Debug>:-O2>)
endif()

add_subdirectory(export)

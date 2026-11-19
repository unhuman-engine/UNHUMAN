# Shader compilation harness.
#
# Compiles the real .slang sources with the real SlangCompiler TU (the same
# code path engine startup uses) so a shader that fails to produce SPIR-V
# fails the test run instead of failing at engine launch. Links the vendored
# slang the engine links.

set(SCENE_SRC_DIR ${CMAKE_SOURCE_DIR}/UHE/src/UHE)

add_executable(shader_test
    main.cpp
    ${SCENE_SRC_DIR}/Renderer/SlangCompiler.cpp
    # The reflection reader, the artifact store/library and the pipeline helpers
    # the renderers build from them: all of it is device-free, so the harness
    # covers the whole path a shader takes from source to pipeline inputs.
    ${SCENE_SRC_DIR}/Renderer/ShaderReflection.cpp
    ${SCENE_SRC_DIR}/Renderer/ShaderCache.cpp
    ${SCENE_SRC_DIR}/Renderer/ShaderResources.cpp
    # Reuses the rendergraph log stub (UHE::Log statics without ImGui).
    ${CMAKE_SOURCE_DIR}/tests/rendergraph/log_stub.cpp
)

target_include_directories(shader_test PRIVATE
    ${CMAKE_SOURCE_DIR}/UHE/src
    ${CMAKE_SOURCE_DIR}/UHE/vendor/spdlog/include
    ${CMAKE_SOURCE_DIR}/UHE/vendor/glm
    ${CMAKE_SOURCE_DIR}/UHE/vendor/volk
    ${CMAKE_SOURCE_DIR}/UHE/vendor/VulkanMemoryAllocator/include
    ${CMAKE_SOURCE_DIR}/UHE/vendor/slang/include
    ${CMAKE_SOURCE_DIR}/UHE/vendor/tracy/public
)

target_compile_definitions(shader_test PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLFW_INCLUDE_NONE
    _CRT_SECURE_NO_WARNINGS
)

if(WIN32)
    target_compile_definitions(shader_test PRIVATE UHE_PLATFORM_WINDOWS)
elseif(UNIX)
    target_compile_definitions(shader_test PRIVATE UHE_PLATFORM_LINUX)
endif()
target_compile_definitions(shader_test PRIVATE $<$<CONFIG:Debug>:UHE_DEBUG>)

# spdlog's bundled fmt static_asserts on MSVC without /utf-8.
if(MSVC)
    target_compile_options(shader_test PRIVATE /utf-8)
endif()

target_link_libraries(shader_test PRIVATE slang::slang)
# The harness loads Slang at run time; wait for its runtime copy
# (UHE/CMakeLists.txt). Defined later in the tree, resolved at generate time.
add_dependencies(shader_test uhe_slang_runtime)

# The slang package target is normally registered by UHE/CMakeLists.txt; find
# it here too, because tests/ is processed before UHE in the root CMakeLists.
if(NOT TARGET slang::slang)
    if(WIN32)
        set(SLANG_DIR "${CMAKE_SOURCE_DIR}/UHE/vendor/bin/windows/slang")
    else()
        set(SLANG_DIR "${CMAKE_SOURCE_DIR}/UHE/vendor/bin/linux/slang")
    endif()
    find_package(slang REQUIRED CONFIG PATHS "${SLANG_DIR}" NO_DEFAULT_PATH)
endif()

# The shaders live with the editor's assets.
add_test(NAME shader COMMAND shader_test ${CMAKE_SOURCE_DIR}/UHE_EDITOR/assets/shaders)
set_tests_properties(shader PROPERTIES LABELS "unit")

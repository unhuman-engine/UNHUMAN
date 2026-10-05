# glTF loader correctness harness (issue #29).
#
# Compiles the loader's CPU-side translation units directly instead of linking
# UHE: LoadModelUpload.cpp is deliberately excluded because it is the only part
# that needs a live Vulkan device, and this test never uploads anything. That
# keeps the test runnable on any machine with no GPU and no ICD.

set(GLTF_TEST_SOURCES
    main.cpp
    ${CMAKE_SOURCE_DIR}/UHE/src/UHE/Renderer3D/LoadModel.cpp
    ${CMAKE_SOURCE_DIR}/UHE/src/UHE/Renderer3D/LoadModelGeometry.cpp
    ${CMAKE_SOURCE_DIR}/UHE/src/UHE/Renderer3D/LoadModelAnimation.cpp
    ${CMAKE_SOURCE_DIR}/UHE/src/UHE/Renderer3D/MeshoptDecode.cpp
    # Renderer-facing but GPU-free: material packing (what reaches the shader's
    # per-material buffer), light extraction (what reaches the frame's light
    # list) and KTX2 decoding (what reaches the texture upload). All are pure
    # CPU code, so the issue #42 assertions run in this same no-device harness.
    ${CMAKE_SOURCE_DIR}/UHE/src/UHE/Renderer3D/MaterialGPU.cpp
    ${CMAKE_SOURCE_DIR}/UHE/src/UHE/Renderer3D/LightSystem.cpp
    ${CMAKE_SOURCE_DIR}/UHE/src/UHE/Renderer/KTX2.cpp
    ${CMAKE_SOURCE_DIR}/UHE/src/UHE/Renderer/Mipmap.cpp
)

# The loader reports failures through UHE::Log macros, which the test does not
# exercise; uhepch.h still has to compile.
add_executable(gltf_test ${GLTF_TEST_SOURCES} log_stub.cpp gpu_stub.cpp)

target_include_directories(gltf_test PRIVATE
    ${CMAKE_SOURCE_DIR}/UHE/src
    ${CMAKE_SOURCE_DIR}/UHE/vendor/spdlog/include
    ${CMAKE_SOURCE_DIR}/UHE/vendor/glm
    ${CMAKE_SOURCE_DIR}/UHE/vendor/stb_image
    ${CMAKE_SOURCE_DIR}/UHE/vendor/entt/include
    ${CMAKE_SOURCE_DIR}/UHE/vendor/volk
    ${CMAKE_SOURCE_DIR}/UHE/vendor/VulkanMemoryAllocator/include
    ${CMAKE_SOURCE_DIR}/UHE/vendor/meshoptimizer/src
    ${CMAKE_SOURCE_DIR}/UHE/vendor/tracy/public
    # LightSystem.cpp pulls in Scene/Components.h, which needs these.
    ${CMAKE_SOURCE_DIR}/UHE/vendor/box2d/include
    ${CMAKE_SOURCE_DIR}/UHE/vendor/GLFW/include
)

# Same link set the loader's own TU needs, minus the RHI. fastgltf and
# meshoptimizer are the only two the CPU path actually calls into; the
# transcoder decodes KTX2 (KTX2.cpp) and the test-only encoder generates the
# round-trip fixtures.
target_link_libraries(gltf_test PRIVATE fastgltf::fastgltf meshoptimizer basisu_transcoder basisu_encoder_testlib)

target_compile_definitions(gltf_test PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLFW_INCLUDE_NONE
    _CRT_SECURE_NO_WARNINGS
)

if(WIN32)
    target_compile_definitions(gltf_test PRIVATE UHE_PLATFORM_WINDOWS)
elseif(UNIX)
    target_compile_definitions(gltf_test PRIVATE UHE_PLATFORM_LINUX)
endif()
target_compile_definitions(gltf_test PRIVATE $<$<CONFIG:Debug>:UHE_DEBUG>)

add_test(NAME gltf COMMAND gltf_test)
set_tests_properties(gltf PROPERTIES LABELS "unit")

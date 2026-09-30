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
)

# Same link set the loader's own TU needs, minus the RHI. fastgltf and
# meshoptimizer are the only two the CPU path actually calls into.
target_link_libraries(gltf_test PRIVATE fastgltf::fastgltf meshoptimizer)

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

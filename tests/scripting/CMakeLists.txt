# Scripting harness (docs/architecture/scripting.md).
#
# Runs the real ScriptEngine against a real Scene, headlessly: no window, no
# device, no ImGui. That is the property the robotics path needs -- a training
# server steps a scene the same way the editor plays one -- and it is also what
# makes the lifecycle, the error handling and hot reload testable at all.
#
# Like the scene harness it compiles the real scene + physics units and stubs
# only the GPU-facing renderer layer, so a script really moves a real entity.
set(SCENE_SRC_DIR ${CMAKE_SOURCE_DIR}/UHE/src/UHE)

add_executable(scripting_test
    main.cpp
    stubs.cpp
    # The scene harness's renderer/font/texture stubs: ScriptEngine only needs
    # the scene, and the scene needs a renderer that is not there.
    ${CMAKE_SOURCE_DIR}/tests/scene/stubs.cpp
    # Reuses the rendergraph log stub (UHE::Log statics without ImGui).
    ${CMAKE_SOURCE_DIR}/tests/rendergraph/log_stub.cpp
    # Real units under test
    ${SCENE_SRC_DIR}/Scripting/ScriptEngine.cpp
    ${SCENE_SRC_DIR}/Scene/Scene.cpp
    ${SCENE_SRC_DIR}/Scene/SceneCamera.cpp
    ${SCENE_SRC_DIR}/Scene/Entity.cpp
    ${SCENE_SRC_DIR}/Scene/SceneSerializer.cpp
    ${SCENE_SRC_DIR}/Core/UUID.cpp
    ${SCENE_SRC_DIR}/AssestsManager/VfsSystem.cpp
    ${SCENE_SRC_DIR}/Physics/PhysicsSystem3D.cpp
    ${SCENE_SRC_DIR}/Math/Math.cpp
    ${SCENE_SRC_DIR}/Debug/TracySetup.cpp
    ${SCENE_SRC_DIR}/Animation/Animation2D/SpriteAnimation.cpp
    ${SCENE_SRC_DIR}/Debug/Profiler.cpp
)

find_package(Vulkan REQUIRED)
target_include_directories(scripting_test PRIVATE
    ${Vulkan_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/UHE/src
    ${CMAKE_SOURCE_DIR}/UHE/vendor/spdlog/include
    ${CMAKE_SOURCE_DIR}/UHE/vendor/GLFW/include
    ${CMAKE_SOURCE_DIR}/UHE/vendor/imgui
    ${CMAKE_SOURCE_DIR}/UHE/vendor/imGuizmo
    ${CMAKE_SOURCE_DIR}/UHE/vendor/glm
    ${CMAKE_SOURCE_DIR}/UHE/vendor/stb_image
    ${CMAKE_SOURCE_DIR}/UHE/vendor/entt/include
    ${CMAKE_SOURCE_DIR}/UHE/vendor/volk
    ${CMAKE_SOURCE_DIR}/UHE/vendor/VulkanMemoryAllocator/include
    ${CMAKE_SOURCE_DIR}/UHE/vendor/yaml-cpp/include
    ${CMAKE_SOURCE_DIR}/UHE/vendor/tracy/public
    ${CMAKE_SOURCE_DIR}/UHE/vendor/slang/include
    ${CMAKE_SOURCE_DIR}/UHE/vendor/box2d/include
    ${CMAKE_SOURCE_DIR}/UHE/vendor/jolt
    ${CMAKE_SOURCE_DIR}/UHE/vendor/miniaudio
    ${CMAKE_SOURCE_DIR}/UHE/vendor/sol2/include
    ${CMAKE_SOURCE_DIR}/UHE/vendor/lua/src
)

target_compile_definitions(scripting_test PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLFW_INCLUDE_NONE
    _CRT_SECURE_NO_WARNINGS
    TRACY_ENABLE
)

if(WIN32)
    target_compile_definitions(scripting_test PRIVATE UHE_PLATFORM_WINDOWS)
elseif(UNIX)
    target_compile_definitions(scripting_test PRIVATE UHE_PLATFORM_LINUX)
endif()
target_compile_definitions(scripting_test PRIVATE $<$<CONFIG:Debug>:UHE_DEBUG>)

# The real box2d + Jolt + Lua, and GLFW for the input binding's context check.
target_link_libraries(scripting_test PRIVATE box2d Jolt fastgltf::fastgltf Vulkan::Vulkan lua glfw yaml-cpp)

if(MSVC)
    target_compile_options(scripting_test PRIVATE /utf-8)
endif()

add_test(NAME scripting COMMAND scripting_test ${CMAKE_SOURCE_DIR})
set_tests_properties(scripting PROPERTIES LABELS "unit")

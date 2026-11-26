# Scene-lifetime validation harness (issue #17 follow-up).
#
# Unlike rendergraph (module sources, no engine link), Scene.cpp pulls the
# renderer, box2d, and Jolt. The harness therefore compiles the REAL scene +
# physics translation units, links the REAL box2d and Jolt, and stubs only the
# GPU-facing renderer/font/texture layer (stubs.cpp). Under the ASan preset
# this is the regression net for the entt lifetime crashes.
set(SCENE_SRC_DIR ${CMAKE_SOURCE_DIR}/UHE/src/UHE)

add_executable(scene_test
    main.cpp
    stubs.cpp
    # Reuses the rendergraph log stub (UHE::Log statics without ImGui).
    ${CMAKE_SOURCE_DIR}/tests/rendergraph/log_stub.cpp
    # Real units under test
    ${SCENE_SRC_DIR}/Scene/Scene.cpp
    # Scene.cpp drives the Lua scripts attached to entities; the VM stays shut
    # until a test calls Init(), which this harness does not.
    ${SCENE_SRC_DIR}/Scripting/ScriptEngine.cpp
    ${SCENE_SRC_DIR}/Scene/SceneCamera.cpp
    # Terrain heights, mesh and heightfield body are CPU work; stb reads heightmaps.
    ${SCENE_SRC_DIR}/Scene/Terrain.cpp
    ${SCENE_SRC_DIR}/UI/UISystem.cpp
    ${CMAKE_SOURCE_DIR}/UHE/vendor/stb_image/stb_image.cpp
    ${SCENE_SRC_DIR}/Scene/Entity.cpp
    ${SCENE_SRC_DIR}/Core/UUID.cpp
    ${SCENE_SRC_DIR}/Core/SimulationClock.cpp
    ${SCENE_SRC_DIR}/AssetsManager/VfsSystem.cpp
    ${SCENE_SRC_DIR}/Physics/PhysicsSystem3D.cpp
    ${SCENE_SRC_DIR}/Math/Math.cpp
    ${SCENE_SRC_DIR}/Debug/TracySetup.cpp
    # SpriteAnimation::Tick is invoked from RenderSprites (stubbed renderer,
    # but the symbol must resolve)
    ${SCENE_SRC_DIR}/Animation/Animation2D/SpriteAnimation.cpp
    # Scene.cpp carries UHE_PROFILE_* scopes; Profiler.cpp is dependency-free
    # (std only), so the harness compiles the real TU instead of stubbing it.
    ${SCENE_SRC_DIR}/Debug/Profiler.cpp
    # The scene step drives animation graphs; both are pure logic (the
    # Animator they act on is stubbed in tests/scene/stubs.cpp).
    ${SCENE_SRC_DIR}/Animation/AnimationGraph.cpp
    ${SCENE_SRC_DIR}/Renderer3D/AnimationStateMachine.cpp
)

# Same include set as UHE (mirrors UHE/CMakeLists.txt) so the real TUs resolve
# the same vendored headers.
find_package(Vulkan REQUIRED)
target_include_directories(scene_test PRIVATE
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

target_compile_definitions(scene_test PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLFW_INCLUDE_NONE
    _CRT_SECURE_NO_WARNINGS
    TRACY_ENABLE
)

# Mirrors UHE's platform defines; NOT UHE_BUILD_DLL/UHE_DYNAMIC_LINK: the
# harness compiles the engine TUs itself, it is not an engine client.
if(WIN32)
    target_compile_definitions(scene_test PRIVATE UHE_PLATFORM_WINDOWS)
elseif(UNIX)
    target_compile_definitions(scene_test PRIVATE UHE_PLATFORM_LINUX)
endif()
target_compile_definitions(scene_test PRIVATE $<$<CONFIG:Debug>:UHE_DEBUG>)

# Real box2d + Jolt + fastgltf (same vendored targets the engine uses;
# fastgltf is needed for headers pulled in via Components.h -> LoadModel.h).
target_link_libraries(scene_test PRIVATE box2d Jolt fastgltf::fastgltf Vulkan::Vulkan lua glfw)

if(MSVC)
    target_compile_options(scene_test PRIVATE /utf-8)
endif()

add_test(NAME scene COMMAND scene_test)
set_tests_properties(scene PROPERTIES LABELS "unit")

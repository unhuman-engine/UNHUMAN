# Animation runtime harness (issue #41).
#
# Compiles the REAL Animator, AnimationStateMachine, AnimationCompression and
# IKSolver translation units plus the real Jobsystem, and stubs only the
# Model destructor (which lives in the GPU-heavy LoadModel.cpp). No GPU and
# no glTF parsing required: models are populated through the cooked-data API.
set(SCENE_SRC_DIR ${CMAKE_SOURCE_DIR}/UHE/src/UHE)

add_executable(animation_test
    main.cpp
    stubs.cpp
    # Real units under test
    ${SCENE_SRC_DIR}/Renderer3D/Animator.cpp
    ${SCENE_SRC_DIR}/Renderer3D/AnimationStateMachine.cpp
    ${SCENE_SRC_DIR}/Renderer3D/AnimationCompression.cpp
    ${SCENE_SRC_DIR}/Renderer3D/IKSolver.cpp
    ${SCENE_SRC_DIR}/Jobsystem/Jobsystem.cpp
    # Reuses the rendergraph log stub (UHE::Log statics without ImGui).
    ${CMAKE_SOURCE_DIR}/tests/rendergraph/log_stub.cpp
)

# Same include set as UHE (mirrors UHE/CMakeLists.txt).
find_package(Vulkan REQUIRED)
target_include_directories(animation_test PRIVATE
    ${Vulkan_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/UHE/src
    ${CMAKE_SOURCE_DIR}/UHE/vendor/spdlog/include
    ${CMAKE_SOURCE_DIR}/UHE/vendor/glm
    ${CMAKE_SOURCE_DIR}/UHE/vendor/entt/include
    ${CMAKE_SOURCE_DIR}/UHE/vendor/volk
    ${CMAKE_SOURCE_DIR}/UHE/vendor/VulkanMemoryAllocator/include
    ${CMAKE_SOURCE_DIR}/UHE/vendor/tracy/public
    ${CMAKE_SOURCE_DIR}/UHE/vendor/fastgltf/include
)

target_compile_definitions(animation_test PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
    TRACY_ENABLE
)

# Mirrors UHE's platform defines; NOT UHE_BUILD_DLL/UHE_DYNAMIC_LINK: the
# harness compiles the engine TUs itself, it is not an engine client.
if(WIN32)
    target_compile_definitions(animation_test PRIVATE UHE_PLATFORM_WINDOWS)
elseif(UNIX)
    target_compile_definitions(animation_test PRIVATE UHE_PLATFORM_LINUX)
endif()
target_compile_definitions(animation_test PRIVATE $<$<CONFIG:Debug>:UHE_DEBUG>)

target_link_libraries(animation_test PRIVATE fastgltf::fastgltf Vulkan::Vulkan)

if(MSVC)
    target_compile_options(animation_test PRIVATE /utf-8)
endif()

add_test(NAME animation COMMAND animation_test)
set_tests_properties(animation PROPERTIES LABELS "unit")

# Asset system harness (issue #38).
#
# Compiles the REAL UUID, AssetManager and VfsSystem translation units plus
# the real Jobsystem, exercising identity, registry, refcounted handles,
# async loading, hot reload and VFS mounts. No GPU required.
set(SCENE_SRC_DIR ${CMAKE_SOURCE_DIR}/UHE/src/UHE)

add_executable(assets_test
    main.cpp
    # Reuses the rendergraph log stub (UHE::Log statics without ImGui).
    ${CMAKE_SOURCE_DIR}/tests/rendergraph/log_stub.cpp
    # Real units under test
    ${SCENE_SRC_DIR}/Core/UUID.cpp
    ${SCENE_SRC_DIR}/AssestsManager/AssetManager.cpp
    ${SCENE_SRC_DIR}/AssestsManager/VfsSystem.cpp
    ${SCENE_SRC_DIR}/Jobsystem/Jobsystem.cpp
)

# Same include set as UHE (mirrors UHE/CMakeLists.txt).
find_package(Vulkan REQUIRED)
target_include_directories(assets_test PRIVATE
    ${Vulkan_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/UHE/src
    ${CMAKE_SOURCE_DIR}/UHE/vendor/spdlog/include
    ${CMAKE_SOURCE_DIR}/UHE/vendor/glm
    ${CMAKE_SOURCE_DIR}/UHE/vendor/entt/include
    ${CMAKE_SOURCE_DIR}/UHE/vendor/volk
    ${CMAKE_SOURCE_DIR}/UHE/vendor/VulkanMemoryAllocator/include
    ${CMAKE_SOURCE_DIR}/UHE/vendor/tracy/public
)

target_compile_definitions(assets_test PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    TRACY_ENABLE
)

# Mirrors UHE's platform defines; NOT UHE_BUILD_DLL/UHE_DYNAMIC_LINK: the
# harness compiles the engine TUs itself, it is not an engine client.
if(WIN32)
    target_compile_definitions(assets_test PRIVATE UHE_PLATFORM_WINDOWS)
elseif(UNIX)
    target_compile_definitions(assets_test PRIVATE UHE_PLATFORM_LINUX)
endif()
target_compile_definitions(assets_test PRIVATE $<$<CONFIG:Debug>:UHE_DEBUG>)

target_link_libraries(assets_test PRIVATE Vulkan::Vulkan)

if(MSVC)
    target_compile_options(assets_test PRIVATE /utf-8)
endif()

add_test(NAME assets COMMAND assets_test)
set_tests_properties(assets PROPERTIES LABELS "unit")

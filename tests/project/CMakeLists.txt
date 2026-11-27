# Project system harness: compiles the real Project/VfsSystem/CommandLine units
# and checks project creation, manifest round-trips, VFS mounting and asset path
# resolution. No GPU, no ImGui.
set(PROJECT_SRC_DIR ${CMAKE_SOURCE_DIR}/UHE/src/UHE)

add_executable(project_test
    main.cpp
    # Reuses the rendergraph log stub (UHE::Log statics without ImGui).
    ${CMAKE_SOURCE_DIR}/tests/rendergraph/log_stub.cpp
    # Real units under test
    ${PROJECT_SRC_DIR}/AssetsManager/AssetImport.cpp
    ${PROJECT_SRC_DIR}/AssetsManager/Project.cpp
    ${PROJECT_SRC_DIR}/AssetsManager/GameExporter.cpp
    ${PROJECT_SRC_DIR}/AssetsManager/GltfFiles.cpp
    ${PROJECT_SRC_DIR}/AssetsManager/AssetCatalog.cpp
    ${PROJECT_SRC_DIR}/Audio/CookedAudio.cpp
    ${PROJECT_SRC_DIR}/AssetsManager/CookedFile.cpp
    ${PROJECT_SRC_DIR}/Core/UUID.cpp
    ${PROJECT_SRC_DIR}/AssetsManager/VfsSystem.cpp
    ${PROJECT_SRC_DIR}/Core/CommandLine.cpp
)

# Same include set as UHE (mirrors UHE/CMakeLists.txt).
find_package(Vulkan REQUIRED)
target_include_directories(project_test PRIVATE
    ${Vulkan_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/UHE/src
    ${CMAKE_SOURCE_DIR}/UHE/vendor/spdlog/include
    ${CMAKE_SOURCE_DIR}/UHE/vendor/glm
    ${CMAKE_SOURCE_DIR}/UHE/vendor/entt/include
    ${CMAKE_SOURCE_DIR}/UHE/vendor/volk
    ${CMAKE_SOURCE_DIR}/UHE/vendor/VulkanMemoryAllocator/include
    ${CMAKE_SOURCE_DIR}/UHE/vendor/tracy/public
    ${CMAKE_SOURCE_DIR}/UHE/vendor/yaml-cpp/include
)

# Manifests are YAML, same library the scene serializer uses.
# CookedFile.cpp hashes files for the asset manifest; its zstd comes from basisu_transcoder.
target_link_libraries(project_test PRIVATE yaml-cpp basisu_transcoder)

target_compile_definitions(project_test PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLFW_INCLUDE_NONE
    _CRT_SECURE_NO_WARNINGS
    # The shipped example projects live in <repo>/projects; the harness checks
    # that they all still load.
    UHE_REPO_DIR="${CMAKE_SOURCE_DIR}"
)

# Mirrors UHE's platform defines; NOT UHE_BUILD_DLL/UHE_DYNAMIC_LINK: the
# harness compiles the engine TUs itself, it is not an engine client.
if(WIN32)
    target_compile_definitions(project_test PRIVATE UHE_PLATFORM_WINDOWS)
elseif(UNIX)
    target_compile_definitions(project_test PRIVATE UHE_PLATFORM_LINUX)
endif()
target_compile_definitions(project_test PRIVATE $<$<CONFIG:Debug>:UHE_DEBUG>)

if(MSVC)
    target_compile_options(project_test PRIVATE /utf-8)
endif()

add_test(NAME project COMMAND project_test)
set_tests_properties(project PROPERTIES LABELS "unit")

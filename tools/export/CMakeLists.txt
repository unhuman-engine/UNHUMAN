# uhe_export: the editor's File > Export Game as a command-line tool, for CI
# and scripted builds. Compiles the project and exporter units directly, like
# tests/project, so it needs no window, GPU or engine DLL.
set(EXPORT_SRC_DIR ${CMAKE_SOURCE_DIR}/UHE/src/UHE)

add_executable(uhe_export
    main.cpp
    ${CMAKE_SOURCE_DIR}/tests/rendergraph/log_stub.cpp
    ${EXPORT_SRC_DIR}/AssetsManager/GameExporter.cpp
    ${EXPORT_SRC_DIR}/AssetsManager/AssetCatalog.cpp
    ${EXPORT_SRC_DIR}/Core/UUID.cpp
    ${EXPORT_SRC_DIR}/AssetsManager/Project.cpp
    ${EXPORT_SRC_DIR}/AssetsManager/VfsSystem.cpp
    ${EXPORT_SRC_DIR}/AssetsManager/AssetImport.cpp
    ${EXPORT_SRC_DIR}/Core/CommandLine.cpp
    # Cooking models (issue #45): the loader's CPU side and the cooked format,
    # with the GPU entry points stubbed (nothing uploads while cooking).
    ${EXPORT_SRC_DIR}/AssetsManager/CookedFile.cpp
    ${EXPORT_SRC_DIR}/Renderer3D/CookedModel.cpp
    ${EXPORT_SRC_DIR}/Renderer3D/LoadModel.cpp
    ${EXPORT_SRC_DIR}/Renderer3D/LoadModelGeometry.cpp
    ${EXPORT_SRC_DIR}/Renderer3D/LoadModelAnimation.cpp
    ${EXPORT_SRC_DIR}/Renderer3D/MeshoptDecode.cpp
    ${EXPORT_SRC_DIR}/Renderer3D/AnimationCompression.cpp
    ${EXPORT_SRC_DIR}/Jobsystem/Jobsystem.cpp
    ${EXPORT_SRC_DIR}/AssetsManager/GltfFiles.cpp
    cook_stubs.cpp
)

find_package(Vulkan REQUIRED)
target_include_directories(uhe_export PRIVATE
    ${Vulkan_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/UHE/src
    ${CMAKE_SOURCE_DIR}/UHE/vendor/spdlog/include
    ${CMAKE_SOURCE_DIR}/UHE/vendor/glm
    ${CMAKE_SOURCE_DIR}/UHE/vendor/entt/include
    ${CMAKE_SOURCE_DIR}/UHE/vendor/volk
    ${CMAKE_SOURCE_DIR}/UHE/vendor/VulkanMemoryAllocator/include
    ${CMAKE_SOURCE_DIR}/UHE/vendor/tracy/public
    ${CMAKE_SOURCE_DIR}/UHE/vendor/yaml-cpp/include
    ${CMAKE_SOURCE_DIR}/UHE/vendor/meshoptimizer/src
    ${CMAKE_SOURCE_DIR}/UHE/vendor/stb_image
)
target_link_libraries(uhe_export PRIVATE yaml-cpp fastgltf::fastgltf meshoptimizer basisu_transcoder uhe_texcook)
target_compile_definitions(uhe_export PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLFW_INCLUDE_NONE
    _CRT_SECURE_NO_WARNINGS
    UHE_ENGINE_ASSETS="${CMAKE_SOURCE_DIR}/UHE_EDITOR/assets"
)
if(WIN32)
    target_compile_definitions(uhe_export PRIVATE UHE_PLATFORM_WINDOWS)
elseif(UNIX)
    target_compile_definitions(uhe_export PRIVATE UHE_PLATFORM_LINUX)
endif()
if(MSVC)
    target_compile_options(uhe_export PRIVATE /utf-8)
endif()

# uhe_cook: cook one model, or inspect and diff cooked files (issue #45). The
# same cooking units as uhe_export.
add_executable(uhe_cook
    ${CMAKE_SOURCE_DIR}/tools/cook/main.cpp
    ${CMAKE_SOURCE_DIR}/tests/rendergraph/log_stub.cpp
    ${EXPORT_SRC_DIR}/AssetsManager/VfsSystem.cpp
    ${EXPORT_SRC_DIR}/AssetsManager/CookedFile.cpp
    ${EXPORT_SRC_DIR}/Renderer3D/CookedModel.cpp
    ${EXPORT_SRC_DIR}/Renderer3D/LoadModel.cpp
    ${EXPORT_SRC_DIR}/Renderer3D/LoadModelGeometry.cpp
    ${EXPORT_SRC_DIR}/Renderer3D/LoadModelAnimation.cpp
    ${EXPORT_SRC_DIR}/Renderer3D/MeshoptDecode.cpp
    ${EXPORT_SRC_DIR}/Renderer3D/AnimationCompression.cpp
    ${EXPORT_SRC_DIR}/Jobsystem/Jobsystem.cpp
    ${EXPORT_SRC_DIR}/AssetsManager/GltfFiles.cpp
    cook_stubs.cpp
)
get_target_property(UHE_EXPORT_INCLUDES uhe_export INCLUDE_DIRECTORIES)
get_target_property(UHE_EXPORT_DEFINES uhe_export COMPILE_DEFINITIONS)
target_include_directories(uhe_cook PRIVATE ${UHE_EXPORT_INCLUDES})
target_compile_definitions(uhe_cook PRIVATE ${UHE_EXPORT_DEFINES})
target_link_libraries(uhe_cook PRIVATE fastgltf::fastgltf meshoptimizer basisu_transcoder uhe_texcook)
if(MSVC)
    target_compile_options(uhe_cook PRIVATE /utf-8)
endif()

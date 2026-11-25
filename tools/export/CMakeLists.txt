# uhe_export: the editor's File > Export Game as a command-line tool, for CI
# and scripted builds. Compiles the project and exporter units directly, like
# tests/project, so it needs no window, GPU or engine DLL.
set(EXPORT_SRC_DIR ${CMAKE_SOURCE_DIR}/UHE/src/UHE)

add_executable(uhe_export
    main.cpp
    ${CMAKE_SOURCE_DIR}/tests/rendergraph/log_stub.cpp
    ${EXPORT_SRC_DIR}/AssetsManager/GameExporter.cpp
    ${EXPORT_SRC_DIR}/AssetsManager/Project.cpp
    ${EXPORT_SRC_DIR}/AssetsManager/VfsSystem.cpp
    ${EXPORT_SRC_DIR}/AssetsManager/AssetImport.cpp
    ${EXPORT_SRC_DIR}/Core/CommandLine.cpp
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
)
target_link_libraries(uhe_export PRIVATE yaml-cpp)
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

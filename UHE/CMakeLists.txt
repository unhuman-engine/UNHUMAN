# UHE Shared Library
project(UHE)



file(GLOB_RECURSE UHE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/stb_image/*.cpp"
)

list(FILTER UHE_SOURCES EXCLUDE REGEX "src/UHE/Renderer2D/Renderer2D.cpp")
list(FILTER UHE_SOURCES EXCLUDE REGEX "src/UHE/Renderer/Shader.cpp")

if(WIN32)
    list(FILTER UHE_SOURCES EXCLUDE REGEX "src/Platform/Linux/.*")
elseif(UNIX)
    # Only exclude Windows-specific files (Win32 file dialogs etc.)
    # WindowsWindow.h/cpp is pure GLFW and works on Linux
    list(FILTER UHE_SOURCES EXCLUDE REGEX "WindowPlatformUtils\\.cpp")
endif()

find_package(Vulkan REQUIRED)
if(WIN32)
    set(SLANG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/bin/windows/slang")
else()
    set(SLANG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/bin/linux/slang")
endif()
find_package(slang REQUIRED CONFIG PATHS "${SLANG_DIR}" NO_DEFAULT_PATH)

file(GLOB_RECURSE GLM_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/glm/glm/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/glm/glm/*.inl"
)

set(ALL_UHE_FILES ${UHE_SOURCES} ${GLM_FILES})


add_library(UHE SHARED ${ALL_UHE_FILES}  )


source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "" FILES ${ALL_UHE_FILES})


target_include_directories(UHE PUBLIC
    "src"
    "vendor/spdlog/include"
    "vendor/GLFW/include"
    "vendor/imgui"
    "vendor/imGuizmo"
    "vendor/glm"
    "vendor/stb_image"
    "vendor/entt/include"
    "vendor/volk"
    "vendor/VulkanMemoryAllocator/include"
    "vendor/yaml-cpp/include"
    "vendor/tracy/public"
    "vendor/slang/include"
    "vendor/box2d/include"
    "vendor/jolt"
    "vendor/miniaudio"
    "vendor/meshoptimizer/src"
)

target_sources(UHE PRIVATE
    vendor/tracy/public/TracyClient.cpp
)

if(WIN32)
    target_compile_definitions(UHE PUBLIC UHE_PLATFORM_WINDOWS)
    target_compile_definitions(UHE PUBLIC UHE_DYNAMIC_LINK WINDLL)
elseif(UNIX)
    target_compile_definitions(UHE PUBLIC UHE_PLATFORM_LINUX)
    # UNIX doesn't typically need WINDLL or explicit dllexport macros for shared objects if visibility is handled
endif()

target_compile_definitions(UHE PUBLIC
    GLFW_INCLUDE_NONE
    _CRT_SECURE_NO_WARNINGS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    $<$<CONFIG:Debug>:UHE_DEBUG>
)

target_compile_definitions(UHE PRIVATE UHE_BUILD_DLL ENTT_API_EXPORT TRACY_ENABLE)

if(UHE_PROFILE)
    target_compile_definitions(UHE PUBLIC UHE_PROFILE=1 TRACY_ENABLE=1)
    target_compile_definitions(UHE PRIVATE TRACY_EXPORTS=1)
endif()

if(MSVC)
    target_compile_options(UHE PRIVATE /utf-8)
endif()

# Enable precompiled headers for ALL compilers (MSVC, GCC, Clang)
target_precompile_headers(UHE PRIVATE src/uhepch.h)

target_link_libraries(UHE PUBLIC
    glfw Vulkan::Vulkan fastgltf::fastgltf
PRIVATE
    VulkanMemoryAllocator volk ImGui ImGuizmo
    yaml-cpp
    TracyClient
    slang::slang
    box2d
    Jolt
    msdf-atlas-gen
    meshoptimizer
    basisu_transcoder
)

if(UNIX)
    target_link_libraries(UHE PRIVATE dl X11)
endif()

if(WIN32)
    set(SLANG_BIN_DIR "${SLANG_DIR}/bin")
else()
    set(SLANG_BIN_DIR "${SLANG_DIR}/lib")
endif()

# Copy Slang shared libraries to output directory
add_custom_command(TARGET UHE POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${SLANG_BIN_DIR}"
    "$<TARGET_FILE_DIR:UHE>"
)
set_target_properties(UHE PROPERTIES DEBUG_POSTFIX d)

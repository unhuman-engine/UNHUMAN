# RenderGraph step 1 validation harness (docs/architecture/rendergraph.md §14).
#
# Compiles the RenderGraph module sources directly instead of linking the UHE
# shared library: self-contained, no GPU, no engine link. log_stub.cpp supplies
# UHE::Log's statics without the ImGui sink tree; it is test-only and must
# never be compiled into the engine (symbol clash with Log.cpp).

set(RG_SRC_DIR ${CMAKE_SOURCE_DIR}/UHE/src/Platform/Vulkan/RenderGraph)

add_executable(rg_test
    main.cpp
    log_stub.cpp
    ${RG_SRC_DIR}/VulkanRenderGraphTypes.cpp
    ${RG_SRC_DIR}/VulkanRenderGraphResources.cpp
    ${RG_SRC_DIR}/VulkanRenderGraphBuilder.cpp
    ${RG_SRC_DIR}/VulkanRenderGraphCompiler.cpp
    ${RG_SRC_DIR}/VulkanRenderGraph.cpp
    ${RG_SRC_DIR}/VulkanRenderGraphExecutor.cpp
    ${CMAKE_SOURCE_DIR}/UHE/src/UHE/Jobsystem/Taskgraph.cpp
    ${CMAKE_SOURCE_DIR}/UHE/src/UHE/Jobsystem/Jobsystem.cpp
    ${CMAKE_SOURCE_DIR}/UHE/src/Platform/Vulkan/VulkanSemaphore.cpp
    ${CMAKE_SOURCE_DIR}/UHE/src/Platform/Vulkan/VulkanBarrierEncoder.cpp
    ${CMAKE_SOURCE_DIR}/UHE/src/Platform/Vulkan/VulkanTypes.cpp
    # The executor carries UHE_PROFILE_* scopes. Profiler.cpp has no ImGui or
    # engine dependencies (std only), so the harness compiles the real TU
    # instead of stubbing ProfilerScope.
    ${CMAKE_SOURCE_DIR}/UHE/src/UHE/Debug/Profiler.cpp
)

# Mirrors the UHE target's PUBLIC include list (UHE/CMakeLists.txt) so the
# harness resolves the same vendored headers as the engine (glm, volk, entt, ...).
# Vulkan SDK headers: system-wide on Linux but only via the SDK path on Windows
# (mirrors find_package at UHE/CMakeLists.txt:23; siblings do not share scope).
find_package(Vulkan REQUIRED)

target_include_directories(rg_test PRIVATE
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
)

# The executor pulls vulkan-hpp, whose dynamic dispatcher storage is defined in
# exactly one TU of the real engine (VulkanDevice.cpp). The test provides its own.
# Header-affecting defines mirror UHE's PUBLIC set (main.cpp/log_stub.cpp do not
# include uhepch.h, so they would otherwise miss GLM_FORCE_DEPTH_ZERO_TO_ONE).
target_compile_definitions(rg_test PRIVATE
    VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLFW_INCLUDE_NONE
    _CRT_SECURE_NO_WARNINGS
)
target_sources(rg_test PRIVATE main.cpp log_stub.cpp VmaDispatchStub.cpp)

# Same platform defines UHE gets (mirrors UHE/CMakeLists.txt), but deliberately
# NOT UHE_DYNAMIC_LINK: the harness builds the module itself, it is not an
# engine client. Debug builds keep asserts on, matching engine behavior.
if(WIN32)
    target_compile_definitions(rg_test PRIVATE UHE_PLATFORM_WINDOWS)
elseif(UNIX)
    target_compile_definitions(rg_test PRIVATE UHE_PLATFORM_LINUX)
endif()
target_compile_definitions(rg_test PRIVATE $<$<CONFIG:Debug>:UHE_DEBUG>)

# spdlog's bundled fmt static_asserts on MSVC without /utf-8 (UHE/CMakeLists.txt:91).
if(MSVC)
    target_compile_options(rg_test PRIVATE /utf-8)
endif()

add_test(NAME rendergraph COMMAND rg_test)
set_tests_properties(rendergraph PROPERTIES LABELS "unit")

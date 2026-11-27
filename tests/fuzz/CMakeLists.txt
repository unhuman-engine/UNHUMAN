# libFuzzer targets for the parsers that read untrusted files (issue #39,
# "nightly soak/fuzz"). Built only with -DUHE_FUZZ=ON and Clang, usually with
# the `fuzz` preset (ASan + UBSan); the nightly workflow runs each for a few
# minutes. Like the unit tests, each compiles the units it exercises directly
# instead of linking UHE, so no GPU is involved.
if(NOT UHE_FUZZ)
    return()
endif()
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "UHE_FUZZ needs Clang (libFuzzer); found ${CMAKE_CXX_COMPILER_ID}")
endif()

set(FUZZ_SRC ${CMAKE_SOURCE_DIR}/UHE/src/UHE)
# The cooked model reader is the glTF loader's other half, so it needs the
# loader's units; tests/gltf's stubs stand in for logging and the GPU.
set(FUZZ_MODEL_SOURCES
    ${FUZZ_SRC}/Renderer3D/LoadModel.cpp
    ${FUZZ_SRC}/Renderer3D/CookedModel.cpp
    ${FUZZ_SRC}/AssetsManager/CookedFile.cpp
    ${FUZZ_SRC}/Renderer3D/LoadModelGeometry.cpp
    ${FUZZ_SRC}/Renderer3D/LoadModelAnimation.cpp
    ${FUZZ_SRC}/Renderer3D/MeshoptDecode.cpp
    ${FUZZ_SRC}/Renderer3D/AnimationCompression.cpp
    ${FUZZ_SRC}/Jobsystem/Jobsystem.cpp
    ${FUZZ_SRC}/AssetsManager/GltfFiles.cpp
    ${FUZZ_SRC}/Renderer3D/MaterialGPU.cpp
    ${FUZZ_SRC}/Renderer3D/LightSystem.cpp
    ${FUZZ_SRC}/Renderer/KTX2.cpp
    ${FUZZ_SRC}/Renderer/Mipmap.cpp
    ${FUZZ_SRC}/AssetsManager/VfsSystem.cpp
    ${FUZZ_SRC}/AssetsManager/PrimitiveGltf.cpp
    ${CMAKE_SOURCE_DIR}/tests/gltf/log_stub.cpp
    ${CMAKE_SOURCE_DIR}/tests/gltf/gpu_stub.cpp
)

function(uhe_add_fuzzer name)
    add_executable(${name} ${name}.cpp ${FUZZ_MODEL_SOURCES})
    target_include_directories(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/UHE/src
        ${CMAKE_SOURCE_DIR}/UHE/vendor/spdlog/include
        ${CMAKE_SOURCE_DIR}/UHE/vendor/glm
        ${CMAKE_SOURCE_DIR}/UHE/vendor/stb_image
        ${CMAKE_SOURCE_DIR}/UHE/vendor/entt/include
        ${CMAKE_SOURCE_DIR}/UHE/vendor/volk
        ${CMAKE_SOURCE_DIR}/UHE/vendor/VulkanMemoryAllocator/include
        ${CMAKE_SOURCE_DIR}/UHE/vendor/meshoptimizer/src
        ${CMAKE_SOURCE_DIR}/UHE/vendor/tracy/public
        ${CMAKE_SOURCE_DIR}/UHE/vendor/box2d/include
        ${CMAKE_SOURCE_DIR}/UHE/vendor/GLFW/include
    )
    target_link_libraries(${name} PRIVATE fastgltf::fastgltf meshoptimizer basisu_transcoder)
    target_compile_definitions(${name} PRIVATE GLM_FORCE_DEPTH_ZERO_TO_ONE GLFW_INCLUDE_NONE UHE_PLATFORM_LINUX)
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
endfunction()

uhe_add_fuzzer(fuzz_cooked_container)
uhe_add_fuzzer(fuzz_cooked_model)
uhe_add_fuzzer(fuzz_ktx2)

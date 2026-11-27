# FrameGraph frontend harness: declaration, validation, blackboard, hashing and
# lowering. Pure CPU: the frontend has no backend or GPU dependency.

add_executable(framegraph_test
    main.cpp
    ${CMAKE_SOURCE_DIR}/UHE/src/UHE/RHI/FrameGraph/FrameGraph.cpp
    ${CMAKE_SOURCE_DIR}/UHE/src/UHE/RHI/FrameGraph/TransientPlacement.cpp
)

target_include_directories(framegraph_test PRIVATE
    ${CMAKE_SOURCE_DIR}/UHE/src
    ${CMAKE_SOURCE_DIR}/UHE/vendor/glm
)

if(WIN32)
    target_compile_definitions(framegraph_test PRIVATE UHE_PLATFORM_WINDOWS)
elseif(UNIX)
    target_compile_definitions(framegraph_test PRIVATE UHE_PLATFORM_LINUX)
endif()

if(MSVC)
    target_compile_options(framegraph_test PRIVATE /utf-8)
endif()

add_test(NAME framegraph COMMAND framegraph_test)
set_tests_properties(framegraph PROPERTIES LABELS "unit")

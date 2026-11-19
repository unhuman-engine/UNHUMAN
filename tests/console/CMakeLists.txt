# Console (cvars + commands) harness. Pure CPU: the one unit under test and
# the log stub.

add_executable(console_test
    main.cpp
    ${CMAKE_SOURCE_DIR}/UHE/src/UHE/Core/Console.cpp
    ${CMAKE_SOURCE_DIR}/tests/rendergraph/log_stub.cpp
)

# Log.h pulls Core.h (platform defines) and the Tracy header; same minimal
# set the animation harness uses.
target_include_directories(console_test PRIVATE
    ${CMAKE_SOURCE_DIR}/UHE/src
    ${CMAKE_SOURCE_DIR}/UHE/vendor/spdlog/include
    ${CMAKE_SOURCE_DIR}/UHE/vendor/glm
    ${CMAKE_SOURCE_DIR}/UHE/vendor/tracy/public
)

target_compile_definitions(console_test PRIVATE TRACY_ENABLE)
if(WIN32)
    target_compile_definitions(console_test PRIVATE UHE_PLATFORM_WINDOWS)
elseif(UNIX)
    target_compile_definitions(console_test PRIVATE UHE_PLATFORM_LINUX)
endif()
target_compile_definitions(console_test PRIVATE $<$<CONFIG:Debug>:UHE_DEBUG>)

find_package(Threads REQUIRED)
target_link_libraries(console_test PRIVATE Threads::Threads)

if(MSVC)
    target_compile_options(console_test PRIVATE /utf-8)
endif()

add_test(NAME console COMMAND console_test)
set_tests_properties(console PROPERTIES LABELS "unit")

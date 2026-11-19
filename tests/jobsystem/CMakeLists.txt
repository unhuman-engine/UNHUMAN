# Job system + TaskGraph harness. Pure CPU: compiles the two units under test
# and the log stub, nothing else. It is the TSan target for the scheduler, and
# its shutdown loop has a CTest timeout so a lost wake-up fails instead of
# hanging the run.
set(JOBS_SRC_DIR ${CMAKE_SOURCE_DIR}/UHE/src/UHE/Jobsystem)

add_executable(jobsystem_test
    main.cpp
    ${JOBS_SRC_DIR}/Jobsystem.cpp
    ${JOBS_SRC_DIR}/Taskgraph.cpp
    ${CMAKE_SOURCE_DIR}/tests/rendergraph/log_stub.cpp
)

# Log.h pulls Core.h (platform defines) and the Tracy header; same minimal
# set the animation harness uses.
target_include_directories(jobsystem_test PRIVATE
    ${CMAKE_SOURCE_DIR}/UHE/src
    ${CMAKE_SOURCE_DIR}/UHE/vendor/spdlog/include
    ${CMAKE_SOURCE_DIR}/UHE/vendor/glm
    ${CMAKE_SOURCE_DIR}/UHE/vendor/tracy/public
)

target_compile_definitions(jobsystem_test PRIVATE TRACY_ENABLE)
if(WIN32)
    target_compile_definitions(jobsystem_test PRIVATE UHE_PLATFORM_WINDOWS)
elseif(UNIX)
    target_compile_definitions(jobsystem_test PRIVATE UHE_PLATFORM_LINUX)
endif()
target_compile_definitions(jobsystem_test PRIVATE $<$<CONFIG:Debug>:UHE_DEBUG>)

find_package(Threads REQUIRED)
target_link_libraries(jobsystem_test PRIVATE Threads::Threads)

if(MSVC)
    target_compile_options(jobsystem_test PRIVATE /utf-8)
endif()

add_test(NAME jobsystem COMMAND jobsystem_test)
set_tests_properties(jobsystem PROPERTIES LABELS "unit" TIMEOUT 60)

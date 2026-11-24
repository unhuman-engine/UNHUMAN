# Capture harness: the UHE_CAPTURE_* / UHE_FIXED_TIMESTEP parser and the
# golden-image comparison policy. Pure CPU: no GPU, no PNG decoding.

add_executable(capture_test
    main.cpp
    ${CMAKE_SOURCE_DIR}/UHE/src/UHE/Core/CaptureSettings.cpp
)

target_include_directories(capture_test PRIVATE
    ${CMAKE_SOURCE_DIR}/UHE/src
    ${CMAKE_SOURCE_DIR}/tools/imgdiff
)

if(WIN32)
    target_compile_definitions(capture_test PRIVATE UHE_PLATFORM_WINDOWS)
elseif(UNIX)
    target_compile_definitions(capture_test PRIVATE UHE_PLATFORM_LINUX)
endif()

if(MSVC)
    target_compile_options(capture_test PRIVATE /utf-8)
endif()

add_test(NAME capture COMMAND capture_test)
set_tests_properties(capture PROPERTIES LABELS "unit")

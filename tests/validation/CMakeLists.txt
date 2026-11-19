# Validation gate harness.
#
# The gate's policy -- what counts as an error, whether UHE_VALIDATION_FATAL
# stops the run, and which of a flood of identical messages is worth a line --
# decides whether an invalid frame fails a build. It is Vulkan-free by
# construction, so it is exercised here with no GPU, no engine link and no
# display; the other half of the gate, a real layer reporting into a real engine
# run, is script/check_validation.sh.
#
# The severity bits are a mirror of the layer's, so the one thing a Vulkan-free
# test cannot check is that the mirror still matches -- VulkanInstance.cpp
# static_asserts that where the Vulkan header is in scope.

set(VK_SRC_DIR ${CMAKE_SOURCE_DIR}/UHE/src/Platform/Vulkan)

add_executable(validation_test
    main.cpp
    ${VK_SRC_DIR}/ValidationGate.cpp
)

target_include_directories(validation_test PRIVATE
    ${CMAKE_SOURCE_DIR}/UHE/src
)

find_package(Threads REQUIRED)
target_link_libraries(validation_test PRIVATE Threads::Threads)

add_test(NAME validation COMMAND validation_test)
set_tests_properties(validation PROPERTIES LABELS "unit")

# UHE_VALIDATION_FATAL is read once per process, so the two readings need a
# process each. `=0` is the case that used to abort the run anyway.
add_test(NAME validation.env_fatal_off COMMAND validation_test --expect-fatal=off)
set_tests_properties(validation.env_fatal_off PROPERTIES
    LABELS "unit"
    ENVIRONMENT "UHE_VALIDATION_FATAL=0")

add_test(NAME validation.env_fatal_on COMMAND validation_test --expect-fatal=on)
set_tests_properties(validation.env_fatal_on PROPERTIES
    LABELS "unit"
    ENVIRONMENT "UHE_VALIDATION_FATAL=1")

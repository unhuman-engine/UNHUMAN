# Meshoptimizer integration harness.
#
# Self-contained: no engine headers, no GPU, no RHI. Links the vendored
# meshoptimizer static lib directly so a broken submodule or an upstream API
# change fails here in seconds rather than during a full engine build.
#
# Runs the entry points the glTF asset pipeline will actually call:
#   - generateVertexRemap / remapIndexBuffer / remapVertexBuffer  (merge)
#   - optimizeVertexCache                                        (post-transform)
#   - optimizeVertexFetchRemap                                   (post-transform)
#   - simplify                                                   (lossy, last)
#
# Mirrors tests/rendergraph/CMakeLists.txt in style: the library is built by
# the top-level CMakeLists (UHE/vendor/meshoptimizer), this only links it.

add_executable(meshopt_test
    main.cpp
)

target_include_directories(meshopt_test PRIVATE
    ${CMAKE_SOURCE_DIR}/UHE/vendor/meshoptimizer/src
)

target_link_libraries(meshopt_test PRIVATE meshoptimizer)

# meshoptimizer is plain C++ with no engine/platform defines; it only needs a
# C++17 baseline (its own CMakeLists sets that for the lib itself).
target_compile_features(meshopt_test PRIVATE cxx_std_17)

add_test(NAME meshopt COMMAND meshopt_test)
set_tests_properties(meshopt PROPERTIES LABELS "unit")

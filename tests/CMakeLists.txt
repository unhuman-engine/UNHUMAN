# No-GPU unit tests, registered with CTest. Each subdirectory is one
# self-contained harness that compiles the module sources it exercises
# directly (no UHE link), so tests stay fast and runnable on any machine.
# GPU-dependent tests will need a `gpu` label + ICD gating - see
# docs/architecture/ci-cd.md ("Test infrastructure", labels: unit, gpu, slow, soak).
add_subdirectory(rendergraph)
add_subdirectory(meshopt)
add_subdirectory(gltf)
add_subdirectory(scene)
add_subdirectory(animation)
add_subdirectory(assets)
add_subdirectory(project)
add_subdirectory(shader)
add_subdirectory(validation)
add_subdirectory(scripting)
add_subdirectory(jobsystem)
add_subdirectory(console)
add_subdirectory(capture)
add_subdirectory(framegraph)

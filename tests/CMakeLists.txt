# No-GPU unit tests, registered with CTest. Each subdirectory is one
# self-contained harness that compiles the module sources it exercises
# directly (no UHE link), so tests stay fast and runnable on any machine.
# GPU-dependent tests will need a `gpu` label + ICD gating - see
# docs/architecture/ci-cd.md ("Test infrastructure", labels: unit, gpu, slow, soak).
add_subdirectory(rendergraph)
add_subdirectory(scene)
add_subdirectory(animation)

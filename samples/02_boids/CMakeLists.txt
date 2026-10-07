mce_add_executable(sample_02_boids SOURCES main.cpp DEPS mce::mce_ecs)
if(MCE_BUILD_TESTS)
    # Sample ladder rule: every sample runs 300 headless steps in CI.
    add_test(NAME sample_02_boids_300 COMMAND sample_02_boids -ticks 300 -boids 20000)
endif()

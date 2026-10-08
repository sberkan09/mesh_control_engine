mce_add_executable(sample_physics3d_headless SOURCES main.cpp DEPS mce::mce_physics3d)
if(MCE_BUILD_TESTS)
    # Sample ladder rule: every sample runs 300 headless steps in CI.
    add_test(NAME sample_physics3d_headless_300 COMMAND sample_physics3d_headless -ticks 300)
endif()

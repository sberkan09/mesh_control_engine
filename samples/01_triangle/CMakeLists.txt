mce_add_executable(01_triangle SOURCES main.cpp DEPS mce::mce_platform mce::mce_rhi_default)
set_target_properties(01_triangle PROPERTIES OUTPUT_NAME "01_triangle")

if(MCE_BUILD_TESTS AND MCE_GPU_TESTS)
    # Windowed smoke run: 120 frames through the real loop, GL debug layer on, exit 0.
    add_test(NAME sample_01_triangle COMMAND 01_triangle -frames 120 -platform.error_box 0 -r.vsync 0)
    set_tests_properties(sample_01_triangle PROPERTIES LABELS gpu)
endif()

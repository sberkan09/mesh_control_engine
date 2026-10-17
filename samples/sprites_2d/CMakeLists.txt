mce_add_executable(sprites_2d SOURCES main.cpp DEPS mce::mce_platform mce::mce_render mce::mce_rhi_default)
target_include_directories(sprites_2d PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
set_target_properties(sprites_2d PROPERTIES OUTPUT_NAME "sprites_2d")

if(MCE_BUILD_TESTS AND MCE_GPU_TESTS)
    # Windowed smoke run: 10k sprites + tilemap + HUD text for 120 frames, GL debug layer on, exit 0.
    add_test(NAME sample_sprites_2d COMMAND sprites_2d -frames 120 -sprites 10000 -platform.error_box 0 -r.vsync 0)
    set_tests_properties(sample_sprites_2d PROPERTIES LABELS gpu)
    # 50k sprites, past the frame arena and geometry ring defaults (an RTX 3070 run crashed here): the sample
    # sizes both, so nothing overflows and nothing is dropped.
    add_test(NAME sample_sprites_2d_50k COMMAND sprites_2d -frames 3 -sprites 50000 -platform.error_box 0 -r.vsync 0)
    set_tests_properties(sample_sprites_2d_50k PROPERTIES LABELS gpu FAIL_REGULAR_EXPRESSION "overflow")
endif()

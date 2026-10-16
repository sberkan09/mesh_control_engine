# M-VISUAL demo: one scene rendered under the M5a frame (Current) and the Low/Medium/High quality tiers,
# side by side (-sheet). Procedural content (tools/gen_assets.py) cooked at build time into a pack.
mce_add_executable(sample_06_showcase SOURCES main.cpp
    DEPS mce::mce_render mce::mce_asset mce::mce_platform mce::mce_rhi_default)
target_include_directories(sample_06_showcase PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../common")
mce_sample_pack(sample_06_showcase)

if(MCE_BUILD_TESTS)
    # Sample ladder rule: 300 headless steps (layout, scatter, scene build per profile).
    add_test(NAME sample_06_showcase_300 COMMAND sample_06_showcase -headless 1 -ticks 300)
    set_tests_properties(sample_06_showcase_300 PROPERTIES PASS_REGULAR_EXPRESSION "300 ticks .* hash [0-9a-f]+")
    if(MCE_GPU_TESTS)
        add_test(NAME sample_06_showcase_gpu COMMAND sample_06_showcase -frames 60 -platform.error_box 0 -r.vsync 0)
        set_tests_properties(sample_06_showcase_gpu PROPERTIES LABELS gpu)
        # 210 s under llvmpipe (M4): the full 60 frames run only in the integration run (`slow`); --task runs 10.
        mce_mark_slow(sample_06_showcase_gpu)
        add_test(NAME sample_06_showcase_gpu_smoke COMMAND sample_06_showcase -frames 10 -platform.error_box 0 -r.vsync 0)
        set_tests_properties(sample_06_showcase_gpu_smoke PROPERTIES LABELS gpu)
    endif()
endif()

# ROADMAP M5a: the PBR showcase, plus the M5b capsule walk (-walk 1). Procedural content
# (tools/gen_assets.py, regenerated deterministically) cooked at build time into a pack the sample mounts.
mce_add_executable(sample_04_cubes SOURCES main.cpp
    DEPS mce::mce_render mce::mce_asset mce::mce_platform mce::mce_rhi_gl mce::mce_physics3d)
target_include_directories(sample_04_cubes PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../common")
mce_sample_pack(sample_04_cubes)

if(MCE_BUILD_TESTS)
    # Sample ladder rule: 300 headless steps (animation, culling, cascade fit, light clustering).
    add_test(NAME sample_04_cubes_300 COMMAND sample_04_cubes -headless 1 -ticks 300)
    set_tests_properties(sample_04_cubes_300 PROPERTIES PASS_REGULAR_EXPRESSION "300 ticks .* hash [0-9a-f]+")
    # M5b: the capsule walk (move_character + camera_sweep); the boom must have been pulled in.
    add_test(NAME sample_04_cubes_walk_300 COMMAND sample_04_cubes -headless 1 -walk 1 -ticks 300)
    set_tests_properties(sample_04_cubes_walk_300 PROPERTIES PASS_REGULAR_EXPRESSION "camera hits [1-9][0-9]*, .*300 ticks .* hash [0-9a-f]+")
    if(MCE_GPU_TESTS)
        add_test(NAME sample_04_cubes_gpu COMMAND sample_04_cubes -frames 60 -platform.error_box 0 -r.vsync 0)
        set_tests_properties(sample_04_cubes_gpu PROPERTIES LABELS gpu)
    endif()
endif()

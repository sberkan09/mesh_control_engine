# Artifact B (ROADMAP M4): the lit 3D diorama. Its content is Kenney CC0 kit meshes (CREDITS.md)
# under assets/, cooked at build time by mce_cook into a pack the sample mounts.
mce_add_executable(sample_04_diorama SOURCES main.cpp
    DEPS mce::mce_render mce::mce_asset mce::mce_platform mce::mce_rhi_default)
target_include_directories(sample_04_diorama PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../common")
mce_sample_pack(sample_04_diorama)

if(MCE_BUILD_TESTS)
    # Sample ladder rule: every sample runs 300 headless steps in CI (here: animation, culling,
    # sort and shadow fit over the cooked meshes, no GL).
    add_test(NAME sample_04_diorama_300 COMMAND sample_04_diorama -headless 1 -ticks 300)
    set_tests_properties(sample_04_diorama_300 PROPERTIES PASS_REGULAR_EXPRESSION "300 ticks .* hash [0-9a-f]+")
    if(MCE_GPU_TESTS)
        add_test(NAME sample_04_diorama_gpu COMMAND sample_04_diorama -frames 120 -platform.error_box 0 -r.vsync 0)
        set_tests_properties(sample_04_diorama_gpu PROPERTIES LABELS gpu)
    endif()
endif()
mce_ship_metadata(sample_04_diorama NAME "MCE Diorama" DESCRIPTION "Mesh Control Engine - 3D diorama (Artifact B)"
    ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.ico")

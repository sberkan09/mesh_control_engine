# Artifact B (ROADMAP M4): the lit 3D diorama. Its content is Kenney CC0 kit meshes (CREDITS.md)
# under assets/, cooked at build time by mce_cook into a pack the sample mounts.
set(DIORAMA_PROJECT "${CMAKE_CURRENT_BINARY_DIR}/project")
set(DIORAMA_PACK "${DIORAMA_PROJECT}/assets.mce")
file(GLOB_RECURSE DIORAMA_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/*")
add_custom_command(
    OUTPUT "${DIORAMA_PACK}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${DIORAMA_PROJECT}/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets" "${DIORAMA_PROJECT}/assets"
    COMMAND $<TARGET_FILE:mce_cook> "${DIORAMA_PROJECT}" --pack --quiet
    DEPENDS mce_cook ${DIORAMA_SOURCES}
    COMMENT "Cooking 04_diorama content"
    VERBATIM)
add_custom_target(diorama_content DEPENDS "${DIORAMA_PACK}")

mce_add_executable(sample_04_diorama SOURCES main.cpp
    DEPS mce::mce_render mce::mce_asset mce::mce_platform mce::mce_rhi_gl)
target_include_directories(sample_04_diorama PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../common")
target_compile_definitions(sample_04_diorama PRIVATE MCE_DIORAMA_DEV_PACK="${DIORAMA_PACK}")
add_dependencies(sample_04_diorama diorama_content)

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

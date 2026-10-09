# extras_2d: an editor project (assets/ + scenes/*.mcscene, no code) showing the M6 2D extras in one
# scene: parallax backdrops (FR-R2D-050), normal-mapped bricks under Light2D torches (FR-R2D-090),
# ember particles (FR-R2D-070) and 2D post (FR-R2D-100). mce_player plays its cooked pack; the art
# comes from gen_art.py. CI cooks a copy and plays it headless and on the GPU.
set(project_dir "${CMAKE_CURRENT_BINARY_DIR}/project")
set(pack "${project_dir}/assets.mce")
file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/*")
add_custom_command(
    OUTPUT "${pack}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${project_dir}/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets" "${project_dir}/assets"
    COMMAND $<TARGET_FILE:mce_cook> "${project_dir}" --pack --quiet
    DEPENDS mce_cook ${sources}
    COMMENT "Cooking extras_2d content"
    VERBATIM)
add_custom_target(extras_2d_content ALL DEPENDS "${pack}")
add_dependencies(extras_2d_content mce_player)

if(MCE_BUILD_TESTS)
    add_test(NAME extras_2d_player_600 COMMAND mce_player -headless 1 -ticks 600 -pack "${pack}")
    set_tests_properties(extras_2d_player_600 PROPERTIES PASS_REGULAR_EXPRESSION "600 ticks, 99 entities, state_hash [0-9a-f]+")
    # FR-RUN-060: a clean quit writes the -save slot, the next start loads it (tick carried over).
    set(save_dir "${CMAKE_CURRENT_BINARY_DIR}/saves")
    add_test(NAME extras_2d_save_clean COMMAND ${CMAKE_COMMAND} -E rm -rf "${save_dir}")
    add_test(NAME extras_2d_save_write
             COMMAND mce_player -headless 1 -ticks 120 -pack "${pack}" -save ci -save_dir "${save_dir}")
    add_test(NAME extras_2d_save_load
             COMMAND mce_player -headless 1 -ticks 60 -pack "${pack}" -save ci -save_dir "${save_dir}")
    set_tests_properties(extras_2d_save_clean PROPERTIES FIXTURES_SETUP extras_2d_save_dir)
    set_tests_properties(extras_2d_save_write PROPERTIES FIXTURES_REQUIRED extras_2d_save_dir FIXTURES_SETUP extras_2d_save
                         PASS_REGULAR_EXPRESSION "save 'ci' written \\(tick 120\\)")
    set_tests_properties(extras_2d_save_load PROPERTIES FIXTURES_REQUIRED "extras_2d_save_dir;extras_2d_save"
                         PASS_REGULAR_EXPRESSION "save 'ci' loaded \\([0-9]+ entities, tick 120\\).*save 'ci' written \\(tick 180\\)")
    if(MCE_GPU_TESTS)
        add_test(NAME extras_2d_player_gpu
                 COMMAND mce_player -pack "${pack}" -frames 120 -capture "${CMAKE_CURRENT_BINARY_DIR}/extras_2d.ppm"
                         -platform.error_box 0 -r.vsync 0)
        set_tests_properties(extras_2d_player_gpu PROPERTIES LABELS gpu)
    endif()
endif()

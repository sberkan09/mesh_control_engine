# 08_movement: the M-MOVE3D demo, an editor project (assets/ + scenes/course.mcscene and race.mcscene, no code) built
# over MCP alone by tests/mcp/test_mcp_movement.cpp (MCE_MOVEMENT_EXPORT writes it here).
# course: a humanoid rides a lift, jumps a gap inside coyote time and crouches through a tunnel, a spring-arm camera
# behind. race: a cart you drive (WASD / the left stick, Space handbrake) against two AI karts on a nine-waypoint
# track; corners skid. mce_player -scene scenes/race.mcscene plays the second; the first is the default.
set(project_dir "${CMAKE_CURRENT_BINARY_DIR}/project")
set(pack "${project_dir}/assets.mce")
file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/*")
add_custom_command(
    OUTPUT "${pack}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${project_dir}/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets" "${project_dir}/assets"
    COMMAND $<TARGET_FILE:mce_cook> "${project_dir}" --pack --quiet
    DEPENDS mce_cook ${sources}
    COMMENT "Cooking 08_movement content"
    VERBATIM)
add_custom_target(movement_content ALL DEPENDS "${pack}")
add_dependencies(movement_content mce_player)

if(MCE_BUILD_TESTS)
    # Sample ladder rule: both scenes play headless with their scripts and print their hash.
    add_test(NAME sample_08_movement_course_player_900
             COMMAND mce_player -headless 1 -ticks 900 -pack "${pack}" -scene scenes/course.mcscene
                     -input "${CMAKE_CURRENT_SOURCE_DIR}/course.input")
    set_tests_properties(sample_08_movement_course_player_900 PROPERTIES
                         PASS_REGULAR_EXPRESSION "900 ticks, [0-9]+ entities, state_hash [0-9a-f]+"
                         FAIL_REGULAR_EXPRESSION "\\[error\\]")
    add_test(NAME sample_08_movement_race_player_2600
             COMMAND mce_player -headless 1 -ticks 2600 -pack "${pack}" -scene scenes/race.mcscene
                     -input "${CMAKE_CURRENT_SOURCE_DIR}/race.input")
    set_tests_properties(sample_08_movement_race_player_2600 PROPERTIES
                         PASS_REGULAR_EXPRESSION "2600 ticks, [0-9]+ entities, state_hash [0-9a-f]+"
                         FAIL_REGULAR_EXPRESSION "\\[error\\]")
    if(MCE_GPU_TESTS)
        add_test(NAME sample_08_movement_player_gpu
                 COMMAND mce_player -pack "${pack}" -frames 120 -input "${CMAKE_CURRENT_SOURCE_DIR}/course.input"
                         -platform.error_box 0 -r.vsync 0)
        set_tests_properties(sample_08_movement_player_gpu PROPERTIES LABELS gpu)
    endif()
endif()

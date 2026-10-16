# 08_movement: the M-MOVE3D demo, an editor project (assets/ + scenes/course.mcscene, no code) built
# over MCP alone by tests/mcp/test_mcp_move3d.cpp (MCE_MOVE3D_EXPORT writes it here). A humanoid runs an
# obstacle course (a hurdle, a low tunnel, a lift up to a ledge) on a CharacterController3D with
# acceleration, coyote time and crouch, animated by Locomotion3D, under a CameraFollow3D spring arm;
# three karts race two laps of a waypoint track on VehicleDriver3D. mce_player plays the cooked
# pack; WASD / the left stick move, Space jumps, C crouches.
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
    # Sample ladder rule: the scene plays headless with the drive script, and prints its hash.
    add_test(NAME sample_08_movement_player_900
             COMMAND mce_player -headless 1 -ticks 900 -pack "${pack}" -input "${CMAKE_CURRENT_SOURCE_DIR}/drive.input")
    set_tests_properties(sample_08_movement_player_900 PROPERTIES
                         PASS_REGULAR_EXPRESSION "900 ticks, 17 entities, state_hash [0-9a-f]+"
                         FAIL_REGULAR_EXPRESSION "\\[error\\]")
    if(MCE_GPU_TESTS)
        add_test(NAME sample_08_movement_player_gpu
                 COMMAND mce_player -pack "${pack}" -frames 120 -input "${CMAKE_CURRENT_SOURCE_DIR}/drive.input"
                         -platform.error_box 0 -r.vsync 0)
        set_tests_properties(sample_08_movement_player_gpu PROPERTIES LABELS gpu)
    endif()
endif()

# 07_gameplay3d: the M-GAMEPLAY3D demo, an editor project (assets/ + scenes/arena.mcscene, no code)
# built over MCP alone by tests/mcp/test_mcp_gameplay3d.cpp (MCE_GAMEPLAY3D_EXPORT writes it here).
# Grunts chase the cart across the baked navmesh and keep apart, the cart pushes the hinged door
# open, the drawbridge lowers on its motor, and the cart's bumper sends the grunts it hits into
# ragdolls. mce_player plays the cooked pack; WASD / the left stick drive the cart.
set(project_dir "${CMAKE_CURRENT_BINARY_DIR}/project")
set(pack "${project_dir}/assets.mce")
file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/*")
add_custom_command(
    OUTPUT "${pack}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${project_dir}/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets" "${project_dir}/assets"
    COMMAND $<TARGET_FILE:mce_cook> "${project_dir}" --pack --quiet
    DEPENDS mce_cook ${sources}
    COMMENT "Cooking 07_gameplay3d content"
    VERBATIM)
add_custom_target(gameplay3d_content ALL DEPENDS "${pack}")
add_dependencies(gameplay3d_content mce_player)

if(MCE_BUILD_TESTS)
    # Sample ladder rule: the scene plays headless with the drive script, and prints its hash.
    add_test(NAME sample_07_gameplay3d_player_600
             COMMAND mce_player -headless 1 -ticks 600 -pack "${pack}" -input "${CMAKE_CURRENT_SOURCE_DIR}/drive.input")
    set_tests_properties(sample_07_gameplay3d_player_600 PROPERTIES
                         PASS_REGULAR_EXPRESSION "1 segments, last tick 300.*600 ticks, 13 entities, state_hash [0-9a-f]+"
                         FAIL_REGULAR_EXPRESSION "\\[error\\]")
    if(MCE_GPU_TESTS)
        add_test(NAME sample_07_gameplay3d_player_gpu
                 COMMAND mce_player -pack "${pack}" -frames 120 -input "${CMAKE_CURRENT_SOURCE_DIR}/drive.input"
                         -platform.error_box 0 -r.vsync 0)
        set_tests_properties(sample_07_gameplay3d_player_gpu PROPERTIES LABELS gpu)
    endif()
endif()

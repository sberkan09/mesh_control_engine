# stream_demo: scene streaming (FR-ENT-120) as data alone, an editor project with no code. A base
# scene holds the traveler (camera + StreamingFocus + a looping patrol sequence) and four
# SceneChunk entities; each area is its own scene and loads as the traveler nears it. The build
# cooks a copy (like scene_demo) and mce_player plays it.
set(project_dir "${CMAKE_CURRENT_BINARY_DIR}/project")
set(pack "${project_dir}/assets.mce")
file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/*")
add_custom_command(
    OUTPUT "${pack}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${project_dir}/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets" "${project_dir}/assets"
    COMMAND $<TARGET_FILE:mce_cook> "${project_dir}" --pack --quiet
    DEPENDS mce_cook ${sources}
    COMMENT "Cooking stream_demo content"
    VERBATIM)
add_custom_target(stream_demo_content ALL DEPENDS "${pack}")
add_dependencies(stream_demo_content mce_player)

if(MCE_BUILD_TESTS)
    # Tick 300: the traveler is at x = 27, inside area 1 and 3 m from area 2 (both in); area 0,
    # 17 m behind, went out past its 8 m unload distance. Headless streaming is synchronous, so
    # the counts and the hash are a function of the tick.
    add_test(NAME stream_demo_player_300 COMMAND mce_player -headless 1 -ticks 300 -pack "${pack}" -scene scenes/world.mcscene)
    set_tests_properties(stream_demo_player_300 PROPERTIES PASS_REGULAR_EXPRESSION
        "300 ticks, 5 entities, state_hash [0-9a-f]+.*streaming 2/4 chunks loaded, 8 streamed entities, 3 loads, 1 unloads")
    if(MCE_GPU_TESTS)
        # Windowed: async reads on the loader thread, one area instantiated per tick.
        add_test(NAME stream_demo_player_gpu
                 COMMAND mce_player -pack "${pack}" -scene scenes/world.mcscene -frames 240 -platform.error_box 0 -r.vsync 0)
        set_tests_properties(stream_demo_player_gpu PROPERTIES LABELS gpu)
    endif()
endif()

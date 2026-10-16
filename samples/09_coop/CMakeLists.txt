# 09_coop: M-VIEWS' demo, a data-only local co-op game the stock mce_player plays. Four players share one
# arena: each joins with their Join action (keyboard keys or a pad), the window splits into one cell per
# player (SplitScreen + a PlayerCamera/CameraView per player), and a RenderTexture fed by an overview camera
# shows a minimap on the monitor sprite. CoopGame spawns assets/prefabs/player.mcprefab at the PlayerSpawns.
# assets/input/coop.mcinput has one map per player ("player": 1..4). gen_assets.py rebuilds assets/.
# `mce_player -pack <build>/samples/09_coop/project/assets.mce`; input.txt is the headless script.
if(NOT TARGET mce_cook)
    return()
endif()
set(project_dir "${CMAKE_CURRENT_BINARY_DIR}/project")
set(pack "${project_dir}/assets.mce")
file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/*")
add_custom_command(
    OUTPUT "${pack}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${project_dir}/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets" "${project_dir}/assets"
    COMMAND $<TARGET_FILE:mce_cook> "${project_dir}" --pack --quiet
    DEPENDS mce_cook ${sources}
    COMMENT "Cooking 09_coop content"
    VERBATIM)
add_custom_target(coop_sample_content ALL DEPENDS "${pack}")
add_dependencies(coop_sample_content mce_player)

if(MCE_BUILD_TESTS)
    # Headless: four players join by script, one leaves; the player count is not in that number, the same hash twice.
    add_test(NAME coop_sample_player
             COMMAND ${CMAKE_COMMAND} -DMCE_PLAYER=$<TARGET_FILE:mce_player> -DPACK=${pack} -DTICKS=240 -DENTITIES=18
                     -DSCENE=scenes/arena.mcscene -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/input.txt -DTOUR=09_coop
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/../tour_test.cmake)
    if(MCE_GPU_TESTS)
        # Windowed: the four-way split and the minimap, a sheet of frames after all four have joined.
        add_test(NAME coop_sample_player_gpu
                 COMMAND mce_player -pack "${pack}" -scene scenes/arena.mcscene -input ${CMAKE_CURRENT_SOURCE_DIR}/input.txt
                         -frames 120 -capture "${CMAKE_CURRENT_BINARY_DIR}/coop.ppm" -platform.error_box 0 -r.vsync 0)
        set_tests_properties(coop_sample_player_gpu PROPERTIES LABELS gpu)
    endif()
endif()

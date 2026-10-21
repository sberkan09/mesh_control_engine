# ui_screens: a HUD and a pause menu as UI documents (M-UI-DATA step 2), with no host code. The
# scene (assets/scenes/main.mcscene) places two kit::UiScreen entities (day theme; night.mcscene
# shows the same documents under the night theme, M-UI-DATA step 3); the stock mce_player builds
# the .mcui documents, draws them over the game and turns their widgets' actions into input. The
# game module (game/) only reads actions, writes the Score the HUD binds and toggles the pause
# screen's `visible`: it never includes mce/ui (check_walls.py).
if(NOT MCE_GAME_MODULES_SUPPORTED OR NOT TARGET mce_cook)
    return()
endif()
mce_add_game_module(ui_screens_game SOURCES game/ui_screens_game.cpp)

set(project_dir "${CMAKE_CURRENT_BINARY_DIR}/project")
set(pack "${project_dir}/assets.mce")
file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/*")
add_custom_command(
    OUTPUT "${pack}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${project_dir}/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets" "${project_dir}/assets"
    COMMAND $<TARGET_FILE:mce_cook> --module $<TARGET_FILE:ui_screens_game> "${project_dir}" --pack --quiet
    DEPENDS mce_cook ui_screens_game ${sources}
    COMMENT "Cooking ui_screens content"
    VERBATIM)
add_custom_target(ui_screens_content ALL DEPENDS "${pack}")
add_dependencies(ui_screens_content mce_player)

if(MCE_BUILD_TESTS)
    # Headless: the screens load (their actions declared), the pause menu opens on a scripted
    # "Pause" and closes on "Resume", and the state hash is the same on a second run.
    add_test(NAME ui_screens_player
             COMMAND ${CMAKE_COMMAND} -DMCE_PLAYER=$<TARGET_FILE:mce_player> -DMODULE=$<TARGET_FILE:ui_screens_game>
                     -DPACK=${pack} -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/player_test
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/player_test.cmake)
    if(MCE_GPU_TESTS)
        # Windowed: both documents built and drawn over the scene, the pause menu opened by script.
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/pause_script.txt" "30 Pause\n")
        add_test(NAME ui_screens_player_gpu
                 COMMAND mce_player -pack "${pack}" -module $<TARGET_FILE:ui_screens_game> -frames 90
                         -input "${CMAKE_CURRENT_BINARY_DIR}/pause_script.txt" -platform.error_box 0 -r.vsync 0)
        set_tests_properties(ui_screens_player_gpu PROPERTIES LABELS gpu)
        # The same documents under the night theme (M-UI-DATA step 3).
        add_test(NAME ui_screens_night_gpu
                 COMMAND mce_player -pack "${pack}" -scene scenes/night.mcscene -module $<TARGET_FILE:ui_screens_game> -frames 90
                         -input "${CMAKE_CURRENT_BINARY_DIR}/pause_script.txt" -platform.error_box 0 -r.vsync 0)
        set_tests_properties(ui_screens_night_gpu PROPERTIES LABELS gpu)
    endif()
endif()

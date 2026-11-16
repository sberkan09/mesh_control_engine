# 10_missions: M-MISSION's demo, a data-only game the stock mce_player plays (assets/ + one Luau script, no C++).
# Three missions chained by data on a flat 3D field: reach a zone, kill a target with Health3D (the player's
# Weapon3D), deliver at a second zone within a time limit. Objective markers show on the Minimap widget of
# the .mcui HUD (the stock player feeds it); assets/scripts/missions.luau starts mission 1 and starts the
# next one when the one before completes (mission.* and marker.*). input.txt is the scripted run.
# `mce_player -pack <build>/samples/10_missions/project/assets.mce -input samples/10_missions/input.txt`
if(NOT TARGET mce_cook OR NOT MCE_WITH_SCRIPT)
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
    COMMENT "Cooking 10_missions content"
    VERBATIM)
add_custom_target(missions_sample_content ALL DEPENDS "${pack}")
add_dependencies(missions_sample_content mce_player)

if(MCE_BUILD_TESTS)
    add_test(NAME sample_missions_headless_300
             COMMAND ${CMAKE_COMMAND} -DMCE_PLAYER=$<TARGET_FILE:mce_player> -DPACK=${pack}
                     -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/input.txt -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/headless
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/missions_test.cmake)
    if(MCE_GPU_TESTS)
        # Windowed: the HUD minimap fed by the stock player, the last frame of a scripted run.
        add_test(NAME sample_missions_gpu
                 COMMAND mce_player -pack "${pack}" -input ${CMAKE_CURRENT_SOURCE_DIR}/input.txt -frames 120
                         -capture "${CMAKE_CURRENT_BINARY_DIR}/missions.ppm" -platform.error_box 0 -r.vsync 0)
        set_tests_properties(sample_missions_gpu PROPERTIES LABELS gpu)
    endif()
endif()

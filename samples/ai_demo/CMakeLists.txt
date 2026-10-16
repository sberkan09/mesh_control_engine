# ai_demo: M-AI's demo, a data-only project the stock mce_player plays. A guard patrols between two
# posts on a navmesh (move_to, look_at, wait in a repeat) until a bell's tree rings the alarm; the
# guard's tree hears it (wait_event), and a guard that observes lower-priority branches cuts the
# patrol short and chases the thief (abort "lower"). Steps 3-6 of M-AI grow it.
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
    COMMENT "Cooking ai_demo content"
    VERBATIM)
add_custom_target(ai_demo_content ALL DEPENDS "${pack}")
add_dependencies(ai_demo_content mce_player)

if(MCE_BUILD_TESTS)
    # Headless: the alarm cuts the patrol short and the guard catches the thief, the same way twice.
    add_test(NAME ai_demo_player
             COMMAND ${CMAKE_COMMAND} -DMCE_PLAYER=$<TARGET_FILE:mce_player> -DPACK=${pack}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/player_test.cmake)
endif()

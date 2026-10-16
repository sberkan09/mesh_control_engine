# kit2d_tour: M-COVER's 2D and gameplay tour, an editor-style project (assets/ + scenes/tour2d.mcscene,
# no code). A cut-out puppet waves (animator, slots, IK driven by a goal entity, a charm that follows
# its forearm bone), a ball drops through a sensor gate that bursts embers, shakes the camera, plays a
# thud with a caption and counts the events, a walker follows a path across a nav grid built from the
# colliders, and a token pool waits for the AI. mce_player plays the cooked pack:
# `mce_player -pack <build>/samples/kit2d_tour/project/assets.mce`.
set(project_dir "${CMAKE_CURRENT_BINARY_DIR}/project")
set(pack "${project_dir}/assets.mce")
file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/*")
add_custom_command(
    OUTPUT "${pack}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${project_dir}/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets" "${project_dir}/assets"
    COMMAND $<TARGET_FILE:mce_cook> "${project_dir}" --pack --quiet
    DEPENDS mce_cook ${sources}
    COMMENT "Cooking kit2d_tour content"
    VERBATIM)
add_custom_target(kit2d_tour_content ALL DEPENDS "${pack}")
add_dependencies(kit2d_tour_content mce_player)

if(MCE_BUILD_TESTS)
    add_test(NAME kit2d_tour_player
             COMMAND ${CMAKE_COMMAND} -DMCE_PLAYER=$<TARGET_FILE:mce_player> -DPACK=${pack} -DTICKS=600 -DENTITIES=10
                     -DTOUR=kit2d_tour -P ${CMAKE_CURRENT_SOURCE_DIR}/../tour_test.cmake)
endif()

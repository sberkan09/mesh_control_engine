# kit3d_tour: M-COVER's 3D tour, an editor-style project (assets/ + scenes/tour.mcscene, no code)
# that shows the 3D kit's data components in one scene: a hero with a look-at IK chain, a hat on its
# head bone and a locomotion feed, a shaking camera and a second camera that blends from it, a
# reflection probe, a GI volume, foliage, a decal, a fire, a flat terrain with its collider, a navmesh
# with a mud area, an obstacle and a jump link, a down-facing ray sensor, and a car that drives a
# three-gate circuit. mce_player plays the cooked pack: `mce_player -pack <build>/samples/kit3d_tour/project/assets.mce`.
set(project_dir "${CMAKE_CURRENT_BINARY_DIR}/project")
set(pack "${project_dir}/assets.mce")
file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/*")
add_custom_command(
    OUTPUT "${pack}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${project_dir}/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets" "${project_dir}/assets"
    COMMAND $<TARGET_FILE:mce_cook> "${project_dir}" --pack --quiet
    DEPENDS mce_cook ${sources}
    COMMENT "Cooking kit3d_tour content"
    VERBATIM)
add_custom_target(kit3d_tour_content ALL DEPENDS "${pack}")
add_dependencies(kit3d_tour_content mce_player)

if(MCE_BUILD_TESTS)
    add_test(NAME kit3d_tour_player
             COMMAND ${CMAKE_COMMAND} -DMCE_PLAYER=$<TARGET_FILE:mce_player> -DPACK=${pack} -DTICKS=600 -DENTITIES=21
                     -DTOUR=kit3d_tour -P ${CMAKE_CURRENT_SOURCE_DIR}/../tour_test.cmake)
endif()

if(MCE_BUILD_TESTS AND MCE_GPU_TESTS)
    # -split_views 4: the tour's primary camera drawn in four split-screen cells (GPU ms per view count).
    add_test(NAME kit3d_tour_split_views
             COMMAND mce_player -pack ${pack} -frames 5 -split_views 4 -platform.error_box 0 -r.vsync 0)
    set_tests_properties(kit3d_tour_split_views PROPERTIES LABELS gpu PASS_REGULAR_EXPRESSION "drawn 4 times")
endif()

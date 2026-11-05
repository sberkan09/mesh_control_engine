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
             COMMAND ${CMAKE_COMMAND} -DMCE_PLAYER=$<TARGET_FILE:mce_player> -DPACK=${pack} -DTICKS=600 -DENTITIES=23
                     -DTOUR=kit3d_tour -P ${CMAKE_CURRENT_SOURCE_DIR}/../tour_test.cmake)
    # The ink scene (M-OUTLINE 39e): scenes/tour_ink.mcscene, the tour with an EdgeOutline3D on the world entity. It is named so that
    # it sorts after scenes/tour.mcscene (the player's default scene is the first by source path, so the default run and its hash
    # stay as they were); the render-side outline changes no state, so the 23 entities are the tour's.
    add_test(NAME sample_kit3d_tour_ink_headless
             COMMAND ${CMAKE_COMMAND} -DMCE_PLAYER=$<TARGET_FILE:mce_player> -DPACK=${pack} -DSCENE=scenes/tour_ink.mcscene
                     -DTICKS=60 -DENTITIES=23 -DTOUR=kit3d_tour_ink -P ${CMAKE_CURRENT_SOURCE_DIR}/../tour_test.cmake)
    # The cookie scene (M-LIGHTCOOKIE 40h): scenes/tour_cookie.mcscene, a room with a window wall, a SpotLight3D + LightCookie3D shining a
    # window grid onto the floor and a SunCookie3D leaf dapple over the terrain; its textures are made by gen_cookies.py. Named tour_cookie so it sorts after
    # scenes/tour.mcscene, so the default scene stays the tour. 15 entities. (Chosen over 11_daynight and 09_openworld: the plan puts the
    # cookie sample in the tour, which already shows the 3D kit's data components in one project.)
    add_test(NAME sample_kit3d_tour_cookie_headless
             COMMAND ${CMAKE_COMMAND} -DMCE_PLAYER=$<TARGET_FILE:mce_player> -DPACK=${pack} -DSCENE=scenes/tour_cookie.mcscene
                     -DTICKS=60 -DENTITIES=15 -DTOUR=kit3d_tour_cookie -P ${CMAKE_CURRENT_SOURCE_DIR}/../tour_test.cmake)
    # The translucency scene (M-TRANSLUCENT 41g): scenes/tour_translucent.mcscene, a low sun behind a Foliage3D row of leaf cards, a paper
    # screen, a paper lantern around a PointLight3D and wax / plain / skin spheres; its models, textures and .mcmat files are made by
    # gen_translucent.py. Named tour_translucent so it sorts after scenes/tour.mcscene, so the default scene stays the tour. 10 entities.
    # (Chosen over 09_openworld: its trees and grass are opaque scenery of a driving test whose hashes and cook must not move, while the
    # tour already shows the 3D kit's data components in one project and a leaf card, a paper screen and a lantern need a staged light.)
    add_test(NAME sample_kit3d_tour_translucent_headless
             COMMAND ${CMAKE_COMMAND} -DMCE_PLAYER=$<TARGET_FILE:mce_player> -DPACK=${pack} -DSCENE=scenes/tour_translucent.mcscene
                     -DTICKS=60 -DENTITIES=10 -DTOUR=kit3d_tour_translucent -P ${CMAKE_CURRENT_SOURCE_DIR}/../tour_test.cmake)
endif()

if(MCE_BUILD_TESTS AND MCE_GPU_TESTS)
    # -split_views 4: the tour's primary camera drawn in four split-screen cells (GPU ms per view count).
    add_test(NAME kit3d_tour_split_views
             COMMAND mce_player -pack ${pack} -frames 5 -split_views 4 -platform.error_box 0 -r.vsync 0)
    set_tests_properties(kit3d_tour_split_views PROPERTIES LABELS gpu PASS_REGULAR_EXPRESSION "drawn 4 times")
    # The ink capture: the ink scene at tick 30, written for a look (build/.../kit3d_tour_ink.ppm; samples keep no goldens). Lines
    # show on the slabs, the platform and the hero; `-r.edge_outline 0` on the same command draws the frame without them.
    add_test(NAME sample_kit3d_tour_ink_gpu
             COMMAND mce_player -pack "${pack}" -scene scenes/tour_ink.mcscene -capture_tick 30
                     -capture "${CMAKE_CURRENT_BINARY_DIR}/kit3d_tour_ink.ppm" -platform.error_box 0 -r.vsync 0
                     -sim.fast_forward 1 -sim.max_catchup 32 -pause_on_focus_loss 0)
    set_tests_properties(sample_kit3d_tour_ink_gpu PROPERTIES LABELS gpu TIMEOUT 120)
    mce_mark_slow(sample_kit3d_tour_ink_gpu)
endif()

if(MCE_BUILD_TESTS AND MCE_GPU_TESTS)
    # The cookie capture: the cookie scene at tick 30, written for a look (build/.../kit3d_tour_cookie.ppm; samples keep no goldens).
    # The window grid lies on the floor and the leaf dapple on the terrain; `-r.light_cookies 0` draws the plain lights.
    add_test(NAME sample_kit3d_tour_cookie_gpu
             COMMAND mce_player -pack "${pack}" -scene scenes/tour_cookie.mcscene -capture_tick 30
                     -capture "${CMAKE_CURRENT_BINARY_DIR}/kit3d_tour_cookie.ppm" -platform.error_box 0 -r.vsync 0
                     -sim.fast_forward 1 -sim.max_catchup 32 -pause_on_focus_loss 0)
    set_tests_properties(sample_kit3d_tour_cookie_gpu PROPERTIES LABELS gpu TIMEOUT 120)
    mce_mark_slow(sample_kit3d_tour_cookie_gpu)
endif()

if(MCE_BUILD_TESTS AND MCE_GPU_TESTS)
    # The translucency capture: the scene at tick 30, written for a look (build/.../kit3d_tour_translucent.ppm; samples keep no goldens).
    # The leaf row glows against the low sun, the paper screen and the lantern glow, the wax sphere has a soft terminator;
    # `-r.translucency 0` draws the plain materials.
    add_test(NAME sample_kit3d_tour_translucent_gpu
             COMMAND mce_player -pack "${pack}" -scene scenes/tour_translucent.mcscene -capture_tick 30
                     -capture "${CMAKE_CURRENT_BINARY_DIR}/kit3d_tour_translucent.ppm" -platform.error_box 0 -r.vsync 0
                     -sim.fast_forward 1 -sim.max_catchup 32 -pause_on_focus_loss 0)
    set_tests_properties(sample_kit3d_tour_translucent_gpu PROPERTIES LABELS gpu TIMEOUT 120)
    mce_mark_slow(sample_kit3d_tour_translucent_gpu)
endif()

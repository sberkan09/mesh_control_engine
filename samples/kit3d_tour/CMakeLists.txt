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
    # The lens rain scene (M-LENSRAIN 42e): scenes/tour_lensrain.mcscene, lit spheres and coloured lamps in a dusk, a Weather entity told to
    # rain (2 s transition) and a LensRain3D (wet_rate 2); made by gen_lensrain.py. Named tour_lensrain so it sorts after scenes/tour.mcscene,
    # so the default scene stays the tour. 13 entities. 300 ticks (5 s): the lens is wet from about tick 120, so wet_q (hashed) has moved and
    # the two plays must agree on it. (Chosen over 12_weather: its weather script, entity count and hashes are pinned by its own test.)
    add_test(NAME sample_kit3d_tour_lensrain_headless
             COMMAND ${CMAKE_COMMAND} -DMCE_PLAYER=$<TARGET_FILE:mce_player> -DPACK=${pack} -DSCENE=scenes/tour_lensrain.mcscene
                     -DTICKS=300 -DENTITIES=13 -DTOUR=kit3d_tour_lensrain -P ${CMAKE_CURRENT_SOURCE_DIR}/../tour_test.cmake)
    # The heat haze scene (M-HEATHAZE 43e): scenes/tour_haze.mcscene, a campfire (glowing stack, fire particles, flickering point light)
    # under a HeatHaze3D with fire_follow, a backdrop grid of tinted spheres and a thin pole in front; made by gen_haze.py. Named
    # tour_haze so it sorts after scenes/tour.mcscene, so the default scene stays the tour. 30 entities. 300 ticks (5 s): the flicker and
    # the fire particles have run, so the two plays must agree on them (the haze itself is render-side and hashes nothing).
    add_test(NAME sample_kit3d_tour_haze_headless
             COMMAND ${CMAKE_COMMAND} -DMCE_PLAYER=$<TARGET_FILE:mce_player> -DPACK=${pack} -DSCENE=scenes/tour_haze.mcscene
                     -DTICKS=300 -DENTITIES=30 -DTOUR=kit3d_tour_haze -P ${CMAKE_CURRENT_SOURCE_DIR}/../tour_test.cmake)
    # The underwater scene (M-UNDERWATER 44e): scenes/tour_underwater.mcscene, a calm Water (level 0, amplitude 0.2) over the tour's terrain
    # moved 6 m down, a pillar that rises through the surface, three white spheres at about 3, 8 and 20 m and the camera 2 m below the
    # surface; made by gen_underwater.py. Named tour_underwater so it sorts after scenes/tour.mcscene, so the default scene stays the
    # tour. 8 entities. 300 ticks (5 s) like the haze (the look is render-side and hashes nothing; the two plays must still agree).
    add_test(NAME sample_kit3d_tour_underwater_headless
             COMMAND ${CMAKE_COMMAND} -DMCE_PLAYER=$<TARGET_FILE:mce_player> -DPACK=${pack} -DSCENE=scenes/tour_underwater.mcscene
                     -DTICKS=300 -DENTITIES=8 -DTOUR=kit3d_tour_underwater -P ${CMAKE_CURRENT_SOURCE_DIR}/../tour_test.cmake)
    # The sun shafts scene (M-SUNSHAFTS 45e): scenes/tour_shafts.mcscene, a sun 4 degrees above the horizon ahead of the camera, a row of
    # 9 tall trunks and a two-part wall with a gap between them, light exp2 fog and a SunShafts3D on the world entity; made by
    # gen_shafts.py. Named tour_shafts so it sorts after scenes/tour.mcscene, so the default scene stays the tour. 14 entities. 300 ticks
    # (5 s) like the haze (the look is render-side and hashes nothing; the two plays must still agree).
    add_test(NAME sample_kit3d_tour_shafts_headless
             COMMAND ${CMAKE_COMMAND} -DMCE_PLAYER=$<TARGET_FILE:mce_player> -DPACK=${pack} -DSCENE=scenes/tour_shafts.mcscene
                     -DTICKS=300 -DENTITIES=14 -DTOUR=kit3d_tour_shafts -P ${CMAKE_CURRENT_SOURCE_DIR}/../tour_test.cmake)
    # The cloud shadow scene (M-CLOUDSHADOW 51e): scenes/tour_cloudshadow.mcscene, a mid-high sun over a wide flat terrain (field_wide, 600 m), a Weather + CloudLayer
    # (half the sky covered, 60 m cells, a fast drift), a CloudShadow3D on the world entity and a row of buildings and trees the patches cross; made
    # by gen_cloudshadow.py. Named tour_cloudshadow so it sorts after scenes/tour.mcscene, so the default scene stays the tour. 14 entities.
    # 300 ticks (5 s) like the shafts (the look is render-side and hashes nothing; the two plays must still agree).
    add_test(NAME sample_kit3d_tour_cloudshadow_headless
             COMMAND ${CMAKE_COMMAND} -DMCE_PLAYER=$<TARGET_FILE:mce_player> -DPACK=${pack} -DSCENE=scenes/tour_cloudshadow.mcscene
                     -DTICKS=300 -DENTITIES=14 -DTOUR=kit3d_tour_cloudshadow -P ${CMAKE_CURRENT_SOURCE_DIR}/../tour_test.cmake)
    # The dynamic resolution scene (M-DYNRES 46e): scenes/tour_dynres.mcscene, a heavy-looking set (two rings of 12 shadow-casting spheres,
    # 8 coloured point lights, a shadowed sun, volumetric fog, bloom) and a DynamicResolution3D with target_ms 12 on the world entity;
    # made by gen_dynres.py. Named tour_dynres so it sorts after scenes/tour.mcscene, so the default scene stays the tour. 35 entities.
    # 300 ticks (5 s) like the haze: the controller is render-side and hashes nothing, so the two plays must agree (and in a headless
    # play there is no renderer at all).
    add_test(NAME sample_kit3d_tour_dynres_headless
             COMMAND ${CMAKE_COMMAND} -DMCE_PLAYER=$<TARGET_FILE:mce_player> -DPACK=${pack} -DSCENE=scenes/tour_dynres.mcscene
                     -DTICKS=300 -DENTITIES=35 -DTOUR=kit3d_tour_dynres -P ${CMAKE_CURRENT_SOURCE_DIR}/../tour_test.cmake)
    # The reflections scene (M-REFLECT2 48f): scenes/tour_reflect.mcscene, a ladder of five floor strips (roughness 0.0 .. 0.7) with a
    # coloured pillar behind each, a normal-mapped strip, a mirror strip with a sphere that a looping TimelinePlayer slides over it,
    # and an Environment3D with ssr on and ssr_max_roughness 0.8; made by gen_reflect.py (box.glb, a 64 x 64 normal map, the
    # reflect_* materials and timelines/reflect_slide.mctimeline too). Named tour_reflect so it sorts after scenes/tour.mcscene, so the
    # default scene stays the tour. 17 entities. 300 ticks (5 s): the sphere has slid across the strip (its transform is hashed), the
    # SSR itself is render-side and hashes nothing; the two plays must agree.
    add_test(NAME sample_kit3d_tour_reflect_headless
             COMMAND ${CMAKE_COMMAND} -DMCE_PLAYER=$<TARGET_FILE:mce_player> -DPACK=${pack} -DSCENE=scenes/tour_reflect.mcscene
                     -DTICKS=300 -DENTITIES=17 -DTOUR=kit3d_tour_reflect -P ${CMAKE_CURRENT_SOURCE_DIR}/../tour_test.cmake)
    # The river scene (M-WATER2 49h): scenes/tour_river.mcscene, a channel between two banks with a Water (amplitude 0.08, wavelength
    # 3) and a WaterFlow of 1.2 m/s down the channel, coloured pillars and an arch standing in the water (the reflections) and a
    # striped seabed stepping from 0.5 to 2.5 m deep with stones on it (the refraction and the absorption); made by gen_river.py
    # (no new assets: box.glb and the reflect_ball material of gen_reflect.py). Named tour_river so it sorts after scenes/tour.mcscene,
    # so the default scene stays the tour. 43 entities. 300 ticks (5 s) like the haze: the current is a pure function of the scene
    # time and hashes nothing that moves, so the two plays must agree.
    add_test(NAME sample_kit3d_tour_river_headless
             COMMAND ${CMAKE_COMMAND} -DMCE_PLAYER=$<TARGET_FILE:mce_player> -DPACK=${pack} -DSCENE=scenes/tour_river.mcscene
                     -DTICKS=300 -DENTITIES=43 -DTOUR=kit3d_tour_river -P ${CMAKE_CURRENT_SOURCE_DIR}/../tour_test.cmake)
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

if(MCE_BUILD_TESTS AND MCE_GPU_TESTS)
    # The lens rain capture: the scene at tick 300, written for a look (build/.../kit3d_tour_lensrain.ppm; samples keep no goldens). Drops
    # lie on the lens over the lit spheres and lamps; `-r.lens_rain 0` draws the dry lens.
    add_test(NAME sample_kit3d_tour_lensrain_gpu
             COMMAND mce_player -pack "${pack}" -scene scenes/tour_lensrain.mcscene -capture_tick 300
                     -capture "${CMAKE_CURRENT_BINARY_DIR}/kit3d_tour_lensrain.ppm" -platform.error_box 0 -r.vsync 0
                     -sim.fast_forward 1 -sim.max_catchup 32 -pause_on_focus_loss 0)
    set_tests_properties(sample_kit3d_tour_lensrain_gpu PROPERTIES LABELS gpu TIMEOUT 120)
    mce_mark_slow(sample_kit3d_tour_lensrain_gpu)
endif()

if(MCE_BUILD_TESTS AND MCE_GPU_TESTS)
    # The heat haze capture: the scene at tick 300, written for a look (build/.../kit3d_tour_haze.ppm; samples keep no goldens). The
    # grid behind the fire wobbles, the pole in front stays sharp; `-r.heat_haze 0` draws it without the haze.
    add_test(NAME sample_kit3d_tour_haze_gpu
             COMMAND mce_player -pack "${pack}" -scene scenes/tour_haze.mcscene -capture_tick 300
                     -capture "${CMAKE_CURRENT_BINARY_DIR}/kit3d_tour_haze.ppm" -platform.error_box 0 -r.vsync 0
                     -sim.fast_forward 1 -sim.max_catchup 32 -pause_on_focus_loss 0)
    set_tests_properties(sample_kit3d_tour_haze_gpu PROPERTIES LABELS gpu TIMEOUT 120)
    mce_mark_slow(sample_kit3d_tour_haze_gpu)
endif()

if(MCE_BUILD_TESTS AND MCE_GPU_TESTS)
    # The underwater capture: the scene at tick 300, written for a look (build/.../kit3d_tour_underwater.ppm; samples keep no goldens).
    # The spheres fade red first into the blue-green fog, the surface is a lid overhead; `-r.underwater 0` draws it without the look.
    add_test(NAME sample_kit3d_tour_underwater_gpu
             COMMAND mce_player -pack "${pack}" -scene scenes/tour_underwater.mcscene -capture_tick 300
                     -capture "${CMAKE_CURRENT_BINARY_DIR}/kit3d_tour_underwater.ppm" -platform.error_box 0 -r.vsync 0
                     -sim.fast_forward 1 -sim.max_catchup 32 -pause_on_focus_loss 0)
    set_tests_properties(sample_kit3d_tour_underwater_gpu PROPERTIES LABELS gpu TIMEOUT 120)
    mce_mark_slow(sample_kit3d_tour_underwater_gpu)
endif()

if(MCE_BUILD_TESTS AND MCE_GPU_TESTS)
    # The sun shafts capture: the scene at tick 300, written for a look (build/.../kit3d_tour_shafts.ppm; samples keep no goldens).
    # Rays fan out from the low sun through the gaps between the trunks and the wall; `-r.sun_shafts 0` draws it without them.
    add_test(NAME sample_kit3d_tour_shafts_gpu
             COMMAND mce_player -pack "${pack}" -scene scenes/tour_shafts.mcscene -capture_tick 300
                     -capture "${CMAKE_CURRENT_BINARY_DIR}/kit3d_tour_shafts.ppm" -platform.error_box 0 -r.vsync 0
                     -sim.fast_forward 1 -sim.max_catchup 32 -pause_on_focus_loss 0)
    set_tests_properties(sample_kit3d_tour_shafts_gpu PROPERTIES LABELS gpu TIMEOUT 120)
    mce_mark_slow(sample_kit3d_tour_shafts_gpu)
endif()

if(MCE_BUILD_TESTS AND MCE_GPU_TESTS)
    # The cloud shadow capture: the scene at tick 300, written for a look (build/.../kit3d_tour_cloudshadow.ppm; samples keep no goldens).
    # Patches of shade drift over the ground and the buildings, the sky stays as it is; `-r.cloud_shadows 0` draws it without them.
    add_test(NAME sample_kit3d_tour_cloudshadow_gpu
             COMMAND mce_player -pack "${pack}" -scene scenes/tour_cloudshadow.mcscene -capture_tick 300
                     -capture "${CMAKE_CURRENT_BINARY_DIR}/kit3d_tour_cloudshadow.ppm" -platform.error_box 0 -r.vsync 0
                     -sim.fast_forward 1 -sim.max_catchup 32 -pause_on_focus_loss 0)
    set_tests_properties(sample_kit3d_tour_cloudshadow_gpu PROPERTIES LABELS gpu TIMEOUT 120)
    mce_mark_slow(sample_kit3d_tour_cloudshadow_gpu)
endif()

if(MCE_BUILD_TESTS AND MCE_GPU_TESTS)
    # The dynamic resolution capture: the scene's frame 660 with a fixed injected cost (24 ms at scale 1 from frame 0, 8 ms from frame
    # 240; the recovery steps are 60 frames apart, so the scale is back at 1.0 near frame 600), written for a look
    # (build/.../kit3d_tour_dynres.ppm; samples keep no goldens). The log must show the scale stepping down in 1/16 steps after
    # frame 0 and returning to 1.0000 after frame 240 (PASS_REGULAR_EXPRESSION on the `dynres: scale` lines). A capture
    # run holds dynres off unless a valid inject schedule is given (the player's rule), so the inject cvar is what turns it on here;
    # `-r.dynres 0` or the same command without the cvar draws the frame at the tier's fixed scale.
    add_test(NAME sample_kit3d_tour_dynres_gpu
             COMMAND mce_player -pack "${pack}" -scene scenes/tour_dynres.mcscene -frames 660
                     -capture "${CMAKE_CURRENT_BINARY_DIR}/kit3d_tour_dynres.ppm" -platform.error_box 0 -r.vsync 0
                     -pause_on_focus_loss 0 "-r.dynres.inject" "24@0,8@240")
    set_tests_properties(sample_kit3d_tour_dynres_gpu PROPERTIES LABELS gpu TIMEOUT 300
        PASS_REGULAR_EXPRESSION "dynres: scale 1\\.0000 -> 0\\.[0-9]+.*dynres: scale 0\\.[0-9]+ -> 1\\.0000")
    mce_mark_slow(sample_kit3d_tour_dynres_gpu)
endif()

if(MCE_BUILD_TESTS AND MCE_GPU_TESTS)
    # The reflections capture: the scene's frame 120 on the High tier (TAA on, so r.ssr.motion matters), written for a look
    # (build/.../kit3d_tour_reflect.ppm; samples keep no goldens). Pass: the 3D path starts at the High tier, the scene loads with its
    # 17 entities and the capture is written (the log lines; the frame itself is looked at, the render3d_gl tests pin the pixels).
    # The strips left to right go from a sharp mirror to a soft smear, the bumpy strip shows a wavy reflection and the sphere's
    # reflection stays under it; `-r.ssr.blur 0`, `-r.ssr.motion 0` or `-r.ssr 0` on the same command draw the frame without each.
    # About 30 s on llvmpipe, under the 60 s smoke limit, so there is no separate smoke variant.
    add_test(NAME sample_kit3d_tour_reflect_gpu
             COMMAND mce_player -pack "${pack}" -scene scenes/tour_reflect.mcscene -frames 120 -r.quality high
                     -capture "${CMAKE_CURRENT_BINARY_DIR}/kit3d_tour_reflect.ppm" -platform.error_box 0 -r.vsync 0
                     -pause_on_focus_loss 0)
    set_tests_properties(sample_kit3d_tour_reflect_gpu PROPERTIES LABELS gpu TIMEOUT 300
        PASS_REGULAR_EXPRESSION "3D path on \\(high quality.*with 17 entities.*wrote [^\n]*kit3d_tour_reflect\\.ppm")
    mce_mark_slow(sample_kit3d_tour_reflect_gpu)
endif()

if(MCE_BUILD_TESTS AND MCE_GPU_TESTS)
    # The river capture: the scene's frame 120 on the High tier, written for a look (build/.../kit3d_tour_river.ppm; samples keep
    # no goldens). Pass: the 3D path starts at the High tier, the scene loads with its 43 entities and the capture is written (the
    # log lines; the frame itself is looked at, the render3d_gl water optics and water tests pin the pixels). The pillars and the
    # arch are mirrored in the water and broken by the waves, the seabed stripes show through it, bent, and lose their red as it
    # deepens, the ripples drift down the channel; `-r.water.reflect 0` or `-r.water.refract 0` on the same command draw the frame
    # without each effect.
    add_test(NAME sample_kit3d_tour_river_gpu
             COMMAND mce_player -pack "${pack}" -scene scenes/tour_river.mcscene -frames 120 -r.quality high
                     -capture "${CMAKE_CURRENT_BINARY_DIR}/kit3d_tour_river.ppm" -platform.error_box 0 -r.vsync 0
                     -pause_on_focus_loss 0)
    set_tests_properties(sample_kit3d_tour_river_gpu PROPERTIES LABELS gpu TIMEOUT 300
        PASS_REGULAR_EXPRESSION "3D path on \\(high quality.*with 43 entities.*wrote [^\n]*kit3d_tour_river\\.ppm")
    mce_mark_slow(sample_kit3d_tour_river_gpu)
endif()

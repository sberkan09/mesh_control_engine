# skel2d_demo: M-ANIM2D's demo, a data-only project the stock mce_player plays. A cut-out courier
# (assets/skeletons/courier.mcskel2d: 15 bones, 13 slots on one atlas, a weighted cape mesh with
# deform keys, a face attachment swap, two IK constraints) idles, walks and jumps under a .mcanim
# state graph that a timeline drives through the animator's parameters. Its feet stay planted on
# IKTarget2D entities while it idles and land on them after the jump; a lantern follows a hand bone.
# The art comes from gen_art.py.
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
    COMMENT "Cooking skel2d_demo content"
    VERBATIM)
add_custom_target(skel2d_demo_content ALL DEPENDS "${pack}")
add_dependencies(skel2d_demo_content mce_player)

if(MCE_BUILD_TESTS)
    # Headless: the pack plays 900 ticks (one and a half laps of the patrol) with no error, the same hash twice.
    add_test(NAME skel2d_demo_player
             COMMAND ${CMAKE_COMMAND} -DMCE_PLAYER=$<TARGET_FILE:mce_player> -DPACK=${pack} -DTICKS=900 -DENTITIES=15
                     -DTOUR=skel2d_demo -P ${CMAKE_CURRENT_SOURCE_DIR}/../tour_test.cmake)
    if(MCE_GPU_TESTS)
        add_test(NAME skel2d_demo_player_gpu
                 COMMAND mce_player -pack "${pack}" -frames 240 -capture "${CMAKE_CURRENT_BINARY_DIR}/skel2d_demo.ppm"
                         -platform.error_box 0 -r.vsync 0)
        set_tests_properties(skel2d_demo_player_gpu PROPERTIES LABELS gpu)
    endif()
endif()

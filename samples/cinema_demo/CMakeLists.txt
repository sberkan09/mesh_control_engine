# cinema_demo: M-CINEMA's demo, a data-only project the stock mce_player plays. The intro scene
# opens on a studio splash film (assets/video/splash.mpg, a timeline video track over a fullscreen
# kit::VideoPlayer), then a cutscene (assets/cutscenes/gate_intro.mctimeline under a kit::Cutscene):
# a dolly camera on a CameraRail3D with a look-at, lamps fading up, a gate rising, music, subtitles,
# a blended cut to a second camera and a courier walking through. tools/gen_content.py makes the
# music and the film.
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
    COMMENT "Cooking cinema_demo content"
    VERBATIM)
add_custom_target(cinema_demo_content ALL DEPENDS "${pack}")
add_dependencies(cinema_demo_content mce_player)

if(MCE_BUILD_TESTS)
    # Headless: the cutscene plays to its end the same way twice, a held skip changes the run, and
    # the skip replayed from the same input reaches the same state hash.
    add_test(NAME cinema_demo_player
             COMMAND ${CMAKE_COMMAND} -DMCE_PLAYER=$<TARGET_FILE:mce_player> -DPACK=${pack}
                     -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/player_test -P ${CMAKE_CURRENT_SOURCE_DIR}/player_test.cmake)
    if(MCE_GPU_TESTS)
        # Windowed: the splash film, the dolly and the cut drawn, with the GL debug layer clean.
        add_test(NAME cinema_demo_player_gpu
                 COMMAND mce_player -pack "${pack}" -frames 600 -platform.error_box 0 -r.vsync 0)
        set_tests_properties(cinema_demo_player_gpu PROPERTIES LABELS gpu PASS_REGULAR_EXPRESSION "600 frames, [0-9]+ ticks, 0 GL errors")
        # 271 s under llvmpipe (M4): the full 600 frames run only in the integration run (`slow`); --task runs the
        # first 120 frames (the splash film and the dolly's start) with the same GL-error check.
        mce_mark_slow(cinema_demo_player_gpu)
        add_test(NAME cinema_demo_player_gpu_smoke
                 COMMAND mce_player -pack "${pack}" -frames 120 -platform.error_box 0 -r.vsync 0)
        set_tests_properties(cinema_demo_player_gpu_smoke PROPERTIES LABELS gpu PASS_REGULAR_EXPRESSION "120 frames, [0-9]+ ticks, 0 GL errors")
    endif()
endif()

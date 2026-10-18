# 11_daynight: M-DAYNIGHT's demo, a data-only game the stock mce_player plays (assets/ + one Luau script, no C++).
# A small street at dawn: a ground box, three buildings with emissive windows, two lamp posts with a PointLight3D.
# One WorldClock (05:00, a 60 s day) moves the sun, a DayCycle (the default preset) colours the sun, the ambient
# light and the fog from the hour, and TimedLight3D switches the lamps and the windows on at dusk and off at dawn.
# The atmosphere is off, so the sky clears to the fog colour. assets/scripts/daynight.luau skips to noon on Jump
# (clock.set(12)) and logs the clock. The meshes are 10_missions' two boxes (same bytes), scaled; the window glow
# is a native .mcmat. input.txt is the scripted run.
# `mce_player -pack <build>/samples/11_daynight/project/assets.mce -input samples/11_daynight/input.txt`
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
    COMMENT "Cooking 11_daynight content"
    VERBATIM)
add_custom_target(daynight_sample_content ALL DEPENDS "${pack}")
add_dependencies(daynight_sample_content mce_player)

if(MCE_BUILD_TESTS)
    # The numbers (sun, lamps, windows, midnight, state hash) are tests/script/test_daynight_demo.cpp; this is the
    # player run of the cooked pack with the sample's script and input.
    add_test(NAME sample_daynight_headless_600
             COMMAND ${CMAKE_COMMAND} -DMCE_PLAYER=$<TARGET_FILE:mce_player> -DPACK=${pack}
                     -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/input.txt -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/headless
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/daynight_test.cmake)
    if(MCE_GPU_TESTS)
        # Windowed smoke: one frame at 06:00 (tick 150 at the 05:00 start, 2.5 s of real time). The simulation runs in real
        # time, and longer captures (12:00, 19:00, 23:00) stalled under a loaded --task and --premerge run, so the
        # dusk numbers are pinned by test_daynight_demo (headless) and not rendered here. No goldens: the frame is
        # written for a look (build/.../daynight_150.ppm).
        add_test(NAME sample_daynight_gpu
                 COMMAND mce_player -pack "${pack}" -capture_tick 150
                         -capture "${CMAKE_CURRENT_BINARY_DIR}/daynight_150.ppm" -platform.error_box 0 -r.vsync 0
                         -sim.fast_forward 1 -sim.max_catchup 32 -pause_on_focus_loss 0)
        set_tests_properties(sample_daynight_gpu PROPERTIES LABELS gpu TIMEOUT 120)
        # Stalled past its timeout in other threads' loaded --task runs although it takes 4 s alone (cause not found,
        # see M-GPU-VERIFY.md): it runs in the integration run only (`slow`); --task has the headless gates.
        mce_mark_slow(sample_daynight_gpu)
    endif()
endif()

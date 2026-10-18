# 12_weather: M-WEATHER's demo, a data-only game the stock mce_player plays (assets/ + one Luau script, no C++).
# A small street under a changing sky: a ground box, three buildings, a camera. One WorldClock (noon, a 600 s day)
# with a default DayCycle gives the base light; a Weather (10 s transitions) modulates sun, ambient, exposure and fog
# from it; a Precipitation3D on a ParticleEmitter3D makes rain and snow around the camera; a Lightning flashes in
# storms. assets/scripts/weather.luau is the forecast (weather.set at ticks 60, 600, 1500, 2100) and logs the
# weather every 150 ticks. The meshes are 10_missions' two boxes (same bytes), scaled. input.txt is empty.
# `mce_player -pack <build>/samples/12_weather/project/assets.mce -input samples/12_weather/input.txt`
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
    COMMENT "Cooking 12_weather content"
    VERBATIM)
add_custom_target(weather_sample_content ALL DEPENDS "${pack}")
add_dependencies(weather_sample_content mce_player)

if(MCE_BUILD_TESTS)
    # The numbers (sun, rain and snow rates, lightning, hash) are tests/script/test_weather_demo.cpp; this is the
    # player run of the cooked pack with the sample's script and input.
    add_test(NAME sample_weather_headless_600
             COMMAND ${CMAKE_COMMAND} -DMCE_PLAYER=$<TARGET_FILE:mce_player> -DPACK=${pack}
                     -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/input.txt -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/headless
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/weather_test.cmake)
    if(MCE_GPU_TESTS)
        # Windowed smoke, ONE frame in the storm: tick 1500 (storm at 0.6 intensity: the 10 s transitions make rain fade
        # out and the storm in, 1140..1500, so a storm at tick 700 does not exist). The simulation runs in real time
        # (25 s), and longer captures stalled under a loaded --task and --premerge run, so this is the only frame and the
        # numbers are pinned by test_weather_demo (headless). No goldens: the frame is written for a look
        # (build/.../weather_1500.ppm).
        add_test(NAME sample_weather_gpu
                 COMMAND mce_player -pack "${pack}" -capture_tick 1500
                         -capture "${CMAKE_CURRENT_BINARY_DIR}/weather_1500.ppm" -platform.error_box 0 -r.vsync 0)
        set_tests_properties(sample_weather_gpu PROPERTIES LABELS gpu TIMEOUT 120)
        # Same as sample_daynight_gpu: windowed GPU captures stalled past their timeout in other threads' loaded --task
        # runs although they take seconds alone (cause not found, see M-GPU-VERIFY.md): it runs in the integration run
        # only (`slow`); --task has the headless gates.
        mce_mark_slow(sample_weather_gpu)
    endif()
endif()

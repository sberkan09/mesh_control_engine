# 13_water: M-WATER's demo (step 15d), a data-only game the stock mce_player plays (assets/ + one Luau script, no C++).
# A sloped sand island rises out of a sea: a seabed box, a thick box tilted 8 degrees so its top crosses the water level
# about 28 m east of the origin (a shore for the depth tint and the foam), a fixed sun and sky (Environment3D; the WorldClock has drive_environment 0),
# one default Water (level 0, four Gerstner waves, 0.4 m) and a crate. assets/scripts/water.luau puts the crate on
# water.height_at(x, z) every tick, tilts it to water.normal_at and logs `water:` every 150 ticks. 12_weather is untouched
# (its hashes and goldens do not move); M-FREEZE 20c adds a WaterIce, a Season and a WorldClock (2 s day) to the Water entity and an `ice:` log; the meshes are 10_missions' two boxes (same bytes), scaled. input.txt is empty.
# `mce_player -pack <build>/samples/13_water/project/assets.mce -input samples/13_water/input.txt`
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
    COMMENT "Cooking 13_water content"
    VERBATIM)
add_custom_target(water_sample_content ALL DEPENDS "${pack}")
add_dependencies(water_sample_content mce_player)

if(MCE_BUILD_TESTS)
    # The height and normal helpers against the Luau table are tests/script/test_script_water.cpp; this is the player run
    # of the cooked pack with the sample's script and input.
    add_test(NAME sample_water_headless_600
             COMMAND ${CMAKE_COMMAND} -DMCE_PLAYER=$<TARGET_FILE:mce_player> -DPACK=${pack}
                     -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/input.txt -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/headless
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/water_test.cmake)
    if(MCE_GPU_TESTS)
        # Windowed smoke, ONE frame at tick 300 (5 s of real time): the frame is written for a look
        # (build/.../water_300.ppm), no goldens, like sample_daynight_gpu and sample_weather_gpu.
        add_test(NAME sample_water_gpu
                 COMMAND mce_player -pack "${pack}" -capture_tick 300
                         -capture "${CMAKE_CURRENT_BINARY_DIR}/water_300.ppm" -platform.error_box 0 -r.vsync 0
                         -sim.fast_forward 1 -sim.max_catchup 32 -pause_on_focus_loss 0)
        set_tests_properties(sample_water_gpu PROPERTIES LABELS gpu TIMEOUT 120)
        # Windowed GPU captures stall under loaded --task runs (see sample_daynight_gpu, M-GPU-VERIFY.md): the integration
        # run executes it (`slow`); --task has the headless gates.
        mce_mark_slow(sample_water_gpu)
    endif()
endif()

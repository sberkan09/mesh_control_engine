# 14_seasons: M-SEASONTOUR's demo (step 22b), a data-only game the stock mce_player plays (assets/ + one Luau script, no C++).
# One whole year in 60 s (3600 ticks at 60 Hz): a WorldClock with a 2 s day and a 30-day Season (drive_environment 0, the sun is
# fixed) on the Lake entity, which also carries a Water and a WaterIce; a Weather (clear) with a WindDirection, a WindSway, a
# SnowCover and a LeafFall with manual 0 (the leaves fall only because the season reaches autumn); five trunk boxes under
# Foliage3D canopies, which the Season tints and thins; a ground of four boxes around the lake pit. assets/scripts/seasons.luau
# orbits the camera once per year and logs `season:` every 150 ticks. The meshes are 10_missions' two boxes (same bytes).
# 12_weather and 13_water are untouched. input.txt is empty.
# `mce_player -pack <build>/samples/14_seasons/project/assets.mce -input samples/14_seasons/input.txt`
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
    COMMENT "Cooking 14_seasons content"
    VERBATIM)
add_custom_target(seasons_sample_content ALL DEPENDS "${pack}")
add_dependencies(seasons_sample_content mce_player)

if(MCE_BUILD_TESTS)
    # The player run of the cooked pack with the sample's script and input: the whole year, 3600 ticks.
    add_test(NAME sample_seasons_headless_3600
             COMMAND ${CMAKE_COMMAND} -DMCE_PLAYER=$<TARGET_FILE:mce_player> -DPACK=${pack}
                     -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/input.txt -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/headless
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/seasons_test.cmake)
    if(MCE_GPU_TESTS)
        # Windowed smoke, ONE frame each at the autumn and winter centres (37.5 s and 52.5 s of real time): the frames are
        # written for a look (build/.../seasons_2250.ppm, seasons_3150.ppm), no goldens, like sample_water_gpu.
        foreach(tick 2250 3150)
            if(tick EQUAL 2250)
                set(name autumn)
            else()
                set(name winter)
            endif()
            add_test(NAME sample_seasons_gpu_${name}
                     COMMAND mce_player -pack "${pack}" -capture_tick ${tick}
                             -capture "${CMAKE_CURRENT_BINARY_DIR}/seasons_${tick}.ppm" -platform.error_box 0 -r.vsync 0
                             -sim.fast_forward 1 -sim.max_catchup 32 -pause_on_focus_loss 0)
            set_tests_properties(sample_seasons_gpu_${name} PROPERTIES LABELS gpu TIMEOUT 120)
            # Windowed GPU captures stall under loaded --task runs: the integration run executes them (`slow`).
            mce_mark_slow(sample_seasons_gpu_${name})
        endforeach()
    endif()
endif()

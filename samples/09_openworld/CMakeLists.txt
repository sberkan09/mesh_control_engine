# 09_openworld: M-OPENWORLD E1, a 1 km2 city slice that is data only (assets/ + scenes/city.mcscene), played by the stock mce_player.
# A 5 x 5 junction grid (assets/roads/city.mcroads: two avenues, traffic lights at the inner junctions), boxes for buildings and parks,
# traffic (TrafficSpawner3D) and a pedestrian crowd (CrowdSpawner3D) gathering around the player, a parked car to take (VehicleSeat3D
# + Rider3D), a hitscan gun on the orbit camera (Weapon3D), and police (Police3D) who answer the player's wanted level (Wanted3D).
# gen_assets.py rebuilds assets/ (python3 gen_assets.py). WASD walk or drive, mouse looks, left mouse fires, E gets in or out,
# Shift sprints, R reloads. drive.input is the headless script: walk to the car, drive west along the avenue, get out.
# `mce_player -pack <build>/samples/09_openworld/project/assets.mce`
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
    COMMENT "Cooking 09_openworld content"
    VERBATIM)
add_custom_target(openworld_content ALL DEPENDS "${pack}")
add_dependencies(openworld_content mce_player)

if(MCE_BUILD_TESTS)
    # Headless: the city plays the drive script twice to the same hash (the state hash covers roads, traffic, crowd, wanted).
    # The entity count is not fixed (spawners add and remove), so this test only checks the run and the replay.
    add_test(NAME openworld_sample_player
             COMMAND ${CMAKE_COMMAND} -DMCE_PLAYER=$<TARGET_FILE:mce_player> -DPACK=${pack} -DTICKS=900
                     -DSCENE=scenes/city.mcscene -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/drive.input -DTOUR=09_openworld
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/openworld_test.cmake)
    if(MCE_GPU_TESTS)
        add_test(NAME openworld_sample_player_gpu
                 COMMAND mce_player -pack "${pack}" -scene scenes/city.mcscene -input ${CMAKE_CURRENT_SOURCE_DIR}/drive.input
                         -frames 180 -capture "${CMAKE_CURRENT_BINARY_DIR}/openworld.ppm" -platform.error_box 0 -r.vsync 0)
        set_tests_properties(openworld_sample_player_gpu PROPERTIES LABELS gpu)
    endif()
endif()

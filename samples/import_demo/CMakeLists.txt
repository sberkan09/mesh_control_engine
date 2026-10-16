# import_demo: M-IMPORT's demo, a data-only project the stock mce_player plays. Its sprite sheet, its
# cave level and its two LDtk levels are not typed: import/ holds an Aseprite, a Tiled and an LDtk
# export and `mce import` (tools/mce_cli/mce_import.py) turned them into assets/sprites/hero.mcsheet,
# assets/levels/cave* and assets/levels/world_*. scenes/main.mcscene plays the imported hero over the
# imported cave ground; the cook proves the other imported scenes valid. gen_art.py rebuilds everything.
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
    COMMENT "Cooking import_demo content"
    VERBATIM)
add_custom_target(import_demo_content ALL DEPENDS "${pack}")
add_dependencies(import_demo_content mce_player)

if(MCE_BUILD_TESTS)
    # Headless: the main scene plays 300 ticks with no error, the same hash twice (3 entities).
    add_test(NAME import_demo_player
             COMMAND ${CMAKE_COMMAND} -DMCE_PLAYER=$<TARGET_FILE:mce_player> -DPACK=${pack} -DTICKS=300 -DENTITIES=3
                     -DSCENE=scenes/main.mcscene -DTOUR=import_demo -P ${CMAKE_CURRENT_SOURCE_DIR}/../tour_test.cmake)
    # The converters give the committed assets again, byte for byte.
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_test(NAME import_demo_reimport
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/check_reimport.py
                         ${CMAKE_CURRENT_BINARY_DIR}/reimport ${PROJECT_SOURCE_DIR}/tools/mce_cli/mce_import.py)
    endif()
    if(MCE_GPU_TESTS)
        add_test(NAME import_demo_player_gpu
                 COMMAND mce_player -pack "${pack}" -scene scenes/main.mcscene -frames 120
                         -capture "${CMAKE_CURRENT_BINARY_DIR}/import_demo.ppm" -platform.error_box 0 -r.vsync 0)
        set_tests_properties(import_demo_player_gpu PROPERTIES LABELS gpu)
    endif()
endif()

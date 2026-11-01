# mce_script (ADR-0041, M-SCRIPT): sandbox, budgets, reflected field access, determinism, the
# CHNK_SCRIPT round trip and scene scripts.
mce_add_test(test_script UMBRELLA
    SOURCES
        test_script.cpp
        test_scene_scripts.cpp
        test_script_api.cpp
        test_script_missions.cpp
        test_script_inventory.cpp
        test_script_clock.cpp
        test_script_weather.cpp
        test_script_water.cpp
        test_script_aurora.cpp
        test_script_meteor.cpp
        test_script_season.cpp
        test_script_water_ice.cpp
        test_script_leaf_fall.cpp
        test_script_firefly.cpp
        test_script_pollen.cpp
        test_script_dust.cpp
        test_script_flock.cpp
        test_script_rainbow.cpp
        test_daynight_demo.cpp
        test_weather_demo.cpp
        test_script_state.cpp
        test_script_check.cpp
        test_script_crash.cpp
    DEPS
        mce::mce_script_compiler
        mce::mce_scene
        mce::mce_asset_cook)
target_compile_definitions(test_script PRIVATE MCE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp" MCE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_include_directories(test_script PRIVATE ${PROJECT_SOURCE_DIR}/tests)

# The stock player runs a cooked scene's scripts from bytecode (M-SCRIPT 1).
if(TARGET mce_cook AND TARGET mce_player)
    add_test(NAME player_scripts
             COMMAND ${CMAKE_COMMAND} -DMCE_COOK=$<TARGET_FILE:mce_cook> -DMCE_PLAYER=$<TARGET_FILE:mce_player>
                     -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/player_scripts_test
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/player_scripts_test.cmake)
endif()

# The compiler wall in the binaries (M-SCRIPT 1): the player and the engine image carry the VM
# where scripting is on, never the Luau compiler.
if(TARGET mce_player)
    set(_mce_wall_bins $<TARGET_FILE:mce_player>)
    if(TARGET mce_engine)
        list(APPEND _mce_wall_bins $<TARGET_FILE:mce_engine>)
    endif()
    add_test(NAME player_no_luau_compiler
             COMMAND ${CMAKE_COMMAND} "-DBINARIES=${_mce_wall_bins}" -P ${CMAKE_CURRENT_SOURCE_DIR}/no_compiler_check.cmake)
endif()

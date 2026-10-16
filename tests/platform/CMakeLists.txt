# Runs on SDL's "dummy" video driver: no display needed, so this is a plain unit test everywhere.
mce_add_test(test_platform SOURCES test_platform.cpp test_review.cpp test_haptics.cpp test_power.cpp test_screen_reader.cpp test_steam.cpp
             DEPS mce::mce_platform)
target_compile_definitions(test_platform PRIVATE MCE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")

# Stand-ins for Valve's steam_api (M-STEAM steps 1-4): the flat entry points the loader opens, and an old
# library without manual dispatch. Built here only; the Steamworks SDK is not part of the build.
if(NOT IOS AND NOT ANDROID)
    add_library(mce_steam_stub SHARED steam_stub.cpp)
    add_library(mce_steam_stub_old SHARED steam_stub_old.cpp)
    target_compile_definitions(test_platform PRIVATE MCE_STEAM_STUB="$<TARGET_FILE:mce_steam_stub>"
                                                     MCE_STEAM_STUB_OLD="$<TARGET_FILE:mce_steam_stub_old>")
    add_dependencies(test_platform mce_steam_stub mce_steam_stub_old)
    # The stock player opens the stand-in for -steam.app_id and plays scene_demo to the same hash.
    if(TARGET scene_demo_content)
        add_test(NAME scene_demo_player_steam
                 COMMAND mce_player -headless 1 -ticks 600 -pack "${CMAKE_BINARY_DIR}/samples/scene_demo/project/assets.mce"
                         -steam.app_id 480 -steam.library $<TARGET_FILE:mce_steam_stub>)
        set_tests_properties(scene_demo_player_steam PROPERTIES
            PASS_REGULAR_EXPRESSION "Steam on \\(app 480, Stub Player\\).*600 ticks, 6 entities, state_hash [0-9a-f]+")
    endif()
    # Steam as the game services backend (M-STEAM step 2): the player's -game_services_test on the stand-in.
    if(TARGET mce_player AND TARGET mce_cook)
        add_test(NAME player_game_services_steam
                 COMMAND ${CMAKE_COMMAND} -DMCE_COOK=$<TARGET_FILE:mce_cook> -DMCE_PLAYER=$<TARGET_FILE:mce_player>
                         -DSTEAM_STUB=$<TARGET_FILE:mce_steam_stub> -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/steam_games_test
                         -P ${CMAKE_CURRENT_SOURCE_DIR}/player_steam_games_test.cmake)
        # The overlay holding the game and rich presence from the scene (M-STEAM step 3).
        add_test(NAME player_steam_overlay
                 COMMAND ${CMAKE_COMMAND} -DMCE_COOK=$<TARGET_FILE:mce_cook> -DMCE_PLAYER=$<TARGET_FILE:mce_player>
                         -DSTEAM_STUB=$<TARGET_FILE:mce_steam_stub> -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/steam_overlay_test
                         -P ${CMAKE_CURRENT_SOURCE_DIR}/player_steam_overlay_test.cmake)
        # Steam Input reading the input map's actions, and its configuration files (M-STEAM step 4).
        add_test(NAME player_steam_input
                 COMMAND ${CMAKE_COMMAND} -DMCE_COOK=$<TARGET_FILE:mce_cook> -DMCE_PLAYER=$<TARGET_FILE:mce_player>
                         -DSTEAM_STUB=$<TARGET_FILE:mce_steam_stub> -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/steam_input_test
                         -P ${CMAKE_CURRENT_SOURCE_DIR}/player_steam_input_test.cmake)
    endif()
endif()

# The Steam depot and steamcmd build scripts of ship --steam (M-STEAM step 5); on Linux also the player run
# from its depot the way Steam starts it (SteamAppId, steam_api beside the executable).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
if(UNIX AND NOT APPLE AND NOT ANDROID AND TARGET mce_player AND TARGET scene_demo_content)
    if(TARGET mce_engine)
        set(_mce_engine_file $<TARGET_FILE:mce_engine>)
    else()
        set(_mce_engine_file -)
    endif()
    add_test(NAME steam_depot
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_steam_depot.py" "${CMAKE_CURRENT_BINARY_DIR}/steam_depot_test"
                     $<TARGET_FILE:mce_player> ${_mce_engine_file} $<TARGET_FILE:mce_steam_stub>
                     "${CMAKE_BINARY_DIR}/samples/scene_demo/project/assets.mce")
else()
    add_test(NAME steam_depot
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_steam_depot.py" "${CMAKE_CURRENT_BINARY_DIR}/steam_depot_test")
endif()

# The Android sides of the review prompt and haptics (cmake/mobile/android_app.py stamping MceReview and
# MceHaptics, M-MOBILE steps 11c-11d).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME android_bridges COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_android_bridges.py")

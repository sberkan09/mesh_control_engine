# Runs on SDL's "dummy" video driver: no display needed, so this is a plain unit test everywhere.
mce_add_test(test_platform SOURCES test_platform.cpp test_review.cpp test_haptics.cpp test_power.cpp test_screen_reader.cpp test_steam.cpp
             DEPS mce::mce_platform)
target_compile_definitions(test_platform PRIVATE MCE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")

# Stand-ins for Valve's steam_api (M-STEAM step 1): the flat entry points the loader opens, and an old
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
endif()

# The Android sides of the review prompt and haptics (cmake/mobile/android_app.py stamping MceReview and
# MceHaptics, M-MOBILE steps 11c-11d).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME android_bridges COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_android_bridges.py")

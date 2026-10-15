# mce_player (L6): the runtime host for editor-authored scenes — the "game.exe" of the editor's
# run-in-player button (editor-tools.md §2.1, Appendix A4) and of `Save -> Cook -> load` (ROADMAP
# M2 demo). Mounts a cooked pack, instantiates one scene through mce_scene, runs physics2d and the
# audio bridge on the fixed step, and draws the starter-kit components. Esc pauses (the A9 pause
# overlay); headless mode prints the World state hash for the determinism harness.
#
# The runtime itself is mce_player_runtime (mce/player/player.h), which project hosts made from the
# project template link too. Where game modules are supported it links the mce_engine umbrella,
# so a module loaded with -module shares the host's engine; elsewhere it links the libraries.
add_library(mce_player_runtime STATIC src/player.cpp src/ui_screens.cpp)
add_library(mce::mce_player_runtime ALIAS mce_player_runtime)
target_include_directories(mce_player_runtime
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/samples/common")
if(MCE_GAME_MODULES_SUPPORTED)
    target_link_libraries(mce_player_runtime PUBLIC mce::mce_engine)
else()
    target_link_libraries(mce_player_runtime PUBLIC
        mce::mce_scene mce::mce_gameplay_kit mce::mce_physics2d mce::mce_audio mce::mce_render mce::mce_platform
        mce::mce_ui mce::mce_input mce::mce_module_host mce::mce_rhi_default mce::mce_loc)
endif()
mce_apply_compile_policy(mce_player_runtime)

mce_add_executable(mce_player SOURCES main.cpp DEPS mce::mce_player_runtime)

if(MCE_BUILD_TESTS)
    # -help prints the command-line reference (every convar) and exits 0, before any pack is needed.
    add_test(NAME player_help COMMAND mce_player -help)
    set_tests_properties(player_help PROPERTIES PASS_REGULAR_EXPRESSION "usage: mce_player.*-capture_tick <int>.*-pack <string>")
endif()

# Data mods end to end (ADR-0034): mce_cook builds a game pack and a mod pack over it; mce_player
# -headless plays the game with the mod, with -mods 0 and with a load_order.txt that disables it.
if(MCE_BUILD_TESTS AND TARGET mce_cook)
    add_test(NAME player_mods
             COMMAND ${CMAKE_COMMAND} -DMCE_COOK=$<TARGET_FILE:mce_cook> -DMCE_PLAYER=$<TARGET_FILE:mce_player>
                     -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/mods_test
                     -P ${PROJECT_SOURCE_DIR}/tests/asset/mods_player_test.cmake)
endif()

# Packaging v2: -crash_test writes a crash report that scripts/symbolicate.py resolves against the
# build tree (not under ASan, whose own SEGV report takes over; not on Windows, where the report is
# a minidump read by the debugger).
if(MCE_BUILD_TESTS AND UNIX AND NOT MCE_ASAN)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_test(NAME player_crash_report
                 COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/crash_report_check.py
                         $<TARGET_FILE:mce_player> ${PROJECT_BINARY_DIR} ${CMAKE_CURRENT_BINARY_DIR}/crash_report_check)
    endif()
endif()

# Purchases and ads end to end (M-MONETIZE): mce_player -headless with the fake store and ad network
# through every outcome they cover, and the entitlement across a relaunch.
if(MCE_BUILD_TESTS AND TARGET mce_cook)
    add_test(NAME player_monetize
             COMMAND ${CMAKE_COMMAND} -DMCE_COOK=$<TARGET_FILE:mce_cook> -DMCE_PLAYER=$<TARGET_FILE:mce_player>
                     -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/monetize_test
                     -P ${PROJECT_SOURCE_DIR}/tests/monetize/player_monetize_test.cmake)
endif()

# Live services end to end (M-LIVEOPS step 1): crash reports sent on the next launch only with consent,
# through -liveops.provider local. A real -crash_test report where the crash handler runs (as above).
if(MCE_BUILD_TESTS AND TARGET mce_cook)
    if(UNIX AND NOT MCE_ASAN)
        set(_mce_real_crash 1)
    else()
        set(_mce_real_crash 0)
    endif()
    add_test(NAME player_liveops
             COMMAND ${CMAKE_COMMAND} -DMCE_COOK=$<TARGET_FILE:mce_cook> -DMCE_PLAYER=$<TARGET_FILE:mce_player>
                     -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/liveops_test -DREAL_CRASH=${_mce_real_crash}
                     -P ${PROJECT_SOURCE_DIR}/tests/liveops/player_liveops_test.cmake)
    add_test(NAME player_remote_config
             COMMAND ${CMAKE_COMMAND} -DMCE_COOK=$<TARGET_FILE:mce_cook> -DMCE_PLAYER=$<TARGET_FILE:mce_player>
                     -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/remote_config_test
                     -P ${PROJECT_SOURCE_DIR}/tests/liveops/player_remote_config_test.cmake)
    add_test(NAME player_game_services
             COMMAND ${CMAKE_COMMAND} -DMCE_COOK=$<TARGET_FILE:mce_cook> -DMCE_PLAYER=$<TARGET_FILE:mce_player>
                     -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/game_services_test
                     -P ${PROJECT_SOURCE_DIR}/tests/liveops/player_game_services_test.cmake)
endif()

# Performance captures (M-PERF step 1): -perf_capture leaves the state hash alone and writes a .mcperf
# with the frames, phases and systems of the run.
if(MCE_BUILD_TESTS AND TARGET mce_cook)
    add_test(NAME player_perf_capture
             COMMAND ${CMAKE_COMMAND} -DMCE_COOK=$<TARGET_FILE:mce_cook> -DMCE_PLAYER=$<TARGET_FILE:mce_player>
                     -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/perf_test
                     -P ${PROJECT_SOURCE_DIR}/tests/perf/player_perf_test.cmake)
endif()

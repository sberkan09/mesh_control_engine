# Room3D: a project made from the MCE project template (scripts/new_project.py, assets.md §4.4).
#
#   assets/                 what you edit (the editor opens this project); cooked into assets.mce at build
#   games/room3d/module/    the game module libroom3d: components (components.h), systems (room3d.cpp)
#   games/room3d/host/      the thin host executable room3d
#
# Two ways to build it:
#   - against an installed engine (the SDK): `mce build`, or cmake -S . -B build
#     -DCMAKE_PREFIX_PATH=<sdk prefix> and cmake --build build; find_package(MCE) below brings the
#     engine, the helpers and the tools (`mce editor`, `mce mcp`, `mce cook`, `mce ship`).
#   - inside the engine tree: the engine's CMake adds every projects/<name>/ directory, so
#     `cmake --build --preset <preset> --target room3d` builds the host, the module and the pack
#     and python3 scripts/ship.py --project projects/Room3D ships it.
cmake_minimum_required(VERSION 3.28)
if(NOT COMMAND mce_add_project)
    project(Room3D LANGUAGES C CXX)
    find_package(MCE REQUIRED)
endif()

mce_add_project(room3d
    TITLE "Room 3D"
    GAME_SOURCES games/room3d/module/room3d.cpp
    MODULE_ENTRY games/room3d/module/module.cpp
    TYPES games/room3d/module/types.cpp
    HOST games/room3d/host/main.cpp
    INCLUDE_DIRS games/room3d/module)
# Multiplayer is opt-in: add `DEPS mce::mce_net` to the call above to link the networking module
# (client-server over UDP, snapshot replication; the engine's docs/engine/net.md).

# Room3D's own test: the cooked scene played through the game's systems on the kit's 3D layer with
# scripted input (tests/test_room3d.cpp). Engine tree only, as Kindle's: against the SDK the static
# engine libraries it links are not installed (the host plays it instead).
if(MCE_BUILD_TESTS AND NOT MCE_SDK)
    mce_add_test(room3d_play SOURCES tests/test_room3d.cpp games/room3d/module/room3d.cpp
        DEPS mce::mce_ecs mce::mce_ecs_scene mce::mce_scene mce::mce_asset mce::mce_gameplay_kit mce::mce_physics3d
             mce::mce_input mce::mce_module_host)
    target_include_directories(room3d_play PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/games/room3d/module")
    target_compile_definitions(room3d_play PRIVATE ROOM3D_PACK="${CMAKE_CURRENT_BINARY_DIR}/project/assets.mce")
    add_dependencies(room3d_play room3d_content)

    # -pause_on_focus_loss 0: under ctest -j the GPU tests share one X display, and another test's
    # window taking focus would pause the sim before the capture tick.
    # The stock player's 3D path end to end: scripted input to a tick, a capture, no GL errors.
    if(MCE_GPU_TESTS)
        add_test(NAME room3d_walk_gpu
                 COMMAND room3d -capture_tick 200 -input "${CMAKE_CURRENT_SOURCE_DIR}/tests/walk.input"
                         -capture "${CMAKE_CURRENT_BINARY_DIR}/room3d_walk.ppm" -r.stats 1
                         -platform.error_box 0 -r.vsync 0 -audio.enabled 0 -pause_on_focus_loss 0)
        set_tests_properties(room3d_walk_gpu PROPERTIES LABELS gpu PASS_REGULAR_EXPRESSION "200 ticks, 0 GL errors"
                                                        RESOURCE_LOCK room3d_module)
        # Mesh LOD (#158): the far view draws at least 5x fewer triangles with the rocks' LODs.
        add_test(NAME room3d_lod_gpu COMMAND "${CMAKE_COMMAND}" -DGAME=$<TARGET_FILE:room3d> -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/lod_gate.cmake")
        set_tests_properties(room3d_lod_gpu PROPERTIES LABELS gpu RESOURCE_LOCK room3d_module)
        # The editor shows the 3D scene: the orbit view through the scene's camera, then play-in-editor.
        if(TARGET room3d_editor)
            add_test(NAME room3d_editor_selftest
                     COMMAND room3d_editor -project "${CMAKE_CURRENT_SOURCE_DIR}" -frames 90 -selftest -editor.ini ""
                             -platform.error_box 0 -r.vsync 0)
            add_test(NAME room3d_editor_play_selftest
                     COMMAND room3d_editor -project "${CMAKE_CURRENT_SOURCE_DIR}" -frames 90 -editor.play 1 -selftest
                             -editor.ini "" -platform.error_box 0 -r.vsync 0)
            set_tests_properties(room3d_editor_selftest room3d_editor_play_selftest
                                 PROPERTIES LABELS gpu RESOURCE_LOCK room3d_module PASS_REGULAR_EXPRESSION "0 GL errors")
            set_tests_properties(room3d_editor_selftest PROPERTIES PASS_REGULAR_EXPRESSION "3D drawn")
        endif()
    endif()
    # A save taken mid-walk loads and plays on (Physics3D is rebuilt from the scene and the hero).
    set(save_dir "${CMAKE_CURRENT_BINARY_DIR}/saves")
    add_test(NAME room3d_save_clean COMMAND "${CMAKE_COMMAND}" -E rm -rf "${save_dir}")
    add_test(NAME room3d_save_write
             COMMAND room3d -headless 1 -ticks 200 -input "${CMAKE_CURRENT_SOURCE_DIR}/tests/walk.input" -save walk
                     -save_dir "${save_dir}")
    add_test(NAME room3d_save_resume COMMAND room3d -headless 1 -ticks 100 -save walk -save_dir "${save_dir}")
    set_tests_properties(room3d_save_clean PROPERTIES FIXTURES_SETUP room3d_save_dir)
    set_tests_properties(room3d_save_write PROPERTIES FIXTURES_SETUP room3d_save FIXTURES_REQUIRED room3d_save_dir RESOURCE_LOCK room3d_module
                                                      PASS_REGULAR_EXPRESSION "save 'walk' written \\(tick 200\\)")
    set_tests_properties(room3d_save_resume PROPERTIES FIXTURES_REQUIRED room3d_save RESOURCE_LOCK room3d_module
                                                       PASS_REGULAR_EXPRESSION "save 'walk' loaded .*300 ticks")
endif()

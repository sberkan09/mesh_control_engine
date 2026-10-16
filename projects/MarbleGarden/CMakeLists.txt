# MarbleGarden: a project made from the MCE project template (scripts/new_project.py, assets.md §4.4).
#
#   assets/                 what you edit (the editor opens this project); cooked into assets.mce at build
#   games/marble_garden/module/    the game module libmarble_garden: components (components.h), systems (marble_garden.cpp)
#   games/marble_garden/host/      the thin host executable marble_garden
#
# Two ways to build it:
#   - against an installed engine (the SDK): `mce build`, or cmake -S . -B build
#     -DCMAKE_PREFIX_PATH=<sdk prefix> and cmake --build build; find_package(MCE) below brings the
#     engine, the helpers and the tools (`mce editor`, `mce mcp`, `mce cook`, `mce ship`).
#   - inside the engine tree: the engine's CMake adds every projects/<name>/ directory, so
#     `cmake --build --preset <preset> --target marble_garden` builds the host, the module and the pack
#     and python3 scripts/ship.py --project projects/MarbleGarden ships it.
cmake_minimum_required(VERSION 3.28)
if(NOT COMMAND mce_add_project)
    project(MarbleGarden LANGUAGES C CXX)
    find_package(MCE REQUIRED)
endif()

mce_add_project(marble_garden
    TITLE "Marble Garden"
    GAME_SOURCES games/marble_garden/module/marble_garden.cpp
    MODULE_ENTRY games/marble_garden/module/module.cpp
    TYPES games/marble_garden/module/types.cpp
    HOST games/marble_garden/host/main.cpp
    INCLUDE_DIRS games/marble_garden/module)
# Multiplayer is opt-in: add `DEPS mce::mce_net` to the call above to link the networking module
# (client-server over UDP, snapshot replication; the engine's docs/engine/net.md).

# The M-AGENT demo's gates (docs/research/m-agent-demo/), engine tree only: every .mctest under
# tests/ passes, and each screen in goldens/goldens.json still renders as its golden. Both play a
# copy of the project in the build tree.
if(MCE_BUILD_TESTS AND NOT MCE_SDK AND TARGET marble_garden_mcp)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(demo_gate "${PROJECT_SOURCE_DIR}/scripts/agent_demo/demo_gate.py")
    add_test(NAME marble_garden_mctests
             COMMAND ${Python3_EXECUTABLE} "${demo_gate}" --mcp $<TARGET_FILE:marble_garden_mcp> --project "${CMAKE_CURRENT_SOURCE_DIR}"
                     --work "${CMAKE_CURRENT_BINARY_DIR}/gate_tests" --tests)
    set_tests_properties(marble_garden_mctests PROPERTIES PASS_REGULAR_EXPRESSION "gate: pass" RESOURCE_LOCK marble_garden_module)
    if(MCE_GPU_TESTS)
        add_test(NAME marble_garden_goldens_gpu
                 COMMAND ${Python3_EXECUTABLE} "${demo_gate}" --mcp $<TARGET_FILE:marble_garden_mcp> --project "${CMAKE_CURRENT_SOURCE_DIR}"
                         --work "${CMAKE_CURRENT_BINARY_DIR}/gate_goldens" --goldens)
        set_tests_properties(marble_garden_goldens_gpu PROPERTIES LABELS gpu PASS_REGULAR_EXPRESSION "gate: pass" RESOURCE_LOCK marble_garden_module)
        # M-PERF counter gate: perf/counters.json's draws, triangles, sprites, ... may not grow.
        set(perf_gate "${PROJECT_SOURCE_DIR}/scripts/ci/perf_gate.py")
        add_test(NAME marble_garden_perf_counters
                 COMMAND ${Python3_EXECUTABLE} "${perf_gate}" --mcp $<TARGET_FILE:marble_garden_mcp> --project "${CMAKE_CURRENT_SOURCE_DIR}"
                         --work "${CMAKE_CURRENT_BINARY_DIR}/perf_gate")
        set_tests_properties(marble_garden_perf_counters PROPERTIES LABELS gpu PASS_REGULAR_EXPRESSION "gate: pass" RESOURCE_LOCK marble_garden_module)
    endif()
endif()

# The full win route, scripted (tests/win_route.input): headless, and rendered with captures along
# the way. A capture holds the sim for a frame; the input must still play by sim tick, or the
# rendered run drifts off the route and never wins.
if(MCE_BUILD_TESTS AND NOT MCE_SDK)
    set(garden 5ebadc24-96f4-438c-a342-d6f37fb5ca8c)
    add_test(NAME marble_garden_win_route
             COMMAND marble_garden -headless 1 -ticks 1800 -scene ${garden} -input "${CMAKE_CURRENT_SOURCE_DIR}/tests/win_route.input")
    set_tests_properties(marble_garden_win_route PROPERTIES PASS_REGULAR_EXPRESSION "scene: changed to scenes/win.mcscene"
                                                            RESOURCE_LOCK marble_garden_module)
    if(MCE_GPU_TESTS)
        add_test(NAME marble_garden_win_route_gpu
                 COMMAND marble_garden -window 320x180 -scene ${garden} -input "${CMAKE_CURRENT_SOURCE_DIR}/tests/win_route.input"
                         -capture_ticks 300,600,900,1200,1500,1800 -capture "${CMAKE_CURRENT_BINARY_DIR}/route_{tick}.ppm"
                         -sim.fast_forward 1 -sim.max_catchup 32 -platform.error_box 0 -r.vsync 0 -audio.enabled 0
                         -pause_on_focus_loss 0 -mouse_look 0)
        set_tests_properties(marble_garden_win_route_gpu PROPERTIES LABELS gpu RESOURCE_LOCK marble_garden_module
                                                                PASS_REGULAR_EXPRESSION "scene: changed to scenes/win.mcscene")
    endif()
endif()

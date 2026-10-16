# Glowmoth: a project made from the MCE project template (scripts/new_project.py, assets.md §4.4).
#
#   assets/                 what you edit (the editor opens this project); cooked into assets.mce at build
#   games/glowmoth/module/    the game module libglowmoth: components (components.h), systems (glowmoth.cpp)
#   games/glowmoth/host/      the thin host executable glowmoth
#
# Two ways to build it:
#   - against an installed engine (the SDK): `mce build`, or cmake -S . -B build
#     -DCMAKE_PREFIX_PATH=<sdk prefix> and cmake --build build; find_package(MCE) below brings the
#     engine, the helpers and the tools (`mce editor`, `mce mcp`, `mce cook`, `mce ship`).
#   - inside the engine tree: the engine's CMake adds every projects/<name>/ directory, so
#     `cmake --build --preset <preset> --target glowmoth` builds the host, the module and the pack
#     and python3 scripts/ship.py --project projects/Glowmoth ships it.
cmake_minimum_required(VERSION 3.28)
if(NOT COMMAND mce_add_project)
    project(Glowmoth LANGUAGES C CXX)
    find_package(MCE REQUIRED)
endif()

mce_add_project(glowmoth
    TITLE "Glowmoth"
    GAME_SOURCES games/glowmoth/module/glowmoth.cpp
    MODULE_ENTRY games/glowmoth/module/module.cpp
    TYPES games/glowmoth/module/types.cpp
    HOST games/glowmoth/host/main.cpp
    INCLUDE_DIRS games/glowmoth/module)
# Multiplayer is opt-in: add `DEPS mce::mce_net` to the call above to link the networking module
# (client-server over UDP, snapshot replication; the engine's docs/engine/net.md).

# The M-AGENT demo's gates (docs/research/m-agent-demo/), engine tree only: every .mctest under
# tests/ passes, and each screen in goldens/goldens.json still renders as its golden. Both play a
# copy of the project in the build tree.
if(MCE_BUILD_TESTS AND NOT MCE_SDK AND TARGET glowmoth_mcp)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(demo_gate "${PROJECT_SOURCE_DIR}/scripts/agent_demo/demo_gate.py")
    add_test(NAME glowmoth_mctests
             COMMAND ${Python3_EXECUTABLE} "${demo_gate}" --mcp $<TARGET_FILE:glowmoth_mcp> --project "${CMAKE_CURRENT_SOURCE_DIR}"
                     --work "${CMAKE_CURRENT_BINARY_DIR}/gate_tests" --tests)
    set_tests_properties(glowmoth_mctests PROPERTIES PASS_REGULAR_EXPRESSION "gate: pass" RESOURCE_LOCK glowmoth_module)
    if(MCE_GPU_TESTS)
        add_test(NAME glowmoth_goldens_gpu
                 COMMAND ${Python3_EXECUTABLE} "${demo_gate}" --mcp $<TARGET_FILE:glowmoth_mcp> --project "${CMAKE_CURRENT_SOURCE_DIR}"
                         --work "${CMAKE_CURRENT_BINARY_DIR}/gate_goldens" --goldens)
        set_tests_properties(glowmoth_goldens_gpu PROPERTIES LABELS gpu PASS_REGULAR_EXPRESSION "gate: pass" RESOURCE_LOCK glowmoth_module)
        # M-PERF counter gate: perf/counters.json's draws, triangles, sprites, ... may not grow.
        set(perf_gate "${PROJECT_SOURCE_DIR}/scripts/ci/perf_gate.py")
        add_test(NAME glowmoth_perf_counters
                 COMMAND ${Python3_EXECUTABLE} "${perf_gate}" --mcp $<TARGET_FILE:glowmoth_mcp> --project "${CMAKE_CURRENT_SOURCE_DIR}"
                         --work "${CMAKE_CURRENT_BINARY_DIR}/perf_gate")
        set_tests_properties(glowmoth_perf_counters PROPERTIES LABELS gpu PASS_REGULAR_EXPRESSION "gate: pass" RESOURCE_LOCK glowmoth_module)
        # ... and it catches a planted regression (three sprites with a texture each).
        add_test(NAME glowmoth_perf_counters_seeded
                 COMMAND ${Python3_EXECUTABLE} "${perf_gate}" --mcp $<TARGET_FILE:glowmoth_mcp> --project "${CMAKE_CURRENT_SOURCE_DIR}"
                         --work "${CMAKE_CURRENT_BINARY_DIR}/perf_gate_seeded" --seed)
        set_tests_properties(glowmoth_perf_counters_seeded PROPERTIES LABELS gpu PASS_REGULAR_EXPRESSION "gate: pass" RESOURCE_LOCK glowmoth_module)
    endif()
endif()

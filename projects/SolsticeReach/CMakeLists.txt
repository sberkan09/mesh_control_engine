# SolsticeReach: a project made from the MCE project template (scripts/new_project.py, assets.md §4.4).
#
#   assets/                 what you edit (the editor opens this project); cooked into assets.mce at build
#   games/solstice_reach/module/    the game module libsolstice_reach: components (components.h), the sim
#                                   (solstice_reach.cpp) and its presentation on the 3D kit (present.cpp)
#   games/solstice_reach/host/      the thin host executable solstice_reach (the stock player)
#
# Two ways to build it:
#   - against an installed engine (the SDK): `mce build`, or cmake -S . -B build
#     -DCMAKE_PREFIX_PATH=<sdk prefix> and cmake --build build; find_package(MCE) below brings the
#     engine, the helpers and the tools (`mce editor`, `mce mcp`, `mce cook`, `mce ship`).
#   - inside the engine tree: the engine's CMake adds every projects/<name>/ directory, so
#     `cmake --build --preset <preset> --target solstice_reach` builds the host, the module and the pack
#     and python3 scripts/ship.py --project projects/SolsticeReach ships it.
cmake_minimum_required(VERSION 3.28)
if(NOT COMMAND mce_add_project)
    project(SolsticeReach LANGUAGES C CXX)
    find_package(MCE REQUIRED)
endif()

mce_add_project(solstice_reach
    TITLE "Solstice Reach"
    GAME_SOURCES games/solstice_reach/module/solstice_reach.cpp games/solstice_reach/module/present.cpp
    MODULE_ENTRY games/solstice_reach/module/module.cpp
    TYPES games/solstice_reach/module/types.cpp
    HOST games/solstice_reach/host/main.cpp
    INCLUDE_DIRS games/solstice_reach/module)

# The game's own test: the terrain port against the generator's reference heights, a scripted run
# through the module (the bot aligns every mirror from the cooked scene), and the determinism hash.
# Engine tree only, as Kindle's test.
if(MCE_BUILD_TESTS AND NOT MCE_SDK)
    mce_add_test(solstice_reach_run SOURCES tests/test_solstice_reach.cpp games/solstice_reach/module/solstice_reach.cpp
        games/solstice_reach/module/present.cpp
        DEPS mce::mce_ecs mce::mce_ecs_scene mce::mce_scene mce::mce_asset mce::mce_gameplay_kit mce::mce_physics3d
             mce::mce_input mce::mce_audio mce::mce_module_host)
    target_include_directories(solstice_reach_run PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/games/solstice_reach/module")
    target_compile_definitions(solstice_reach_run PRIVATE
        SOLSTICE_PACK="${CMAKE_CURRENT_BINARY_DIR}/project/assets.mce"
        SOLSTICE_REF="${CMAKE_CURRENT_SOURCE_DIR}/tools/terrain_ref.txt")
    add_dependencies(solstice_reach_run solstice_reach_content)
    # The stock player draws the island: from the title into a scripted walk, a capture, no GL
    # errors (-pause_on_focus_loss 0: the GPU tests share one X display under ctest -j).
    if(MCE_GPU_TESTS)
        add_test(NAME solstice_reach_walk_gpu
                 COMMAND solstice_reach -capture_tick 200 -input "${CMAKE_CURRENT_SOURCE_DIR}/tests/walk.input"
                         -capture "${CMAKE_CURRENT_BINARY_DIR}/solstice_reach_walk.ppm" -r.stats 1
                         -platform.error_box 0 -r.vsync 0 -audio.enabled 0 -pause_on_focus_loss 0)
        set_tests_properties(solstice_reach_walk_gpu PROPERTIES LABELS gpu PASS_REGULAR_EXPRESSION "200 ticks, 0 GL errors"
                                                                RESOURCE_LOCK solstice_reach_module)
    endif()
endif()

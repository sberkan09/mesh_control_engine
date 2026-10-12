# Undelivered: a project made from the MCE project template (scripts/new_project.py, assets.md §4.4).
#
#   assets/                 what you edit (the editor opens this project); cooked into assets.mce at build
#   games/undelivered/module/    the game module libundelivered: components (undelivered.h), systems (undelivered.cpp)
#   games/undelivered/host/      the thin host executable undelivered
#
# Two ways to build it:
#   - against an installed engine (the SDK): `mce build`, or cmake -S . -B build
#     -DCMAKE_PREFIX_PATH=<sdk prefix> and cmake --build build; find_package(MCE) below brings the
#     engine, the helpers and the tools (`mce editor`, `mce mcp`, `mce cook`, `mce ship`).
#   - inside the engine tree: the engine's CMake adds every projects/<name>/ directory, so
#     `cmake --build --preset <preset> --target undelivered` builds the host, the module and the pack
#     and python3 scripts/ship.py --project projects/Undelivered ships it.
cmake_minimum_required(VERSION 3.28)
if(NOT COMMAND mce_add_project)
    project(Undelivered LANGUAGES C CXX)
    find_package(MCE REQUIRED)
endif()

mce_add_project(undelivered
    TITLE "Undelivered"
    GAME_SOURCES games/undelivered/module/undelivered.cpp
    MODULE_ENTRY games/undelivered/module/module.cpp
    TYPES games/undelivered/module/types.cpp
    HOST games/undelivered/host/main.cpp
    INCLUDE_DIRS games/undelivered/module
    DEPS mce::mce_kit_novel)

# The script compiler's output (games/undelivered/module/story_data.h) is checked in; regenerate it
# after editing story/*.story or story/story.json with `mce story compile .` (in the engine tree:
# python3 tools/story/story_compile.py projects/Undelivered).

# Undelivered's own test: the compiled script's tables, the authored scene, five bot policies
# played to their endings through the game's systems, a save in the middle of a run reloaded and
# finished. Engine tree only (the game sources link in statically, as Kindle's test does).
if(MCE_BUILD_TESTS AND NOT MCE_SDK)
    mce_add_test(undelivered_run UMBRELLA SOURCES tests/test_undelivered.cpp games/undelivered/module/undelivered.cpp
        DEPS mce::mce_kit_novel mce::mce_ecs mce::mce_ecs_scene mce::mce_scene mce::mce_asset mce::mce_gameplay_kit
             mce::mce_physics2d mce::mce_input mce::mce_audio mce::mce_module_host)
    target_include_directories(undelivered_run PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/games/undelivered/module")
    target_compile_definitions(undelivered_run PRIVATE UNDELIVERED_PACK="${CMAKE_CURRENT_BINARY_DIR}/project/assets.mce")
    add_dependencies(undelivered_run undelivered_content)
endif()

# Kindle: a project made from the MCE project template (scripts/new_project.py, assets.md §4.4).
#
#   assets/                 what you edit (the editor opens this project); cooked into assets.mce at build
#   games/kindle/module/    the game module libkindle: components (kindle.h), systems (kindle.cpp)
#   games/kindle/host/      the thin host executable kindle
#
# Two ways to build it:
#   - against an installed engine (the SDK, where it is share/mce/examples/Kindle): `mce example
#     Kindle` copies it out, then `mce build Kindle`, `mce play Kindle`, `mce editor Kindle`
#   - inside the engine tree: the engine's CMake adds every projects/<name>/ directory;
#     `cmake --build --preset <preset> --target kindle` builds the host, the module and the pack
#     and python3 scripts/ship.py --project projects/Kindle ships it.
cmake_minimum_required(VERSION 3.28)
if(NOT COMMAND mce_add_project)
    project(Kindle LANGUAGES C CXX)
    find_package(MCE REQUIRED)
endif()

mce_add_project(kindle
    TITLE "Kindle"
    GAME_SOURCES games/kindle/module/kindle.cpp
    MODULE_ENTRY games/kindle/module/module.cpp
    TYPES games/kindle/module/types.cpp
    HOST games/kindle/host/main.cpp
    INCLUDE_DIRS games/kindle/module)

# Kindle's own test (UC-D): the cooked level played through the game's systems with scripted
# input, the switch mechanic, and a full run to the flag (tests/test_kindle.cpp). The game sources
# link in statically, as samples/03_platformer's test_ember_step does. Engine tree only: against
# the SDK the engine's static libraries it links are not installed (the host plays it instead).
if(MCE_BUILD_TESTS AND NOT MCE_SDK)
    mce_add_test(kindle_level SOURCES tests/test_kindle.cpp games/kindle/module/kindle.cpp
        DEPS mce::mce_ecs mce::mce_ecs_scene mce::mce_scene mce::mce_asset mce::mce_gameplay_kit mce::mce_physics2d
             mce::mce_input mce::mce_audio mce::mce_module_host)
    target_include_directories(kindle_level PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/games/kindle/module")
    target_compile_definitions(kindle_level PRIVATE KINDLE_PACK="${CMAKE_CURRENT_BINARY_DIR}/project/assets.mce")
    add_dependencies(kindle_level kindle_content)
endif()

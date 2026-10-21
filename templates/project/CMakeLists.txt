# @PROJECT@: a project made from the MCE project template (scripts/new_project.py, assets.md §4.4).
#
#   assets/                 what you edit (the editor opens this project); cooked into assets.mce at build
#   games/@GAME@/module/    the game module lib@GAME@: components (components.h), systems (@GAME@.cpp)
#   games/@GAME@/host/      the thin host executable @GAME@
#
# Two ways to build it:
#   - against an installed engine (the SDK): `mce build`, or cmake -S . -B build
#     -DCMAKE_PREFIX_PATH=<sdk prefix> and cmake --build build; find_package(MCE) below brings the
#     engine, the helpers and the tools (`mce editor`, `mce mcp`, `mce cook`, `mce ship`).
#   - inside the engine tree: the engine's CMake adds every projects/<name>/ directory, so
#     `cmake --build --preset <preset> --target @GAME@` builds the host, the module and the pack
#     and python3 scripts/ship.py --project projects/@PROJECT@ ships it.
cmake_minimum_required(VERSION 3.28)
if(NOT COMMAND mce_add_project)
    project(@PROJECT@ LANGUAGES C CXX)
    find_package(MCE REQUIRED)
endif()

mce_add_project(@GAME@
    TITLE "@TITLE@"
    GAME_SOURCES games/@GAME@/module/@GAME@.cpp
    MODULE_ENTRY games/@GAME@/module/module.cpp
    TYPES games/@GAME@/module/types.cpp
    HOST games/@GAME@/host/main.cpp
    INCLUDE_DIRS games/@GAME@/module)
# Multiplayer is opt-in: add `DEPS mce::mce_net` to the call above to link the networking module
# (client-server over UDP, snapshot replication; the engine's docs/engine/net.md).

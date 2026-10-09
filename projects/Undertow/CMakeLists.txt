# Undertow: a project made from the MCE project template (scripts/new_project.py, assets.md §4.4).
#
#   assets/                 what you edit (the editor opens this project); cooked into assets.mce at build
#   games/undertow/module/    the game module libundertow: components (undertow.h), systems (undertow.cpp)
#   games/undertow/host/      the thin host executable undertow
#
# Build: the engine tree adds every projects/<name>/ directory; `cmake --build --preset <preset>
# --target undertow` builds the host, the module and the pack. Ship: python3 scripts/ship.py
# --project projects/Undertow
mce_add_project(undertow
    TITLE "Undertow"
    GAME_SOURCES games/undertow/module/undertow.cpp
    MODULE_ENTRY games/undertow/module/module.cpp
    TYPES games/undertow/module/types.cpp
    HOST games/undertow/host/main.cpp
    INCLUDE_DIRS games/undertow/module)

# Undertow's own test: the cooked scenes played through the game's systems with scripted input
# (tests/test_undertow.cpp): thrust and boost, gates and switches, mines, the rope and the dock,
# the win cue, and the attract scene's autopilot run hashed twice. The game sources link in
# statically, as Kindle's kindle_level does.
if(MCE_BUILD_TESTS)
    mce_add_test(undertow_level SOURCES tests/test_undertow.cpp games/undertow/module/undertow.cpp
        DEPS mce::mce_ecs mce::mce_ecs_scene mce::mce_scene mce::mce_asset mce::mce_gameplay_kit mce::mce_physics2d
             mce::mce_input mce::mce_audio mce::mce_module_host)
    target_include_directories(undertow_level PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/games/undertow/module")
    target_compile_definitions(undertow_level PRIVATE UNDERTOW_PACK="${CMAKE_CURRENT_BINARY_DIR}/project/assets.mce")
    add_dependencies(undertow_level undertow_content)
endif()

# PrismBreak: a project made from the MCE project template (scripts/new_project.py, assets.md §4.4).
#
#   assets/                 what you edit (the editor opens this project); cooked into assets.mce at build
#   games/prism_break/module/    the game module libprism_break: components (prism_break.h), systems (prism_break.cpp)
#   games/prism_break/host/      the thin host executable prism_break
#
# Build: the engine tree adds every projects/<name>/ directory; `cmake --build --preset <preset>
# --target prism_break` builds the host, the module and the pack. Ship: python3 scripts/ship.py
# --project projects/PrismBreak
mce_add_project(prism_break
    TITLE "Prism Break"
    GAME_SOURCES games/prism_break/module/prism_break.cpp
    MODULE_ENTRY games/prism_break/module/module.cpp
    TYPES games/prism_break/module/types.cpp
    HOST games/prism_break/host/main.cpp
    INCLUDE_DIRS games/prism_break/module)

# Host tests of this game. Each takes the module's RESOURCE_LOCK: the module host maps
# libprism_break_loaded_0.so beside the module, and two hosts at once overwrite each other's copy
# (mce_add_project locks only the tests that exist when it runs).
if(MCE_BUILD_TESTS)
    # The attract scene (paddle on autopilot) must clear the wall: the whole game loop, headless.
    add_test(NAME prism_break_attract_clears
             COMMAND prism_break -headless 1 -ticks 12000 -scene scenes/attract.mcscene)
    # Without -scene the host plays the .mceproj's start_scene (main), not the first scene by path.
    add_test(NAME prism_break_start_scene COMMAND prism_break -headless 1 -ticks 1)
    set_tests_properties(prism_break_attract_clears PROPERTIES PASS_REGULAR_EXPRESSION "prism_break: cleared at tick")
    set_tests_properties(prism_break_start_scene PROPERTIES PASS_REGULAR_EXPRESSION "scene [0-9a-f-]+ \\(scenes/main\\.mcscene\\)")
    set_tests_properties(prism_break_attract_clears prism_break_start_scene PROPERTIES RESOURCE_LOCK prism_break_module)
endif()

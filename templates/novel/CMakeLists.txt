# @PROJECT@: a visual novel made from the MCE novel template (mce new --template novel).
#
#   story/                  the script (*.story) and its manifest (story.json); the build compiles
#                           them into games/@GAME@/module/story_data.h (mce story compile)
#   assets/                 what you edit (the editor opens this project); cooked into assets.mce at build:
#                           textures/bg/<location>_<layer>.png, textures/cast/<who>_<expr>_<pose>.png,
#                           audio/music/<id>.wav ... named as story.json declares them
#   games/@GAME@/module/    the game module lib@GAME@: the novel kit with the compiled story (@GAME@.cpp)
#   games/@GAME@/host/      the thin host executable @GAME@
#
# Build it against an installed engine (the SDK) with `mce build`, or inside the engine tree with
# `cmake --build --preset <preset> --target @GAME@`. Play the story through with the bot:
# `mce headless . -- -novel.autoplay 1` (or `mce play . -novel.autoplay 1` with a window).
cmake_minimum_required(VERSION 3.28)
if(NOT COMMAND mce_add_project)
    project(@PROJECT@ LANGUAGES C CXX)
    find_package(MCE REQUIRED)
endif()

mce_add_project(@GAME@
    TITLE "@TITLE@"
    STORY
    GAME_SOURCES games/@GAME@/module/@GAME@.cpp
    MODULE_ENTRY games/@GAME@/module/module.cpp
    TYPES games/@GAME@/module/types.cpp
    HOST games/@GAME@/host/main.cpp
    INCLUDE_DIRS games/@GAME@/module
    DEPS mce::mce_kit_novel)

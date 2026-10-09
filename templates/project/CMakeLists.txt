# @PROJECT@: a project made from the MCE project template (scripts/new_project.py, assets.md §4.4).
#
#   assets/                 what you edit (the editor opens this project); cooked into assets.mce at build
#   games/@GAME@/module/    the game module lib@GAME@: components (@GAME@.h), systems (@GAME@.cpp)
#   games/@GAME@/host/      the thin host executable @GAME@
#
# Build: the engine tree adds every projects/<name>/ directory; `cmake --build --preset <preset>
# --target @GAME@` builds the host, the module and the pack. Ship: python3 scripts/ship.py
# --project projects/@PROJECT@
mce_add_project(@GAME@
    TITLE "@TITLE@"
    GAME_SOURCES games/@GAME@/module/@GAME@.cpp
    MODULE_ENTRY games/@GAME@/module/module.cpp
    TYPES games/@GAME@/module/types.cpp
    HOST games/@GAME@/host/main.cpp
    INCLUDE_DIRS games/@GAME@/module)

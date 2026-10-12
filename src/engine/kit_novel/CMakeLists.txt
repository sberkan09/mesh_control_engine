# mce_kit_novel (L3, opt-in — M-NOVEL): the visual-novel kit and the dialogue VM (mce/kit/novel.h):
# the story VM over a program `mce story compile` writes, the stage driver on the 2D kit's sprites
# and labels, the playthrough bot, and NPC dialogue outside a novel stage.
#
# Opt-in like mce_net: not in MCE_ENGINE_LIBS, so libmce_engine never contains it. A game links it
# with mce_add_project(... DEPS mce::mce_kit_novel) (mce::mce_kit_novel against the SDK). UMBRELLA:
# where game modules are supported its engine dependencies resolve against libmce_engine, so the
# copy linked into a game module shares the host's engine.
mce_add_library(mce_kit_novel UMBRELLA
    SOURCES
        src/dialogue.cpp
        src/program.cpp
        src/stage.cpp
    PUBLIC_DEPS
        mce::mce_core
        mce::mce_ecs
        mce::mce_ecs_scene
        mce::mce_asset
        mce::mce_audio
        mce::mce_input
        mce::mce_gameplay_kit)

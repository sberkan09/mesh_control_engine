# mce_player (L6): the runtime host for editor-authored scenes — the "game.exe" of the editor's
# run-in-player button (editor-tools.md §2.1, Appendix A4) and of `Save -> Cook -> load` (ROADMAP
# M2 demo). Mounts a cooked pack, instantiates one scene through mce_scene, runs physics2d and the
# audio bridge on the fixed step, and draws the starter-kit components. Esc pauses (the A9 pause
# overlay); headless mode prints the World state hash for the determinism harness.
mce_add_executable(mce_player
    SOURCES
        main.cpp
    DEPS
        mce::mce_scene
        mce::mce_gameplay_kit
        mce::mce_physics2d
        mce::mce_audio
        mce::mce_render
        mce::mce_platform
        mce::mce_rhi_gl)
target_include_directories(mce_player PRIVATE "${PROJECT_SOURCE_DIR}/samples/common")

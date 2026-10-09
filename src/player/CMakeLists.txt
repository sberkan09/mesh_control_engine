# mce_player (L6): the runtime host for editor-authored scenes — the "game.exe" of the editor's
# run-in-player button (editor-tools.md §2.1, Appendix A4) and of `Save -> Cook -> load` (ROADMAP
# M2 demo). Mounts a cooked pack, instantiates one scene through mce_scene, runs physics2d and the
# audio bridge on the fixed step, and draws the starter-kit components. Esc pauses (the A9 pause
# overlay); headless mode prints the World state hash for the determinism harness.
#
# The runtime itself is mce_player_runtime (mce/player/player.h), which project hosts made from the
# project template link too. Where game modules are supported it links the mce_engine umbrella,
# so a module loaded with -module shares the host's engine; elsewhere it links the libraries.
add_library(mce_player_runtime STATIC src/player.cpp)
add_library(mce::mce_player_runtime ALIAS mce_player_runtime)
target_include_directories(mce_player_runtime
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/samples/common")
if(MCE_GAME_MODULES_SUPPORTED)
    target_link_libraries(mce_player_runtime PUBLIC mce::mce_engine)
else()
    target_link_libraries(mce_player_runtime PUBLIC
        mce::mce_scene mce::mce_gameplay_kit mce::mce_physics2d mce::mce_audio mce::mce_render mce::mce_platform
        mce::mce_ui mce::mce_module_host mce::mce_rhi_default)
endif()
mce_apply_compile_policy(mce_player_runtime)

mce_add_executable(mce_player SOURCES main.cpp DEPS mce::mce_player_runtime)

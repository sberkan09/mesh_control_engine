# The game: gameplay only (ECS, physics2d, input actions, audio events), no host knowledge.
# Where game modules are supported (ELF platforms until the M4 export ledger) it is built as the
# game module `platformer` (libplatformer.so) and the host links only the mce_engine umbrella and
# loads it at startup; elsewhere the host links the same sources statically.
set(PLATFORMER_GAME_SOURCES game/platformer.cpp)

if(MCE_GAME_MODULES_SUPPORTED)
    mce_add_game_module(platformer SOURCES ${PLATFORMER_GAME_SOURCES} game/module.cpp)
    target_include_directories(platformer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/game")

    add_executable(sample_03_platformer main.cpp art.cpp)
    target_link_libraries(sample_03_platformer PRIVATE mce::mce_engine)
    target_compile_definitions(sample_03_platformer PRIVATE MCE_PLATFORMER_MODULE="$<TARGET_FILE:platformer>"
                                                            MCE_PLATFORMER_MODULE_NAME="$<TARGET_FILE_NAME:platformer>")
    add_dependencies(sample_03_platformer platformer)
    mce_apply_compile_policy(sample_03_platformer)
else()
    mce_add_executable(sample_03_platformer SOURCES main.cpp art.cpp ${PLATFORMER_GAME_SOURCES}
        DEPS mce::mce_ecs mce::mce_physics2d mce::mce_input mce::mce_audio mce::mce_platform mce::mce_render
             mce::mce_rhi_gl)
endif()
target_include_directories(sample_03_platformer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../common")
mce_sample_pack(sample_03_platformer)  # Kenney CC0 SFX (assets/sfx, CREDITS.md)

if(MCE_BUILD_TESTS)
    # Sample ladder rule: every sample runs 300 headless steps in CI.
    add_test(NAME sample_03_platformer_300 COMMAND sample_03_platformer -headless 1 -ticks 300)
    if(MCE_GAME_MODULES_SUPPORTED)
        # The module under the generic headless module host (scripting.md §6.5).
        add_test(NAME devhost_platformer_300 COMMAND mce_devhost -module $<TARGET_FILE:platformer> -ticks 300)
        set_tests_properties(devhost_platformer_300 PROPERTIES PASS_REGULAR_EXPRESSION "module platformer v1")
    endif()
    if(MCE_GPU_TESTS)
        # Windowed smoke: the bot plays 240 frames with the GL debug layer on, exit 0.
        add_test(NAME sample_03_platformer_gpu COMMAND sample_03_platformer -frames 240 -autoplay 1
                 -platform.error_box 0 -r.vsync 0)
        set_tests_properties(sample_03_platformer_gpu PROPERTIES LABELS gpu)
    endif()
endif()
mce_ship_metadata(sample_03_platformer NAME "Ember Step" DESCRIPTION "Mesh Control Engine - Ember Step platformer (Artifact A)"
    ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.ico")

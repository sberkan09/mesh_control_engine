# The game: gameplay only (ECS, physics2d, input actions, audio events). This is the part that
# becomes game.dll once the game-module loader lands; the host below links it statically.
mce_add_library(platformer_game SOURCES game/platformer.cpp
    PUBLIC_DEPS mce::mce_ecs mce::mce_physics2d mce::mce_input mce::mce_audio)
target_include_directories(platformer_game PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

mce_add_executable(sample_03_platformer SOURCES main.cpp art.cpp
    DEPS platformer_game mce::mce_platform mce::mce_render mce::mce_rhi_gl)
target_include_directories(sample_03_platformer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../common")

if(MCE_BUILD_TESTS)
    # Sample ladder rule: every sample runs 300 headless steps in CI.
    add_test(NAME sample_03_platformer_300 COMMAND sample_03_platformer -headless 1 -ticks 300)
    if(MCE_GPU_TESTS)
        # Windowed smoke: the bot plays 240 frames with the GL debug layer on, exit 0.
        add_test(NAME sample_03_platformer_gpu COMMAND sample_03_platformer -frames 240 -autoplay 1
                 -platform.error_box 0 -r.vsync 0)
        set_tests_properties(sample_03_platformer_gpu PROPERTIES LABELS gpu)
    endif()
endif()

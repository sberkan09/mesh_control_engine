# ROADMAP M7: Ashen Court, the third-person melee slice. Gameplay lives in game/ (ECS, World3D,
# fsm.h, input actions, audio events; no host knowledge) and is linked statically into the host
# and the test. Procedural content (tools/gen_assets.py, regenerated deterministically, each mesh
# with a 'max_triangles' budget in its sidecar) is cooked at build time into the sample's pack.
set(ASHEN_COURT_GAME_SOURCES game/court.cpp)
set(ASHEN_COURT_GAME_DEPS mce::mce_ecs mce::mce_gameplay_kit mce::mce_physics3d mce::mce_input mce::mce_audio)

mce_add_executable(sample_05_ashen_court SOURCES main.cpp ${ASHEN_COURT_GAME_SOURCES}
    DEPS ${ASHEN_COURT_GAME_DEPS} mce::mce_render mce::mce_asset mce::mce_platform mce::mce_rhi_default)
target_include_directories(sample_05_ashen_court PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../common")
mce_sample_pack(sample_05_ashen_court)

if(MCE_BUILD_TESTS)
    # Enemy FSMs, combat, pickups, the camera boom and the bot, against the real systems.
    mce_add_test(test_ashen_court SOURCES tests/test_ashen_court.cpp ${ASHEN_COURT_GAME_SOURCES}
        DEPS ${ASHEN_COURT_GAME_DEPS})
    target_include_directories(test_ashen_court PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/game")
    # Sample ladder rule: every sample runs 300 headless steps in CI (the bot plays; no GL).
    add_test(NAME sample_05_ashen_court_300 COMMAND sample_05_ashen_court -headless 1 -ticks 300)
    set_tests_properties(sample_05_ashen_court_300 PROPERTIES PASS_REGULAR_EXPRESSION "300 ticks .* hash [0-9a-f]+")
    if(MCE_GPU_TESTS)
        add_test(NAME sample_05_ashen_court_gpu COMMAND sample_05_ashen_court -frames 60 -autoplay 1
                 -platform.error_box 0 -r.vsync 0)
        set_tests_properties(sample_05_ashen_court_gpu PROPERTIES LABELS gpu)
    endif()
endif()

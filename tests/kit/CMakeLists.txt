# The gameplay starter kit's systems without a GPU: particles (FR-R2D-070) simulated, cooked from
# .mcpart and drawn into a CPU-built sprite batch; action-list sequences (FR-ENT-100); the FSM
# helper (FR-ENT-090); grid A* pathfinding and PathAgent (#152); the kit3d components, systems and
# extract into a CPU-side Scene3D (M-KIT3D); the effect components, sprite-sheet animation and
# 3D bodies (M-DATA-REACH 4-6); 3D navigation components (M-GAMEPLAY3D 1), joints (2), ragdolls (3), vehicles (4) and
# animation graphs v2 (5); character feel (M-MOVE3D 1).
mce_add_test(test_kit UMBRELLA
    SOURCES
        test_ai.cpp
        test_ai_tasks.cpp
        test_ai_perception.cpp
        test_ai_utility.cpp
        test_ai_budget.cpp
        test_audio_occlusion.cpp
        test_bodies3d.cpp
        test_move3d.cpp
        test_captions.cpp
        test_presence.cpp
        test_effects.cpp
        test_ui_flow.cpp
        test_fsm.cpp
        test_kit3d.cpp
        test_anim3d.cpp
        test_sensors3d.cpp
        test_nav3d.cpp
        test_joints3d.cpp
        test_ragdolls3d.cpp
        test_vehicles3d.cpp
        test_particles.cpp
        test_pathfinding.cpp
        test_sequence.cpp
        test_skeleton2d.cpp
        test_skeleton2d_anim.cpp
        test_skeleton2d_demo.cpp
        test_skeleton2d_ik.cpp
        test_skeleton2d_mesh.cpp
        test_sprite_anim.cpp
        test_timeline.cpp
        test_cinematic.cpp
        test_video.cpp
        test_view_info.cpp
    DEPS
        mce::mce_gameplay_kit
        mce::mce_scene
        mce::mce_asset_cook)
target_include_directories(test_kit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../asset ${CMAKE_CURRENT_SOURCE_DIR}/../render)
target_compile_definitions(test_kit PRIVATE MCE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp" MCE_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
                           MCE_TEST_VIDEO="${PROJECT_SOURCE_DIR}/tests/asset/data/video/red_blue.mpg")

# The kit extract on a real GL 4.5 driver (CI: Xvfb + Mesa llvmpipe). Always builds; registers only
# with MCE_GPU_TESTS.
mce_add_test(test_kit_gl UMBRELLA NO_CTEST SOURCES test_kit_gl.cpp
             DEPS mce::mce_gameplay_kit mce::mce_asset_cook mce::mce_rhi_default)
target_include_directories(test_kit_gl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../asset)
target_compile_definitions(test_kit_gl PRIVATE MCE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp"
                           MCE_TEST_FONT="${PROJECT_SOURCE_DIR}/projects/Kindle/assets/fonts/ProggyClean.ttf")
if(MCE_GPU_TESTS)
    add_test(NAME test_kit_gl COMMAND test_kit_gl)
    set_tests_properties(test_kit_gl PROPERTIES LABELS gpu)
endif()

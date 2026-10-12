# The gameplay starter kit's systems without a GPU: particles (FR-R2D-070) simulated, cooked from
# .mcpart and drawn into a CPU-built sprite batch; action-list sequences (FR-ENT-100); the FSM
# helper (FR-ENT-090); grid A* pathfinding and PathAgent (#152).
mce_add_test(test_kit UMBRELLA
    SOURCES
        test_fsm.cpp
        test_particles.cpp
        test_pathfinding.cpp
        test_sequence.cpp
        test_view_info.cpp
    DEPS
        mce::mce_gameplay_kit
        mce::mce_scene
        mce::mce_asset_cook)
target_include_directories(test_kit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../asset)
target_compile_definitions(test_kit PRIVATE MCE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")

# The kit extract on a real GL 4.5 driver (CI: Xvfb + Mesa llvmpipe). Always builds; registers only
# with MCE_GPU_TESTS.
mce_add_test(test_kit_gl UMBRELLA NO_CTEST SOURCES test_kit_gl.cpp
             DEPS mce::mce_gameplay_kit mce::mce_asset_cook mce::mce_rhi_default)
target_include_directories(test_kit_gl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../asset)
target_compile_definitions(test_kit_gl PRIVATE MCE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")
if(MCE_GPU_TESTS)
    add_test(NAME test_kit_gl COMMAND test_kit_gl)
    set_tests_properties(test_kit_gl PROPERTIES LABELS gpu)
endif()

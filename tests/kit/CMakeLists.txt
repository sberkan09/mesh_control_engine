# The gameplay starter kit's systems without a GPU: particles (FR-R2D-070) simulated, cooked from
# .mcpart and drawn into a CPU-built sprite batch.
mce_add_test(test_kit
    SOURCES
        test_particles.cpp
    DEPS
        mce::mce_gameplay_kit
        mce::mce_scene
        mce::mce_asset_cook)
target_include_directories(test_kit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../asset)
target_compile_definitions(test_kit PRIVATE MCE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")

# The kit extract on a real GL 4.5 driver (CI: Xvfb + Mesa llvmpipe). Always builds; registers only
# with MCE_GPU_TESTS.
add_executable(test_kit_gl test_kit_gl.cpp)
target_link_libraries(test_kit_gl PRIVATE mce::mce_gameplay_kit mce::mce_asset_cook mce::mce_rhi_default mce::doctest_main Threads::Threads)
target_include_directories(test_kit_gl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../asset)
target_compile_definitions(test_kit_gl PRIVATE MCE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")
mce_apply_compile_policy(test_kit_gl)
if(MCE_GPU_TESTS)
    add_test(NAME test_kit_gl COMMAND test_kit_gl)
    set_tests_properties(test_kit_gl PROPERTIES LABELS gpu)
endif()

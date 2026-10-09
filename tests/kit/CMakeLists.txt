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

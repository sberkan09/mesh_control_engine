set(MCE_ASSET_TEST_DEFS
    MCE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp"
    MCE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

# Runtime side: container reader/writer (negative + fuzz), formats, AssetSystem.
mce_add_test(test_asset
    SOURCES
        test_asset_system.cpp
        test_container.cpp
        test_guid_formats.cpp
    DEPS
        mce::mce_asset)
target_include_directories(test_asset PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_definitions(test_asset PRIVATE ${MCE_ASSET_TEST_DEFS})

# Cook side: JSON, audio processing, importers, full project cooks.
mce_add_test(test_cook
    SOURCES
        test_cook_gltf.cpp
        test_cook_json_audio.cpp
        test_cook_project.cpp
    DEPS
        mce::mce_asset_cook)
target_include_directories(test_cook PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_definitions(test_cook PRIVATE ${MCE_ASSET_TEST_DEFS})

# mce_cook CLI contract (FR-AST-070): exit codes 0 / 1 / 2 and byte-identical re-cooks of the
# committed sample project (assets.md §10 cook determinism, per PR, every preset).
add_test(NAME mce_cook_cli
    COMMAND ${CMAKE_COMMAND}
        -DMCE_COOK=$<TARGET_FILE:mce_cook>
        -DDATA_DIR=${CMAKE_CURRENT_SOURCE_DIR}/data
        -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cook_cli_test.cmake)

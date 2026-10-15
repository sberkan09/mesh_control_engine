set(MCE_ASSET_TEST_DEFS
    MCE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp"
    MCE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

# Runtime side: container reader/writer (negative + fuzz), formats, AssetSystem.
mce_add_test(test_asset
    SOURCES
        test_asset_system.cpp
        test_container.cpp
        test_guid_formats.cpp
        test_mods.cpp
    DEPS
        mce::mce_asset)
target_include_directories(test_asset PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_definitions(test_asset PRIVATE ${MCE_ASSET_TEST_DEFS})

# Cook side: JSON, audio processing, importers, full project cooks.
mce_add_test(test_cook UMBRELLA
    SOURCES
        test_cook_gltf.cpp
        test_cook_lod.cpp
        test_cook_input.cpp
        test_cook_json_audio.cpp
        test_cook_mod.cpp
        test_cook_project.cpp
        test_cook_shader.cpp
        test_cook_store.cpp
        test_cook_config.cpp
        test_cook_services.cpp
        test_cook_strings.cpp
        test_cook_ui.cpp
    DEPS
        mce::mce_asset_cook
        mce::mce_input
        mce::mce_monetize
        mce::mce_liveops
        mce::mce_loc
        mce::mce_ui)
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
if(TARGET template_probe_game)
    # --module: the stock cook with a game module loaded cooks that game's scenes (the template
    # probe's main.mcscene uses the game's components); without it the cook reports them unknown.
    add_test(NAME mce_cook_module
        COMMAND ${CMAKE_COMMAND}
            -DMCE_COOK=$<TARGET_FILE:mce_cook>
            -DMODULE=$<TARGET_FILE:template_probe_game>
            -DPROJECT_DIR=${CMAKE_BINARY_DIR}/template_probe/TemplateProbe
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli_module
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cook_module_test.cmake)
    set_tests_properties(mce_cook_module PROPERTIES RESOURCE_LOCK template_probe_module)
endif()

# Cooked shader variants on a real GL 4.5 driver (CI: Xvfb + Mesa llvmpipe). Always builds;
# registers only with MCE_GPU_TESTS.
mce_add_test(test_shader_gl UMBRELLA NO_CTEST SOURCES test_shader_gl.cpp DEPS mce::mce_asset_cook mce::mce_rhi_default)
target_include_directories(test_shader_gl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_definitions(test_shader_gl PRIVATE ${MCE_ASSET_TEST_DEFS})
if(MCE_GPU_TESTS)
    add_test(NAME test_shader_gl COMMAND test_shader_gl)
    set_tests_properties(test_shader_gl PROPERTIES LABELS gpu)
endif()

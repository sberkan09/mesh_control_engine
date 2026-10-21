# The authored-scene runtime path: .mcscene -> cook -> mount -> mce_scene -> World, and the
# starter-kit components it carries.
mce_add_test(test_scene UMBRELLA
    SOURCES
        test_scene.cpp
        test_streaming.cpp
    DEPS
        mce::mce_scene
        mce::mce_gameplay_kit
        mce::mce_asset_cook)
target_include_directories(test_scene PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_SOURCE_DIR}/../asset)
target_compile_definitions(test_scene PRIVATE MCE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")

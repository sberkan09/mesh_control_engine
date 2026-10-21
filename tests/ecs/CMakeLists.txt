mce_add_test(test_ecs
    SOURCES
        test_world.cpp
        test_commands.cpp
        test_events.cpp
        test_determinism.cpp
        test_change_detection.cpp
        test_hierarchy.cpp
        test_origin.cpp
    DEPS
        mce::mce_ecs)

# Scene/prefab instantiation end to end: author -> mce_asset_cook -> CHNK_PREFAB/SCENE -> World.
mce_add_test(test_ecs_scene UMBRELLA
    SOURCES
        test_prefab.cpp
        test_save.cpp
    DEPS
        mce::mce_ecs_scene
        mce::mce_asset_cook
        mce::mce_input)
target_compile_definitions(test_ecs_scene PRIVATE MCE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")

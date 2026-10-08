# Headless editor tests: editor_core logic and the layer's ImGui-free pieces. No window, no GPU.
mce_add_test(test_editor
    SOURCES
        test_asset_browser.cpp
        test_console_model.cpp
        test_draw_clip.cpp
        test_hierarchy.cpp
        test_hot_reload.cpp
        test_orbit_camera.cpp
        test_play_session.cpp
        test_prefab_preview.cpp
        test_scene_editing.cpp
        test_stub_world.cpp
        test_tilemap.cpp
        test_transform_gizmo.cpp
        test_undo.cpp
    DEPS
        mce::mce_editor_layer)
target_include_directories(test_editor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_SOURCE_DIR}/../asset)
target_compile_definitions(test_editor PRIVATE MCE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp"
    MCE_SOURCE_DIR="${PROJECT_SOURCE_DIR}" MCE_PLAYER_EXE="$<TARGET_FILE:mce_player>")
add_dependencies(test_editor mce_player)

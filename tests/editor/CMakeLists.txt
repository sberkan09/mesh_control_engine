# Headless editor tests: editor_core logic and the layer's ImGui-free pieces. No window, no GPU.
mce_add_test(test_editor UMBRELLA
    SOURCES
        test_animgraph_editor.cpp
        test_asset_browser.cpp
        test_asset_kinds.cpp
        test_asset_ops.cpp
        test_asset_refs.cpp
        test_camera_bookmarks.cpp
        test_quick_open.cpp
        test_selection_history.cpp
        test_selection_sets.cpp
        test_hierarchy_filter.cpp
        test_batch_rename.cpp
        test_select_related.cpp
        test_recent_list.cpp
        test_asset_thumb.cpp
        test_asset_form.cpp
        test_scene_tabs.cpp
        test_mixer_model.cpp
        test_source_doc.cpp
        test_strings_grid.cpp
        test_thumb_cache.cpp
        test_bt_editor.cpp
        test_console_model.cpp
        test_draw_clip.cpp
        test_hierarchy.cpp
        test_hot_reload.cpp
        test_mod_project.cpp
        test_navmesh_bake.cpp
        test_hlod_bake.cpp
        test_orbit_camera.cpp
        test_perf_compare.cpp
        test_perf_monitor.cpp
        test_play_session.cpp
        test_prefab_overrides.cpp
        test_prefab_preview.cpp
        test_scene_editing.cpp
        test_skeleton2d_editor.cpp
        test_stub_world.cpp
        test_terrain_editor.cpp
        test_tilemap.cpp
        test_timeline_editor.cpp
        test_transform_gizmo.cpp
        test_ui_document.cpp
        test_undo.cpp
    DEPS
        mce::mce_editor_layer)
target_include_directories(test_editor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_SOURCE_DIR}/../asset)
target_compile_definitions(test_editor PRIVATE MCE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp"
    MCE_SOURCE_DIR="${PROJECT_SOURCE_DIR}" MCE_PLAYER_EXE="$<TARGET_FILE:mce_player>")
add_dependencies(test_editor mce_player)

# The editor's GPU paths on a real GL driver (CI: Xvfb + Mesa llvmpipe). Always builds; registers
# only with MCE_GPU_TESTS.
add_executable(test_editor_gl test_probe_bake_gl.cpp test_ui_preview_gl.cpp)
target_link_libraries(test_editor_gl PRIVATE mce::mce_editor_layer mce::mce_rhi_default mce::doctest_main)
target_include_directories(test_editor_gl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_SOURCE_DIR}/../asset)
target_compile_definitions(test_editor_gl PRIVATE MCE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp_gl"
    MCE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
mce_apply_compile_policy(test_editor_gl)
if(MCE_GPU_TESTS)
    add_test(NAME test_editor_gl COMMAND test_editor_gl)
    set_tests_properties(test_editor_gl PROPERTIES LABELS gpu)
endif()

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
        test_select_all.cpp
        test_group_entities.cpp
        test_entity_path.cpp
        test_unique_names.cpp
        test_align_entities.cpp
        test_reset_transform.cpp
        test_flip_entities.cpp
        test_snap_entities.cpp
        test_duplicate_offset.cpp
        test_match_transform.cpp
        test_scene_diff.cpp
        test_scene_lint.cpp
        test_scene_migrate.cpp
        test_scene_stats.cpp
        test_scene_find.cpp
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

# mce_scene_diff (M-SCENEDIFF 2): the report over two fixture scenes, and the exit code (1 when they differ).
add_test(NAME scene_diff_cli_report
         COMMAND mce_scene_diff "${CMAKE_CURRENT_SOURCE_DIR}/data/diff_a.mcscene" "${CMAKE_CURRENT_SOURCE_DIR}/data/diff_b.mcscene")
set_tests_properties(scene_diff_cli_report PROPERTIES
    PASS_REGULAR_EXPRESSION "\\+ Wall \\(id 4\\).*- Torch \\(id 3\\)|- Torch \\(id 3\\).*\\+ Wall \\(id 4\\)")
add_test(NAME scene_diff_cli_exit_code
         COMMAND mce_scene_diff "${CMAKE_CURRENT_SOURCE_DIR}/data/diff_a.mcscene" "${CMAKE_CURRENT_SOURCE_DIR}/data/diff_b.mcscene")
set_tests_properties(scene_diff_cli_exit_code PROPERTIES WILL_FAIL TRUE)
add_test(NAME scene_diff_cli_identical
         COMMAND mce_scene_diff "${CMAKE_CURRENT_SOURCE_DIR}/data/diff_a.mcscene" "${CMAKE_CURRENT_SOURCE_DIR}/data/diff_a.mcscene")
set_tests_properties(scene_diff_cli_identical PROPERTIES PASS_REGULAR_EXPRESSION "scenes are identical")

# mce_scene_lint (M-SCENELINT 2): findings over a fixture scene, none over a tidy one.
add_test(NAME scene_lint_cli_findings COMMAND mce_scene_lint "${CMAKE_CURRENT_SOURCE_DIR}/data/lint_bad.mcscene")
set_tests_properties(scene_lint_cli_findings PROPERTIES
    PASS_REGULAR_EXPRESSION "Wall \\(id 2\\): duplicate name among its siblings.*Flat \\(id 3\\): scale of zero on an axis")
add_test(NAME scene_lint_cli_exit_code COMMAND mce_scene_lint "${CMAKE_CURRENT_SOURCE_DIR}/data/lint_bad.mcscene")
set_tests_properties(scene_lint_cli_exit_code PROPERTIES WILL_FAIL TRUE)
add_test(NAME scene_lint_cli_clean COMMAND mce_scene_lint "${CMAKE_CURRENT_SOURCE_DIR}/data/lint_clean.mcscene")
set_tests_properties(scene_lint_cli_clean PROPERTIES PASS_REGULAR_EXPRESSION "no findings")

# mce_scene_stats (M-SCENESTATS 2): the report over a fixture scene, and the heading when there are several.
add_test(NAME scene_stats_cli_report COMMAND mce_scene_stats "${CMAKE_CURRENT_SOURCE_DIR}/data/lint_bad.mcscene")
set_tests_properties(scene_stats_cli_report PROPERTIES
    PASS_REGULAR_EXPRESSION "entities: 3 \\(3 roots, depth 0\\).*bounds: \\(0, 0, 0\\) to \\(4, 0, 0\\).*LocalTransform: 3")
add_test(NAME scene_stats_cli_several COMMAND mce_scene_stats "${CMAKE_CURRENT_SOURCE_DIR}/data/lint_clean.mcscene" "${CMAKE_CURRENT_SOURCE_DIR}/data/lint_bad.mcscene")
set_tests_properties(scene_stats_cli_several PROPERTIES PASS_REGULAR_EXPRESSION "lint_clean.mcscene:.*entities: 2.*lint_bad.mcscene:.*entities: 3")

# mce_scene_find (M-SCENEFIND 2): a name part, a component, no match (exit 1) and an unknown component (exit 2).
add_test(NAME scene_find_cli_name COMMAND mce_scene_find "${CMAKE_CURRENT_SOURCE_DIR}/data/lint_bad.mcscene" --name WALL)
set_tests_properties(scene_find_cli_name PROPERTIES PASS_REGULAR_EXPRESSION "Wall \\(id 1\\).*Wall \\(id 2\\)")
add_test(NAME scene_find_cli_component COMMAND mce_scene_find "${CMAKE_CURRENT_SOURCE_DIR}/data/lint_clean.mcscene" "${CMAKE_CURRENT_SOURCE_DIR}/data/lint_bad.mcscene" --component Camera2DComponent)
set_tests_properties(scene_find_cli_component PROPERTIES PASS_REGULAR_EXPRESSION "lint_clean.mcscene: Camera \\(id 1\\)")
add_test(NAME scene_find_cli_none COMMAND mce_scene_find "${CMAKE_CURRENT_SOURCE_DIR}/data/lint_bad.mcscene" --name Camera)
set_tests_properties(scene_find_cli_none PROPERTIES WILL_FAIL TRUE)
add_test(NAME scene_find_cli_unknown_component COMMAND mce_scene_find "${CMAKE_CURRENT_SOURCE_DIR}/data/lint_bad.mcscene" --component NoSuchThing)
set_tests_properties(scene_find_cli_unknown_component PROPERTIES WILL_FAIL TRUE)

# mce_scene_fmt (M-SCENEFMT 1): report, rewrite and re-check a copy of a scene with a default field spelled out;
# a file that does not exist is exit 2.
add_test(NAME scene_fmt_cli_roundtrip
    COMMAND ${CMAKE_COMMAND} -DFMT=$<TARGET_FILE:mce_scene_fmt> -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/data/fmt_loose.mcscene
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/scene_fmt -P ${CMAKE_CURRENT_SOURCE_DIR}/scene_fmt_roundtrip.cmake)
add_test(NAME scene_fmt_cli_unreadable COMMAND mce_scene_fmt "${CMAKE_CURRENT_SOURCE_DIR}/data/no_such_scene.mcscene")
set_tests_properties(scene_fmt_cli_unreadable PROPERTIES WILL_FAIL TRUE)

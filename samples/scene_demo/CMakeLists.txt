# scene_demo: an editor project (assets/ + scenes/*.mcscene, no code). The editor opens this
# directory; mce_player plays its cooked pack. The build cooks a copy of it (like mce_sample_pack)
# so CI exercises the whole authored-scene path: .mcscene -> cook -> mount -> mce_scene -> World.
set(project_dir "${CMAKE_CURRENT_BINARY_DIR}/project")
set(pack "${project_dir}/assets.mce")
file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/*")
add_custom_command(
    OUTPUT "${pack}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${project_dir}/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets" "${project_dir}/assets"
    COMMAND $<TARGET_FILE:mce_cook> "${project_dir}" --pack --quiet
    DEPENDS mce_cook ${sources}
    COMMENT "Cooking scene_demo content"
    VERBATIM)
add_custom_target(scene_demo_content ALL DEPENDS "${pack}")
add_dependencies(scene_demo_content mce_player)

if(MCE_BUILD_TESTS)
    add_test(NAME scene_demo_player_600 COMMAND mce_player -headless 1 -ticks 600 -pack "${pack}")
    set_tests_properties(scene_demo_player_600 PROPERTIES PASS_REGULAR_EXPRESSION "600 ticks, 6 entities, state_hash [0-9a-f]+")
    if(MCE_GPU_TESTS)
        add_test(NAME scene_demo_player_gpu
                 COMMAND mce_player -pack "${pack}" -frames 90 -platform.error_box 0 -r.vsync 0)
        set_tests_properties(scene_demo_player_gpu PROPERTIES LABELS gpu)
        if(TARGET mce_editor)
            # The editor in scene mode over the cooked copy: preview world in sync, sprites drawn
            # in the viewport, picking hits (mce_editor's scene selftest).
            add_test(NAME editor_scene_smoke
                     COMMAND mce_editor -project "${project_dir}" -scene scenes/demo.mcscene -editor.select Ball -frames 90 -selftest
                             -editor.ini "" -platform.error_box 0 -r.vsync 0)
            set_tests_properties(editor_scene_smoke PROPERTIES LABELS gpu)
            # Play-in-editor (M5b, FR-EDT-060): the scene runs in the viewport, the document stays.
            add_test(NAME editor_play_smoke
                     COMMAND mce_editor -project "${project_dir}" -scene scenes/demo.mcscene -editor.play 1 -frames 90 -selftest
                             -editor.ini "" -platform.error_box 0 -r.vsync 0)
            set_tests_properties(editor_play_smoke PROPERTIES LABELS gpu)
        endif()
    endif()
endif()

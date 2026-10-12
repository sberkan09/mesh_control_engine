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
    # -input: the scripted input stands in for the keyboard (parsed, logged, published every tick).
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/input_script.txt" "# scripted input\n10-70 Move=1,0 Jump\n100 Fire\n")
    add_test(NAME scene_demo_player_input COMMAND mce_player -headless 1 -ticks 120 -pack "${pack}"
                                                  -input "${CMAKE_CURRENT_BINARY_DIR}/input_script.txt")
    set_tests_properties(scene_demo_player_input PROPERTIES PASS_REGULAR_EXPRESSION "3 segments, last tick 100.*120 ticks, 6 entities, state_hash [0-9a-f]+")
    # FR-RUN-100 (M-MOBILE): a trip to the background pauses and resumes with an unchanged state
    # hash and writes the save inside the background callback.
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_test(NAME scene_demo_player_lifecycle
                 COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/lifecycle_check.py $<TARGET_FILE:mce_player>
                         "${pack}" "${CMAKE_CURRENT_BINARY_DIR}/lifecycle_check")
    endif()
    if(MCE_GPU_TESTS)
        add_test(NAME scene_demo_player_gpu
                 COMMAND mce_player -pack "${pack}" -frames 90 -platform.error_box 0 -r.vsync 0)
        set_tests_properties(scene_demo_player_gpu PROPERTIES LABELS gpu)
        # The shipped developer console open over the scene (FR-UI-045): GL debug layer, 0 errors.
        add_test(NAME scene_demo_player_console_gpu
                 COMMAND mce_player -pack "${pack}" -frames 30 -console_open 1 -platform.error_box 0 -r.vsync 0)
        set_tests_properties(scene_demo_player_console_gpu PROPERTIES LABELS gpu)
        # The phone lifecycle rehearsed windowed (FR-RUN-100): the platform delivers the events to
        # the window, app ticks 30..59 draw nothing, then the scene runs to sim tick 120 and quits.
        # -pause_on_focus_loss 0: under ctest -j the GPU tests share one X display, and another test's
        # window taking focus would pause this player for good (it then never reaches tick 120).
        add_test(NAME scene_demo_player_lifecycle_gpu
                 COMMAND mce_player -pack "${pack}" -capture_tick 120 -lifecycle_test 30,30 -pause_on_focus_loss 0
                         -platform.error_box 0 -r.vsync 0)
        set_tests_properties(scene_demo_player_lifecycle_gpu PROPERTIES LABELS gpu
                             PASS_REGULAR_EXPRESSION "in the background.*back in the foreground.*frames, 12[0-9] ticks, 0 GL errors")
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

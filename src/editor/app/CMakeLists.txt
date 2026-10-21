# mce_editor (L6): the docking editor executable — a thin main over mce_editor_layer.
mce_add_executable(mce_editor UMBRELLA SOURCES main.cpp DEPS mce::mce_editor_layer mce::mce_rhi_default mce::mce_module_host)

if(MCE_BUILD_TESTS AND MCE_GPU_TESTS)
    # Windowed smoke run: the shell builds its default layout, the viewport shows the stub world,
    # and -selftest checks both from read-back pixels. GL debug layer on; exit 0 required.
    add_test(NAME editor_smoke
             COMMAND mce_editor -frames 120 -selftest -editor.ini "" -platform.error_box 0 -r.vsync 0)
    set_tests_properties(editor_smoke PROPERTIES LABELS gpu)
    # M-PERF step 4: -editor.perf_capture opens a player's .mcperf in the Profiler's Capture tab.
    add_test(NAME editor_profiler_capture
             COMMAND ${CMAKE_COMMAND} -DMCE_COOK=$<TARGET_FILE:mce_cook> -DMCE_PLAYER=$<TARGET_FILE:mce_player>
                     -DMCE_EDITOR=$<TARGET_FILE:mce_editor> -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/profiler_test
                     -P ${PROJECT_SOURCE_DIR}/tests/perf/editor_profiler_test.cmake)
    set_tests_properties(editor_profiler_capture PROPERTIES LABELS gpu)
endif()

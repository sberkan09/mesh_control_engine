# mce_editor (L6): the docking editor executable — a thin main over mce_editor_layer.
mce_add_executable(mce_editor SOURCES main.cpp DEPS mce::mce_editor_layer mce::mce_rhi_gl)

if(MCE_BUILD_TESTS AND MCE_GPU_TESTS)
    # Windowed smoke run: the shell builds its default layout, the viewport shows the stub world,
    # and -selftest checks both from read-back pixels. GL debug layer on; exit 0 required.
    add_test(NAME editor_smoke
             COMMAND mce_editor -frames 120 -selftest -editor.ini "" -platform.error_box 0 -r.vsync 0)
    set_tests_properties(editor_smoke PROPERTIES LABELS gpu)
endif()

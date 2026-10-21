mce_add_executable(ui_gallery SOURCES main.cpp DEPS mce::mce_ui mce::mce_platform mce::mce_render mce::mce_rhi_default)
set_target_properties(ui_gallery PROPERTIES OUTPUT_NAME "ui_gallery")

if(MCE_BUILD_TESTS AND MCE_GPU_TESTS)
    # Windowed smoke run of each page, GL debug layer on: exit 0, zero GL errors.
    foreach(page title options levels console)
        add_test(NAME sample_ui_gallery_${page} COMMAND ui_gallery -page ${page} -frames 30 -platform.error_box 0 -r.vsync 0)
        set_tests_properties(sample_ui_gallery_${page} PROPERTIES LABELS gpu)
    endforeach()
endif()

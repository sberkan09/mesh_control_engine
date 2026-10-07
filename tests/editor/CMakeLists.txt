# Headless editor tests: editor_core logic and the layer's ImGui-free pieces. No window, no GPU.
mce_add_test(test_editor
    SOURCES
        test_console_model.cpp
        test_draw_clip.cpp
        test_stub_world.cpp
    DEPS
        mce::mce_editor_layer)
target_include_directories(test_editor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

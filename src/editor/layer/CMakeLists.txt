# mce_editor_layer (L5): the only ImGui consumer (ARCHITECTURE §1.2 #3, editor-tools.md §6).
# Its ImGui backends are our own: platform input from mce::platform events (SDL3 only through
# Window::native_handle(), for cursors/clipboard/text input) and rendering through mce_rhi, so the
# editor never touches GL.

mce_add_library(mce_editor_layer
    SOURCES
        src/asset_panel.cpp
        src/draw_clip.cpp
        src/editor_shell.cpp
        src/imgui_draw_ctx.cpp
        src/imgui_platform.cpp
        src/imgui_renderer.cpp
        src/panels.cpp
        src/play_panel.cpp
        src/scene_panels.cpp
        src/tilemap_panel.cpp
    PUBLIC_DEPS
        mce::mce_editor_core
        mce::mce_platform
    PRIVATE_DEPS
        mce::imgui
        SDL3::SDL3-static)

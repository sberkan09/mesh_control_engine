# mce_editor_layer (L5): the only ImGui consumer (ARCHITECTURE §1.2 #3, editor-tools.md §6).
# Its ImGui backends are our own: platform input from mce::platform events (SDL3 only through
# Window::native_handle(), for cursors/clipboard/text input) and rendering through mce_rhi, so the
# editor never touches GL.

# SDL3 is already inside the umbrella (mce_platform links it static); linking it again here
# would make a second SDL. On the umbrella the layer takes SDL's headers only.
if(MCE_GAME_MODULES_SUPPORTED)
    set(_mce_layer_sdl "$<COMPILE_ONLY:SDL3::SDL3-static>")
else()
    set(_mce_layer_sdl SDL3::SDL3-static)
endif()
mce_add_library(mce_editor_layer UMBRELLA
    SOURCES
        src/animation_panel.cpp
        src/bt_panel.cpp
        src/asset_panel.cpp
        src/draw_clip.cpp
        src/editor_shell.cpp
        src/imgui_draw_ctx.cpp
        src/imgui_platform.cpp
        src/imgui_renderer.cpp
        src/panels.cpp
        src/play_panel.cpp
        src/profiler_panel.cpp
        src/project_settings_panel.cpp
        src/scene_panels.cpp
        src/skeleton_panel.cpp
        src/terrain_panel.cpp
        src/tilemap_panel.cpp
        src/timeline_panel.cpp
        src/ui_panel.cpp
    PUBLIC_DEPS
        mce::mce_editor_core
        mce::mce_platform
    PRIVATE_DEPS
        mce::imgui
        ${_mce_layer_sdl})

# mce_editor_core (L5): headless editor logic (editor-tools.md §2.1). No ImGui, no window:
# CI builds and tests it on every leg. It sees the RHI interface only, never a backend.
#
#   SceneDocument + EditorScene   the canonical document and the mutation funnel (+ preview world)
#   inspector                     reflection-driven inspector over the DrawCtx widget seam
#   EditorProject                 project folder, GUID catalog, cook + mount
#   viewport2d / PreviewRenderer  editor camera, picking, preview drawing

mce_add_library(mce_editor_core UMBRELLA
    SOURCES
        src/asset_browser.cpp
        src/console_model.cpp
        src/editor_scene.cpp
        src/editor_session.cpp
        src/engine_components.cpp
        src/gizmo.cpp
        src/hot_reload.cpp
        src/inspector.cpp
        src/orbit_camera.cpp
        src/perf_monitor.cpp
        src/play_session.cpp
        src/preview_renderer.cpp
        src/project.cpp
        src/scene_document.cpp
        src/stub_world.cpp
        src/tilemap_editor.cpp
        src/viewport2d.cpp
    PUBLIC_DEPS
        mce::mce_core
        mce::mce_rhi
        mce::mce_render
        mce::mce_scene
        mce::mce_gameplay_kit
        mce::mce_asset_cook
        mce::mce_physics2d
        mce::mce_physics3d
        mce::mce_audio)

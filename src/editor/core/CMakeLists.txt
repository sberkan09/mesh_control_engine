# mce_editor_core (L5): headless editor logic (editor-tools.md §2.1). No ImGui, no window:
# CI builds and tests it on every leg. It sees the RHI interface only, never a backend.
#
#   SceneDocument + EditorScene   the canonical document and the mutation funnel (+ preview world)
#   inspector                     reflection-driven inspector over the DrawCtx widget seam
#   EditorProject                 project folder, GUID catalog, cook + mount
#   viewport2d / PreviewRenderer  editor camera, picking, preview drawing

mce_add_library(mce_editor_core UMBRELLA
    SOURCES
        src/animgraph_editor.cpp
        src/asset_browser.cpp
        src/asset_thumb.cpp
        src/asset_form.cpp
        src/scene_tabs.cpp
        src/thumb_cache.cpp
        src/bt_editor.cpp
        src/console_model.cpp
        src/editor_scene.cpp
        src/editor_session.cpp
        src/engine_components.cpp
        src/gizmo.cpp
        src/hot_reload.cpp
        src/inspector.cpp
        src/orbit_camera.cpp
        src/perf_compare.cpp
        src/perf_file.cpp
        src/perf_monitor.cpp
        src/play_session.cpp
        src/preview_renderer.cpp
        src/project.cpp
        src/project_settings.cpp
        src/scene_document.cpp
        src/skeleton2d_editor.cpp
        src/stub_world.cpp
        src/terrain_editor.cpp
        src/tilemap_editor.cpp
        src/timeline_editor.cpp
        src/ui_document_editor.cpp
        src/viewport2d.cpp
    PUBLIC_DEPS
        mce::mce_core
        mce::mce_rhi
        mce::mce_render
        mce::mce_scene
        mce::mce_gameplay_kit
        mce::mce_kit_novel  # its components (engine_components.cpp)
        mce::mce_asset_cook
        mce::mce_physics2d
        mce::mce_physics3d
        mce::mce_audio
        mce::mce_ui)

# Scene scripts (M-SCRIPT 1): kit::ScriptSystem components run in this host where scripting is on.
if(MCE_WITH_SCRIPT)
    target_link_libraries(mce_editor_core PRIVATE mce::mce_script)
endif()

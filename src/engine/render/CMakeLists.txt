# mce_render (L3): the renderer — the 2D path (sprite batch, sort keys, Camera2D, tilemaps, text)
# and the 3D path (mesh store, Scene3D cull/sort/cascades, Forward+ clusters, Forward3D passes),
# texture upload. Links mce_rhi only: no GL symbols here (the GL wall). Cooked mesh/texture formats come from mce_asset
# (include direction render -> asset, fix F6).

mce_add_library(mce_render
    SOURCES
        src/accessibility.cpp
        src/asset_upload.cpp
        src/atmosphere.cpp
        src/camera2d.cpp
        src/clusters.cpp
        src/forward3d.cpp
        src/forward3d_scale.cpp
        src/mesh.cpp
        src/quality.cpp
        src/renderer.cpp
        src/scene3d.cpp
        src/sort_key.cpp
        src/sprite_animation.cpp
        src/sprite_batch.cpp
        src/terrain.cpp
        src/text.cpp
        src/texture_store.cpp
        src/types.cpp
        src/video.cpp
        src/plmpeg_impl.c
    PUBLIC_DEPS
        mce::mce_rhi
        mce::mce_asset
        mce::mce_core)
target_compile_definitions(mce_render PUBLIC MCE_RENDER_HAS_ASSET=1)
# pl_mpeg (ADR-0042): its one header, private to src/video.cpp and src/plmpeg_impl.c (the pl_mpeg
# wall); the implementation compiles with warnings off, as third-party code does here.
target_include_directories(mce_render SYSTEM PRIVATE "${pl_mpeg_SOURCE_DIR}")
target_compile_definitions(mce_render PRIVATE PLM_NO_STDIO)  # memory sources only
if(MSVC)
    set_source_files_properties(src/plmpeg_impl.c PROPERTIES COMPILE_OPTIONS "/W0")
else()
    set_source_files_properties(src/plmpeg_impl.c PROPERTIES COMPILE_OPTIONS "-w")
endif()

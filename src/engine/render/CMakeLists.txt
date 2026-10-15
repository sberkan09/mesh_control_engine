# mce_render (L3): the renderer — the 2D path (sprite batch, sort keys, Camera2D, tilemaps, text)
# and the 3D path (mesh store, Scene3D cull/sort/cascades, Forward+ clusters, Forward3D passes),
# texture upload. Links mce_rhi only: no GL symbols here (the GL wall). Cooked mesh/texture formats come from mce_asset
# (include direction render -> asset, fix F6).

mce_add_library(mce_render
    SOURCES
        src/asset_upload.cpp
        src/atmosphere.cpp
        src/camera2d.cpp
        src/clusters.cpp
        src/forward3d.cpp
        src/mesh.cpp
        src/quality.cpp
        src/renderer.cpp
        src/scene3d.cpp
        src/sort_key.cpp
        src/sprite_animation.cpp
        src/sprite_batch.cpp
        src/text.cpp
        src/texture_store.cpp
        src/types.cpp
    PUBLIC_DEPS
        mce::mce_rhi
        mce::mce_asset
        mce::mce_core)
target_compile_definitions(mce_render PUBLIC MCE_RENDER_HAS_ASSET=1)

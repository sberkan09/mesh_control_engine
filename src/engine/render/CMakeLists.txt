# mce_render (L3): the renderer — 2D path today (sprite batch, sort keys, Camera2D, tilemaps,
# text), texture upload. Links mce_rhi only: no GL symbols here (the GL wall).

mce_add_library(mce_render
    SOURCES
        src/camera2d.cpp
        src/renderer.cpp
        src/sort_key.cpp
        src/sprite_animation.cpp
        src/sprite_batch.cpp
        src/text.cpp
        src/texture_store.cpp
        src/types.cpp
    PUBLIC_DEPS
        mce::mce_rhi
        mce::mce_core)

# The IGpuUploadSink implementation (render -> asset include direction, fix F6) builds once
# mce_asset is part of the tree.
if(TARGET mce_asset)
    target_sources(mce_render PRIVATE src/asset_upload.cpp)
    target_link_libraries(mce_render PUBLIC mce::mce_asset)
    target_compile_definitions(mce_render PUBLIC MCE_RENDER_HAS_ASSET=1)
endif()

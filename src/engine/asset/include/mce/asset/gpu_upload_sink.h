#pragma once
// IGpuUploadSink — the one call-shaped seam between assets and the renderer, inverted
// (ARCHITECTURE fix F6, assets.md §7.1). Defined here, implemented by mce_render and registered
// at Renderer::init; the include direction stays mce_render -> mce_asset. The asset system
// produces cooked bytes and calls this from the main thread only (load/commit/reap points).

#include "mce/asset/container.h"

namespace mce::asset {

using AssetKey = u32;

struct RetiredAsset {
    AssetKey key;
    u32 generation;  // the generation whose GPU objects are being destroyed
    ChunkType type;
};

class IGpuUploadSink {
public:
    virtual ~IGpuUploadSink() = default;
    virtual Result<void> upload_mesh(AssetKey key, Span<const byte> mesh_blob) = 0;
    virtual Result<void> upload_texture(AssetKey key, Span<const byte> tex_blob, u32 format_flags) = 0;
    virtual Result<void> update_material(AssetKey key, Span<const byte> mat_blob) = 0;
    virtual void destroy_gpu_objects(Span<const RetiredAsset> retired) = 0;  // post-fence reap
};

}  // namespace mce::asset

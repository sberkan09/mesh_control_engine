#pragma once
// The renderer's side of the assets <-> renderer seam (ARCHITECTURE fix F6, assets.md §7.1):
// AssetUploadSink implements mce_asset's IGpuUploadSink, so cooked CHNK_TEX payloads become GPU
// textures keyed by AssetKey, hot reloads land as a new generation, and retired generations are
// destroyed once the asset system's frame slot comes round. Include direction: render -> asset.
//
//   render::AssetUploadSink sink(renderer, assets);
//   assets.set_upload_sink(&sink);
//   ... sprite sets reference cooked textures by AssetKey (SpriteSetDesc::texture_key).
//
// Only built when mce_asset is part of the build (MCE_RENDER_HAS_ASSET).

#include "mce/asset/asset_system.h"
#include "mce/asset/formats.h"
#include "mce/asset/gpu_upload_sink.h"
#include "mce/render/renderer.h"

namespace mce::render {

class AssetUploadSink final : public asset::IGpuUploadSink {
public:
    AssetUploadSink(Renderer& renderer, const asset::AssetSystem& assets) : renderer_(renderer), assets_(assets) {}

    Result<void> upload_mesh(asset::AssetKey key, Span<const byte> mesh_blob) override;
    Result<void> upload_texture(asset::AssetKey key, Span<const byte> tex_blob, u32 format_flags) override;
    Result<void> update_material(asset::AssetKey key, Span<const byte> mat_blob) override;
    void destroy_gpu_objects(Span<const asset::RetiredAsset> retired) override;

private:
    Renderer& renderer_;
    const asset::AssetSystem& assets_;
};

// The one mapping from a cooked texture's format (and Srgb flag) to the RHI format: RGBA8, ASTC 4x4, RGBA16F, and the
// BC1/BC4/BC5/BC7 block formats (M-CONTENT 1b). BC4 and BC5 are linear data and have no sRGB variant.
rhi::TextureFormat rhi_format_for(asset::TextureFormat format, bool srgb);

// Uploads a cooked texture payload as `generation` of `key` (what upload_texture does, minus the
// generation lookup). Validates the chunk; never trusts its sizes.
Result<rhi::TextureHandle> upload_cooked_texture(TextureStore& store, u32 key, u32 generation, Span<const byte> tex_blob);

// Builds a drawable Font from a cooked CHNK_FONT payload (tables copied, atlas uploaded).
Result<Font> load_cooked_font(Renderer& renderer, Span<const byte> font_blob, const char* debug_name = "font");

}  // namespace mce::render

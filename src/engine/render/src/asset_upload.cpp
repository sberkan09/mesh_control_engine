#include "mce/render/asset_upload.h"

#include <cstring>

#include "mce/log.h"

namespace mce::render {

static_assert(sizeof(asset::FontGlyph) == sizeof(FontGlyph) && sizeof(asset::FontKern) == sizeof(FontKern),
              "render and asset font records must stay layout-identical");

Result<rhi::TextureHandle> upload_cooked_texture(TextureStore& store, u32 key, u32 generation, Span<const byte> blob) {
    MCE_TRY_ASSIGN(asset::TextureView view, asset::TextureView::read(blob));
    const asset::TextureHeader& h = *view.header;
    if (view.mips.size() > 16) return Error(Errc::Unsupported, "render.texture.mips", {}, static_cast<u32>(view.mips.size()));
    TextureMipUpload mips[16];
    for (usize m = 0; m < view.mips.size(); ++m) {
        const asset::TextureMip& mip = view.mips[m];
        const u64 bytes = mip.layer_size * h.layers;
        if (mip.offset > view.payload.size() || bytes > view.payload.size() - mip.offset)
            return Error(Errc::Truncated, "render.texture.mip", {}, static_cast<u32>(m));
        mips[m] = {mip.width, mip.height, view.payload.subspan(static_cast<usize>(mip.offset), static_cast<usize>(bytes))};
    }
    TextureUpload up;
    const bool srgb = (h.flags & asset::TextureFlags::Srgb) != 0;
    if (h.format == asset::TextureFormat::ASTC_4x4)
        up.format = srgb ? rhi::TextureFormat::ASTC_4x4_SRGB : rhi::TextureFormat::ASTC_4x4_UNorm;
    else
        up.format = srgb ? rhi::TextureFormat::RGBA8_SRGB : rhi::TextureFormat::RGBA8_UNorm;
    up.layers = h.layers;
    up.array = true;  // the sprite path samples sampler2DArray; one-layer textures are 1-layer arrays
    up.mips = Span<const TextureMipUpload>(mips, view.mips.size());
    up.debug_name = "asset.texture";
    return store.upload(key, generation, up);
}

Result<void> AssetUploadSink::upload_texture(asset::AssetKey key, Span<const byte> blob, u32 /*format_flags*/) {
    // The asset system bumps the generation before uploading, so this is the generation the
    // matching destroy_gpu_objects() will name.
    MCE_TRY(upload_cooked_texture(renderer_.textures(), key, assets_.generation(key), blob));
    return ok();
}

Result<void> AssetUploadSink::upload_mesh(asset::AssetKey key, Span<const byte> blob) {
    // Base color textures resolve by GUID to their AssetKey; the host loads them (a mesh does not
    // own its textures: kit pieces share one colormap).
    TextureKeyResolver resolve;
    resolve.ctx = const_cast<asset::AssetSystem*>(&assets_);
    resolve.fn = [](void* ctx, asset::Guid g) -> u32 { return static_cast<const asset::AssetSystem*>(ctx)->find(g); };
    return renderer_.meshes().upload(key, assets_.generation(key), blob, resolve);
}

Result<void> AssetUploadSink::update_material(asset::AssetKey, Span<const byte>) {
    return ok();  // 2D sprites carry everything in vertices; 3D materials arrive at M4
}

void AssetUploadSink::destroy_gpu_objects(Span<const asset::RetiredAsset> retired) {
    for (const asset::RetiredAsset& r : retired)
        if (r.type == asset::ChunkType::Texture) renderer_.textures().retire(r.key, r.generation);
        else if (r.type == asset::ChunkType::Mesh) renderer_.meshes().retire(r.key, r.generation);
}

Result<Font> load_cooked_font(Renderer& renderer, Span<const byte> blob, const char* debug_name) {
    MCE_TRY_ASSIGN(asset::FontView view, asset::FontView::read(blob));
    const asset::FontHeader& h = *view.header;
    FontMetrics m;
    m.pixel_height = h.pixel_height;
    m.ascent = h.ascent;
    m.descent = h.descent;
    m.line_gap = h.line_gap;
    m.atlas_width = h.atlas_width;
    m.atlas_height = h.atlas_height;
    Array<FontGlyph> glyphs;
    glyphs.resize(view.glyphs.size());
    if (!glyphs.empty()) std::memcpy(glyphs.data(), view.glyphs.data(), glyphs.size() * sizeof(FontGlyph));
    Array<FontKern> kerning;
    kerning.resize(view.kerning.size());
    if (!kerning.empty()) std::memcpy(kerning.data(), view.kerning.data(), kerning.size() * sizeof(FontKern));
    return renderer.create_font(m, glyphs.span(), kerning.span(), view.atlas, debug_name);
}

}  // namespace mce::render

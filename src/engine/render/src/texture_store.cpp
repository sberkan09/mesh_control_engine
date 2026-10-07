#include "mce/render/texture_store.h"

#include "mce/log.h"
#include "mce/math/math.h"

namespace mce::render {

namespace {

template <class T>
void remove_ordered(Array<T>& a, usize i) {
    for (usize j = i + 1; j < a.size(); ++j) a[j - 1] = a[j];
    a.pop_back();
}

usize level_bytes(rhi::TextureFormat f, u32 w, u32 h) {
    if (rhi::is_compressed_format(f)) {
        const usize block = f == rhi::TextureFormat::BC1_UNorm ? 8 : 16;
        return usize((w + 3) / 4) * ((h + 3) / 4) * block;
    }
    return usize(w) * h * rhi::bytes_per_pixel(f);
}

}  // namespace

Result<rhi::TextureHandle> create_texture(rhi::RenderDevice& device, const TextureUpload& up) {
    const char* name = up.debug_name ? up.debug_name : "";
    if (up.mips.empty()) return Error(Errc::InvalidArgument, "render.texture.upload", name);
    if (rhi::is_depth_format(up.format)) return Error(Errc::Unsupported, "render.texture.format", name);
    const u32 w = up.mips[0].width, h = up.mips[0].height;
    const u32 max_size = device.caps().max_texture_size ? device.caps().max_texture_size : 16384u;
    if (w == 0 || h == 0 || w > max_size || h > max_size)
        return Error(Errc::InvalidArgument, "render.texture.size", name, w > h ? w : h);
    if (up.layers == 0 || up.layers > 2048 || (!up.array && up.layers != 1))
        return Error(Errc::InvalidArgument, "render.texture.layers", name, up.layers);
    if (up.mips.size() > 16) return Error(Errc::InvalidArgument, "render.texture.mips", name, static_cast<u32>(up.mips.size()));
    for (usize m = 0; m < up.mips.size(); ++m) {
        const u32 ew = math::max(1u, w >> m), eh = math::max(1u, h >> m);
        const TextureMipUpload& mip = up.mips[m];
        if (mip.width != ew || mip.height != eh)
            return Error(Errc::InvalidArgument, "render.texture.mip_extent", name, static_cast<u32>(m));
        if (mip.pixels.size() < level_bytes(up.format, ew, eh) * up.layers)
            return Error(Errc::Truncated, "render.texture.mip_bytes", name, static_cast<u32>(m));
    }

    rhi::TextureDesc desc;
    desc.size = {w, h};
    desc.mip_levels = static_cast<u16>(up.mips.size());
    desc.array_layers = static_cast<u16>(up.layers);
    desc.dimension = up.array ? rhi::TextureDimension::Tex2DArray : rhi::TextureDimension::Tex2D;
    desc.format = up.format;
    desc.debug_name = name;
    const rhi::TextureHandle tex = device.create_texture(desc, up.mips[0].pixels);
    for (usize m = 1; m < up.mips.size(); ++m) {
        const TextureMipUpload& mip = up.mips[m];
        const usize layer_bytes = level_bytes(up.format, mip.width, mip.height);
        for (u32 l = 0; l < up.layers; ++l) {
            rhi::TextureRegion r;
            r.mip = static_cast<u32>(m);
            r.layer = l;
            device.update_texture(tex, r, mip.pixels.subspan(layer_bytes * l, layer_bytes));
        }
    }
    return tex;
}

TextureStore::~TextureStore() { shutdown(); }

ResultV TextureStore::init(rhi::RenderDevice& device) {
    device_ = &device;
    static constexpr u8 kWhite[4] = {255, 255, 255, 255};
    const TextureMipUpload white_mip{1, 1, std::as_bytes(Span<const u8>(kWhite))};
    TextureUpload up;
    up.mips = Span<const TextureMipUpload>(&white_mip, 1);
    up.debug_name = "mce.white";
    MCE_TRY_ASSIGN(white_, create_texture(device, up));

    // 8×8 magenta/black checker of 2×2 cells (FR-DX-050's "missing texture" look).
    u8 checker[8 * 8 * 4];
    for (u32 y = 0; y < 8; ++y)
        for (u32 x = 0; x < 8; ++x) {
            u8* p = checker + (y * 8 + x) * 4;
            const bool on = ((x / 2) + (y / 2)) % 2 == 0;
            p[0] = on ? 255 : 0;
            p[1] = 0;
            p[2] = on ? 255 : 0;
            p[3] = 255;
        }
    const TextureMipUpload checker_mip{8, 8, std::as_bytes(Span<const u8>(checker))};
    up.mips = Span<const TextureMipUpload>(&checker_mip, 1);
    up.debug_name = "mce.error_checker";
    MCE_TRY_ASSIGN(error_, create_texture(device, up));
    return ok();
}

void TextureStore::shutdown() {
    if (!device_) return;
    for (const Entry& e : entries_) device_->destroy_texture(e.texture);
    entries_.clear();
    device_->destroy_texture(white_);
    device_->destroy_texture(error_);
    white_ = error_ = {};
    device_ = nullptr;
}

Result<rhi::TextureHandle> TextureStore::upload(u32 key, u32 generation, const TextureUpload& up) {
    MCE_ASSERT_MSG(device_, "TextureStore::upload before init");
    if (!device_) return Error(Errc::InvalidArgument, "render.texture.upload", "store not initialized");
    auto tex = create_texture(*device_, up);
    if (!tex) {
        MCE_LOG_WARN("render: texture upload failed for key {} ({} at {})", key, errc_name(tex.error().code),
                     tex.error().stage);
        return tex.error();
    }
    // Re-uploading a generation that is still live replaces it.
    for (usize i = 0; i < entries_.size(); ++i) {
        if (entries_[i].key == key && entries_[i].generation == generation) {
            device_->destroy_texture(entries_[i].texture);
            remove_ordered(entries_, i);
            break;
        }
    }
    entries_.push_back(Entry{key, generation, tex.value()});
    return tex.value();
}

void TextureStore::retire(u32 key, u32 generation) {
    if (!device_) return;
    for (usize i = 0; i < entries_.size(); ++i) {
        if (entries_[i].key == key && entries_[i].generation == generation) {
            device_->destroy_texture(entries_[i].texture);
            remove_ordered(entries_, i);  // keep newest-last order per key
            return;
        }
    }
}

void TextureStore::release(u32 key) {
    if (!device_) return;
    usize out = 0;
    for (usize i = 0; i < entries_.size(); ++i) {
        if (entries_[i].key == key) {
            device_->destroy_texture(entries_[i].texture);
        } else {
            entries_[out++] = entries_[i];
        }
    }
    entries_.resize(out);
}

rhi::TextureHandle TextureStore::get(u32 key) const {
    for (usize i = entries_.size(); i-- > 0;)
        if (entries_[i].key == key) return entries_[i].texture;
    return error_;
}

bool TextureStore::contains(u32 key) const {
    for (const Entry& e : entries_)
        if (e.key == key) return true;
    return false;
}

u32 TextureStore::live_textures() const { return static_cast<u32>(entries_.size()); }

}  // namespace mce::render

#pragma once
// TextureStore — texture GPU upload and residency bookkeeping for mce_render.
//
// Textures are addressed by a stable u32 key (the asset system's AssetKey for cooked textures,
// or a key from allocate_key() for renderer-owned ones). Each key can hold a few generations at
// once: a hot reload uploads generation N+1 while frames in flight still sample N, and the asset
// system retires N once its frame slot comes round (assets.md §7.1, IGpuUploadSink). get(key)
// always answers the newest generation, so draw code never holds an RHI handle across frames.
//
// Missing or failed textures resolve to the magenta checker (FR-DX-050): never a crash.

#include "mce/containers/array.h"
#include "mce/result.h"
#include "mce/rhi/render_device.h"

namespace mce::render {

// One mip level: every layer's pixels, layers contiguous (the cooked CHNK_TEX layout).
struct TextureMipUpload {
    u32 width = 0;
    u32 height = 0;
    Span<const byte> pixels;
};

struct TextureUpload {
    rhi::TextureFormat format = rhi::TextureFormat::RGBA8_UNorm;
    u32 layers = 1;
    bool array = true;  // create a GL_TEXTURE_2D_ARRAY even for one layer (the sprite path samples arrays)
    Span<const TextureMipUpload> mips;  // mip 0 first
    const char* debug_name = "";
};

class TextureStore {
public:
    static constexpr u32 kFirstLocalKey = 0x80000000u;  // keys below belong to the asset system

    TextureStore() = default;
    TextureStore(const TextureStore&) = delete;
    TextureStore& operator=(const TextureStore&) = delete;
    ~TextureStore();

    // Creates the default textures (1×1 white array, magenta-checker array).
    ResultV init(rhi::RenderDevice& device);
    void shutdown();

    // Uploads `upload` as `generation` of `key` and makes it current. Validates sizes and formats
    // (cooked bytes are untrusted); on failure the key keeps its previous generation.
    Result<rhi::TextureHandle> upload(u32 key, u32 generation, const TextureUpload& upload);
    // Destroys one generation (the device defers the GL delete past frames in flight). Retiring
    // the current generation leaves the key resolving to the error texture.
    void retire(u32 key, u32 generation);
    // Destroys every generation of `key`.
    void release(u32 key);

    // Newest live generation of `key`, or the magenta checker.
    rhi::TextureHandle get(u32 key) const;
    bool contains(u32 key) const;

    // Keys for textures the renderer creates itself (fonts handed over as raw data, tests).
    u32 allocate_key() { return next_local_++; }

    rhi::TextureHandle white() const { return white_; }
    rhi::TextureHandle error_texture() const { return error_; }
    u32 live_textures() const;

private:
    struct Entry {
        u32 key;
        u32 generation;
        rhi::TextureHandle texture;
    };
    rhi::RenderDevice* device_ = nullptr;
    Array<Entry> entries_;  // small: one row per live (key, generation); newest generation last
    rhi::TextureHandle white_;
    rhi::TextureHandle error_;
    u32 next_local_ = kFirstLocalKey;
};

// Validates `upload` against `device` limits and creates the texture (mip 0 at creation, the
// rest through update_texture). Exposed for code that wants a raw handle without a key.
Result<rhi::TextureHandle> create_texture(rhi::RenderDevice& device, const TextureUpload& upload);

}  // namespace mce::render

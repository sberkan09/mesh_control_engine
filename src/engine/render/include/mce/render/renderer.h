#pragma once
// Renderer — the 2D path of mce_render (rhi-renderer.md §2.4, §3.2-§3.5; rendering-2d D1).
//
// There is no Renderer2D: 2D is an orthographic view plus the sprite pipeline family inside the
// one renderer. The renderer owns GPU state (pipelines, samplers, the shared quad index buffer,
// textures, baked tilemap buffers) and records built SpriteBatches; it holds no scene state.
// Extract (reading Sprite / Camera2D / TextLabel components) lives with the gameplay kit, which
// fills a SpriteBatch each frame in ECS iteration order.
//
//   SpriteBatch batch(renderer.sprite_sets(), mem::frame());
//   batch.begin(compute_view_2d(camera, viewport));
//   for (...) batch.add(sprite);
//   renderer.add_tilemap(batch, level_tiles);
//   batch.build();
//   cmd.begin_pass(main_pass);
//   renderer.record_2d(cmd, batch);
//   cmd.end_pass();

#include "mce/containers/array.h"
#include "mce/render/sprite_batch.h"
#include "mce/render/texture_store.h"
#include "mce/rhi/command_buffer.h"
#include "mce/rhi/render_device.h"

namespace mce::render {

struct RendererDesc {
    // The backbuffer and editor targets are plain RGBA8 (no GL_FRAMEBUFFER_SRGB): the sprite
    // shader encodes linear color to sRGB itself. Turn off when rendering into an sRGB target.
    bool encode_srgb_output = true;
};

struct TilemapId {
    u32 value = ~0u;
    constexpr bool valid() const { return value != ~0u; }
};

class Renderer {
public:
    Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    ~Renderer();

    ResultV init(rhi::RenderDevice& device, const RendererDesc& desc = {});
    void shutdown();
    bool initialized() const { return device_ != nullptr; }

    rhi::RenderDevice& device() { return *device_; }
    TextureStore& textures() { return textures_; }
    const TextureStore& textures() const { return textures_; }
    SpriteSets& sprite_sets() { return sets_; }
    const SpriteSets& sprite_sets() const { return sets_; }

    // ---- content
    SpriteSetId create_sprite_set(const SpriteSetDesc& desc) { return sets_.create(desc); }

    // A font from cooked tables + its R8 coverage atlas (CHNK_FONT). The tables are copied; the
    // atlas is uploaded under a renderer-local texture key.
    Result<Font> create_font(const FontMetrics& metrics, Span<const FontGlyph> glyphs, Span<const FontKern> kerning,
                             Span<const byte> atlas_r8, const char* debug_name = "font");

    // Bakes the layer and uploads its quads into a persistent vertex buffer (rendering-2d D5).
    Result<TilemapId> create_tilemap(const TilemapDesc& desc);
    // Re-bakes after an edit (editor paint loop); the old buffer is retired past frames in flight.
    ResultV update_tilemap(TilemapId id, const TilemapDesc& desc);
    void destroy_tilemap(TilemapId id);
    const TilemapBake* tilemap(TilemapId id) const;
    // Culls the layer's chunks against the batch's view and queues the visible ones.
    void add_tilemap(SpriteBatch& batch, TilemapId id) const;

    // ---- frame
    // Records a built batch into the currently open pass of `cmd`: one MDI per batch group.
    void record_2d(rhi::CommandBuffer& cmd, const SpriteBatch& batch);

    rhi::SamplerHandle sampler(rhi::Filter filter) const {
        return filter == rhi::Filter::Nearest ? point_sampler_ : linear_sampler_;
    }

private:
    struct TilemapSlot {
        TilemapBake bake;
        rhi::BufferHandle buffer;
        usize capacity = 0;
        bool alive = false;
    };
    struct FontStorage {
        Array<FontGlyph> glyphs;
        Array<FontKern> kerning;
        u32 texture_key = 0;
        SpriteSetId set;
    };

    ResultV create_pipelines();
    ResultV upload_tilemap(TilemapSlot& slot);

    rhi::RenderDevice* device_ = nullptr;
    RendererDesc desc_;
    TextureStore textures_;
    SpriteSets sets_;
    rhi::PipelineHandle pipelines_[static_cast<u32>(SpriteBlend::Count)];
    rhi::SamplerHandle point_sampler_;
    rhi::SamplerHandle linear_sampler_;
    rhi::BufferHandle quad_indices_;
    Array<TilemapSlot> tilemaps_;
    Array<FontStorage> fonts_;
};

}  // namespace mce::render

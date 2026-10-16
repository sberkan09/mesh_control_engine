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
#include "mce/render/accessibility.h"
#include "mce/render/forward3d.h"
#include "mce/render/mesh.h"
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

// 2D post effects (FR-R2D-100): one full-screen pass over the finished 2D frame, each effect a
// runtime toggle. Works in display (sRGB-encoded) space, like the 2D blending before it.
struct Post2D {
    bool vignette = false;
    f32 vignette_intensity = 0.45f;  // darkening at the corners, 0..1
    f32 vignette_radius = 0.8f;      // where darkening starts, in half-heights from the center
    f32 vignette_softness = 0.6f;    // width of the ramp
    bool color_grade = false;
    u32 lut_key = 0;                 // TextureStore key of a 256 × 16 strip LUT (16³; see make_identity_lut)
    f32 lut_strength = 1.0f;         // 0 = ungraded, 1 = fully graded
    bool chromatic_aberration = false;
    f32 aberration = 0.004f;         // red/blue UV split at the corners (fraction of the screen)
    // Color-vision correction (M-ACCESS), last in the pass; set from the player's settings by
    // apply_accessibility, never by the game.
    ColorVision color_vision = ColorVision::None;
    f32 color_vision_strength = 1.0f;
    bool any() const { return vignette || color_grade || chromatic_aberration || color_vision != ColorVision::None; }
};

// The identity 16³ color-grade LUT as a 256 × 16 RGBA8 strip: 16 slices across (blue), red
// increasing within a slice, green increasing downwards. Paint over it (or grade a screenshot of it)
// in an image editor to author a LUT.
inline constexpr u32 kLutSize = 16;
void make_identity_lut(Array<u8>& out_rgba);

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
    MeshStore& meshes() { return meshes_; }
    const MeshStore& meshes() const { return meshes_; }

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
    // Culls the layer's chunks against the batch's view and queues the visible ones; `offset`
    // translates the layer at draw time (parallax) without a re-bake.
    void add_tilemap(SpriteBatch& batch, TilemapId id, math::vec2 offset = {}) const;

    // ---- 3D (rhi-renderer.md §3.5 "MVP — 3D diorama frame"): created on first use so 2D-only
    // games never allocate the HDR/MSAA/shadow targets.
    ResultV init_3d(const Forward3DDesc& desc = {});
    bool has_3d() const { return forward_.initialized(); }
    Forward3D& forward_3d() { return forward_; }

    // ---- frame
    // Records a built batch into the currently open pass of `cmd`: one MDI per batch group.
    // `linear_output`: the pass's target holds linear color (a float render texture, M-VIEWS): no sRGB encode.
    void record_2d(rhi::CommandBuffer& cmd, const SpriteBatch& batch, bool linear_output = false);
    // Records the 2D post pass (FR-R2D-100): samples `source` (the finished 2D frame, a sampled
    // color target of `size`) and writes `target` (null = backbuffer). Outside any pass. With no
    // effect enabled it is a plain copy.
    void record_post_2d(rhi::CommandBuffer& cmd, rhi::TextureHandle source, rhi::Extent2D size, const Post2D& post,
                        rhi::TextureHandle target = {});
    // Records a built Scene3D (shadow, forward HDR, resolve, tonemap) into `target` (null =
    // backbuffer). Outside any pass; init_3d() first.
    void record_3d(rhi::CommandBuffer& cmd, const Scene3D& scene, rhi::TextureHandle target = {});
    // M-VIEWS: one view of a frame with several (Forward3D::ViewOutput: its slot, its rect of the
    // target, whether it is the frame's first 3D record).
    void record_3d(rhi::CommandBuffer& cmd, const Scene3D& scene, rhi::TextureHandle target, const Forward3D::ViewOutput& out);

    // ---- views (M-VIEWS): stable Forward3D view slots for the views a host draws, by a key of its
    // own (an entity's bits). view_slot() answers the slot a key held last frame, else a free one
    // (~0u when every slot is taken); end_views() ends the frame: a slot no key asked for since the
    // last end_views() is released (its targets and history destroyed). Slot 0 is the single-view
    // path's: released, it keeps its targets for that path.
    u32 view_slot(u64 key);
    void end_views();
    // Forgets every view (an editor play session ends): slots 1.. are released.
    void release_views();

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
    MeshStore meshes_;
    Forward3D forward_;
    rhi::PipelineHandle pipelines_[static_cast<u32>(SpriteBlend::Count)];
    rhi::PipelineHandle post_2d_;
    rhi::SamplerHandle point_sampler_;
    rhi::SamplerHandle linear_sampler_;
    rhi::BufferHandle quad_indices_;
    Array<TilemapSlot> tilemaps_;
    Array<FontStorage> fonts_;
    struct ViewSlot {
        u64 key = 0;
        bool held = false;  // a key owns the slot
        bool asked = false; // view_slot() named it since the last end_views()
    };
    ViewSlot view_slots_[Forward3D::kMaxViews];
};

}  // namespace mce::render

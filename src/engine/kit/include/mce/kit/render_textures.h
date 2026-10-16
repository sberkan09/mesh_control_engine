#pragma once
// Render textures (M-VIEWS 3): a camera draws into a texture the scene shows somewhere else — a
// minimap on the HUD, a mirror, a monitor in the world, a picture-in-picture.
//
//   entity A: RenderTexture{width = 256, height = 256}
//   entity B: Camera3DComponent{primary = 0} + CameraView{target = A}     (the camera that fills it)
//   entity C: Sprite + ShowRenderTexture{source = A}                     (a quad showing it)
//         or: MeshRef + ShowRenderTexture{source = A, slot = 1}          (a glowing monitor screen)
//   a mce_ui image widget named like RenderTexture::ui_image shows it in a UI document
//
// The host (the player, the editor's play view) owns a RenderTextures: once a frame sync() makes,
// resizes and frees the textures to match the scene's RenderTexture components, the extracts
// (SpriteExtract, Kit3DExtract) and the UI resolve ShowRenderTexture through it, and
// kit::record_views draws the camera views into the textures before the window's views.
//
// The textures are linear RGBA16F 2D arrays of one layer (the sprite and material samplers read
// arrays): a view drawn into one skips the window's sRGB encode, so what samples it reads linear
// color like any texture. A view never samples the texture it draws: while a texture is being drawn,
// a surface showing it shows its fallback (a sprite its own texture, a mesh its own material).
// Textures shown in each other's views (two facing monitors) show the other's previous frame.
// `update_hz` > 0 redraws a texture only that many times a second (a security monitor at 10 Hz);
// in between it keeps its last picture. Render state only: the simulation never reads any of it.

#include "mce/containers/array.h"
#include "mce/ecs/world.h"
#include "mce/fixed_string.h"
#include "mce/refl/macros.h"
#include "mce/render/renderer.h"

namespace mce::kit {

struct RenderTexture {
    u32 width = 256;   // pixels
    u32 height = 256;
    f32 update_hz = 0.0f;  // redraws a second; 0 = every frame
    u32 enabled = 1;
    FixedString<32> ui_image;  // a mce_ui image widget of this name shows the texture
};
MCE_COMPONENT(RenderTexture, (width, Range{1, 4096}, Help{"pixels"}), (height, Range{1, 4096}, Help{"pixels"}),
              (update_hz, Range{0, 240}, Help{"redraws a second; 0 = every frame"}), enabled,
              (ui_image, Help{"a UI image widget of this name shows the texture"}))

// Shows a RenderTexture on this entity: its Sprite's texture (slot 0), or its MeshRef's base color
// (slot 0) or emission (slot 1, `intensity` times the picture: a screen that glows in the dark).
struct ShowRenderTexture {
    Entity source;            // the RenderTexture entity
    u32 slot = 0;             // 0 = base color / sprite texture, 1 = emissive (MeshRef)
    f32 intensity = 1.0f;     // slot 1: the emission's strength
};
MCE_COMPONENT(ShowRenderTexture, (source, Help{"the entity with the RenderTexture"}),
              (slot, Range{0, 1}, Help{"0: the sprite's texture or the mesh's base color; 1: the mesh's emission"}),
              (intensity, Range{0, 64}, Help{"slot 1: how bright the emission is"}))

class RenderTextures {
public:
    static constexpr u32 kMaxSize = 4096;

    RenderTextures() = default;
    RenderTextures(const RenderTextures&) = delete;
    RenderTextures& operator=(const RenderTextures&) = delete;
    ~RenderTextures() { shutdown(); }

    void init(render::Renderer& renderer) { renderer_ = &renderer; }
    // Frees every texture (the editor's play stops); the next sync() makes them again.
    void release();
    // release() and forget the renderer (the host shuts down). init() again to reuse.
    void shutdown();
    bool initialized() const { return renderer_ != nullptr; }

    // Once a frame, before the extracts: a texture per enabled RenderTexture (made on first sight,
    // re-made when its size changes), the ones whose entity or component is gone freed, and which
    // are due a redraw this frame (`frame_dt` seconds since the last sync).
    void sync(World& world, f32 frame_dt);
    // Clears the textures made since the last call to black (outside any pass), so a texture no
    // view has drawn yet never shows stale memory. record_views calls it; a host that draws the
    // frame some other way (a single-view debug path) calls it after sync().
    void clear_new(rhi::CommandBuffer& cmd);

    struct Entry {
        Entity entity;
        u32 key = 0;                    // TextureStore key (renderer-local)
        rhi::TextureHandle texture;
        render::SpriteSetId set;        // one region: the whole texture
        rhi::Extent2D size;
        f32 update_hz = 0.0f;
        f32 carry = 0.0f;               // seconds toward the next redraw
        bool due = true;                // draw it this frame
        bool cleared = false;           // record_views cleared it this frame
        bool seen = false;
        bool fresh = true;              // never drawn: record_views clears it once (no stale memory shows)
        FixedString<32> ui_image;
    };
    const Entry* find(Entity e) const;
    Entry* find(Entity e);
    Span<const Entry> entries() const { return entries_.span(); }
    Span<Entry> entries() { return entries_.span(); }
    u32 live() const { return static_cast<u32>(entries_.size()); }

    // What a surface showing `source` samples: its TextureStore key and sprite set; ~0u / invalid
    // when there is none, or while that texture is the one being drawn (no feedback loop).
    u32 texture_key(Entity source) const;
    render::SpriteSetId sprite_set(Entity source) const;
    // record_views marks the texture its view draws into.
    void set_drawing(Entity target) { drawing_ = target; }
    Entity drawing() const { return drawing_; }

private:
    void drop(Entry& e);

    render::Renderer* renderer_ = nullptr;
    Array<Entry> entries_;
    Entity drawing_;
};

// collect_views' size callback for views into render textures (`user` = the RenderTextures).
rhi::Extent2D render_texture_size(World& world, Entity target, void* user);

}  // namespace mce::kit

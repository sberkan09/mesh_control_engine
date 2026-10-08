#pragma once
// SpriteExtract — the render extract for the starter-kit components (renderer.h: "extract lives
// with the gameplay kit"). Reads Sprite / TextLabel / Camera2DComponent + LocalTransform and fills
// a SpriteBatch in ECS iteration order (deterministic draw order, FR-RUN-040).
//
//   kit::SpriteExtract extract;
//   extract.init(renderer, &assets);           // assets: textures/fonts by GUID (optional)
//   render::Camera2D cam; f32 ppm;
//   kit::primary_camera(world, cam, ppm);
//   batch.begin(render::compute_view_2d(cam, viewport, ppm));
//   extract.extract(world, batch);
//
// Textures are loaded on first sight (one reference held per GUID) and uploaded through the
// host's render::AssetUploadSink, so hot reloads swap in transparently. A GUID that is not
// mounted, or fails to load, draws as the magenta checker (FR-DX-050); a null texture GUID draws
// a solid quad.

#include "mce/asset/asset_system.h"
#include "mce/ecs/world.h"
#include "mce/kit/components.h"
#include "mce/render/renderer.h"

namespace mce::kit {

class SpriteExtract {
public:
    SpriteExtract() = default;
    SpriteExtract(const SpriteExtract&) = delete;
    SpriteExtract& operator=(const SpriteExtract&) = delete;
    ~SpriteExtract() { shutdown(); }

    ResultV init(render::Renderer& renderer, asset::AssetSystem* assets);
    void shutdown();
    // Forgets every cached texture/font binding (call after remounting a different pack).
    void reset(asset::AssetSystem* assets);

    void extract(World& world, render::SpriteBatch& batch);

    // Sprite set for a texture GUID (solid white for a null GUID, magenta for a missing one).
    render::SpriteSetId sprite_set(asset::Guid texture);
    // Font for a GUID, or null when it cannot be loaded.
    const render::Font* font(asset::Guid guid);

private:
    struct TextureEntry {
        asset::Guid guid;
        asset::AnyHandle handle;
        render::SpriteSetId set;
    };
    struct FontEntry {
        asset::Guid guid;
        render::Font font;
        bool ok = false;
    };

    void release_handles();

    render::Renderer* renderer_ = nullptr;
    asset::AssetSystem* assets_ = nullptr;
    render::SpriteSetId white_set_;
    render::SpriteSetId missing_set_;
    u32 white_key_ = 0;
    Array<TextureEntry> textures_;
    Array<FontEntry> fonts_;
};

// The first primary Camera2DComponent with a LocalTransform, in ECS order. False (and a default
// camera at the origin) when the scene has none.
bool primary_camera(World& world, render::Camera2D& out, f32& pixels_per_meter, math::Color* clear_color = nullptr);

}  // namespace mce::kit

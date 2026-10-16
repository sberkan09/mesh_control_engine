#pragma once
// SpriteExtract — the render extract for the starter-kit components (renderer.h: "extract lives
// with the gameplay kit"). Reads Sprite / Skeleton2D slots / TextLabel / TilemapLayer / Camera2DComponent + LocalTransform
// (WorldTransform when the hierarchy wrote one), plus the ParticleSystem resource's live particles,
// and fills
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
#include "mce/kit/skeleton2d.h"
#include "mce/render/renderer.h"

namespace mce::kit {

class VideoTextures;
class RenderTextures;

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

    // Sprite set for a texture GUID (solid white for a null GUID, magenta for a missing one). With
    // a normal map GUID the set is lit (FR-R2D-090); a normal map that cannot load leaves it unlit.
    render::SpriteSetId sprite_set(asset::Guid texture, asset::Guid normal_map = {});
    // The primary camera's Post2DComponent as renderer settings (the LUT texture loaded and held);
    // false when the camera has none or every effect is off.
    bool post_2d(World& world, render::Post2D& out);

    // Font for a GUID, or null when it cannot be loaded.
    const render::Font* font(asset::Guid guid);
    // A UI skin's sprite set for a SpriteSheet GUID (M-UI-DATA): the sheet's texture with the sheet's
    // regions in its order, each sized in texels (a ui::Style's sprite_region indexes them and its
    // 9-slice insets are texels). Invalid when the sheet, or the texture it names, cannot load;
    // `region_count` (optional) is the sheet's region count.
    render::SpriteSetId ui_sheet_set(asset::Guid sheet, u32* region_count = nullptr);
    // Draws a VideoPlayer's Sprite with its film (video.h); null (the default) draws the texture.
    void set_videos(const VideoTextures* videos) { videos_ = videos; }
    // M-VIEWS 3: a Sprite with ShowRenderTexture draws the render texture instead of its texture.
    void set_render_textures(const RenderTextures* rts) { render_textures_ = rts; }

private:
    const VideoTextures* videos_ = nullptr;
    const RenderTextures* render_textures_ = nullptr;
    struct TextureEntry {
        asset::Guid guid;
        asset::Guid normal;
        asset::AnyHandle handle;
        asset::AnyHandle normal_handle;
        render::SpriteSetId set;
        Array<render::SpriteRegion> regions;  // [0] = the whole texture; Sprite.region rects after it
    };
    struct SheetEntry {
        asset::Guid guid;
        asset::AnyHandle texture;
        render::SpriteSetId set;  // invalid: the sheet or its texture did not load
        u32 regions = 0;
    };
    struct FontEntry {
        asset::Guid guid;
        render::Font font;
        bool ok = false;
    };
    // One baked TilemapLayer: re-baked when the asset hot-reloads or the layer moves.
    struct TilemapEntry {
        asset::Guid guid;
        asset::AnyHandle handle;
        render::TilemapId id;
        render::SpriteSetId set;  // the tileset cut into its grid
        math::vec2 origin;
        math::Color tint;
        i32 order = 0;
        i16 layer = 0;
        bool used = false;
    };
    // The region index of the texture rect `uv` (u0, v0, u1, v1) in `set`, added on first use;
    // 0 (the whole texture) for the default rect or a set with no texture of its own.
    u32 region_index(render::SpriteSetId set, const math::vec4& uv);
    void collect_lights(World& world, render::SpriteBatch& batch);
    void draw_tilemap(const TilemapLayer& layer, math::vec2 origin, math::vec2 offset, render::SpriteBatch& batch);
    void destroy_tilemap(TilemapEntry& e);

    void release_handles();

    render::Renderer* renderer_ = nullptr;
    asset::AssetSystem* assets_ = nullptr;
    render::SpriteSetId white_set_;
    render::SpriteSetId missing_set_;
    u32 white_key_ = 0;
    Array<TextureEntry> textures_;
    Array<FontEntry> fonts_;
    Array<SheetEntry> sheets_;
    Array<TilemapEntry> tilemaps_;
    // Skeletons for worlds without kit::install_skeleton2d (the editor's edit world).
    Skeleton2DRegistry skeletons_;
    Skeleton2DDraws draws_;      // one skeleton's slots, reused
    Skeleton2DDeform deform_;    // one skeleton's sampled deform offsets, reused
};

// The first primary Camera2DComponent with a LocalTransform, in ECS order. False (and a default
// camera at the origin) when the scene has none.
// Both add a CameraShake2D on the camera entity (effects.h) to the view.
bool primary_camera(World& world, render::Camera2D& out, f32& pixels_per_meter, math::Color* clear_color = nullptr);
// The same for one entity's Camera2DComponent, primary or not (a preview from another camera). False
// (nothing written) when `e` has no LocalTransform + Camera2DComponent.
bool camera_2d_of(World& world, Entity e, render::Camera2D& out, f32& pixels_per_meter, math::Color* clear_color = nullptr);

}  // namespace mce::kit

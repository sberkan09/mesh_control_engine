#pragma once
// Starter-kit 2D components (FR-ENT-080, ecs-gameplay §2.11). Plain reflected structs: what an
// editor-authored scene is made of, alongside LocalTransform and the physics2d components.
//
// Conventions: metres, Y up; an entity's LocalTransform positions the pivot, its rotation about +Z
// turns the quad and its scale.xy multiplies `size`.

#include "mce/asset/handle.h"
#include "mce/fixed_string.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

// A textured (or, without a texture, solid) quad. The texture is shown whole, stretched to
// `size`; atlas regions and animation arrive with the sprite-sheet importer.
struct Sprite {
    asset::AssetRef<asset::Texture> texture;  // null GUID: a solid quad in `tint`
    math::Color tint;
    math::vec2 size{1.0f, 1.0f};    // metres
    math::vec2 pivot{0.5f, 0.5f};   // normalized; (0, 0) = bottom-left
    i32 order = 0;                  // refines stacking inside a layer
    i16 layer = 0;                  // authoritative stacking (rendering-2d D11)
    bool flip_x = false;
    bool flip_y = false;
};
MCE_COMPONENT(Sprite,
    texture,
    tint,
    (size, Help{"world size in metres"}),
    (pivot, Help{"normalized pivot, (0,0) = bottom-left"}),
    order,
    (layer, Range{-100, 100}),
    flip_x,
    flip_y)

enum class TextAlign : u8 { Left, Center, Right };
MCE_ENUM(TextAlign, Left, Center, Right)

// World-space text from a cooked font. `height` is the baked glyph height in metres; the layout
// box is anchored at the entity's position (horizontally by `align`, vertically centered).
struct TextLabel {
    asset::AssetRef<asset::Font> font;
    FixedString<64> text;
    math::Color color;
    f32 height = 0.5f;
    i32 order = 0;
    i16 layer = 0;
    TextAlign align = TextAlign::Center;
    u8 pad_ = 0;
};
MCE_COMPONENT(TextLabel, font, text, color, (height, Range{0.05f, 10}, Help{"glyph height in metres"}), order,
              (layer, Range{-100, 100}), align)

// The view a player renders from: the first `primary` camera in ECS order, at its entity's
// position (rotation about +Z included).
struct Camera2DComponent {
    f32 zoom = 1.0f;                // 1 = one art texel per device pixel at `pixels_per_meter`
    f32 pixels_per_meter = 48.0f;
    math::Color clear_color{0.08f, 0.09f, 0.12f, 1.0f};
    bool primary = true;
    bool pixel_snap = true;
    u8 pad_[2] = {};
};
MCE_COMPONENT(Camera2DComponent, (zoom, Range{0.05f, 32}), (pixels_per_meter, Range{1, 512}), clear_color, primary,
              pixel_snap)

// One baked tile layer (FR-R2D-115): the .mctilemap asset drawn with its tile (0, 0) bottom-left
// corner at the entity's position. The tiles live in the asset; the editor's Tilemap panel
// paints them and the asset hot-reloads (re-bake) on save.
struct TilemapLayer {
    asset::AssetRef<asset::Tilemap> map;
    math::Color tint;
    i32 order = 0;
    i16 layer = 0;
    u8 pad_[2] = {};
};
MCE_COMPONENT(TilemapLayer, map, tint, order, (layer, Range{-100, 100}))

// A particle emitter (FR-R2D-070; the simulation is kit::ParticleSystem in particles.h). Emits
// particles from the entity's position (LocalTransform, or WorldTransform when the
// hierarchy wrote one). The preset holds the look; the component holds the instance state that
// a scene authors.
struct ParticleEmitter {
    asset::AssetRef<asset::ParticlePreset> preset;
    u32 seed = 1;                 // per-emitter random stream (mixed with the entity index)
    i32 order = 0;                // sort order inside the layer
    i16 layer = 0;
    bool playing = true;          // emitting; clearing it stops emission, live particles finish
    bool destroy_when_done = false;  // one-shots: destroy the entity once emission ended and every particle died
};
MCE_COMPONENT(ParticleEmitter, preset, seed, order, (layer, Range{-100, 100}), playing, destroy_when_done)

// A parallax layer (FR-R2D-050): the entity's Sprite, TilemapLayer and TextLabel scroll against
// the camera by `factor` per axis. 1 moves with the world, 0.5 at half speed (a far background), 0
// stays fixed to the camera (a sky); above 1 is a foreground that passes faster. The layer is
// authored where it sits when the camera is at the origin: drawn at position + camera × (1 −
// factor). `repeat_x` tiles a Sprite horizontally across the view, for endless backgrounds.
struct Parallax {
    math::vec2 factor{1.0f, 1.0f};
    bool repeat_x = false;
    u8 pad_[3] = {};
};
MCE_COMPONENT(Parallax, (factor, Help{"scroll factor per axis: 1 = world, 0 = fixed to the camera"}), repeat_x)

}  // namespace mce::kit

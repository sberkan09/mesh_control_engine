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

}  // namespace mce::kit

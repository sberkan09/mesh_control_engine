#pragma once
// mce_render shared vocabulary for the 2D path (rhi-renderer.md §3.2, rendering-2d D1-D11).
//
// World units are meters, Y up, right-handed. Sprite/tile/text art lives in sprite sets: one
// 2D-array texture (rendering-2d D3) plus a region table. Everything the batcher draws is a quad
// of SpriteVertex, so sprites, tilemap chunks and text share one pipeline family and one batch.

#include "mce/math/math.h"
#include "mce/rhi/types.h"
#include "mce/types.h"

namespace mce::render {

// The one 2D vertex format (rhi-renderer.md §3.2). Positions are pre-transformed on the CPU;
// the vertex shader is a single viewProj multiply.
struct SpriteVertex {
    f32 x, y;      // world (or screen) position
    f32 u, v;      // texel-normalized; v = 0 is the top row of the art
    u32 color;     // RGBA8 tint, sRGB-encoded, straight alpha (the shader premultiplies)
    u32 packed;    // [7:0] array layer, [23:8] orientation of lit quads (below), [31:24] flags
};
static_assert(sizeof(SpriteVertex) == 24);

namespace vertex_flags {
inline constexpr u32 kCoverage = 1u << 0;  // sample the font atlas (R8 coverage) instead of the sprite atlas
inline constexpr u32 kLit = 1u << 1;       // normal-mapped: shaded by the batch's Lights2D (FR-R2D-090)
inline constexpr u32 kSdf = 1u << 2;       // with kCoverage: the font atlas is a distance field, thresholded at 0.5
}

constexpr u32 pack_layer_flags(u32 layer, u32 flags, u32 orientation = 0) {
    return (layer & 0xFFu) | ((orientation & 0xFFFFu) << 8) | ((flags & 0xFFu) << 24);
}

// Orientation of a lit quad, so the shader can turn its tangent-space normals into the world:
// [13:0] rotation quantized over one turn, [14] flip_x, [15] flip_y (bits [21:8] / 22 / 23 of
// SpriteVertex::packed).
u32 pack_orientation(f32 rotation, bool flip_x, bool flip_y);

// ---- 2D lighting (FR-R2D-090): ambient + a small set of point lights per batch ----------------

inline constexpr u32 kMaxLights2D = 16;

struct PointLight2D {
    math::vec2 position;   // world
    f32 height = 1.5f;     // metres above the sprite plane: low lights graze, high lights flatten
    f32 radius = 6.0f;     // falloff reaches zero here
    math::Color color = math::Color::white();
    f32 intensity = 1.0f;
};

// Lit sprites receive ambient + Σ points; unlit sprites ignore all of it. The default (white
// ambient, no points) draws lit sprites exactly like unlit ones.
struct Lights2D {
    math::Color ambient = math::Color::white();
    u32 count = 0;
    PointLight2D points[kMaxLights2D];
    bool add(const PointLight2D& p) {
        if (count >= kMaxLights2D) return false;
        points[count++] = p;
        return true;
    }
};

// Linear Color -> RGBA8 with sRGB-encoded rgb (alpha stays linear). Inverse of the shader decode.
u32 pack_color_srgb(math::Color c);

enum class SpriteBlend : u8 {
    Alpha,     // premultiplied-alpha "over" (the default)
    Additive,  // particles, glows
    Count,
};

// Index of a sprite set registered with the Renderer (rendering-2d D3: the batch boundary is
// blend × sprite set, never the atlas page).
struct SpriteSetId {
    u16 value = 0xFFFF;
    constexpr bool valid() const { return value != 0xFFFF; }
    friend constexpr bool operator==(SpriteSetId a, SpriteSetId b) { return a.value == b.value; }
};

// One packed region of a sprite set: which array layer, where (normalized uv) and how big in the
// world (meters). The cook stores texel rects; the region table normalizes them at load.
struct SpriteRegion {
    u16 layer = 0;
    f32 u0 = 0, v0 = 0, u1 = 1, v1 = 1;  // v0 = top edge
    f32 width_m = 1, height_m = 1;
};

// A sprite as the renderer sees it after extract (FR-R2D-020): one per visible Sprite component
// (or particle, or animated tile). Submission order breaks sort-key ties, so callers submit in
// ECS iteration order (entity index) to keep draw order deterministic (FR-RUN-040).
struct SpriteInstance {
    math::vec2 position;          // world position of the pivot (already interpolated with alpha)
    f32 rotation = 0;             // radians, CCW
    math::vec2 scale{1, 1};
    math::vec2 pivot{0.5f, 0.5f}; // normalized within the region; (0,0) = bottom-left
    math::Color tint = math::Color::white();
    SpriteSetId set;
    u32 region = 0;               // index into the set's region table
    i16 sort_layer = 0;           // authoritative stacking (rendering-2d D11)
    i32 sort_order = 0;           // refines inside a layer; clamped to 24 bits
    SpriteBlend blend = SpriteBlend::Alpha;
    bool flip_x = false;
    bool flip_y = false;
    bool visible = true;
};

}  // namespace mce::render

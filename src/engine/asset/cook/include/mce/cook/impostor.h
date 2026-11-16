#pragma once
// The impostor bake (M-OPENWORLD A3): a far stand-in for a streamed area. Each group of sources (a building: one root
// entity and its children) becomes a box whose four sides and top are quads textured with an orthographic albedo
// capture of the group from that side; a tall, thin group (a tree, a pole) becomes two crossed vertical cards instead.
// The captures are rasterised on the CPU from the cooked meshes and textures, so the bake runs anywhere the cook runs
// and is deterministic: the same sources in the same order give the same bytes.
//
//   cook::ImpostorBake out;
//   if (cook::bake_impostor(sources.span(), texture_fn, &ctx, atlas_guid, {}, out)) {
//       // out.mesh: a CHNK_MESH payload (write_hlod_file wraps it into a .mchlod); out.atlas: RGBA8, rows top first,
//       // written as a PNG and cooked as a texture with the GUID `atlas_guid` the mesh's material names.
//   }
//
// The mesh is one or two submeshes over one atlas: boxes single-sided, cards double-sided, both alpha-masked (alpha =
// coverage, cutoff 0.5), base colour = the atlas, roughness 1. Colour is albedo only (base colour x base colour texture
// x tint, alpha-masked materials cut as they draw); the impostor is lit like any mesh, by its quad normals. Texels left
// uncovered take the colour of the nearest covered one (alpha stays 0), so filtering and mips do not bleed black.
//
// Left out (counted in ImpostorStats::skipped): skinned sources and submeshes with an alpha-blended or transmissive
// material (the StaticMerge3D rules). A texture the callback cannot provide, or in a format the CPU does not decode
// (ASTC, RGBA16F), counts as white and in `untextured`.

#include "mce/asset/handle.h"
#include "mce/containers/array.h"
#include "mce/math/math.h"

namespace mce::asset {
struct MeshView;
struct MaterialChunk;
}  // namespace mce::asset

namespace mce::cook {

struct ImpostorSource {
    const asset::MeshView* mesh = nullptr;           // must outlive the call
    math::mat4 world = math::mat4::identity();       // into the impostor's space (relative to the chunk entity)
    math::vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
    const asset::MaterialChunk* material = nullptr;  // replaces every submesh's own (MeshRef::material)
    u32 group = 0;                                   // sources with the same group form one box or one pair of cards
};

// A cooked texture's CHNK_TEX payload by GUID; empty when it is not available. The bytes must stay valid until
// bake_impostor returns.
using ImpostorTextureFn = Span<const byte> (*)(asset::Guid texture, void* user);

struct ImpostorSettings {
    f32 texels_per_metre = 4.0f;  // capture resolution
    u32 max_face = 128;           // texels along a capture's longer side, at most
    f32 card_ratio = 1.5f;        // a group taller than card_ratio x its width ...
    f32 card_size = 8.0f;         // ... and narrower than this (metres) gets cross cards
    u32 atlas_width = 1024;       // grown to the widest capture when needed
};

struct ImpostorStats {
    u32 groups = 0;
    u32 boxes = 0;
    u32 cards = 0;        // groups drawn as cross cards
    u32 quads = 0;
    u32 triangles = 0;    // of the sources, rasterised once per capture
    u32 skipped = 0;      // sources or submeshes left out
    u32 untextured = 0;   // texture references drawn white
    u32 atlas_width = 0;
    u32 atlas_height = 0;
    math::aabb3 bounds{};
};

struct ImpostorBake {
    Array<byte> mesh;   // CHNK_MESH payload
    u32 atlas_width = 0;
    u32 atlas_height = 0;
    Array<u8> atlas;    // atlas_width * atlas_height RGBA8, sRGB colour, rows top first
    ImpostorStats stats;
};

// False, with `out` emptied, when no source leaves anything to capture.
bool bake_impostor(Span<const ImpostorSource> sources, ImpostorTextureFn texture, void* user, asset::Guid atlas_guid,
                   const ImpostorSettings& settings, ImpostorBake& out);

}  // namespace mce::cook

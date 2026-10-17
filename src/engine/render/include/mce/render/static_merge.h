#pragma once
// Static merge (M-VISUAL3 5b): many different rigid meshes placed in the world become one mesh with one
// submesh per material, so a modular kit (dozens of small pieces sharing a colormap) draws in a few calls.
//
//   Array<byte> blob;
//   Array<render::MergeSource> sources;  // {&view_a, world_a}, {&view_b, world_b}, ...
//   if (render::build_merged_mesh(sources.span(), blob)) meshes.upload(meshes.allocate_key(), 1, blob.span());
//
// The result is a CHNK_MESH payload (the TerrainLod::build_mesh precedent) in the space the worlds are in: one
// node at the identity, positions re-quantized (snorm16) over the bounds of each merged submesh, so a cell of
// `s` metres keeps s / 65534 of precision (0.5 mm at 32 m). A merged mesh is an ordinary mesh: it draws with an
// identity world, and Scene3D's instancing folds identical merged cells the same as any other copies.
//
// Merge key: two submeshes share a merged submesh when their materials are identical after the source's tint
// is folded in: the MaterialChunk bytes (base color x tint, emissive x emissive tint, texture GUIDs, flags
// including double-sided, uv repeat) and the MaterialExt bytes. The texture keys follow from the GUIDs when the
// payload is uploaded, so equal GUIDs mean equal albedo, normal, ORM and emissive keys. Submeshes are emitted
// in the order their keys first appear in `sources`, so the result depends only on the sources and their order.
//
// LODs: level k (k = 0 the full detail) of a merged submesh is the union of each source's level
// min(k, its levels - 1), with the error of the coarsest of them (scaled by the source's world scale), so a
// far cell is cheap while a source without levels stays whole.
//
// Only rigid sources merge: a skinned mesh is skipped (MergeStats::skipped). Winding and normals follow the
// world matrix the way an unmerged draw does (normals by its inverse transpose).

#include "mce/containers/array.h"
#include "mce/math/math.h"

namespace mce::asset {
struct MeshView;
struct MaterialChunk;
struct MaterialExt;
}  // namespace mce::asset

namespace mce::render {

struct MergeSource {
    const asset::MeshView* mesh = nullptr;  // the cooked mesh (MeshView::read); must outlive the call
    math::mat4 world = math::mat4::identity();
    math::vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};         // MeshInstance::tint, folded into the base color
    math::vec3 emissive_tint{1.0f, 1.0f, 1.0f};      // MeshInstance::emissive_tint, folded into the emissive color
    const asset::MaterialChunk* material = nullptr;  // MeshInstance::material's params: replace every submesh's own
    const asset::MaterialExt* material_ext = nullptr;  // ... and its lobes (null with `material` set: none)
};

struct MergeStats {
    u32 sources = 0;     // merged
    u32 skipped = 0;     // not merged (no mesh, skinned or without geometry)
    u32 submeshes = 0;   // merged submeshes: the materials
    u32 vertices = 0;
    u32 triangles = 0;   // of level 0
    u32 lod_levels = 1;  // the most any merged submesh has
    math::aabb3 bounds{};
};

// Merges `sources` into `blob` (replacing its contents). False, with an empty blob, when nothing was left to
// merge. Deterministic: the same sources in the same order give the same bytes.
bool build_merged_mesh(Span<const MergeSource> sources, Array<byte>& blob, MergeStats* stats = nullptr);

}  // namespace mce::render

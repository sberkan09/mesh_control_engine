#pragma once
// Static merge (M-VISUAL3 5b): many different rigid meshes placed in the world become one mesh with one
// submesh per material, so a modular kit (dozens of small pieces sharing a colormap) draws in a few calls.
//
//   Array<byte> blob;
//   Array<render::MergeSource> sources;  // {&view_a, world_a}, {&view_b, world_b}, ...
//   if (render::build_merged_mesh(sources.span(), blob)) meshes.upload(meshes.allocate_key(), 1, blob.span());
//
// The result is a CHNK_MESH payload (the TerrainLod::build_mesh precedent) in the space the worlds are in: one
// node at the identity, positions re-quantized (snorm16) over ONE box shared by every submesh (the bounds of all
// the merged vertices), so points shared by two materials round alike and the geometry stays watertight. The
// quantum is extent / 65534 per axis and a vertex moves by at most half of it: about 0.25 mm over a 32 m box. The
// box is the sources' union, not a cell: a piece belongs to a cell by its origin, so a long piece can reach past it.
// A merged mesh is an ordinary mesh: it draws with an identity world, and Scene3D's instancing folds identical
// merged cells the same as any other copies.
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
// Preconditions: only sources whose draw needs no per-draw state merge. A skinned mesh, a blended (AlphaBlend)
// or transmissive material (drawn after the opaques, back to front: the order would be lost) and a parallax
// material (needs MeshInstance::material) keep the whole source out: it is counted in MergeStats::skipped and in
// skipped_blended / skipped_parallax (skipped is every source left out, so it is at least their sum).
// Winding is kept as authored, also under a mirroring (negative determinant) world, as an unmerged draw does;
// normals follow the world matrix by its inverse transpose.

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
    u32 skipped = 0;     // not merged: no mesh, skinned, without geometry, blended / transmissive or parallax
    u32 skipped_blended = 0;   // of them: an alpha-blended or transmissive material
    u32 skipped_parallax = 0;  // ... and a parallax material (blended wins when a source has both)
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

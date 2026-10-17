#pragma once
// Format-neutral mesh back end of the model importers (cook-internal; M-IMPORT3D step 0).
//
// An importer parses its format into `Primitive`s (float positions, normals, UVs, up to 4 joint/weight pairs,
// indices, material, node) and hands them to add_submesh / add_authored_lod; this file de-duplicates and
// reorders them with meshopt, quantizes them (formats.h MeshVertex), generates LODs, builds the skin tables
// and writes the CHNK_MESH. It reports no diagnostics: validating the source (index ranges, joint ranges,
// attribute counts) is the importer's job, and every argument is assumed valid.
//
// Deterministic: the float operation order here is part of the cooked bytes (tests/asset/test_cook_mesh_golden.cpp).

#include "mce/asset/container.h"
#include "mce/asset/formats.h"
#include "mce/math/math.h"

namespace mce::cook::detail {

// One triangle-list primitive in importer-neutral form. Spans must stay alive for the call.
struct Primitive {
    Span<const f32> positions;  // 3 per vertex: the vertex count is positions.size() / 3
    Span<const f32> normals;    // 3 per vertex, or empty: area-weighted smooth normals are generated
    Span<const f32> uvs;        // 2 per vertex (any UV transform already applied), or empty: zeros
    Span<const f32> joints;     // 4 per vertex, skin-local joint indices; empty for a rigid primitive
    Span<const f32> weights;    // 4 per vertex; negatives count as 0, normalized to sum 1 here
    Span<const u32> indices;    // triangle list (a multiple of 3, every index < vertex count)
    asset::MaterialChunk material;
    asset::MaterialExt material_ext;  // ignored by add_authored_lod
    u32 node = 0;                     // the imported node the submesh hangs on
    i32 skin = -1;                    // the importer's skin id (-1 = rigid); needs joints and weights
    u32 skin_joints = 0;              // joints of that skin
};

// Bind-space bounds of the vertices one joint influences.
struct JointBox {
    f32 min[3] = {0, 0, 0};
    f32 max[3] = {0, 0, 0};
    bool any = false;

    void add(const f32 p[3]) {
        for (int c = 0; c < 3; ++c) {
            min[c] = any ? math::min(min[c], p[c]) : p[c];
            max[c] = any ? math::max(max[c], p[c]) : p[c];
        }
        any = true;
    }
};

struct Geometry {
    Array<asset::MeshVertex> vertices;
    Array<asset::MeshSkinVertex> skin_vertices;  // parallel to `vertices` (zero for rigid ones)
    Array<u32> indices;  // relative to the submesh's base_vertex
    Array<asset::MeshSubmesh> submeshes;
    Array<asset::MaterialExt> submesh_ext;  // per submesh (M-VISUAL2 4)
    Array<i32> submesh_skin;                // per submesh: the importer's skin id (Primitive::skin), -1 = rigid
    Array<Array<JointBox>> joint_boxes;     // per importer skin id, per joint
    Array<Array<asset::MeshLod>> lods;      // per submesh: its coarser levels, finest first
};

// Appends the primitive as a new submesh (index = geo.submeshes.size() - 1 afterwards).
void add_submesh(const Primitive& prim, Geometry& geo);

// Appends the primitive as an authored LOD of submesh `base`: its own vertices, quantized with that
// submesh's bounds. Returns the level's range with `error` measured against the base; the caller
// orders and stores it in geo.lods[base].
asset::MeshLod add_authored_lod(const Primitive& prim, u32 base, Geometry& geo);

// Generated levels (sidecar lod_levels): each simplified from the base so errors do not compound.
void generate_lods(Geometry& geo, u32 levels, f32 ratio, f32 max_error);

// Skin tables of the CHNK_MESH.
struct SkinTables {
    Array<asset::MeshSkin> skins;
    Array<asset::MeshJoint> joints;
    Array<i32> submesh_skins;  // per submesh: index into `skins`, -1 = rigid
};

// Appends skin `source_skin` (the importer's id, as in Primitive::skin) and returns its index in
// `tables.skins`. joint_nodes: the imported node of each joint; inverse_bind: 16 floats per joint
// (column-major), or empty for identity. The per-joint bind bounds come from the geometry.
i32 add_skin(SkinTables& tables, const Geometry& geo, i32 source_skin, Span<const u32> joint_nodes, Span<const f32> inverse_bind);

// The non-geometry sections of the mesh, as the importer built them (node tree, clips, strings).
struct MeshTables {
    Span<const asset::MeshNode> nodes;
    Span<const asset::MeshClip> clips;
    Span<const asset::MeshChannel> channels;
    Span<const f32> keys;
    Span<const char> strings;
};

// Computes the rest-pose bounds and the LOD table, fills asset::MeshData and writes the CHNK_MESH.
// Writes the LOD run of each submesh into its reserved[] fields, so `geo` is modified.
void write_mesh_chunk(Geometry& geo, const SkinTables& skins, const MeshTables& tables, bool ragdoll, const asset::Guid& guid,
                      asset::ContainerWriter& writer);

}  // namespace mce::cook::detail

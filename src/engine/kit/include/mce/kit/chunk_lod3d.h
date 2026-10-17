#pragma once
// ChunkLod3D (M-OPENWORLD A4): the render LOD of a streamed area. On a SceneChunk entity of the base scene it names
// a proxy mesh (an HLOD bake of everything static in the area, merged and coarse) and optionally an impostor, and
// the 3D extract draws one of them while the area itself is not loaded:
//
//   SceneChunkStatus  Loaded               -> the area's own entities draw; the proxy hides
//                     Unloaded / Loading / Failed -> the proxy, or beyond `impostor_distance` the impostor
//
// so walking away from an area swaps its entities for the proxy without a hole (Loading keeps the proxy up). The
// distance is from the camera to the chunk's box (SceneChunk::half_extents around the entity), so it is a render
// decision: loading stays focus-based (the simulation). Beyond `max_distance` (0: no limit) nothing draws.
//
// The meshes are placed at the chunk entity's transform: an HLOD bake (the hlod_bake tool) writes them relative to
// the chunk entity's position, so the proxy follows the entity (and a floating-origin rebase) like any mesh.
// Both are ordinary cooked meshes (.mchlod or .glb), so they get culling, instancing, LOD levels and shadows.
//
// Layout: padding-free (World::state_hash hashes component bytes).

#include "mce/asset/handle.h"
#include "mce/containers/array.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::asset {
class AssetSystem;
}

namespace mce::kit {

struct ChunkLod3D {
    asset::AssetRef<asset::Mesh> proxy;     // the HLOD proxy, relative to this entity; null: nothing near
    asset::AssetRef<asset::Mesh> impostor;  // drawn instead of the proxy beyond impostor_distance; null: the proxy
    f32 impostor_distance = 600.0f;         // metres from the camera to the chunk box
    f32 max_distance = 0.0f;                // beyond this nothing draws; 0 = no limit
    u32 cast_shadows = 1;
    u32 enabled = 1;
};
MCE_COMPONENT(ChunkLod3D,
    (proxy, Help{"HLOD proxy mesh (hlod_bake), drawn while the chunk is not loaded"}),
    (impostor, Help{"cheaper mesh beyond impostor_distance; null keeps the proxy"}),
    (impostor_distance, Range{0, 100000}, Help{"camera to chunk box, metres"}),
    (max_distance, Range{0, 100000}, Help{"0 = no limit"}),
    cast_shadows, enabled)

// ---- the HLOD bake (M-OPENWORLD A3; the editor's EditorSession::bake_hlod and MCP's hlod_bake call it) ----------

struct HlodBakeSettings3D {
    f32 min_size = 1.0f;  // metres: meshes whose bounding diameter is smaller are left out (invisible that far away)
};

struct HlodBakeStats3D {
    u32 meshes = 0;         // MeshRefs in the area's scene
    u32 merged = 0;         // ... merged into the proxy
    u32 small = 0;          // ... left out by min_size
    u32 skipped = 0;        // ... left out otherwise: hidden, not loadable, skinned, blended, transmissive or parallax
    u32 submeshes = 0;      // the proxy's submeshes (one per material)
    u32 vertices = 0;
    u32 triangles = 0;      // of level 0
    u32 lod_levels = 1;
    math::aabb3 bounds{};   // relative to the chunk entity
};

// Loads the area scene `scene` from `assets` into a scratch world and merges every visible rigid MeshRef with an
// opaque or masked material (render::build_merged_mesh: one submesh per material, each source's LOD levels kept, so
// a far proxy draws the coarse union) into `out`, a CHNK_MESH payload with positions relative to `origin` (the chunk
// entity's position: area scenes are authored in absolute coordinates). False, with `out` empty, when the scene does
// not load or nothing qualifies. Deterministic: the same scene and settings give the same bytes.
bool bake_chunk_hlod(asset::AssetSystem& assets, asset::Guid scene, const math::vec3& origin, const HlodBakeSettings3D& settings,
                     Array<byte>& out, HlodBakeStats3D* stats = nullptr);

}  // namespace mce::kit

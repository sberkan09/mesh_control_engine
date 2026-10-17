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
#include "mce/refl/macros.h"

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

}  // namespace mce::kit

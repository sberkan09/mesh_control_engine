#pragma once
// kit3d's collider builders, shared by the Startup system (kit3d.cpp) and the runtime sync of
// bodies3d.cpp (colliders on runtime-spawned entities), the Animator pose samples shared by the
// extract (kit3d_extract.cpp) and AttachToBone (anim3d.cpp), and the ragdoll pose override
// (ragdolls3d.cpp) both of them apply.

#include "mce/containers/hash_map.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/render/mesh.h"

namespace mce::kit {

// The entity's world pose: WorldTransform when the hierarchy wrote one, else LocalTransform.
void world_pose(World& w, Entity e, const LocalTransform& local, math::vec3& pos, math::quat& rot, math::vec3& scale);
bool create_box_collider(World& w, Physics3D& phys, Entity e, const BoxCollider3D& c, const LocalTransform& t);
bool create_mesh_collider(World& w, Physics3D& phys, asset::AssetSystem* assets, Entity e, const MeshCollider3D& c,
                          const LocalTransform& t);
// Needs the entity's Terrain3D; false (with a warning) without it or when the terrain does not load.
bool create_terrain_collider(World& w, Physics3D& phys, asset::AssetSystem* assets, Entity e, const TerrainCollider3D& c,
                             const LocalTransform& t);
// install_kit3d's asset system (null when none).
asset::AssetSystem* kit3d_assets(World& w);

// anim3d.cpp: registers AttachToBone, AnimEvent3D and the animation asset cache; adds
// "kit3d_animators" and "kit3d_attach".
void install_anim3d(World& world);
void add_anim3d_systems(World& world);

// anim3d.cpp: the CPU copy of a mesh the animation systems read (null without an asset system,
// or when the mesh does not load).
const render::GpuMesh* anim3d_mesh(World& w, asset::Guid guid);
// Model-space node matrices of `mesh` posed by Animator `a` (the rest pose without one, or when it
// plays nothing). `out` holds mesh.nodes.size() matrices.
void animated_pose(const render::GpuMesh& mesh, const Animator* a, Span<math::mat4> out);
// Rotation of an orthonormal 3x3 (columns c0, c1, c2).
math::quat quat_from_basis(math::vec3 c0, math::vec3 c1, math::vec3 c2);

// ragdolls3d.cpp: the pose a ragdoll gives its mesh, per entity: model-space node matrices and how
// much they override the animation (1 while the physics drives it, falling to 0 as it blends back).
struct RagdollPose {
    Array<math::mat4> nodes;
    f32 weight = 1.0f;
};
struct Ragdoll3DPoses {
    HashMap<Entity, RagdollPose> poses;
};
// Blends `pose` (model-space node matrices of e's mesh) toward e's ragdoll pose. False (and `pose`
// untouched) when e has none.
bool apply_ragdoll_pose(World& w, Entity e, Span<math::mat4> pose);

// The weighted clip samples posing `mesh` under Animator `a`: the playing clip, a blend state's
// second clip (at the same phase) and the clip fading out. `find` maps a clip StringId to the
// mesh's clip index (~0u when absent). 0 samples = the rest pose.
template <typename Find>
usize anim_samples(const Animator& a, const render::GpuMesh& mesh, Find find, render::ClipSample (&out)[3]) {
    const u32 clip = a.playing != 0 ? find(a.playing) : ~0u;
    if (clip == ~0u) return 0;
    usize n = 0;
    const f32 t = a.time + a.phase;
    const u32 second = a.blend_clip != 0 && a.blend_weight > 0.0f ? find(a.blend_clip) : ~0u;
    if (second != ~0u) {
        const f32 from = mesh.clips[clip].duration, to = mesh.clips[second].duration;
        out[n++] = {clip, t, a.blend * (1.0f - a.blend_weight)};
        out[n++] = {second, from > 0.0f ? t / from * to : t, a.blend * a.blend_weight};
    } else {
        out[n++] = {clip, t, a.blend};
    }
    const u32 prev = a.previous != 0 && a.blend < 1.0f ? find(a.previous) : ~0u;
    if (prev != ~0u) out[n++] = {prev, a.previous_time + a.phase, 1.0f - a.blend};
    return n;
}

// pose_nodes for one sample (bit-identical to the single-clip path), pose_nodes_blend otherwise.
inline void pose_samples(const render::GpuMesh& mesh, const render::ClipSample* samples, usize n, Span<math::mat4> out) {
    if (n == 1) render::pose_nodes(mesh, samples[0].clip, samples[0].time, out);
    else render::pose_nodes_blend(mesh, Span<const render::ClipSample>(samples, n), out);
}

}  // namespace mce::kit

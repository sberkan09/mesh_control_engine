#pragma once
// MeshStore — cooked CHNK_MESH payloads as GPU meshes (rhi-renderer.md §2.4, assets.md §7.1).
//
// Like TextureStore, meshes are addressed by a stable u32 key (the asset system's AssetKey) and
// can hold a few generations at once for hot reload; get(key) answers the newest. Each mesh keeps
// a CPU copy of what draw/cull/animation need: submesh ranges, dequantization, inline materials,
// the node tree, its rest pose and its animation clips. The vertex/index bytes go to two
// immutable GPU buffers and are not kept.
//
// A store created without a device keeps only the CPU side: culling, sorting and animation are
// testable headless.
//
// Skinning is optional per asset (FR-R3D-060): a mesh cooked from a glTF with skins also keeps
// its skins (joint nodes, inverse bind matrices, per-joint bind bounds) and uploads one
// interleaved SkinnedMeshVertex stream instead of the plain one. Joints are nodes, so the clips,
// pose_nodes and AnimationPlayer below drive skeletons exactly as they drive node hierarchies;
// skin_palette() turns a node pose into the joint matrices the skinned shaders read. A mesh
// without skins is untouched by all of this.

#include "mce/asset/formats.h"
#include "mce/containers/array.h"
#include "mce/containers/hash_map.h"
#include "mce/math/math.h"
#include "mce/rhi/render_device.h"

namespace mce::render {

// One level of a submesh's index ranges (level 0 = the submesh itself).
struct GpuLod {
    u32 first_index = 0;
    u32 index_count = 0;
    u32 base_vertex = 0;
    f32 error = 0.0f;  // node-local deviation from level 0
};

struct GpuSubmesh {
    u32 first_index = 0;
    u32 index_count = 0;
    u32 base_vertex = 0;
    GpuLod lods[asset::kMaxMeshLods];  // [0] mirrors the fields above
    u32 lod_count = 1;
    u32 node = 0;
    math::mat4 dequant;     // quantized [-1, 1] -> node-local (translate(center) * scale(extent))
    math::aabb3 bounds;     // node-local
    math::vec4 uv_rect;     // offset.xy, scale.xy
    asset::MaterialChunk material;
    asset::MaterialExt material_ext{};  // clear coat, sheen, anisotropy, transmission (M-VISUAL2 4); zero = none
    u32 albedo_key = ~0u;   // texture key of the base color map (~0u = none: white)
    u32 normal_key = ~0u;   // tangent-space normal map (~0u = none: the vertex normal)
    u32 orm_key = ~0u;      // occlusion (R, when OcclusionInMR) / roughness (G) / metallic (B)
    u32 emissive_key = ~0u; // emissive map, multiplied by the emissive factor
    u32 skin = ~0u;         // GpuMesh::skins index; ~0u = rigid (placed by `node`)
};

struct GpuSkin {
    u32 first_joint = 0;  // into GpuMesh::joints
    u32 joint_count = 0;
};

struct GpuJoint {
    u32 node = 0;
    math::mat4 inverse_bind;
    math::aabb3 bounds;       // bind space, of the vertices this joint moves
    bool influences = false;  // false: the joint moves no vertex (bounds unused)
    // The sidecar's ragdoll overrides for this bone (asset::MeshRagdoll flags; 0 = none).
    u32 ragdoll_flags = 0;
    f32 ragdoll_radius = 0.0f;  // metres
    f32 ragdoll_swing = 0.0f;   // degrees
    f32 ragdoll_twist = 0.0f;   // degrees
    f32 ragdoll_mass = 0.0f;    // weight on the bone's share of the mass
};

// The GPU vertex of a skinned mesh: the plain stream plus its influences (24 bytes).
struct SkinnedMeshVertex {
    asset::MeshVertex base;
    asset::MeshSkinVertex skin;
};
static_assert(sizeof(SkinnedMeshVertex) == 24);

struct GpuMeshNode {
    i32 parent = -1;
    math::vec3 translation;
    math::quat rotation;
    math::vec3 scale{1.0f, 1.0f, 1.0f};
};

struct GpuMeshClip {
    FixedString<64> name;
    f32 duration = 0.0f;
    u32 first_channel = 0;
    u32 channel_count = 0;
};

struct GpuMesh {
    u32 key = 0;
    u32 generation = 0;
    rhi::BufferHandle vertices;
    rhi::BufferHandle indices;
    rhi::IndexFormat index_format = rhi::IndexFormat::U16;
    math::aabb3 bounds;  // rest pose, model space
    Array<GpuSubmesh> submeshes;
    Array<GpuMeshNode> nodes;
    Array<FixedString<64>> node_names;
    Array<math::mat4> rest_pose;  // model-space node matrices
    Array<GpuMeshClip> clips;
    Array<asset::MeshChannel> channels;
    Array<f32> keys;
    Array<GpuSkin> skins;     // empty = a static / node-animated mesh
    Array<GpuJoint> joints;
    // LOD levels of the mesh as a whole: the most any submesh has, and per level the largest
    // submesh error (a submesh with fewer levels draws its coarsest past them).
    u32 lod_levels = 1;
    f32 lod_error[asset::kMaxMeshLods] = {};

    // The index range submesh `s` draws at mesh level `level`.
    const GpuLod& lod(u32 s, u32 level) const {
        const GpuSubmesh& sm = submeshes[s];
        return sm.lods[level < sm.lod_count ? level : sm.lod_count - 1];
    }

    // True when the vertex buffer holds SkinnedMeshVertex (every submesh then draws through the
    // skinned pipelines; rigid submeshes of such a mesh just skip the palette).
    bool skinned() const { return !skins.empty(); }
    u32 find_clip(StringView name) const;
    // Index of the node named `name`, or nodes.size() when absent (attachment points, joints).
    u32 find_node(StringView name) const;
    u32 triangle_count() const;
};

// Texture GUID -> texture key (the AssetKey the texture is or will be resident under). Returns
// ~0u when the GUID is unknown; the submesh then samples white.
struct TextureKeyResolver {
    u32 (*fn)(void* ctx, asset::Guid guid) = nullptr;
    void* ctx = nullptr;
    u32 operator()(asset::Guid g) const { return fn ? fn(ctx, g) : ~0u; }
};

class MeshStore {
public:
    static constexpr u32 kFirstLocalKey = 0x80000000u;  // keys below belong to the asset system

    MeshStore() = default;
    MeshStore(const MeshStore&) = delete;
    MeshStore& operator=(const MeshStore&) = delete;
    ~MeshStore();

    // `device` may be null: CPU side only (tests, headless tools).
    void init(rhi::RenderDevice* device) { device_ = device; }
    void shutdown();

    // Validates (MeshView::read) and uploads `blob` as `generation` of `key`; on failure the key
    // keeps its previous generation.
    ResultV upload(u32 key, u32 generation, Span<const byte> blob, TextureKeyResolver textures = {});
    void retire(u32 key, u32 generation);
    void release(u32 key);

    // Newest live generation of `key`, or null.
    const GpuMesh* get(u32 key) const;
    u32 live_meshes() const { return static_cast<u32>(meshes_.size()); }
    bool has_device() const { return device_ != nullptr; }

    // Keys for meshes made at runtime rather than cooked (terrain nodes, tools), above the asset
    // system's.
    u32 allocate_key() { return next_local_++; }

private:
    void destroy(GpuMesh& m);
    void refresh_newest(u32 key);

    rhi::RenderDevice* device_ = nullptr;
    Array<mem::UniquePtr<GpuMesh>> meshes_;  // newest generation of a key last
    HashMap<u32, GpuMesh*> newest_;          // key -> its newest generation (get() is per draw)
    u32 next_local_ = kFirstLocalKey;
};

// Model-space node matrices of `mesh` posed by clip `clip` at `time` seconds (looping), or the
// rest pose when `clip` is out of range. `out` must hold mesh.nodes.size() matrices. Every
// channel of the clip drives its node (multi-node tracks); LINEAR, STEP and CUBICSPLINE keys.
void pose_nodes(const GpuMesh& mesh, u32 clip, f32 time, Span<math::mat4> out);

// Joint matrices of skin `skin` for a node pose (pose_nodes output; empty = rest pose):
// out[j] = pose[joint j's node] * inverse_bind[j], bind space -> model space. `out` must hold the
// skin's joint_count matrices. The skinned vertex is sum_j weight_j * out[j] * bind position.
void skin_palette(const GpuMesh& mesh, u32 skin, Span<const math::mat4> pose, Span<math::mat4> out);

// Model-space bounds of skin `skin` under `palette` (skin_palette output): the union of every
// influencing joint's bind box moved by its matrix. Conservative: a blended vertex lies in the
// convex hull of its joints' moved positions.
math::aabb3 skin_bounds(const GpuMesh& mesh, u32 skin, Span<const math::mat4> palette);

// One clip's contribution to a blended pose.
struct ClipSample {
    u32 clip = ~0u;   // out of range = the rest pose
    f32 time = 0.0f;  // seconds, looping
    f32 weight = 1.0f;
};

// Weighted blend of clips (cross-fades, layered tracks) in local TRS space: translation and scale
// lerp, rotation nlerp on the shortest arc. A node a clip does not animate contributes its rest
// TRS with that clip's weight. Weights are normalized; all-zero weights give the rest pose.
void pose_nodes_blend(const GpuMesh& mesh, Span<const ClipSample> samples, Span<math::mat4> out);

// The same blend left in local TRS (M-GAMEPLAY3D 5: layers, masks and IK work on it before
// compose_local): `out` holds mesh.nodes.size() nodes. One sample with a positive weight samples
// exactly as pose_nodes does.
void sample_local(const GpuMesh& mesh, Span<const ClipSample> samples, Span<GpuMeshNode> out);
// One node's local TRS in clip `clip` at `time`, looping (its rest TRS where the clip does not
// animate it, or for an out-of-range clip).
GpuMeshNode sample_node(const GpuMesh& mesh, u32 clip, f32 time, u32 node);
// Model-space matrices of local TRS nodes (parents before children, as GpuMesh::nodes orders them).
void compose_local(Span<const GpuMeshNode> local, Span<math::mat4> out);

// A marker on a clip's timeline (FR-R3D-060 playback events: footsteps, hit frames). The game
// owns the list; `id` is whatever it dispatches on (a StringId, an enum).
struct ClipEvent {
    u32 clip = ~0u;
    f32 time = 0.0f;  // seconds into the clip
    u64 id = 0;
};

// Plays one clip at a time with timed cross-fades between clips (FR-R3D-060's "cross-fade with no
// pop", for node hierarchies and skeletons alike). Pure function of the calls made: deterministic.
class AnimationPlayer {
public:
    // Starts `clip` from time 0, cross-fading from the current clip over `fade` seconds (0 = cut).
    void play(u32 clip, f32 fade = 0.0f, f32 speed = 1.0f);
    void advance(f32 dt);
    void pose(const GpuMesh& mesh, Span<math::mat4> out) const;
    // Markers of the current clip the last advance() passed over: time in [before, after) of the
    // clip's looping timeline, once per pass (a long step over several loops repeats them). Ids go
    // to `out` in marker order, then by loop; returns how many were written (capped at out.size()).
    u32 events(const GpuMesh& mesh, Span<const ClipEvent> markers, Span<u64> out) const;

    u32 clip() const { return current_.clip; }
    f32 time() const { return current_.time; }
    // 1 when the current clip has fully faded in.
    f32 blend() const { return fade_length_ > 0.0f ? math::min(fade_ / fade_length_, 1.0f) : 1.0f; }

private:
    struct Track {
        u32 clip = ~0u;
        f32 time = 0.0f;
        f32 speed = 1.0f;
    };
    Track current_, previous_;
    f32 fade_ = 0.0f, fade_length_ = 0.0f;
    f32 last_time_ = 0.0f;  // current_.time before the last advance()
};

}  // namespace mce::render

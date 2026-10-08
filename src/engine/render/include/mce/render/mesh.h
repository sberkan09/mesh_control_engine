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

#include "mce/asset/formats.h"
#include "mce/containers/array.h"
#include "mce/math/math.h"
#include "mce/rhi/render_device.h"

namespace mce::render {

struct GpuSubmesh {
    u32 first_index = 0;
    u32 index_count = 0;
    u32 base_vertex = 0;
    u32 node = 0;
    math::mat4 dequant;     // quantized [-1, 1] -> node-local (translate(center) * scale(extent))
    math::aabb3 bounds;     // node-local
    math::vec4 uv_rect;     // offset.xy, scale.xy
    asset::MaterialChunk material;
    u32 albedo_key = ~0u;   // texture key of the base color map (~0u = none: white)
    u32 normal_key = ~0u;   // tangent-space normal map (~0u = none: the vertex normal)
    u32 orm_key = ~0u;      // occlusion (R, when OcclusionInMR) / roughness (G) / metallic (B)
    u32 emissive_key = ~0u; // emissive map, multiplied by the emissive factor
};

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
    Array<math::mat4> rest_pose;  // model-space node matrices
    Array<GpuMeshClip> clips;
    Array<asset::MeshChannel> channels;
    Array<f32> keys;

    u32 find_clip(StringView name) const;
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

private:
    void destroy(GpuMesh& m);

    rhi::RenderDevice* device_ = nullptr;
    Array<mem::UniquePtr<GpuMesh>> meshes_;  // newest generation of a key last
};

// Model-space node matrices of `mesh` posed by clip `clip` at `time` seconds (looping), or the
// rest pose when `clip` is out of range. `out` must hold mesh.nodes.size() matrices. Every
// channel of the clip drives its node (multi-node tracks); LINEAR, STEP and CUBICSPLINE keys.
void pose_nodes(const GpuMesh& mesh, u32 clip, f32 time, Span<math::mat4> out);

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

// Plays one clip at a time with timed cross-fades between clips (FR-R3D-060's "cross-fade with no
// pop" for node tracks). Pure function of the calls made: deterministic.
class AnimationPlayer {
public:
    // Starts `clip` from time 0, cross-fading from the current clip over `fade` seconds (0 = cut).
    void play(u32 clip, f32 fade = 0.0f, f32 speed = 1.0f);
    void advance(f32 dt);
    void pose(const GpuMesh& mesh, Span<math::mat4> out) const;

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
};

}  // namespace mce::render

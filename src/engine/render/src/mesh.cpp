#include "mce/render/mesh.h"

#include <cmath>

#include "mce/log.h"

namespace mce::render {

using namespace mce::math;

u32 GpuMesh::find_clip(StringView name) const {
    for (u32 i = 0; i < clips.size(); ++i)
        if (clips[i].name.view() == name) return i;
    return static_cast<u32>(clips.size());
}

u32 GpuMesh::triangle_count() const {
    u32 n = 0;
    for (const GpuSubmesh& s : submeshes) n += s.index_count / 3;
    return n;
}

MeshStore::~MeshStore() { shutdown(); }

void MeshStore::shutdown() {
    for (auto& m : meshes_) destroy(*m);
    meshes_.clear();
    device_ = nullptr;
}

void MeshStore::destroy(GpuMesh& m) {
    if (!device_) return;
    if (!m.vertices.is_null()) device_->destroy_buffer(m.vertices);
    if (!m.indices.is_null()) device_->destroy_buffer(m.indices);
    m.vertices = {};
    m.indices = {};
}

namespace {

mat4 node_local(const GpuMeshNode& n) { return trs(n.translation, n.rotation, n.scale); }

}  // namespace

ResultV MeshStore::upload(u32 key, u32 generation, Span<const byte> blob, TextureKeyResolver textures) {
    MCE_TRY_ASSIGN(asset::MeshView view, asset::MeshView::read(blob));
    auto mesh = mem::make_unique<GpuMesh>(mem::heap());
    GpuMesh& m = *mesh;
    m.key = key;
    m.generation = generation;
    const asset::MeshHeader& h = *view.header;
    m.bounds = {{h.bounds_min[0], h.bounds_min[1], h.bounds_min[2]}, {h.bounds_max[0], h.bounds_max[1], h.bounds_max[2]}};
    m.index_format = view.index32() ? rhi::IndexFormat::U32 : rhi::IndexFormat::U16;

    for (const asset::MeshNode& n : view.nodes) {
        GpuMeshNode g;
        g.parent = n.parent;
        g.translation = {n.translation[0], n.translation[1], n.translation[2]};
        g.rotation = normalize(quat{n.rotation[0], n.rotation[1], n.rotation[2], n.rotation[3]});
        g.scale = {n.scale[0], n.scale[1], n.scale[2]};
        m.nodes.push_back(g);
    }
    m.rest_pose.resize(m.nodes.size());
    for (usize i = 0; i < m.nodes.size(); ++i)
        m.rest_pose[i] = m.nodes[i].parent < 0 ? node_local(m.nodes[i]) : m.rest_pose[static_cast<usize>(m.nodes[i].parent)] * node_local(m.nodes[i]);

    for (const asset::MeshSubmesh& s : view.submeshes) {
        GpuSubmesh g;
        g.first_index = s.first_index;
        g.index_count = s.index_count;
        g.base_vertex = s.base_vertex;
        g.node = s.node;
        const vec3 c{s.center[0], s.center[1], s.center[2]}, e{s.extent[0], s.extent[1], s.extent[2]};
        g.dequant = translation(c) * scaling(e);
        g.bounds = {c - e, c + e};
        g.uv_rect = {s.uv_offset[0], s.uv_offset[1], s.uv_scale[0], s.uv_scale[1]};
        g.material = s.material;
        const asset::Guid tex = s.material.texture(asset::MaterialSlot::BaseColor);
        if (tex.hi != 0 || tex.lo != 0) g.albedo_key = textures(tex);
        m.submeshes.push_back(g);
    }
    for (u32 c = 0; c < view.clips.size(); ++c) {
        GpuMeshClip clip;
        clip.name.assign(view.clip_name(c));
        clip.duration = view.clips[c].duration;
        clip.first_channel = view.clips[c].first_channel;
        clip.channel_count = view.clips[c].channel_count;
        m.clips.push_back(clip);
    }
    for (const asset::MeshChannel& ch : view.channels) m.channels.push_back(ch);
    for (f32 k : view.keys) m.keys.push_back(k);

    if (device_ && !view.vertices.empty() && !view.indices.empty()) {
        rhi::BufferDesc vd;
        vd.usage = rhi::BufferUsage::Vertex;
        vd.size_bytes = view.vertices.size_bytes();
        vd.debug_name = "asset.mesh.vertices";
        m.vertices = device_->create_buffer(vd, std::as_bytes(view.vertices));
        rhi::BufferDesc id;
        id.usage = rhi::BufferUsage::Index;
        id.size_bytes = view.indices.size();
        id.debug_name = "asset.mesh.indices";
        m.indices = device_->create_buffer(id, view.indices);
    }
    meshes_.push_back(std::move(mesh));
    return ok();
}

void MeshStore::retire(u32 key, u32 generation) {
    for (usize i = 0; i < meshes_.size(); ++i) {
        if (meshes_[i]->key != key || meshes_[i]->generation != generation) continue;
        destroy(*meshes_[i]);
        meshes_.erase(i);
        return;
    }
}

void MeshStore::release(u32 key) {
    for (usize i = meshes_.size(); i-- > 0;) {
        if (meshes_[i]->key != key) continue;
        destroy(*meshes_[i]);
        meshes_.erase(i);
    }
}

const GpuMesh* MeshStore::get(u32 key) const {
    for (usize i = meshes_.size(); i-- > 0;)
        if (meshes_[i]->key == key) return meshes_[i].get();
    return nullptr;
}

namespace {

// Index of the last key with time <= t (0 when t precedes every key).
u32 find_key(const f32* times, u32 count, f32 t) {
    u32 lo = 0, hi = count;
    while (hi - lo > 1) {
        const u32 mid = (lo + hi) / 2;
        if (times[mid] <= t) lo = mid;
        else hi = mid;
    }
    return lo;
}

}  // namespace

void pose_nodes(const GpuMesh& mesh, u32 clip, f32 time, Span<mat4> out) {
    const usize n = mesh.nodes.size();
    MCE_ASSERT_MSG(out.size() >= n, "pose_nodes: output too small");
    if (clip >= mesh.clips.size()) {
        for (usize i = 0; i < n; ++i) out[i] = mesh.rest_pose[i];
        return;
    }
    const GpuMeshClip& c = mesh.clips[clip];
    const f32 t = c.duration > 0.0f ? std::fmod(math::max(time, 0.0f), c.duration) : 0.0f;
    // Local TRS per node, starting from the rest pose; channels overwrite their component.
    constexpr usize kInline = 64;
    GpuMeshNode inline_nodes[kInline];
    Array<GpuMeshNode> heap_nodes;
    GpuMeshNode* local = inline_nodes;
    if (n > kInline) {
        heap_nodes.resize(n);
        local = heap_nodes.data();
    }
    for (usize i = 0; i < n; ++i) local[i] = mesh.nodes[i];
    for (u32 ci = 0; ci < c.channel_count; ++ci) {
        const asset::MeshChannel& ch = mesh.channels[c.first_channel + ci];
        const f32* times = mesh.keys.data() + ch.times_offset;
        const f32* values = mesh.keys.data() + ch.values_offset;
        const u32 k = find_key(times, ch.key_count, t);
        const u32 k1 = k + 1 < ch.key_count ? k + 1 : k;
        f32 a = 0.0f;
        if (ch.interp == asset::AnimInterp::Linear && k1 != k && times[k1] > times[k])
            a = math::clamp((t - times[k]) / (times[k1] - times[k]), 0.0f, 1.0f);
        GpuMeshNode& node = local[ch.node];
        if (ch.path == asset::AnimPath::Rotation) {
            const f32* q0 = values + usize(k) * 4;
            const f32* q1 = values + usize(k1) * 4;
            node.rotation = normalize(slerp(quat{q0[0], q0[1], q0[2], q0[3]}, quat{q1[0], q1[1], q1[2], q1[3]}, a));
        } else {
            const f32* v0 = values + usize(k) * 3;
            const f32* v1 = values + usize(k1) * 3;
            const vec3 v = lerp(vec3{v0[0], v0[1], v0[2]}, vec3{v1[0], v1[1], v1[2]}, a);
            if (ch.path == asset::AnimPath::Translation) node.translation = v;
            else node.scale = v;
        }
    }
    for (usize i = 0; i < n; ++i)
        out[i] = local[i].parent < 0 ? node_local(local[i]) : out[static_cast<usize>(local[i].parent)] * node_local(local[i]);
}

}  // namespace mce::render

#include "mce/render/mesh.h"

#include <cmath>

#include "mce/log.h"
#include "mce/render/scene3d.h"

namespace mce::render {

using namespace mce::math;

u32 GpuMesh::find_clip(StringView name) const {
    for (u32 i = 0; i < clips.size(); ++i)
        if (clips[i].name.view() == name) return i;
    return static_cast<u32>(clips.size());
}

u32 GpuMesh::find_node(StringView name) const {
    for (u32 i = 0; i < node_names.size(); ++i)
        if (node_names[i].view() == name) return i;
    return static_cast<u32>(nodes.size());
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
    newest_.clear();
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

    for (u32 i = 0; i < view.nodes.size(); ++i) {
        const asset::MeshNode& n = view.nodes[i];
        m.node_names.push_back(FixedString<64>(view.node_name(i)));
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

    for (u32 si = 0; si < view.submeshes.size(); ++si) {
        const asset::MeshSubmesh& s = view.submeshes[si];
        GpuSubmesh g;
        g.first_index = s.first_index;
        g.index_count = s.index_count;
        g.base_vertex = s.base_vertex;
        g.lods[0] = GpuLod{s.first_index, s.index_count, s.base_vertex, 0.0f};
        for (const asset::MeshLod& l : view.submesh_lods(si))
            g.lods[g.lod_count++] = GpuLod{l.first_index, l.index_count, l.base_vertex, l.error};
        m.lod_levels = max(m.lod_levels, g.lod_count);
        for (u32 level = 1; level < asset::kMaxMeshLods; ++level)
            m.lod_error[level] = max(m.lod_error[level], g.lods[min(level, g.lod_count - 1)].error);
        g.node = s.node;
        const vec3 c{s.center[0], s.center[1], s.center[2]}, e{s.extent[0], s.extent[1], s.extent[2]};
        g.dequant = translation(c) * scaling(e);
        g.bounds = {c - e, c + e};
        g.uv_rect = {s.uv_offset[0], s.uv_offset[1], s.uv_scale[0], s.uv_scale[1]};
        g.material = s.material;
        auto key_of = [&](asset::MaterialSlot slot) {
            const asset::Guid tex = s.material.texture(slot);
            return tex.hi != 0 || tex.lo != 0 ? textures(tex) : ~0u;
        };
        g.albedo_key = key_of(asset::MaterialSlot::BaseColor);
        g.normal_key = key_of(asset::MaterialSlot::Normal);
        g.orm_key = key_of(asset::MaterialSlot::MetallicRoughness);
        g.emissive_key = key_of(asset::MaterialSlot::Emissive);
        m.submeshes.push_back(g);
    }
    if (view.skinned()) {
        for (u32 s = 0; s < view.submeshes.size(); ++s)
            if (view.submesh_skins[s] >= 0) m.submeshes[s].skin = static_cast<u32>(view.submesh_skins[s]);
        for (const asset::MeshSkin& sk : view.skins) m.skins.push_back(GpuSkin{sk.first_joint, sk.joint_count});
        for (const asset::MeshJoint& j : view.joints) {
            GpuJoint g;
            g.node = j.node;
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r) g.inverse_bind.c[c][r] = j.inverse_bind[c * 4 + r];
            g.influences = j.bounds_min[0] <= j.bounds_max[0];
            g.bounds = {{j.bounds_min[0], j.bounds_min[1], j.bounds_min[2]}, {j.bounds_max[0], j.bounds_max[1], j.bounds_max[2]}};
            m.joints.push_back(g);
        }
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
        vd.debug_name = "asset.mesh.vertices";
        if (m.skinned()) {  // one interleaved stream: the RHI binds a single vertex buffer
            Array<SkinnedMeshVertex> sv;
            sv.resize(view.vertices.size());
            for (usize i = 0; i < sv.size(); ++i) sv[i] = SkinnedMeshVertex{view.vertices[i], view.skin_vertices[i]};
            vd.size_bytes = sv.span().size_bytes();
            vd.debug_name = "asset.mesh.skinned_vertices";
            m.vertices = device_->create_buffer(vd, std::as_bytes(sv.span()));
        } else {
            vd.size_bytes = view.vertices.size_bytes();
            m.vertices = device_->create_buffer(vd, std::as_bytes(view.vertices));
        }
        rhi::BufferDesc id;
        id.usage = rhi::BufferUsage::Index;
        id.size_bytes = view.indices.size();
        id.debug_name = "asset.mesh.indices";
        m.indices = device_->create_buffer(id, view.indices);
    }
    newest_.insert_or_assign(key, mesh.get());
    meshes_.push_back(std::move(mesh));
    return ok();
}

void MeshStore::refresh_newest(u32 key) {
    for (usize i = meshes_.size(); i-- > 0;)
        if (meshes_[i]->key == key) {
            newest_.insert_or_assign(key, meshes_[i].get());
            return;
        }
    newest_.erase(key);
}

void MeshStore::retire(u32 key, u32 generation) {
    for (usize i = 0; i < meshes_.size(); ++i) {
        if (meshes_[i]->key != key || meshes_[i]->generation != generation) continue;
        destroy(*meshes_[i]);
        meshes_.erase(i);
        refresh_newest(key);
        return;
    }
}

void MeshStore::release(u32 key) {
    for (usize i = meshes_.size(); i-- > 0;) {
        if (meshes_[i]->key != key) continue;
        destroy(*meshes_[i]);
        meshes_.erase(i);
    }
    newest_.erase(key);
}

const GpuMesh* MeshStore::get(u32 key) const {
    GpuMesh* const* m = newest_.find(key);
    return m ? *m : nullptr;
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


// Local TRS of every node, inline for small meshes.
class LocalPose {
public:
    explicit LocalPose(usize n) : n_(n) {
        if (n > kInline) {
            heap_.resize(n);
            data_ = heap_.data();
        }
    }
    GpuMeshNode& operator[](usize i) { return data_[i]; }
    const GpuMeshNode& operator[](usize i) const { return data_[i]; }
    usize size() const { return n_; }

private:
    static constexpr usize kInline = 64;
    GpuMeshNode inline_[kInline];
    Array<GpuMeshNode> heap_;
    GpuMeshNode* data_ = inline_;
    usize n_;
};

// Cubic Hermite over glTF CUBICSPLINE triplets {in, value, out}: p0 = value k, m0 = out k * dt,
// p1 = value k1, m1 = in k1 * dt (glTF 2.0 Appendix C).
void hermite(const f32* v0, const f32* v1, u32 width, f32 a, f32 dt, f32* out) {
    const f32 a2 = a * a, a3 = a2 * a;
    const f32 h00 = 2 * a3 - 3 * a2 + 1, h10 = a3 - 2 * a2 + a, h01 = -2 * a3 + 3 * a2, h11 = a3 - a2;
    for (u32 c = 0; c < width; ++c) {
        const f32 p0 = v0[width + c], m0 = v0[2 * width + c] * dt;
        const f32 p1 = v1[width + c], m1 = v1[c] * dt;
        out[c] = h00 * p0 + h10 * m0 + h01 * p1 + h11 * m1;
    }
}

// Overwrites the animated components of `local` with clip `clip` sampled at `time` (looping).
// `touched`, when given, gets bit 0/1/2 (T/R/S) set per animated node component.
void sample_clip(const GpuMesh& mesh, u32 clip, f32 time, LocalPose& local, u8* touched) {
    const GpuMeshClip& c = mesh.clips[clip];
    const f32 t = c.duration > 0.0f ? std::fmod(math::max(time, 0.0f), c.duration) : 0.0f;
    for (u32 ci = 0; ci < c.channel_count; ++ci) {
        const asset::MeshChannel& ch = mesh.channels[c.first_channel + ci];
        const f32* times = mesh.keys.data() + ch.times_offset;
        const f32* values = mesh.keys.data() + ch.values_offset;
        const u32 width = ch.path == asset::AnimPath::Rotation ? 4 : 3;
        const bool cubic = ch.interp == asset::AnimInterp::CubicSpline;
        const u32 stride = cubic ? width * 3 : width;
        const u32 k = find_key(times, ch.key_count, t);
        const u32 k1 = k + 1 < ch.key_count ? k + 1 : k;
        const f32 dt = times[k1] - times[k];
        f32 a = 0.0f;
        if (ch.interp != asset::AnimInterp::Step && k1 != k && dt > 0.0f) a = math::clamp((t - times[k]) / dt, 0.0f, 1.0f);
        const f32* v0 = values + usize(k) * stride;
        const f32* v1 = values + usize(k1) * stride;
        f32 v[4];
        if (cubic) {
            if (k1 == k) {
                for (u32 i = 0; i < width; ++i) v[i] = v0[width + i];
            } else {
                hermite(v0, v1, width, a, dt, v);
            }
        } else if (ch.path == asset::AnimPath::Rotation) {
            const quat q = slerp(quat{v0[0], v0[1], v0[2], v0[3]}, quat{v1[0], v1[1], v1[2], v1[3]}, a);
            v[0] = q.x, v[1] = q.y, v[2] = q.z, v[3] = q.w;
        } else {
            for (u32 i = 0; i < 3; ++i) v[i] = math::lerp(v0[i], v1[i], a);
        }
        GpuMeshNode& node = local[ch.node];
        switch (ch.path) {
            case asset::AnimPath::Translation: node.translation = {v[0], v[1], v[2]}; break;
            case asset::AnimPath::Rotation: node.rotation = normalize(quat{v[0], v[1], v[2], v[3]}); break;
            case asset::AnimPath::Scale: node.scale = {v[0], v[1], v[2]}; break;
        }
        if (touched) touched[ch.node] |= static_cast<u8>(1u << static_cast<u32>(ch.path));
    }
}

void compose(const LocalPose& local, Span<mat4> out) {
    for (usize i = 0; i < local.size(); ++i)
        out[i] = local[i].parent < 0 ? node_local(local[i]) : out[static_cast<usize>(local[i].parent)] * node_local(local[i]);
}

}  // namespace

void pose_nodes(const GpuMesh& mesh, u32 clip, f32 time, Span<mat4> out) {
    const usize n = mesh.nodes.size();
    MCE_ASSERT_MSG(out.size() >= n, "pose_nodes: output too small");
    if (clip >= mesh.clips.size()) {
        for (usize i = 0; i < n; ++i) out[i] = mesh.rest_pose[i];
        return;
    }
    // Local TRS per node, starting from the rest pose; channels overwrite their component.
    LocalPose local(n);
    for (usize i = 0; i < n; ++i) local[i] = mesh.nodes[i];
    sample_clip(mesh, clip, time, local, nullptr);
    compose(local, out);
}

void pose_nodes_blend(const GpuMesh& mesh, Span<const ClipSample> samples, Span<mat4> out) {
    const usize n = mesh.nodes.size();
    MCE_ASSERT_MSG(out.size() >= n, "pose_nodes_blend: output too small");
    f32 total = 0.0f;
    for (const ClipSample& s : samples) total += math::max(s.weight, 0.0f);
    if (!(total > 0.0f)) {
        for (usize i = 0; i < n; ++i) out[i] = mesh.rest_pose[i];
        return;
    }
    LocalPose acc(n), one(n);
    for (usize i = 0; i < n; ++i) {
        acc[i] = mesh.nodes[i];
        acc[i].translation = {};
        acc[i].scale = {};
        acc[i].rotation = {0, 0, 0, 0};
    }
    for (const ClipSample& s : samples) {
        const f32 w = math::max(s.weight, 0.0f) / total;
        if (w <= 0.0f) continue;
        for (usize i = 0; i < n; ++i) one[i] = mesh.nodes[i];
        if (s.clip < mesh.clips.size()) sample_clip(mesh, s.clip, s.time, one, nullptr);
        for (usize i = 0; i < n; ++i) {
            acc[i].translation += one[i].translation * w;
            acc[i].scale += one[i].scale * w;
            quat q = one[i].rotation;
            if (dot(q, mesh.nodes[i].rotation) < 0.0f) q = {-q.x, -q.y, -q.z, -q.w};  // one hemisphere
            acc[i].rotation = {acc[i].rotation.x + q.x * w, acc[i].rotation.y + q.y * w, acc[i].rotation.z + q.z * w,
                               acc[i].rotation.w + q.w * w};
        }
    }
    for (usize i = 0; i < n; ++i) {
        const quat q = normalize(acc[i].rotation);
        acc[i].rotation = dot(q, q) > 0.0f ? q : mesh.nodes[i].rotation;
    }
    compose(acc, out);
}

void skin_palette(const GpuMesh& mesh, u32 skin, Span<const mat4> pose, Span<mat4> out) {
    MCE_ASSERT_MSG(skin < mesh.skins.size(), "skin_palette: skin out of range");
    const GpuSkin& s = mesh.skins[skin];
    MCE_ASSERT_MSG(out.size() >= s.joint_count, "skin_palette: output too small");
    for (u32 j = 0; j < s.joint_count; ++j) {
        const GpuJoint& joint = mesh.joints[s.first_joint + j];
        const mat4& node = joint.node < pose.size() ? pose[joint.node] : mesh.rest_pose[joint.node];
        out[j] = node * joint.inverse_bind;
    }
}

aabb3 skin_bounds(const GpuMesh& mesh, u32 skin, Span<const mat4> palette) {
    const GpuSkin& s = mesh.skins[skin];
    aabb3 out{};
    bool any = false;
    for (u32 j = 0; j < s.joint_count && j < palette.size(); ++j) {
        const GpuJoint& joint = mesh.joints[s.first_joint + j];
        if (!joint.influences) continue;
        const aabb3 b = transform_aabb(palette[j], joint.bounds);
        out = any ? merge(out, b) : b;
        any = true;
    }
    return out;
}

void AnimationPlayer::play(u32 clip, f32 fade, f32 speed) {
    if (fade > 0.0f && current_.clip != ~0u) {
        previous_ = current_;
        fade_length_ = fade;
    } else {
        previous_ = Track{};
        fade_length_ = 0.0f;
    }
    fade_ = 0.0f;
    current_ = Track{clip, 0.0f, speed};
    last_time_ = 0.0f;
}

void AnimationPlayer::advance(f32 dt) {
    last_time_ = current_.time;
    current_.time += dt * current_.speed;
    if (previous_.clip != ~0u) {
        previous_.time += dt * previous_.speed;
        fade_ += dt;
        if (fade_ >= fade_length_) {
            previous_ = Track{};
            fade_length_ = 0.0f;
        }
    }
}

u32 AnimationPlayer::events(const GpuMesh& mesh, Span<const ClipEvent> markers, Span<u64> out) const {
    if (current_.clip >= mesh.clips.size()) return 0;
    const f32 duration = mesh.clips[current_.clip].duration;
    const f32 lo = math::min(last_time_, current_.time), hi = math::max(last_time_, current_.time);
    u32 n = 0;
    for (const ClipEvent& e : markers) {
        if (e.clip != current_.clip) continue;
        if (!(duration > 0.0f)) {  // a single-key clip: its markers fire once, when time starts
            if (lo <= 0.0f && hi > 0.0f && e.time <= 0.0f && n < out.size()) out[n++] = e.id;
            continue;
        }
        // Passes of e.time + k * duration through [lo, hi): ceil((hi - t) / d) - ceil((lo - t) / d).
        const f32 t = math::clamp(e.time, 0.0f, duration);
        const auto passes = static_cast<i64>(std::ceil(f64(hi - t) / duration) - std::ceil(f64(lo - t) / duration));
        for (i64 k = 0; k < passes && n < out.size(); ++k) out[n++] = e.id;
    }
    return n;
}

void AnimationPlayer::pose(const GpuMesh& mesh, Span<mat4> out) const {
    if (previous_.clip == ~0u) {
        pose_nodes(mesh, current_.clip, current_.time, out);
        return;
    }
    // Smoothstep weight: no velocity pop at either end of the fade.
    const f32 x = blend();
    const f32 w = x * x * (3.0f - 2.0f * x);
    const ClipSample s[2] = {{previous_.clip, previous_.time, 1.0f - w}, {current_.clip, current_.time, w}};
    pose_nodes_blend(mesh, Span<const ClipSample>(s, 2), out);
}

}  // namespace mce::render

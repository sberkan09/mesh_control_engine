#pragma once
// Builds CHNK_MESH payloads in tests (no cook): boxes as submeshes, quantized exactly the way the
// glTF importer quantizes, plus an optional one-node rotation clip and optional skins.

#include <cmath>
#include <initializer_list>

#include "mce/asset/formats.h"
#include "mce/math/math.h"

namespace mesh_fixture {

using namespace mce;

struct Box {
    math::vec3 center;
    math::vec3 half;
    u32 node = 0;
    f32 color[4] = {1, 1, 1, 1};
    u32 flags = 0;  // asset::MaterialFlags
};

struct Builder {
    Array<asset::MeshVertex> vertices;
    Array<u16> indices;
    Array<asset::MeshSubmesh> submeshes;
    Array<asset::MeshNode> nodes;
    Array<asset::MeshClip> clips;
    Array<asset::MeshChannel> channels;
    Array<f32> keys;
    Array<char> strings;
    Array<asset::MeshSkin> skins;
    Array<asset::MeshJoint> joints;
    Array<asset::MeshSkinVertex> skin_vertices;  // sized by build()
    Array<i32> submesh_skins;                    // sized by build(); -1 = rigid
    Array<Array<asset::MeshLod>> lods;           // per submesh (add_lod), finest first

    // Rest-pose model translation of `node` (fixture nodes only translate).
    math::vec3 rest_position(u32 node) const {
        math::vec3 p{};
        for (i32 n = static_cast<i32>(node); n >= 0; n = nodes[static_cast<usize>(n)].parent)
            p += math::vec3{nodes[static_cast<usize>(n)].translation[0], nodes[static_cast<usize>(n)].translation[1],
                            nodes[static_cast<usize>(n)].translation[2]};
        return p;
    }

    // Skins submesh `s` over `joint_nodes` (inverse bind = the inverse of each joint's rest
    // matrix); every vertex goes wholly to joint `joint_of(bind position)`.
    template <class F>
    void skin_submesh(u32 s, std::initializer_list<u32> joint_nodes, F joint_of) {
        asset::MeshSkin skin{};
        skin.first_joint = static_cast<u32>(joints.size());
        skin.joint_count = static_cast<u32>(joint_nodes.size());
        for (u32 node : joint_nodes) {
            asset::MeshJoint j{};
            j.node = node;
            const math::vec3 t = rest_position(node);
            for (int k = 0; k < 16; ++k) j.inverse_bind[k] = k % 5 == 0 ? 1.0f : 0.0f;
            j.inverse_bind[12] = -t.x;
            j.inverse_bind[13] = -t.y;
            j.inverse_bind[14] = -t.z;
            j.bounds_min[0] = 1.0f;  // empty until a vertex lands on it
            j.bounds_max[0] = -1.0f;
            joints.push_back(j);
        }
        skins.push_back(skin);
        skin_more(s, static_cast<u32>(skins.size() - 1), joint_of);
    }

    // Skins submesh `s` over the joints of an existing skin (several submeshes, one skeleton).
    template <class F>
    void skin_more(u32 s, u32 skin_index, F joint_of) {
        skin_vertices.resize(vertices.size(), asset::MeshSkinVertex{});
        submesh_skins.resize(submeshes.size(), -1);
        const asset::MeshSkin& skin = skins[skin_index];
        const asset::MeshSubmesh& sm = submeshes[s];
        for (u32 v = 0; v < sm.vertex_count; ++v) {
            const asset::MeshVertex& q = vertices[sm.base_vertex + v];
            const f32 p[3] = {sm.center[0] + q.position[0] / 32767.0f * sm.extent[0], sm.center[1] + q.position[1] / 32767.0f * sm.extent[1],
                              sm.center[2] + q.position[2] / 32767.0f * sm.extent[2]};
            const u32 j = joint_of(math::vec3{p[0], p[1], p[2]});
            asset::MeshSkinVertex& sv = skin_vertices[sm.base_vertex + v];
            sv = {};
            sv.joints[0] = static_cast<u8>(j);
            sv.weights[0] = 255;
            asset::MeshJoint& mj = joints[skin.first_joint + j];
            const bool first = mj.bounds_min[0] > mj.bounds_max[0];
            for (int c = 0; c < 3; ++c) {
                mj.bounds_min[c] = first ? p[c] : math::min(mj.bounds_min[c], p[c]);
                mj.bounds_max[c] = first ? p[c] : math::max(mj.bounds_max[c], p[c]);
            }
        }
        submesh_skins[s] = static_cast<i32>(skin_index);
    }

    u32 add_node(i32 parent, math::vec3 t = {}, const char* name = nullptr) {
        asset::MeshNode n{};
        n.parent = parent;
        n.name_offset = static_cast<u32>(strings.size());
        for (const char* p = name; p && *p; ++p) strings.push_back(*p);
        n.name_size = static_cast<u32>(strings.size()) - n.name_offset;
        n.translation[0] = t.x;
        n.translation[1] = t.y;
        n.translation[2] = t.z;
        n.scale[0] = n.scale[1] = n.scale[2] = 1.0f;
        n.rotation[3] = 1.0f;
        nodes.push_back(n);
        return static_cast<u32>(nodes.size() - 1);
    }

    static i16 snorm(f32 v) { return static_cast<i16>(std::lround(math::clamp(v, -1.0f, 1.0f) * 32767.0f)); }

    void add_box(const Box& b) {
        asset::MeshSubmesh sm{};
        sm.first_index = static_cast<u32>(indices.size());
        sm.base_vertex = static_cast<u32>(vertices.size());
        sm.node = b.node;
        sm.center[0] = b.center.x;
        sm.center[1] = b.center.y;
        sm.center[2] = b.center.z;
        sm.extent[0] = math::max(b.half.x, 1e-6f);
        sm.extent[1] = math::max(b.half.y, 1e-6f);
        sm.extent[2] = math::max(b.half.z, 1e-6f);
        sm.uv_scale[0] = sm.uv_scale[1] = 1.0f;
        for (int i = 0; i < 4; ++i) sm.material.base_color[i] = b.color[i];
        sm.material.roughness = 0.8f;
        sm.material.flags = b.flags;
        const f32 axes[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        u32 local = 0;
        for (const auto& n : axes) {
            // Two in-plane axes with u x v = n (CCW faces seen from outside).
            math::vec3 nn{n[0], n[1], n[2]};
            math::vec3 u = std::fabs(nn.y) > 0.5f ? math::vec3{1, 0, 0} : math::vec3{0, 1, 0};
            math::vec3 v = math::cross(nn, u);
            u = math::cross(v, nn);
            const f32 su[4] = {-1, 1, 1, -1}, sv[4] = {-1, -1, 1, 1};
            for (int c = 0; c < 4; ++c) {
                const math::vec3 p = nn + u * su[c] + v * sv[c];  // in [-1, 1]^3: already quantized space
                asset::MeshVertex q{};
                q.position[0] = snorm(p.x);
                q.position[1] = snorm(p.y);
                q.position[2] = snorm(p.z);
                // Octahedral encode of an axis normal.
                f32 ox = nn.x, oy = nn.y;
                if (nn.z < 0) {
                    ox = (1.0f - std::fabs(nn.y)) * (nn.x >= 0 ? 1.0f : -1.0f);
                    oy = (1.0f - std::fabs(nn.x)) * (nn.y >= 0 ? 1.0f : -1.0f);
                }
                q.normal[0] = snorm(ox);
                q.normal[1] = snorm(oy);
                vertices.push_back(q);
            }
            // With u x v = n and corners (-,-) (+,-) (+,+) (-,+) the quad winds CCW about n.
            const u16 quad[6] = {0, 1, 2, 2, 3, 0};
            for (u16 i : quad) indices.push_back(static_cast<u16>(local + i));
            local += 4;
        }
        sm.vertex_count = local;
        sm.index_count = 36;
        submeshes.push_back(sm);
    }

    // A coarser level of submesh `s`: its first `triangles` triangles again (same vertices), with
    // geometric error `error`.
    void add_lod(u32 s, u32 triangles, f32 error) {
        lods.resize(submeshes.size());
        const asset::MeshSubmesh& sm = submeshes[s];
        asset::MeshLod l{};
        l.first_index = static_cast<u32>(indices.size());
        l.index_count = triangles * 3;
        l.base_vertex = sm.base_vertex;
        l.vertex_count = sm.vertex_count;
        l.error = error;
        for (u32 i = 0; i < l.index_count; ++i) indices.push_back(indices[sm.first_index + i]);
        lods[s].push_back(l);
    }

    // One clip rotating `node` a full turn about +Y over `duration` seconds (5 linear keys).
    void add_spin_clip(u32 node, f32 duration, const char* name = "spin") {
        asset::MeshClip c{};
        c.name_offset = static_cast<u32>(strings.size());
        for (const char* p = name; *p; ++p) strings.push_back(*p);
        c.name_size = static_cast<u32>(strings.size()) - c.name_offset;
        c.duration = duration;
        c.first_channel = static_cast<u32>(channels.size());
        c.channel_count = 1;
        asset::MeshChannel ch{};
        ch.node = node;
        ch.path = asset::AnimPath::Rotation;
        ch.interp = asset::AnimInterp::Linear;
        ch.key_count = 5;
        ch.times_offset = static_cast<u32>(keys.size());
        for (int k = 0; k < 5; ++k) keys.push_back(duration * f32(k) / 4.0f);
        ch.values_offset = static_cast<u32>(keys.size());
        for (int k = 0; k < 5; ++k) {
            const math::quat q = math::quat_from_axis_angle({0, 1, 0}, math::kPi * 0.5f * f32(k));
            keys.push_back(q.x);
            keys.push_back(q.y);
            keys.push_back(q.z);
            keys.push_back(q.w);
        }
        channels.push_back(ch);
        clips.push_back(c);
    }

    Array<byte> build() const {
        asset::MeshData d;
        d.vertices = vertices.span();
        d.indices = std::as_bytes(indices.span());
        Array<asset::MeshSubmesh> subs = submeshes;
        Array<asset::MeshLod> table;
        for (usize s = 0; s < lods.size() && s < subs.size(); ++s) {
            subs[s].reserved[0] = static_cast<u32>(lods[s].size());
            subs[s].reserved[1] = lods[s].empty() ? 0u : static_cast<u32>(table.size());
            for (const asset::MeshLod& l : lods[s]) table.push_back(l);
        }
        d.lods = table.span();
        d.submeshes = subs.span();
        d.nodes = nodes.span();
        d.clips = clips.span();
        d.channels = channels.span();
        d.keys = keys.span();
        d.strings = strings.span();
        Array<asset::MeshSkinVertex> sv = skin_vertices;
        Array<i32> ss = submesh_skins;
        if (!skins.empty()) {
            sv.resize(vertices.size(), asset::MeshSkinVertex{});
            ss.resize(submeshes.size(), -1);
            d.skins = skins.span();
            d.joints = joints.span();
            d.skin_vertices = sv.span();
            d.submesh_skins = ss.span();
            for (const asset::MeshJoint& j : joints)
                if (j.ragdoll_flags != 0) d.flags |= asset::MeshFlags::Ragdoll;
        }
        bool any = false;
        for (const asset::MeshSubmesh& s : submeshes)
            for (int k = 0; k < 3; ++k) {
                const f32 lo = s.center[k] - s.extent[k] + nodes[s.node].translation[k];
                const f32 hi = s.center[k] + s.extent[k] + nodes[s.node].translation[k];
                d.bounds_min[k] = any ? math::min(d.bounds_min[k], lo) : lo;
                d.bounds_max[k] = any ? math::max(d.bounds_max[k], hi) : hi;
                if (k == 2) any = true;
            }
        Array<byte> out;
        asset::write_mesh(d, out);
        return out;
    }
};

}  // namespace mesh_fixture

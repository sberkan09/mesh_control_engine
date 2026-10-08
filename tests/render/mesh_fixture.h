#pragma once
// Builds CHNK_MESH payloads in tests (no cook): boxes as submeshes, quantized exactly the way the
// glTF importer quantizes, plus an optional one-node rotation clip.

#include <cmath>

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

    u32 add_node(i32 parent, math::vec3 t = {}) {
        asset::MeshNode n{};
        n.parent = parent;
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
        d.submeshes = submeshes.span();
        d.nodes = nodes.span();
        d.clips = clips.span();
        d.channels = channels.span();
        d.keys = keys.span();
        d.strings = strings.span();
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

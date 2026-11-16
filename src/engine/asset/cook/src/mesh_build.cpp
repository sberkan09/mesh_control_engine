// Format-neutral mesh back end (see mesh_build.h). Moved from gltf_importer.cpp without changing a float operation.

#include "mesh_build.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "meshoptimizer.h"

namespace mce::cook::detail {

namespace {

using namespace mce::math;

struct FatVertex {
    f32 p[3];
    f32 n[3];
    f32 uv[2];
    f32 joints[4];   // skinned primitives only (zero otherwise): skin-local joint indices
    f32 weights[4];  // normalized to sum 1
};

i16 snorm16(f32 v) {
    const f32 c = math::clamp(v, -1.0f, 1.0f);
    return static_cast<i16>(std::lround(c * 32767.0f));
}

u16 unorm16(f32 v) { return static_cast<u16>(std::lround(math::clamp(v, 0.0f, 1.0f) * 65535.0f)); }

// Octahedral encoding (Cigolle et al. 2014) of a unit vector into [-1, 1]^2.
void oct_encode(const f32 n[3], f32 out[2]) {
    const f32 l1 = std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2]);
    f32 x = n[0] / l1, y = n[1] / l1;
    if (n[2] < 0.0f) {
        const f32 ox = x, oy = y;
        x = (1.0f - std::fabs(oy)) * (ox >= 0.0f ? 1.0f : -1.0f);
        y = (1.0f - std::fabs(ox)) * (oy >= 0.0f ? 1.0f : -1.0f);
    }
    out[0] = x;
    out[1] = y;
}

// Rounds normalized weights to unorm8 summing to exactly 255 (largest remainder; ties by slot).
void quantize_weights(const f32 w[4], u8 out[4]) {
    u32 total = 0;
    f32 rem[4];
    for (int c = 0; c < 4; ++c) {
        const f32 x = math::clamp(w[c], 0.0f, 1.0f) * 255.0f;
        out[c] = static_cast<u8>(std::floor(x));
        rem[c] = x - std::floor(x);
        total += out[c];
    }
    while (total < 255) {
        int best = 0;
        for (int c = 1; c < 4; ++c)
            if (rem[c] > rem[best]) best = c;
        if (rem[best] <= 0.0f) {  // all remainders spent: the heaviest influence takes the rest
            for (int c = 1; c < 4; ++c)
                if (out[c] > out[best]) best = c;
            out[best] = static_cast<u8>(out[best] + (255 - total));
            return;
        }
        ++out[best];
        rem[best] = 0.0f;
        ++total;
    }
}

// Vertex pass shared by base submeshes and authored levels: fat vertices (weights normalized, normals
// generated when absent), meshopt de-duplication, vertex cache order, vertex fetch order.
void optimize_vertices(const Primitive& prim, Array<FatVertex>& opt, Array<u32>& idx, usize& used) {
    const usize vcount = prim.positions.size() / 3;
    const f32* pos = prim.positions.data();
    const f32* nrm = prim.normals.data();
    const f32* uv = prim.uvs.data();
    const bool skinned = prim.skin >= 0;
    const f32* joints = prim.joints.data();
    const f32* weights = prim.weights.data();
    idx.resize(prim.indices.size());
    for (usize i = 0; i < idx.size(); ++i) idx[i] = prim.indices[i];

    Array<FatVertex> fat;
    fat.resize(vcount);
    for (usize v = 0; v < vcount; ++v) {
        FatVertex& f = fat[v];
        for (int c = 0; c < 3; ++c) f.p[c] = pos[v * 3 + c];
        for (int c = 0; c < 3; ++c) f.n[c] = prim.normals.empty() ? 0.0f : nrm[v * 3 + c];
        f.uv[0] = prim.uvs.empty() ? 0.0f : uv[v * 2];
        f.uv[1] = prim.uvs.empty() ? 0.0f : uv[v * 2 + 1];
        if (skinned) {
            f32 sum = 0.0f;
            for (int c = 0; c < 4; ++c) {
                const f32 wt = math::max(weights[v * 4 + c], 0.0f);
                const f32 j = joints[v * 4 + c];
                f.joints[c] = wt > 0.0f ? j : 0.0f;
                f.weights[c] = wt;
                sum += wt;
            }
            for (f32& wt : f.weights) wt = sum > 0.0f ? wt / sum : 0.0f;
        }
    }
    if (prim.normals.empty()) {  // area-weighted smooth normals over the indexed triangles
        for (usize t = 0; t + 2 < idx.size(); t += 3) {
            const vec3 a{fat[idx[t]].p[0], fat[idx[t]].p[1], fat[idx[t]].p[2]};
            const vec3 b{fat[idx[t + 1]].p[0], fat[idx[t + 1]].p[1], fat[idx[t + 1]].p[2]};
            const vec3 c{fat[idx[t + 2]].p[0], fat[idx[t + 2]].p[1], fat[idx[t + 2]].p[2]};
            const vec3 fn = cross(b - a, c - a);
            for (int k = 0; k < 3; ++k) {
                FatVertex& f = fat[idx[t + k]];
                f.n[0] += fn.x;
                f.n[1] += fn.y;
                f.n[2] += fn.z;
            }
        }
    }
    for (FatVertex& f : fat) {
        const f32 l = std::sqrt(f.n[0] * f.n[0] + f.n[1] * f.n[1] + f.n[2] * f.n[2]);
        if (l > 1e-12f) {
            for (f32& c : f.n) c /= l;
        } else {
            f.n[0] = 0.0f;
            f.n[1] = 1.0f;
            f.n[2] = 0.0f;
        }
    }

    // meshopt: de-duplicate, then vertex cache, then vertex fetch order.
    Array<u32> remap;
    remap.resize(vcount);
    const usize unique = meshopt_generateVertexRemap(remap.data(), idx.data(), idx.size(), fat.data(), vcount, sizeof(FatVertex));
    Array<FatVertex> uniq;
    uniq.resize(unique);
    meshopt_remapVertexBuffer(uniq.data(), fat.data(), vcount, sizeof(FatVertex), remap.data());
    meshopt_remapIndexBuffer(idx.data(), idx.data(), idx.size(), remap.data());
    meshopt_optimizeVertexCache(idx.data(), idx.data(), idx.size(), unique);
    opt.resize(unique);
    used = meshopt_optimizeVertexFetch(opt.data(), idx.data(), idx.size(), uniq.data(), unique, sizeof(FatVertex));
}

// Node-local positions of a submesh's vertices [base_vertex, base_vertex + count) (dequantized).
void dequantize_positions(const Geometry& geo, const asset::MeshSubmesh& sm, u32 base_vertex, u32 count, Array<f32>& out) {
    out.resize(usize(count) * 3);
    for (u32 v = 0; v < count; ++v) {
        const asset::MeshVertex& q = geo.vertices[base_vertex + v];
        for (int c = 0; c < 3; ++c)
            out[usize(v) * 3 + c] = sm.center[c] + math::max(static_cast<f32>(q.position[c]) / 32767.0f, -1.0f) * sm.extent[c];
    }
}

// Squared distance from `p` to triangle abc (Ericson, Real-Time Collision Detection 5.1.5).
f32 point_triangle_dist2(vec3 p, vec3 a, vec3 b, vec3 c) {
    const vec3 ab = b - a, ac = c - a, ap = p - a;
    const f32 d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return length_sq(p - a);
    const vec3 bp = p - b;
    const f32 d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return length_sq(p - b);
    const f32 vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) return length_sq(p - (a + ab * (d1 / (d1 - d3))));
    const vec3 cp = p - c;
    const f32 d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return length_sq(p - c);
    const f32 vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) return length_sq(p - (a + ac * (d2 / (d2 - d6))));
    const f32 va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
        return length_sq(p - (b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)))));
    const f32 denom = 1.0f / (va + vb + vc);
    return length_sq(p - (a + ab * (vb * denom) + ac * (vc * denom)));
}

// An authored level's deviation from its base: the larger of the two one-sided distances from
// one surface's vertices to the other's triangles (a vertex-sampled Hausdorff distance).
f32 lod_deviation(const Array<f32>& a_pos, Span<const u32> a_idx, const Array<f32>& b_pos, Span<const u32> b_idx) {
    auto at = [](const Array<f32>& pos, u32 i) { return vec3{pos[usize(i) * 3], pos[usize(i) * 3 + 1], pos[usize(i) * 3 + 2]}; };
    auto one_sided = [&](const Array<f32>& from, const Array<f32>& to_pos, Span<const u32> to_idx) {
        f32 worst = 0.0f;
        for (usize i = 0; i < from.size(); i += 3) {
            const vec3 p{from[i], from[i + 1], from[i + 2]};
            f32 best = 1e30f;
            for (usize t = 0; t + 2 < to_idx.size() && best > worst; t += 3)
                best = math::min(best, point_triangle_dist2(p, at(to_pos, to_idx[t]), at(to_pos, to_idx[t + 1]), at(to_pos, to_idx[t + 2])));
            worst = math::max(worst, best);
        }
        return std::sqrt(worst);
    };
    return math::max(one_sided(a_pos, b_pos, b_idx), one_sided(b_pos, a_pos, a_idx));
}

mat4 node_local(const asset::MeshNode& n) {
    return trs({n.translation[0], n.translation[1], n.translation[2]},
               normalize(quat{n.rotation[0], n.rotation[1], n.rotation[2], n.rotation[3]}), {n.scale[0], n.scale[1], n.scale[2]});
}

mat4 mat4_from(const f32 m[16]) {
    mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int k = 0; k < 4; ++k) r.c[c][k] = m[c * 4 + k];
    return r;
}

}  // namespace

// Generated levels (sidecar lod_levels): each simplified from the base so errors do not compound.
void generate_lods(Geometry& geo, u32 levels, f32 ratio, f32 max_error) {
    Array<f32> pos;
    Array<u32> base_idx, out_idx;
    for (usize si = 0; si < geo.submeshes.size(); ++si) {
        if (!geo.lods[si].empty()) continue;  // authored levels win
        const asset::MeshSubmesh sm = geo.submeshes[si];
        if (sm.index_count < 3 * 16) continue;  // too small to be worth a level
        dequantize_positions(geo, sm, sm.base_vertex, sm.vertex_count, pos);
        base_idx.resize(sm.index_count);
        for (u32 i = 0; i < sm.index_count; ++i) base_idx[i] = geo.indices[sm.first_index + i];
        const f32 scale = meshopt_simplifyScale(pos.data(), sm.vertex_count, sizeof(f32) * 3);
        usize previous = sm.index_count;
        f32 keep = 1.0f;
        for (u32 level = 1; level <= levels; ++level) {
            keep *= ratio;
            const usize target = math::max<usize>(3, static_cast<usize>(static_cast<f32>(sm.index_count) * keep) / 3 * 3);
            out_idx.resize(sm.index_count);
            f32 error = 0.0f;
            const usize n = meshopt_simplify(out_idx.data(), base_idx.data(), sm.index_count, pos.data(), sm.vertex_count,
                                             sizeof(f32) * 3, target, max_error, 0, &error);
            if (n < 3 || static_cast<f32>(n) > static_cast<f32>(previous) * 0.9f) break;
            out_idx.resize(n);
            meshopt_optimizeVertexCache(out_idx.data(), out_idx.data(), n, sm.vertex_count);
            asset::MeshLod lod{};
            lod.first_index = static_cast<u32>(geo.indices.size());
            lod.index_count = static_cast<u32>(n);
            lod.base_vertex = sm.base_vertex;
            lod.vertex_count = sm.vertex_count;
            lod.error = error * scale;
            for (u32 i : out_idx) geo.indices.push_back(i);
            geo.lods[si].push_back(lod);
            previous = n;
        }
    }
}

void add_submesh(const Primitive& prim, Geometry& geo) {
    Array<FatVertex> opt;
    Array<u32> idx;
    usize used = 0;
    optimize_vertices(prim, opt, idx, used);
    const i32 skin = prim.skin;
    const u32 skin_joints = prim.skin_joints;

    asset::MeshSubmesh sm{};
    sm.first_index = static_cast<u32>(geo.indices.size());
    sm.index_count = static_cast<u32>(idx.size());
    sm.base_vertex = static_cast<u32>(geo.vertices.size());
    sm.vertex_count = static_cast<u32>(used);
    sm.node = prim.node;
    sm.material = prim.material;
    f32 pmin[3] = {0, 0, 0}, pmax[3] = {0, 0, 0}, umin[2] = {0, 0}, umax[2] = {0, 0};
    for (usize v = 0; v < used; ++v) {
        for (int c = 0; c < 3; ++c) {
            pmin[c] = v ? math::min(pmin[c], opt[v].p[c]) : opt[v].p[c];
            pmax[c] = v ? math::max(pmax[c], opt[v].p[c]) : opt[v].p[c];
        }
        for (int c = 0; c < 2; ++c) {
            umin[c] = v ? math::min(umin[c], opt[v].uv[c]) : opt[v].uv[c];
            umax[c] = v ? math::max(umax[c], opt[v].uv[c]) : opt[v].uv[c];
        }
    }
    for (int c = 0; c < 3; ++c) {
        sm.center[c] = (pmin[c] + pmax[c]) * 0.5f;
        sm.extent[c] = math::max((pmax[c] - pmin[c]) * 0.5f, 1e-6f);
    }
    for (int c = 0; c < 2; ++c) {
        sm.uv_offset[c] = umin[c];
        sm.uv_scale[c] = umax[c] > umin[c] ? umax[c] - umin[c] : 1.0f;
    }
    for (usize v = 0; v < used; ++v) {
        asset::MeshVertex q{};
        for (int c = 0; c < 3; ++c) q.position[c] = snorm16((opt[v].p[c] - sm.center[c]) / sm.extent[c]);
        f32 o[2];
        oct_encode(opt[v].n, o);
        q.normal[0] = snorm16(o[0]);
        q.normal[1] = snorm16(o[1]);
        for (int c = 0; c < 2; ++c) q.uv[c] = unorm16((opt[v].uv[c] - sm.uv_offset[c]) / sm.uv_scale[c]);
        geo.vertices.push_back(q);
    }
    if (skin >= 0 && geo.joint_boxes.size() <= static_cast<usize>(skin)) geo.joint_boxes.resize(static_cast<usize>(skin) + 1);
    for (usize v = 0; v < used; ++v) {
        asset::MeshSkinVertex sv{};
        if (skin >= 0) {
            quantize_weights(opt[v].weights, sv.weights);
            Array<JointBox>& boxes = geo.joint_boxes[static_cast<usize>(skin)];
            if (boxes.size() < skin_joints) boxes.resize(skin_joints);
            for (int c = 0; c < 4; ++c) {
                if (sv.weights[c] == 0) continue;
                sv.joints[c] = static_cast<u8>(opt[v].joints[c]);
                boxes[sv.joints[c]].add(opt[v].p);
            }
        }
        geo.skin_vertices.push_back(sv);
    }
    for (u32 i : idx) geo.indices.push_back(i);
    geo.submeshes.push_back(sm);
    geo.submesh_ext.push_back(prim.material_ext);
    geo.submesh_skin.push_back(skin);
    geo.lods.emplace_back();
}

asset::MeshLod add_authored_lod(const Primitive& prim, u32 base_index, Geometry& geo) {
    Array<FatVertex> opt;
    Array<u32> idx;
    usize used = 0;
    optimize_vertices(prim, opt, idx, used);

    asset::MeshLod lod{};
    lod.first_index = static_cast<u32>(geo.indices.size());
    lod.index_count = static_cast<u32>(idx.size());
    lod.base_vertex = static_cast<u32>(geo.vertices.size());
    lod.vertex_count = static_cast<u32>(used);
    {  // an authored level: the base's quantization, its own vertex range
        const asset::MeshSubmesh& base = geo.submeshes[base_index];
        for (usize v = 0; v < used; ++v) {
            asset::MeshVertex q{};
            for (int c = 0; c < 3; ++c) q.position[c] = snorm16((opt[v].p[c] - base.center[c]) / base.extent[c]);
            f32 o[2];
            oct_encode(opt[v].n, o);
            q.normal[0] = snorm16(o[0]);
            q.normal[1] = snorm16(o[1]);
            for (int c = 0; c < 2; ++c) q.uv[c] = unorm16((opt[v].uv[c] - base.uv_offset[c]) / base.uv_scale[c]);
            geo.vertices.push_back(q);
            geo.skin_vertices.push_back(asset::MeshSkinVertex{});
        }
        for (u32 i : idx) geo.indices.push_back(i);
    }
    Array<f32> base_pos, lod_pos;
    const asset::MeshSubmesh& sm = geo.submeshes[base_index];
    dequantize_positions(geo, sm, sm.base_vertex, sm.vertex_count, base_pos);
    dequantize_positions(geo, sm, lod.base_vertex, lod.vertex_count, lod_pos);
    const Span<const u32> base_idx = geo.indices.span().subspan(sm.first_index, sm.index_count);
    const Span<const u32> lod_idx = geo.indices.span().subspan(lod.first_index, lod.index_count);
    lod.error = lod_deviation(base_pos, base_idx, lod_pos, lod_idx);
    return lod;
}

i32 add_skin(SkinTables& tables, const Geometry& geo, i32 source_skin, Span<const u32> joint_nodes, Span<const f32> inverse_bind) {
    asset::MeshSkin skin{};
    skin.first_joint = static_cast<u32>(tables.joints.size());
    skin.joint_count = static_cast<u32>(joint_nodes.size());
    const Array<JointBox>* boxes =
        static_cast<usize>(source_skin) < geo.joint_boxes.size() ? &geo.joint_boxes[static_cast<usize>(source_skin)] : nullptr;
    for (u32 j = 0; j < skin.joint_count; ++j) {
        asset::MeshJoint mj{};
        mj.node = joint_nodes[j];
        for (u32 k = 0; k < 16; ++k) mj.inverse_bind[k] = inverse_bind.empty() ? (k % 5 == 0 ? 1.0f : 0.0f) : inverse_bind[usize(j) * 16 + k];
        const JointBox* b = boxes && j < boxes->size() ? &(*boxes)[j] : nullptr;
        for (int c = 0; c < 3; ++c) {
            mj.bounds_min[c] = b && b->any ? b->min[c] : 1.0f;  // min > max: influences nothing
            mj.bounds_max[c] = b && b->any ? b->max[c] : -1.0f;
        }
        tables.joints.push_back(mj);
    }
    const auto index = static_cast<i32>(tables.skins.size());
    tables.skins.push_back(skin);
    return index;
}

void write_mesh_chunk(Geometry& geo, const SkinTables& skins, const MeshTables& tables, bool ragdoll, const asset::Guid& guid,
                      asset::ContainerWriter& writer) {
    // Rest-pose model bounds over every submesh's node-local box.
    Array<mat4> world;
    world.resize(tables.nodes.size());
    for (usize i = 0; i < tables.nodes.size(); ++i)
        world[i] = tables.nodes[i].parent < 0 ? node_local(tables.nodes[i]) : world[static_cast<usize>(tables.nodes[i].parent)] * node_local(tables.nodes[i]);
    asset::MeshData d;
    bool any = false;
    u32 max_vertices = 0;
    auto grow = [&](const vec3& w) {
        const f32 c[3] = {w.x, w.y, w.z};
        for (int k = 0; k < 3; ++k) {
            d.bounds_min[k] = any ? math::min(d.bounds_min[k], c[k]) : c[k];
            d.bounds_max[k] = any ? math::max(d.bounds_max[k], c[k]) : c[k];
        }
        any = true;
    };
    for (usize si = 0; si < geo.submeshes.size(); ++si) {
        const asset::MeshSubmesh& sm = geo.submeshes[si];
        max_vertices = math::max(max_vertices, sm.vertex_count);
        for (const asset::MeshLod& l : geo.lods[si]) max_vertices = math::max(max_vertices, l.vertex_count);
        if (const i32 sk = skins.submesh_skins[si]; sk >= 0) {
            // Skinned: the rest pose of each joint (world * inverse bind) moves its bind box.
            const asset::MeshSkin& skin = skins.skins[static_cast<usize>(sk)];
            for (u32 j = 0; j < skin.joint_count; ++j) {
                const asset::MeshJoint& mj = skins.joints[skin.first_joint + j];
                if (mj.bounds_min[0] > mj.bounds_max[0]) continue;
                const mat4 m = world[mj.node] * mat4_from(mj.inverse_bind);
                for (int corner = 0; corner < 8; ++corner)
                    grow(transform_point(m, {(corner & 1) ? mj.bounds_max[0] : mj.bounds_min[0],
                                             (corner & 2) ? mj.bounds_max[1] : mj.bounds_min[1],
                                             (corner & 4) ? mj.bounds_max[2] : mj.bounds_min[2]}));
            }
            continue;
        }
        for (int corner = 0; corner < 8; ++corner) {
            const vec3 p{sm.center[0] + ((corner & 1) ? sm.extent[0] : -sm.extent[0]),
                         sm.center[1] + ((corner & 2) ? sm.extent[1] : -sm.extent[1]),
                         sm.center[2] + ((corner & 4) ? sm.extent[2] : -sm.extent[2])};
            grow(transform_point(world[sm.node], p));
        }
    }

    Array<u16> idx16;
    if (max_vertices > 0xFFFF) {
        d.flags |= asset::MeshFlags::Index32;
        d.indices = std::as_bytes(geo.indices.span());
    } else {
        idx16.resize(geo.indices.size());
        for (usize i = 0; i < idx16.size(); ++i) idx16[i] = static_cast<u16>(geo.indices[i]);
        d.indices = std::as_bytes(idx16.span());
    }
    // LOD table: each submesh's levels in a run, named by its reserved[0] (count) / [1] (first).
    Array<asset::MeshLod> lods;
    for (usize si = 0; si < geo.submeshes.size(); ++si) {
        asset::MeshSubmesh& sm = geo.submeshes[si];
        sm.reserved[0] = static_cast<u32>(geo.lods[si].size());
        sm.reserved[1] = sm.reserved[0] ? static_cast<u32>(lods.size()) : 0u;
        for (const asset::MeshLod& l : geo.lods[si]) lods.push_back(l);
    }
    d.lods = lods.span();
    bool any_ext = false;
    for (const asset::MaterialExt& e : geo.submesh_ext) any_ext = any_ext || e.any();
    if (any_ext) d.materials = geo.submesh_ext.span();
    d.vertices = geo.vertices.span();
    d.submeshes = geo.submeshes.span();
    d.nodes = tables.nodes;
    d.clips = tables.clips;
    d.channels = tables.channels;
    d.keys = tables.keys;
    d.strings = tables.strings;
    if (!skins.skins.empty()) {
        d.skins = skins.skins.span();
        d.joints = skins.joints.span();
        d.skin_vertices = geo.skin_vertices.span();
        d.submesh_skins = skins.submesh_skins.span();
    }
    if (ragdoll) d.flags |= asset::MeshFlags::Ragdoll;
    Array<byte> payload;
    asset::write_mesh(d, payload);
    asset::ContainerWriter::ChunkDesc desc;
    desc.type = asset::ChunkType::Mesh;
    desc.version = asset::chunk_version(asset::MESH_VERSION, any_ext                  ? asset::MESH_MINOR_MATERIALS
                                                             : ragdoll                ? asset::MESH_MINOR_RAGDOLL
                                                             : !lods.empty()          ? asset::MESH_MINOR_LODS
                                                             : skins.skins.empty() ? 0
                                                                                      : asset::MESH_MINOR_SKIN);
    desc.guid = guid;
    writer.add(desc, payload.span());
}

}  // namespace mce::cook::detail

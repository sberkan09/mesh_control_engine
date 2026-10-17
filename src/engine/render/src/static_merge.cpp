#include "mce/render/static_merge.h"

#include <cmath>
#include <cstring>

#include "mce/asset/formats.h"

namespace mce::render {

using namespace mce::math;

namespace {

i16 snorm16(f32 v) { return static_cast<i16>(std::lround(clamp(v, -1.0f, 1.0f) * 32767.0f)); }
u16 unorm16(f32 v) { return static_cast<u16>(std::lround(clamp(v, 0.0f, 1.0f) * 65535.0f)); }

// Octahedral encoding (the glTF cook's) of a unit vector into [-1, 1]^2, and the shader's decode.
void oct_encode(vec3 n, f32 out[2]) {
    const f32 l1 = std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z);
    f32 x = n.x / l1, y = n.y / l1;
    if (n.z < 0.0f) {
        const f32 ox = x, oy = y;
        x = (1.0f - std::fabs(oy)) * (ox >= 0.0f ? 1.0f : -1.0f);
        y = (1.0f - std::fabs(ox)) * (oy >= 0.0f ? 1.0f : -1.0f);
    }
    out[0] = x;
    out[1] = y;
}

vec3 oct_decode(f32 ex, f32 ey) {
    vec3 n{ex, ey, 1.0f - std::fabs(ex) - std::fabs(ey)};
    if (n.z < 0.0f) {
        const f32 nx = n.x, ny = n.y;
        n.x = (1.0f - std::fabs(ny)) * (nx >= 0.0f ? 1.0f : -1.0f);
        n.y = (1.0f - std::fabs(nx)) * (ny >= 0.0f ? 1.0f : -1.0f);
    }
    const f32 l = length(n);
    return l > 0.0f ? n / l : vec3{0.0f, 1.0f, 0.0f};
}

mat4 node_local(const asset::MeshNode& n) {
    const quat q = normalize(quat{n.rotation[0], n.rotation[1], n.rotation[2], n.rotation[3]});
    return trs({n.translation[0], n.translation[1], n.translation[2]}, q, {n.scale[0], n.scale[1], n.scale[2]});
}

// One submesh of one source, placed.
struct Ref {
    u32 source = 0;
    u32 sub = 0;
    mat4 place;    // source world * the submesh's node (rest pose)
    mat4 normal;   // transpose(inverse(place)); only its upper 3x3 is used
    f32 scale = 1.0f;  // the largest axis scale of `place`: node-local errors to world
    u32 lods = 0;      // levels beyond the base
    // The vertex windows of the source submesh already copied into the group: (source base, count) -> offset
    // into the group's vertices. A generated level shares the submesh's own window; an authored one has its own.
    u32 win_n = 0;
    u32 win_base[asset::kMaxMeshLods] = {};
    u32 win_count[asset::kMaxMeshLods] = {};
    u32 win_dst[asset::kMaxMeshLods] = {};
};

struct Group {
    asset::MaterialChunk chunk{};
    asset::MaterialExt ext{};
    Array<u32> refs;  // into the refs array
};

struct WorldVertex {
    vec3 p;
    vec3 n;
    vec2 uv;
};

bool bytes_equal(const void* a, const void* b, usize n) { return std::memcmp(a, b, n) == 0; }

}  // namespace

bool build_merged_mesh(Span<const MergeSource> sources, Array<byte>& blob, MergeStats* stats) {
    blob.clear();
    MergeStats st{};
    if (stats) *stats = st;

    // ---- place every submesh and sort it into its material group -------------------------------------------
    Array<Ref> refs;
    Array<Group> groups;
    Array<mat4> nodes;  // scratch: one source's model-space node matrices
    for (u32 si = 0; si < sources.size(); ++si) {
        const MergeSource& src = sources[si];
        if (!src.mesh || !src.mesh->header || src.mesh->skinned() || src.mesh->submeshes.empty()) {
            ++st.skipped;
            continue;
        }
        const asset::MeshView& v = *src.mesh;
        nodes.resize(v.nodes.size());
        for (usize i = 0; i < v.nodes.size(); ++i)
            nodes[i] = v.nodes[i].parent < 0 ? node_local(v.nodes[i]) : nodes[static_cast<usize>(v.nodes[i].parent)] * node_local(v.nodes[i]);
        bool any = false;
        for (u32 s = 0; s < v.submeshes.size(); ++s) {
            const asset::MeshSubmesh& sm = v.submeshes[s];
            if (sm.index_count == 0 || sm.vertex_count == 0) continue;
            Ref r;
            r.source = si;
            r.sub = s;
            r.place = src.world * nodes[sm.node];
            bool ok = true;
            const mat4 inv = inverse(r.place, &ok);
            r.normal = transpose(ok ? inv : mat4::identity());
            r.scale = max(length(vec3{r.place.c[0][0], r.place.c[0][1], r.place.c[0][2]}),
                          max(length(vec3{r.place.c[1][0], r.place.c[1][1], r.place.c[1][2]}),
                              length(vec3{r.place.c[2][0], r.place.c[2][1], r.place.c[2][2]})));
            r.lods = static_cast<u32>(v.submesh_lods(s).size());

            // The material as an unmerged draw would see it, the tints folded in.
            asset::MaterialChunk chunk = src.material ? *src.material : sm.material;
            asset::MaterialExt ext = src.material ? (src.material_ext ? *src.material_ext : asset::MaterialExt{}) : v.material_ext(s);
            for (int c = 0; c < 4; ++c) chunk.base_color[c] *= src.tint[c];
            chunk.emissive[0] *= src.emissive_tint.x;
            chunk.emissive[1] *= src.emissive_tint.y;
            chunk.emissive[2] *= src.emissive_tint.z;
            u32 g = 0;
            for (; g < groups.size(); ++g)
                if (bytes_equal(&groups[g].chunk, &chunk, sizeof(chunk)) && bytes_equal(&groups[g].ext, &ext, sizeof(ext))) break;
            if (g == groups.size()) {
                Group ng;
                ng.chunk = chunk;
                ng.ext = ext;
                groups.push_back(static_cast<Group&&>(ng));
            }
            groups[g].refs.push_back(static_cast<u32>(refs.size()));
            refs.push_back(r);
            any = true;
        }
        if (any) ++st.sources;
        else ++st.skipped;
    }
    if (groups.empty()) return false;

    // ---- build the groups ------------------------------------------------------------------------------------
    Array<asset::MeshVertex> out_vertices;
    Array<u32> out_indices;  // relative to each group's first vertex
    Array<asset::MeshSubmesh> out_subs;
    Array<asset::MeshLod> out_lods;
    Array<asset::MaterialExt> out_ext;
    Array<WorldVertex> wv;  // scratch: one group's vertices in world space
    bool any_ext = false;
    bool wide = false;
    vec3 box_min{}, box_max{};
    bool box_any = false;

    for (Group& g : groups) {
        wv.clear();
        u32 levels = 1;
        for (u32 ri : g.refs) levels = max(levels, 1 + refs[ri].lods);

        // Index lists per level, as group-relative offsets into wv.
        const u32 first_index = static_cast<u32>(out_indices.size());
        u32 level_first[asset::kMaxMeshLods] = {};
        u32 level_count[asset::kMaxMeshLods] = {};
        f32 level_error[asset::kMaxMeshLods] = {};

        for (u32 k = 0; k < levels; ++k) {
            level_first[k] = static_cast<u32>(out_indices.size());
            for (u32 ri : g.refs) {
                Ref& r = refs[ri];
                const asset::MeshView& v = *sources[r.source].mesh;
                const asset::MeshSubmesh& sm = v.submeshes[r.sub];
                const u32 j = min(k, r.lods);  // this source's level
                u32 fi = sm.first_index, ic = sm.index_count, bv = sm.base_vertex, vc = sm.vertex_count;
                if (j > 0) {
                    const asset::MeshLod& l = v.submesh_lods(r.sub)[j - 1];
                    fi = l.first_index;
                    ic = l.index_count;
                    bv = l.base_vertex;
                    vc = l.vertex_count;
                    level_error[k] = max(level_error[k], l.error * r.scale);
                }
                // The window of vertices this level indexes: copied once per distinct (base, count).
                u32 w = 0;
                for (; w < r.win_n; ++w)
                    if (r.win_base[w] == bv && r.win_count[w] == vc) break;
                if (w == r.win_n) {
                    r.win_base[w] = bv;
                    r.win_count[w] = vc;
                    r.win_dst[w] = static_cast<u32>(wv.size());
                    ++r.win_n;
                    const vec3 c{sm.center[0], sm.center[1], sm.center[2]}, e{sm.extent[0], sm.extent[1], sm.extent[2]};
                    for (u32 i = 0; i < vc; ++i) {
                        const asset::MeshVertex& q = v.vertices[bv + i];
                        WorldVertex o;
                        o.p = transform_point(r.place, c + vec3{q.position[0] / 32767.0f * e.x, q.position[1] / 32767.0f * e.y,
                                                                q.position[2] / 32767.0f * e.z});
                        const vec3 nl = oct_decode(q.normal[0] / 32767.0f, q.normal[1] / 32767.0f);
                        const vec3 nw = transform_dir(r.normal, nl);
                        const f32 nl2 = length(nw);
                        o.n = nl2 > 0.0f ? nw / nl2 : vec3{0.0f, 1.0f, 0.0f};
                        o.uv = {sm.uv_offset[0] + q.uv[0] / 65535.0f * sm.uv_scale[0], sm.uv_offset[1] + q.uv[1] / 65535.0f * sm.uv_scale[1]};
                        wv.push_back(o);
                    }
                }
                const u32 dst = r.win_dst[w];
                for (u32 i = 0; i < ic; ++i) out_indices.push_back(dst + v.index(fi + i));
            }
            level_count[k] = static_cast<u32>(out_indices.size()) - level_first[k];
        }
        // Make errors monotonic: a coarser level is never more exact than the one before it.
        for (u32 k = 1; k < levels; ++k) level_error[k] = max(level_error[k], level_error[k - 1]);

        // ---- quantize the group over its own bounds ----------------------------------------------------------
        vec3 lo = wv[0].p, hi = wv[0].p;
        vec2 uv_lo = wv[0].uv, uv_hi = wv[0].uv;
        for (const WorldVertex& x : wv) {
            lo = min(lo, x.p);
            hi = max(hi, x.p);
            uv_lo = min(uv_lo, x.uv);
            uv_hi = max(uv_hi, x.uv);
        }
        const vec3 center = (lo + hi) * 0.5f, extent = max((hi - lo) * 0.5f, vec3{1e-4f, 1e-4f, 1e-4f});
        asset::MeshSubmesh sm{};
        sm.first_index = first_index;
        sm.index_count = level_count[0];
        sm.base_vertex = static_cast<u32>(out_vertices.size());
        sm.vertex_count = static_cast<u32>(wv.size());
        sm.node = 0;
        sm.center[0] = center.x;
        sm.center[1] = center.y;
        sm.center[2] = center.z;
        sm.extent[0] = extent.x;
        sm.extent[1] = extent.y;
        sm.extent[2] = extent.z;
        sm.uv_offset[0] = uv_lo.x;
        sm.uv_offset[1] = uv_lo.y;
        sm.uv_scale[0] = uv_hi.x > uv_lo.x ? uv_hi.x - uv_lo.x : 1.0f;
        sm.uv_scale[1] = uv_hi.y > uv_lo.y ? uv_hi.y - uv_lo.y : 1.0f;
        sm.material = g.chunk;
        if (levels > 1) {
            sm.reserved[0] = levels - 1;
            sm.reserved[1] = static_cast<u32>(out_lods.size());
            for (u32 k = 1; k < levels; ++k) {
                asset::MeshLod l{};
                l.first_index = level_first[k];
                l.index_count = level_count[k];
                l.base_vertex = sm.base_vertex;
                l.vertex_count = sm.vertex_count;
                l.error = level_error[k];
                out_lods.push_back(l);
            }
        }
        for (const WorldVertex& x : wv) {
            asset::MeshVertex q{};
            q.position[0] = snorm16((x.p.x - center.x) / extent.x);
            q.position[1] = snorm16((x.p.y - center.y) / extent.y);
            q.position[2] = snorm16((x.p.z - center.z) / extent.z);
            f32 oct[2];
            oct_encode(x.n, oct);
            q.normal[0] = snorm16(oct[0]);
            q.normal[1] = snorm16(oct[1]);
            q.uv[0] = unorm16((x.uv.x - uv_lo.x) / sm.uv_scale[0]);
            q.uv[1] = unorm16((x.uv.y - uv_lo.y) / sm.uv_scale[1]);
            out_vertices.push_back(q);
        }
        wide = wide || wv.size() > 0x10000;
        out_subs.push_back(sm);
        out_ext.push_back(g.ext);
        static constexpr asset::MaterialExt kNone{};
        any_ext = any_ext || !bytes_equal(&g.ext, &kNone, sizeof(kNone));
        box_min = box_any ? min(box_min, lo) : lo;
        box_max = box_any ? max(box_max, hi) : hi;
        box_any = true;
        st.triangles += level_count[0] / 3;
        st.lod_levels = max(st.lod_levels, levels);
    }

    // ---- write the payload ------------------------------------------------------------------------------------
    Array<byte> ib;
    if (wide) {
        ib.resize(out_indices.size() * 4);
        std::memcpy(ib.data(), out_indices.data(), ib.size());
    } else {
        ib.resize(out_indices.size() * 2);
        for (usize i = 0; i < out_indices.size(); ++i) {
            const u16 x = static_cast<u16>(out_indices[i]);
            std::memcpy(ib.data() + i * 2, &x, 2);
        }
    }
    asset::MeshNode node{};
    node.parent = -1;
    node.scale[0] = node.scale[1] = node.scale[2] = 1.0f;
    node.rotation[3] = 1.0f;
    asset::MeshData d;
    d.vertices = out_vertices.span();
    d.indices = ib.span();
    d.submeshes = out_subs.span();
    d.nodes = Span<const asset::MeshNode>(&node, 1);
    d.lods = out_lods.span();
    if (any_ext) d.materials = out_ext.span();
    d.flags = wide ? asset::MeshFlags::Index32 : 0u;
    d.bounds_min[0] = box_min.x;
    d.bounds_min[1] = box_min.y;
    d.bounds_min[2] = box_min.z;
    d.bounds_max[0] = box_max.x;
    d.bounds_max[1] = box_max.y;
    d.bounds_max[2] = box_max.z;
    asset::write_mesh(d, blob);

    st.submeshes = static_cast<u32>(out_subs.size());
    st.vertices = static_cast<u32>(out_vertices.size());
    st.bounds = {box_min, box_max};
    if (stats) *stats = st;
    return true;
}

}  // namespace mce::render

#include "mce/render/terrain.h"

#include <cmath>
#include <cstring>

#include "mce/log.h"

namespace mce::render {

using namespace mce::math;

namespace {

i16 snorm16(f32 v) { return static_cast<i16>(std::lround(clamp(v, -1.0f, 1.0f) * 32767.0f)); }
u16 unorm16(f32 v) { return static_cast<u16>(std::lround(clamp(v, 0.0f, 1.0f) * 65535.0f)); }

// Octahedral encoding (as the glTF cook's) of a unit vector into [-1, 1]^2.
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

// Distance from `p` to the box (0 inside).
f32 box_distance(const aabb3& b, vec3 p) {
    const vec3 q = min(max(p, b.min), b.max);
    return length(p - q);
}

}  // namespace

ResultV TerrainLod::init(const asset::TerrainView& terrain) {
    const asset::TerrainHeader& h = *terrain.header;
    resolution_ = h.resolution;
    chunk_ = h.chunk;
    size_ = {h.size[0], h.size[1]};
    spacing_ = {terrain.spacing_x(), terrain.spacing_z()};
    height_min_ = h.height_min;
    height_scale_ = (h.height_max - h.height_min) / 65535.0f;
    heights_.resize(terrain.heights.size());
    std::memcpy(heights_.data(), terrain.heights.data(), terrain.heights.size_bytes());
    holes_.clear();
    for (u8 b : terrain.holes) holes_.push_back(b);

    const u32 leaves = (resolution_ - 1) / chunk_;  // finest nodes per side, a power of two
    levels_ = 1;
    while ((1u << (levels_ - 1)) < leaves) ++levels_;
    if (levels_ > kMaxLevels) return Error(Errc::Unsupported, "render.terrain.levels", {}, levels_);
    nodes_.clear();
    // Root first, then each finer level, row by row.
    for (u32 li = levels_; li-- > 0;) {
        level_first_[li] = static_cast<u32>(nodes_.size());
        level_side_[li] = leaves >> li;
        for (u32 z = 0; z < level_side_[li]; ++z)
            for (u32 x = 0; x < level_side_[li]; ++x) {
                TerrainNode n;
                n.level = li;
                n.x = x;
                n.z = z;
                nodes_.push_back(n);
            }
    }
    compute_errors();
    return ok();
}

void TerrainLod::compute_errors() {
    for (TerrainNode& n : nodes_) {
        const u32 span = chunk_ * stride(n.level);  // samples per side of the node
        const u32 x0 = n.x * span, z0 = n.z * span;
        f32 lo = sample(x0, z0), hi = lo, err = 0.0f;
        const u32 s = stride(n.level);
        n.holed = false;
        for (u32 z = z0; z <= z0 + span; ++z)
            for (u32 x = x0; x <= x0 + span; ++x) {
                const f32 v = sample(x, z);
                n.holed |= hole(x, z);
                lo = min(lo, v);
                hi = max(hi, v);
                if (s == 1) continue;
                // The node's grid over this sample: its quad split along the (x0, z0)-(x1, z1) diagonal,
                // as build_mesh() triangulates it.
                const u32 qx = min((x - x0) / s, chunk_ - 1), qz = min((z - z0) / s, chunk_ - 1);
                const u32 ax = x0 + qx * s, az = z0 + qz * s;
                const f32 u = static_cast<f32>(x - ax) / static_cast<f32>(s), w = static_cast<f32>(z - az) / static_cast<f32>(s);
                const f32 h00 = sample(ax, az), h10 = sample(ax + s, az), h01 = sample(ax, az + s), h11 = sample(ax + s, az + s);
                const f32 g = u >= w ? h00 + u * (h10 - h00) + w * (h11 - h10) : h00 + w * (h01 - h00) + u * (h11 - h01);
                err = max(err, std::fabs(g - v));
            }
        n.error = err;
        n.bounds.min = {-0.5f * size_.x + static_cast<f32>(x0) * spacing_.x, lo, -0.5f * size_.y + static_cast<f32>(z0) * spacing_.y};
        n.bounds.max = {-0.5f * size_.x + static_cast<f32>(x0 + span) * spacing_.x, hi,
                        -0.5f * size_.y + static_cast<f32>(z0 + span) * spacing_.y};
    }
    // A node is never more exact than its children: finer levels first (they come last).
    for (u32 li = 1; li < levels_; ++li)
        for (u32 z = 0; z < level_side_[li]; ++z)
            for (u32 x = 0; x < level_side_[li]; ++x) {
                TerrainNode& n = nodes_[node_index(li, x, z)];
                for (u32 c = 0; c < 4; ++c) n.error = max(n.error, nodes_[node_index(li - 1, x * 2 + (c & 1), z * 2 + (c >> 1))].error);
            }
    // Skirts: deep enough for a neighbour up to two levels coarser, plus half a sample of slack.
    const f32 slack = 0.5f * max(spacing_.x, spacing_.y);
    for (TerrainNode& n : nodes_) {
        const u32 up = min(n.level + 2, levels_ - 1);
        n.skirt = nodes_[node_index(up, n.x >> (up - n.level), n.z >> (up - n.level))].error + slack;
    }
}

u32 TerrainLod::node_triangles(u32 index) const {
    const TerrainNode& n = nodes_[index];
    const u32 side = level_side_[n.level];
    const u32 inner_edges = (n.x > 0) + (n.x + 1 < side) + (n.z > 0) + (n.z + 1 < side);
    if (!n.holed) return chunk_ * chunk_ * 2 + inner_edges * chunk_ * 2;
    // Level 0 with holes: count what build_mesh() keeps.
    const u32 x0 = n.x * chunk_, z0 = n.z * chunk_;
    u32 count = 0;
    for (u32 j = 0; j < chunk_; ++j)
        for (u32 i = 0; i < chunk_; ++i) {
            const bool h00 = hole(x0 + i, z0 + j), h10 = hole(x0 + i + 1, z0 + j), h01 = hole(x0 + i, z0 + j + 1),
                       h11 = hole(x0 + i + 1, z0 + j + 1);
            count += !(h00 || h01 || h11);
            count += !(h00 || h11 || h10);
        }
    auto edge = [&](bool present, u32 sx, u32 sz, u32 dx, u32 dz) {
        if (!present) return;
        for (u32 e = 0; e < chunk_; ++e)
            if (!hole(sx + e * dx, sz + e * dz) && !hole(sx + (e + 1) * dx, sz + (e + 1) * dz)) count += 2;
    };
    edge(n.z > 0, x0, z0, 1, 0);
    edge(n.z + 1 < side, x0, z0 + chunk_, 1, 0);
    edge(n.x > 0, x0, z0, 0, 1);
    edge(n.x + 1 < side, x0 + chunk_, z0, 0, 1);
    return count;
}

f32 TerrainLod::surface_at(f32 x, f32 z) const {
    const f32 fx = clamp((x / size_.x + 0.5f) * static_cast<f32>(resolution_ - 1), 0.0f, static_cast<f32>(resolution_ - 1));
    const f32 fz = clamp((z / size_.y + 0.5f) * static_cast<f32>(resolution_ - 1), 0.0f, static_cast<f32>(resolution_ - 1));
    const u32 x0 = min(static_cast<u32>(fx), resolution_ - 2), z0 = min(static_cast<u32>(fz), resolution_ - 2);
    const f32 u = fx - static_cast<f32>(x0), w = fz - static_cast<f32>(z0);
    const f32 h00 = sample(x0, z0), h10 = sample(x0 + 1, z0), h01 = sample(x0, z0 + 1), h11 = sample(x0 + 1, z0 + 1);
    return u >= w ? h00 + u * (h10 - h00) + w * (h11 - h10) : h00 + w * (h01 - h00) + u * (h11 - h01);
}

f32 TerrainLod::height_at(f32 x, f32 z) const {
    const f32 fx = clamp((x / size_.x + 0.5f) * static_cast<f32>(resolution_ - 1), 0.0f, static_cast<f32>(resolution_ - 1));
    const f32 fz = clamp((z / size_.y + 0.5f) * static_cast<f32>(resolution_ - 1), 0.0f, static_cast<f32>(resolution_ - 1));
    const u32 x0 = min(static_cast<u32>(fx), resolution_ - 2), z0 = min(static_cast<u32>(fz), resolution_ - 2);
    const f32 tx = fx - static_cast<f32>(x0), tz = fz - static_cast<f32>(z0);
    const f32 a = sample(x0, z0) + (sample(x0 + 1, z0) - sample(x0, z0)) * tx;
    const f32 b = sample(x0, z0 + 1) + (sample(x0 + 1, z0 + 1) - sample(x0, z0 + 1)) * tx;
    return a + (b - a) * tz;
}

vec3 TerrainLod::normal_at(u32 x, u32 z) const {
    const u32 xl = x > 0 ? x - 1 : x, xr = x + 1 < resolution_ ? x + 1 : x;
    const u32 zl = z > 0 ? z - 1 : z, zr = z + 1 < resolution_ ? z + 1 : z;
    const f32 dx = (sample(xr, z) - sample(xl, z)) / (static_cast<f32>(xr - xl) * spacing_.x);
    const f32 dz = (sample(x, zr) - sample(x, zl)) / (static_cast<f32>(zr - zl) * spacing_.y);
    return normalize(vec3{-dx, 1.0f, -dz});
}

void TerrainLod::select(const View3D& view, const TerrainSelectParams& p, Array<u32>& out) const {
    if (nodes_.empty()) return;
    const mat4 to_local = inverse(p.world);
    const vec3 eye = transform_point(to_local, view.position);
    // World size of a terrain-space metre (the largest axis scale), for spheres in world space.
    const f32 scale = max(length(vec3{p.world.c[0][0], p.world.c[0][1], p.world.c[0][2]}),
                          max(length(vec3{p.world.c[1][0], p.world.c[1][1], p.world.c[1][2]}),
                              length(vec3{p.world.c[2][0], p.world.c[2][1], p.world.c[2][2]})));
    // Pixels per unit of error at unit distance: half the viewport height over tan(fov / 2).
    const f32 k = view.proj.c[1][1] * static_cast<f32>(view.viewport.height) * 0.5f;
    const f32 limit = max(p.pixel_error, 0.01f);

    u32 stack[kMaxLevels * 4];
    u32 top = 0;
    stack[top++] = root();
    while (top > 0) {
        const u32 index = stack[--top];
        const TerrainNode& n = nodes_[index];
        aabb3 b = n.bounds;
        b.min.y -= n.skirt;
        const vec3 c = transform_point(p.world, (b.min + b.max) * 0.5f);
        const f32 r = length(b.max - b.min) * 0.5f * scale;
        if (!sphere_visible(view, c, r) && length(c - view.position) - r > p.shadow_reach) continue;
        const f32 d = box_distance(n.bounds, eye);
        if (n.level == 0 || (!n.holed && d > 0.0f && n.error * k <= limit * d)) {
            out.push_back(index);
            continue;
        }
        // Children in reverse so they come out in row order.
        for (u32 ci = 4; ci-- > 0;) stack[top++] = node_index(n.level - 1, n.x * 2 + (ci & 1), n.z * 2 + (ci >> 1));
    }
}

void TerrainLod::build_mesh(u32 index, f32 uv_tile, Array<byte>& out) const {
    const TerrainNode& n = nodes_[index];
    const u32 s = stride(n.level), q = chunk_, side = level_side_[n.level];
    const u32 x0 = n.x * q * s, z0 = n.z * q * s;
    const u32 grid = q + 1;

    // Positions and normals in terrain space; the skirts follow the grid (south, north, west, east).
    struct V {
        vec3 p;
        vec3 n;
        vec2 uv;
    };
    Array<V> verts;
    verts.reserve(usize(grid) * grid + 4 * grid);
    const vec2 uv_span = uv_tile > 0.0f ? vec2{1.0f / uv_tile, 1.0f / uv_tile} : vec2{1.0f / size_.x, 1.0f / size_.y};
    auto vertex = [&](u32 sx, u32 sz, f32 drop) {
        V v;
        v.p = {-0.5f * size_.x + static_cast<f32>(sx) * spacing_.x, sample(sx, sz) - drop, -0.5f * size_.y + static_cast<f32>(sz) * spacing_.y};
        v.n = normal_at(sx, sz);
        v.uv = {(v.p.x + 0.5f * size_.x) * uv_span.x, (v.p.z + 0.5f * size_.y) * uv_span.y};
        verts.push_back(v);
    };
    for (u32 j = 0; j < grid; ++j)
        for (u32 i = 0; i < grid; ++i) vertex(x0 + i * s, z0 + j * s, 0.0f);

    Array<u32> idx;
    idx.reserve(usize(q) * q * 6 + 4 * usize(q) * 6);
    for (u32 j = 0; j < q; ++j)
        for (u32 i = 0; i < q; ++i) {
            const u32 v00 = j * grid + i, v10 = v00 + 1, v01 = v00 + grid, v11 = v01 + 1;
            // Counter-clockwise seen from above, split along v00-v11 (compute_errors() measures this split).
            const u32 tri[6] = {v00, v01, v11, v00, v11, v10};
            if (n.holed) {  // level 0: a triangle with a hole corner is left out, as Jolt leaves it out
                const u32 sx = x0 + i, sz = z0 + j;
                const bool h00 = hole(sx, sz), h10 = hole(sx + 1, sz), h01 = hole(sx, sz + 1), h11 = hole(sx + 1, sz + 1);
                if (!(h00 || h01 || h11))
                    for (u32 t = 0; t < 3; ++t) idx.push_back(tri[t]);
                if (!(h00 || h11 || h10))
                    for (u32 t = 3; t < 6; ++t) idx.push_back(tri[t]);
                continue;
            }
            for (u32 t : tri) idx.push_back(t);
        }
    // Skirts on the edges shared with another node, each quad wound to face away from the node.
    const vec3 node_center = (n.bounds.min + n.bounds.max) * 0.5f;
    auto skirt = [&](bool present, u32 first, u32 step) {
        if (!present) return;
        const u32 base = static_cast<u32>(verts.size());
        for (u32 e = 0; e < grid; ++e) {
            const u32 g = first + e * step;
            const u32 sx = x0 + (g % grid) * s, sz = z0 + (g / grid) * s;
            vertex(sx, sz, n.skirt);
        }
        for (u32 e = 0; e < q; ++e) {
            const u32 a = first + e * step, b = first + (e + 1) * step, as = base + e, bs = base + e + 1;
            if (n.holed && (hole(x0 + (a % grid) * s, z0 + (a / grid) * s) || hole(x0 + (b % grid) * s, z0 + (b / grid) * s))) continue;
            const vec3 face = cross(verts[b].p - verts[a].p, verts[as].p - verts[a].p);
            const vec3 outward = verts[a].p - node_center;
            const bool keep = dot(vec3{face.x, 0.0f, face.z}, vec3{outward.x, 0.0f, outward.z}) >= 0.0f;
            const u32 t[6] = {a, keep ? b : as, keep ? as : b, b, keep ? bs : as, keep ? as : bs};
            for (u32 v : t) idx.push_back(v);
        }
    };
    skirt(n.z > 0, 0, 1);                       // south: row 0
    skirt(n.z + 1 < side, q * grid, 1);         // north: the last row
    skirt(n.x > 0, 0, grid);                    // west: column 0
    skirt(n.x + 1 < side, q, grid);             // east: the last column

    // Quantize over the node's box, skirts included.
    aabb3 box = n.bounds;
    box.min.y -= n.skirt;
    vec2 uv_lo = verts[0].uv, uv_hi = verts[0].uv;
    for (const V& v : verts) {
        uv_lo = min(uv_lo, v.uv);
        uv_hi = max(uv_hi, v.uv);
    }
    asset::MeshSubmesh sm{};
    sm.first_index = 0;
    sm.index_count = static_cast<u32>(idx.size());
    sm.base_vertex = 0;
    sm.vertex_count = static_cast<u32>(verts.size());
    sm.node = 0;
    const vec3 center = (box.min + box.max) * 0.5f, extent = max((box.max - box.min) * 0.5f, vec3{1e-4f, 1e-4f, 1e-4f});
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
    sm.material.base_color[0] = sm.material.base_color[1] = sm.material.base_color[2] = 0.5f;
    sm.material.base_color[3] = 1.0f;
    sm.material.roughness = 0.9f;
    sm.material.alpha_cutoff = 0.5f;

    Array<asset::MeshVertex> qv;
    qv.resize(verts.size());
    for (usize i = 0; i < verts.size(); ++i) {
        const V& v = verts[i];
        asset::MeshVertex& o = qv[i];
        o = {};
        o.position[0] = snorm16((v.p.x - center.x) / extent.x);
        o.position[1] = snorm16((v.p.y - center.y) / extent.y);
        o.position[2] = snorm16((v.p.z - center.z) / extent.z);
        f32 oct[2];
        oct_encode(v.n, oct);
        o.normal[0] = snorm16(oct[0]);
        o.normal[1] = snorm16(oct[1]);
        o.uv[0] = unorm16((v.uv.x - uv_lo.x) / sm.uv_scale[0]);
        o.uv[1] = unorm16((v.uv.y - uv_lo.y) / sm.uv_scale[1]);
    }
    const bool wide = verts.size() > 0xFFFFu;
    Array<byte> ib;
    ib.resize(idx.size() * (wide ? 4 : 2));
    for (usize i = 0; i < idx.size(); ++i) {
        if (wide) std::memcpy(ib.data() + i * 4, &idx[i], 4);
        else {
            const u16 v = static_cast<u16>(idx[i]);
            std::memcpy(ib.data() + i * 2, &v, 2);
        }
    }
    asset::MeshNode node{};
    node.parent = -1;
    node.scale[0] = node.scale[1] = node.scale[2] = 1.0f;
    node.rotation[3] = 1.0f;

    asset::MeshData d;
    d.vertices = qv.span();
    d.indices = ib.span();
    d.submeshes = Span<const asset::MeshSubmesh>(&sm, 1);
    d.nodes = Span<const asset::MeshNode>(&node, 1);
    d.flags = wide ? asset::MeshFlags::Index32 : 0u;
    d.bounds_min[0] = box.min.x;
    d.bounds_min[1] = box.min.y;
    d.bounds_min[2] = box.min.z;
    d.bounds_max[0] = box.max.x;
    d.bounds_max[1] = box.max.y;
    d.bounds_max[2] = box.max.z;
    asset::write_mesh(d, out);
}

bool compose_layer_array(Span<const asset::TextureView* const> layers, LayerArray& out, const char* debug_name) {
    out.present = 0;
    out.pixels.clear();
    out.mips.clear();
    const asset::TextureView* ref = nullptr;
    auto srgb = [](const asset::TextureView& v) { return (v.header->flags & asset::TextureFlags::Srgb) != 0; };
    for (u32 i = 0; i < static_cast<u32>(layers.size()); ++i) {
        const asset::TextureView* v = layers[i];
        if (!v || !v->header || v->mips.empty()) continue;
        if (!ref) ref = v;
        const asset::TextureHeader &a = *ref->header, &b = *v->header;
        if (b.format != a.format || srgb(*v) != srgb(*ref) || b.width != a.width || b.height != a.height || v->mips.size() != ref->mips.size()) {
            MCE_LOG_WARN("terrain: layer {} texture is {}x{} with {} mips, layer textures must match the first ({}x{}, {} mips, same format)",
                         i, b.width, b.height, v->mips.size(), a.width, a.height, ref->mips.size());
            continue;
        }
        out.present |= 1u << i;
    }
    if (!ref) return false;
    const u32 count = static_cast<u32>(layers.size());
    usize total = 0;
    for (const asset::TextureMip& m : ref->mips) total += static_cast<usize>(m.layer_size) * count;
    out.pixels.resize(total, byte{0});
    usize at = 0;
    for (u32 m = 0; m < ref->mips.size(); ++m) {
        const usize layer_size = static_cast<usize>(ref->mips[m].layer_size);
        for (u32 i = 0; i < count; ++i) {
            if (!(out.present & (1u << i))) continue;
            const Span<const byte> src = layers[i]->pixels(m, 0);
            if (src.size() >= layer_size) std::memcpy(out.pixels.data() + at + usize(i) * layer_size, src.data(), layer_size);
        }
        out.mips.push_back(TextureMipUpload{ref->mips[m].width, ref->mips[m].height, Span<const byte>(out.pixels.data() + at, layer_size * count)});
        at += layer_size * count;
    }
    const bool is_srgb = srgb(*ref);
    if (ref->header->format == asset::TextureFormat::ASTC_4x4)
        out.upload.format = is_srgb ? rhi::TextureFormat::ASTC_4x4_SRGB : rhi::TextureFormat::ASTC_4x4_UNorm;
    else if (ref->header->format == asset::TextureFormat::RGBA16F)
        out.upload.format = rhi::TextureFormat::RGBA16F;
    else
        out.upload.format = is_srgb ? rhi::TextureFormat::RGBA8_SRGB : rhi::TextureFormat::RGBA8_UNorm;
    out.upload.layers = count;
    out.upload.array = true;
    out.upload.mips = out.mips.span();
    out.upload.debug_name = debug_name;
    return true;
}

TextureUpload splat_upload(const asset::TerrainView& terrain, TextureMipUpload& mip) {
    const u32 n = terrain.splat_resolution();
    mip = {n, n, Span<const byte>(reinterpret_cast<const byte*>(terrain.splat.data()), terrain.splat.size())};
    TextureUpload up;
    up.format = rhi::TextureFormat::RGBA8_UNorm;
    up.layers = 2;
    up.array = true;
    up.mips = Span<const TextureMipUpload>(&mip, 1);
    up.debug_name = "terrain.splat";
    return up;
}

// ---- foliage on the terrain (M-TERRAIN 5) ------------------------------------------------------------

namespace {

u64 scatter_hash(u64 seed, u64 cell) {
    u64 z = seed * 0x9E3779B97F4A7C15ull + cell * 0xD1B54A32D192ED03ull + 0x632BE59BD9B4E019ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// The layer's weight (0..1) at terrain-space (x, z), bilinear over the splat grid.
f32 layer_weight(const asset::TerrainView& t, u32 layer, f32 x, f32 z) {
    if (layer >= TerrainScatter::kAnyLayer) return 1.0f;
    if (t.header->layer_count == 0) return layer == 0 ? 1.0f : 0.0f;
    if (layer >= t.header->layer_count) return 0.0f;
    const u32 n = t.splat_resolution();
    const f32 last = static_cast<f32>(n - 1);
    const f32 fx = clamp((x / t.header->size[0] + 0.5f) * last, 0.0f, last), fz = clamp((z / t.header->size[1] + 0.5f) * last, 0.0f, last);
    const u32 x0 = min(static_cast<u32>(fx), n - 2), z0 = min(static_cast<u32>(fz), n - 2);
    const f32 u = fx - static_cast<f32>(x0), w = fz - static_cast<f32>(z0);
    const f32 w00 = t.weight(x0, z0, layer), w10 = t.weight(x0 + 1, z0, layer);
    const f32 w01 = t.weight(x0, z0 + 1, layer), w11 = t.weight(x0 + 1, z0 + 1, layer);
    const f32 a = w00 + (w10 - w00) * u, b = w01 + (w11 - w01) * u;
    return (a + (b - a) * w) / 255.0f;
}

}  // namespace

void scatter_on_terrain(const asset::TerrainView& t, const TerrainScatter& s, Array<mat4>& out) {
    out.clear();
    if (!(s.density > 0.0f) || s.max_count == 0) return;
    const f32 sx = t.header->size[0], sz = t.header->size[1];
    f32 cell = 1.0f / std::sqrt(s.density);
    const f64 cells = f64(sx) * sz / (f64(cell) * cell);
    if (cells > 4.0 * s.max_count) cell *= static_cast<f32>(std::sqrt(cells / (4.0 * s.max_count)));
    const u32 nx = max(1u, static_cast<u32>(std::ceil(sx / cell))), nz = max(1u, static_cast<u32>(std::ceil(sz / cell)));
    const u32 n = t.resolution();
    const f32 last = static_cast<f32>(n - 1);
    const f32 max_grad = s.max_slope_deg >= 90.0f ? 1e30f : std::tan(radians(max(s.max_slope_deg, 0.0f)));
    const f32 hx = 0.5f * t.spacing_x(), hz = 0.5f * t.spacing_z();
    for (u32 cz = 0; cz < nz; ++cz)
        for (u32 cx = 0; cx < nx; ++cx) {
            u64 r = scatter_hash(s.seed, u64(cz) * nx + cx);
            auto unit = [&r]() {
                r = r * 6364136223846793005ull + 1442695040888963407ull;
                return static_cast<f32>(r >> 40) * (1.0f / 16777216.0f);
            };
            const f32 x = -0.5f * sx + (static_cast<f32>(cx) + unit()) * cell;
            const f32 z = -0.5f * sz + (static_cast<f32>(cz) + unit()) * cell;
            const f32 keep = unit(), yaw = unit(), size = unit();
            if (x > 0.5f * sx || z > 0.5f * sz) continue;
            if (keep >= layer_weight(t, s.layer, x, z)) continue;
            if (!t.holes.empty()) {
                const u32 qx = min(static_cast<u32>(clamp((x / sx + 0.5f) * last, 0.0f, last)), n - 2);
                const u32 qz = min(static_cast<u32>(clamp((z / sz + 0.5f) * last, 0.0f, last)), n - 2);
                if (t.hole(qx, qz) || t.hole(qx + 1, qz) || t.hole(qx, qz + 1) || t.hole(qx + 1, qz + 1)) continue;
            }
            if (max_grad < 1e29f) {
                const f32 gx = (t.surface_at(x + hx, z) - t.surface_at(x - hx, z)) / (2.0f * hx);
                const f32 gz = (t.surface_at(x, z + hz) - t.surface_at(x, z - hz)) / (2.0f * hz);
                if (gx * gx + gz * gz > max_grad * max_grad) continue;
            }
            const vec3 local{x * s.scale.x, t.surface_at(x, z) * s.scale.y, z * s.scale.z};
            const f32 k = s.scale_min + (s.scale_max - s.scale_min) * size;
            const quat turn = s.random_yaw ? s.rotation * quat_from_axis_angle({0, 1, 0}, yaw * 6.28318531f) : s.rotation;
            out.push_back(trs(s.position + rotate(s.rotation, local), turn, {k, k, k}));
            if (out.size() >= s.max_count) return;
        }
}

}  // namespace mce::render

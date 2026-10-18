// The impostor bake (M-OPENWORLD A3): orthographic albedo captures of each group of sources, rasterised on the CPU,
// packed into one atlas and placed on box sides or cross cards. See mce/cook/impostor.h.

#include "mce/cook/impostor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "mce/asset/formats.h"
#include "texture_bc.h"

namespace mce::cook {

using namespace mce::math;

namespace {

constexpr u32 kPad = 2;  // texels around each capture, copies of its edge: bilinear filtering stays inside it

i16 snorm16(f32 v) { return static_cast<i16>(std::lround(clamp(v, -1.0f, 1.0f) * 32767.0f)); }
u16 unorm16(f32 v) { return static_cast<u16>(std::lround(clamp(v, 0.0f, 1.0f) * 65535.0f)); }
f32 snorm(i16 q) { return max(static_cast<f32>(q) / 32767.0f, -1.0f); }

// Octahedral encoding (the glTF cook's) of a unit vector into [-1, 1]^2.
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

mat4 node_local(const asset::MeshNode& n) {
    const quat q = normalize(quat{n.rotation[0], n.rotation[1], n.rotation[2], n.rotation[3]});
    return trs({n.translation[0], n.translation[1], n.translation[2]}, q, {n.scale[0], n.scale[1], n.scale[2]});
}

// sRGB <-> linear through one table, so encoding is a search and the bytes do not depend on a libm's pow.
struct Srgb {
    f32 linear[256];
    f32 mid[255];  // the linear value halfway between codes i and i + 1
};
const Srgb& srgb() {
    static const Srgb t = [] {
        Srgb s{};
        for (u32 i = 0; i < 256; ++i) {
            const double c = i / 255.0;
            s.linear[i] = static_cast<f32>(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
        }
        for (u32 i = 0; i < 255; ++i) s.mid[i] = (s.linear[i] + s.linear[i + 1]) * 0.5f;
        return s;
    }();
    return t;
}
u8 to_srgb8(f32 lin) {
    const Srgb& s = srgb();
    return static_cast<u8>(std::upper_bound(s.mid, s.mid + 255, lin) - s.mid);
}

// A cooked texture decoded to RGBA8, every mip of layer 0.
struct Level {
    u32 w = 0, h = 0;
    Array<u8> rgba;
};
struct Decoded {
    asset::Guid guid;
    bool ok = false;
    bool srgb = false;
    Array<Level> mips;
};

bool decode(Span<const byte> payload, Decoded& d) {
    const Result<asset::TextureView> tv = asset::TextureView::read(payload);
    if (!tv) return false;
    const asset::TextureView& v = tv.value();
    BcFormat bc = BcFormat::BC1;
    const asset::TextureFormat f = v.header->format;
    if (f == asset::TextureFormat::BC1) bc = BcFormat::BC1;
    else if (f == asset::TextureFormat::BC4) bc = BcFormat::BC4;
    else if (f == asset::TextureFormat::BC5) bc = BcFormat::BC5;
    else if (f == asset::TextureFormat::BC7) bc = BcFormat::BC7;
    else if (f != asset::TextureFormat::RGBA8) return false;  // ASTC, RGBA16F: not decoded here
    d.srgb = (v.header->flags & asset::TextureFlags::Srgb) != 0;
    for (u32 m = 0; m < v.header->mip_count && m < v.mips.size(); ++m) {
        Level l;
        l.w = v.mips[m].width;
        l.h = v.mips[m].height;
        const Span<const byte> px = v.pixels(m);
        if (l.w == 0 || l.h == 0 || px.size() < asset::texture_level_bytes(f, l.w, l.h)) break;
        l.rgba.resize(static_cast<usize>(l.w) * l.h * 4);
        if (f == asset::TextureFormat::RGBA8) std::memcpy(l.rgba.data(), px.data(), l.rgba.size());
        else decode_bc_level(bc, reinterpret_cast<const u8*>(px.data()), l.w, l.h, l.rgba.data());
        d.mips.push_back(std::move(l));
    }
    return !d.mips.empty();
}

struct Mat {
    vec4 color;               // base colour x tint, linear
    const Decoded* texture = nullptr;
    bool mask = false;
    f32 cutoff = 0.5f;
};

struct Tri {
    vec3 p[3];
    vec2 uv[3];
    u32 mat = 0;
};

// One capture: the group seen along -n, an orthographic window `right` x `up` of 2hw x 2hh around the box face.
struct Face {
    vec3 n, right, up;
    vec3 at;          // the quad's centre (the box face, or the box centre for a card)
    vec3 eye;         // the capture plane's centre (the box face)
    f32 hw = 0, hh = 0;
    u32 w = 0, h = 0;   // inner texels
    bool card = false;
    Array<u8> image;  // (w + 2 kPad) x (h + 2 kPad) RGBA8
    u32 ax = 0, ay = 0;  // the image's corner in the atlas
};

f32 edge(f32 ax, f32 ay, f32 bx, f32 by, f32 px, f32 py) { return (bx - ax) * (py - ay) - (by - ay) * (px - ax); }

vec4 sample(const Mat& m, vec2 uv, u32 mip) {
    vec4 c = m.color;
    if (!m.texture) return c;
    const Level& l = m.texture->mips[min<usize>(mip, m.texture->mips.size() - 1)];
    const f32 u = uv.x - std::floor(uv.x), v = uv.y - std::floor(uv.y);
    const u32 x = min(static_cast<u32>(u * static_cast<f32>(l.w)), l.w - 1);
    const u32 y = min(static_cast<u32>(v * static_cast<f32>(l.h)), l.h - 1);
    const u8* t = &l.rgba[(static_cast<usize>(y) * l.w + x) * 4];
    const Srgb& s = srgb();
    const bool lin = m.texture->srgb;
    c.x *= lin ? s.linear[t[0]] : t[0] / 255.0f;
    c.y *= lin ? s.linear[t[1]] : t[1] / 255.0f;
    c.z *= lin ? s.linear[t[2]] : t[2] / 255.0f;
    c.w *= t[3] / 255.0f;
    return c;
}

void capture(Face& f, Span<const Tri> tris, Span<const Mat> mats) {
    const u32 w = f.w, h = f.h, n = w * h;
    Array<f32> depth;
    depth.resize(n, -1e30f);
    Array<vec3> color;
    color.resize(n, vec3{0.0f, 0.0f, 0.0f});
    Array<u8> covered;
    covered.resize(n, 0);
    const f32 sx = static_cast<f32>(w) / (2.0f * f.hw), sy = static_cast<f32>(h) / (2.0f * f.hh);
    for (const Tri& t : tris) {
        f32 x[3], y[3], z[3];
        for (int i = 0; i < 3; ++i) {
            const vec3 d = t.p[i] - f.eye;
            x[i] = (dot(d, f.right) + f.hw) * sx;
            y[i] = (f.hh - dot(d, f.up)) * sy;
            z[i] = dot(d, f.n);
        }
        const f32 area = edge(x[0], y[0], x[1], y[1], x[2], y[2]);
        if (std::fabs(area) < 1e-9f) continue;
        const Mat& m = mats[t.mat];
        // The mip whose texels are about one per captured texel.
        u32 mip = 0;
        if (m.texture) {
            const Level& l0 = m.texture->mips[0];
            const vec2 e1 = t.uv[1] - t.uv[0], e2 = t.uv[2] - t.uv[0];
            f32 ratio = std::fabs(cross(e1, e2)) * static_cast<f32>(l0.w) * static_cast<f32>(l0.h) / std::fabs(area);
            while (ratio > 2.0f && mip + 1 < m.texture->mips.size()) {
                ratio *= 0.25f;
                ++mip;
            }
        }
        const f32 inv = 1.0f / area;
        const i32 x0 = max(0, static_cast<i32>(std::floor(min(x[0], min(x[1], x[2])))));
        const i32 x1 = min(static_cast<i32>(w) - 1, static_cast<i32>(std::ceil(max(x[0], max(x[1], x[2])))));
        const i32 y0 = max(0, static_cast<i32>(std::floor(min(y[0], min(y[1], y[2])))));
        const i32 y1 = min(static_cast<i32>(h) - 1, static_cast<i32>(std::ceil(max(y[0], max(y[1], y[2])))));
        for (i32 py = y0; py <= y1; ++py)
            for (i32 px = x0; px <= x1; ++px) {
                const f32 cx = static_cast<f32>(px) + 0.5f, cy = static_cast<f32>(py) + 0.5f;
                const f32 b0 = edge(x[1], y[1], x[2], y[2], cx, cy) * inv;
                const f32 b1 = edge(x[2], y[2], x[0], y[0], cx, cy) * inv;
                const f32 b2 = 1.0f - b0 - b1;
                if (b0 < 0.0f || b1 < 0.0f || b2 < 0.0f) continue;
                const usize i = static_cast<usize>(py) * w + static_cast<usize>(px);
                const f32 zz = b0 * z[0] + b1 * z[1] + b2 * z[2];
                if (zz <= depth[i]) continue;
                const vec4 c = sample(m, t.uv[0] * b0 + t.uv[1] * b1 + t.uv[2] * b2, mip);
                if (m.mask && c.w < m.cutoff) continue;
                depth[i] = zz;
                color[i] = vec3{c.x, c.y, c.z};
                covered[i] = 1;
            }
    }

    // Uncovered texels take a covered neighbour's colour, ring by ring; alpha stays 0.
    Array<u8> filled = covered;
    bool any = false;
    for (u32 i = 0; i < n; ++i) any = any || covered[i];
    if (!any) {
        f.image.clear();
        return;
    }
    for (u32 pass = 0; pass < w + h; ++pass) {
        bool open = false;
        Array<u8> next = filled;
        for (u32 py = 0; py < h; ++py)
            for (u32 px = 0; px < w; ++px) {
                const u32 i = py * w + px;
                if (filled[i]) continue;
                open = true;
                const i32 nb[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
                for (const auto& o : nb) {
                    const i32 qx = static_cast<i32>(px) + o[0], qy = static_cast<i32>(py) + o[1];
                    if (qx < 0 || qy < 0 || qx >= static_cast<i32>(w) || qy >= static_cast<i32>(h)) continue;
                    const u32 j = static_cast<u32>(qy) * w + static_cast<u32>(qx);
                    if (!filled[j]) continue;
                    color[i] = color[j];
                    next[i] = 1;
                    break;
                }
            }
        filled = std::move(next);
        if (!open) break;
    }

    const u32 bw = w + 2 * kPad, bh = h + 2 * kPad;
    f.image.resize(static_cast<usize>(bw) * bh * 4);
    for (u32 by = 0; by < bh; ++by)
        for (u32 bx = 0; bx < bw; ++bx) {
            const u32 px = static_cast<u32>(clamp(static_cast<i32>(bx) - static_cast<i32>(kPad), 0, static_cast<i32>(w) - 1));
            const u32 py = static_cast<u32>(clamp(static_cast<i32>(by) - static_cast<i32>(kPad), 0, static_cast<i32>(h) - 1));
            const u32 i = py * w + px;
            u8* o = &f.image[(static_cast<usize>(by) * bw + bx) * 4];
            o[0] = to_srgb8(color[i].x);
            o[1] = to_srgb8(color[i].y);
            o[2] = to_srgb8(color[i].z);
            o[3] = covered[i] ? 255 : 0;
        }
}

u32 pow2_at_least(u32 v) {
    u32 p = 4;
    while (p < v) p <<= 1;
    return p;
}

}  // namespace

bool bake_impostor(Span<const ImpostorSource> sources, ImpostorTextureFn texture, void* user, asset::Guid atlas_guid,
                   const ImpostorSettings& settings, ImpostorBake& out) {
    out = ImpostorBake{};
    ImpostorStats st;

    // Sources by group (then by their order), so a group's members need not be adjacent.
    Array<u32> order;
    order.reserve(sources.size());
    for (u32 i = 0; i < sources.size(); ++i) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](u32 a, u32 b) { return sources[a].group < sources[b].group; });

    Array<Decoded> textures;  // decoded once per GUID; Mat::texture points into it, so it is reserved up front
    usize max_textures = 0;
    for (const ImpostorSource& s : sources)
        if (s.mesh) max_textures += s.mesh->submeshes.size() + 1;
    textures.reserve(max_textures);
    const auto find_texture = [&](asset::Guid g) -> const Decoded* {
        if (g.is_null()) return nullptr;
        for (const Decoded& d : textures)
            if (d.guid == g) return d.ok ? &d : nullptr;
        Decoded d;
        d.guid = g;
        const Span<const byte> bytes = texture ? texture(g, user) : Span<const byte>{};
        d.ok = !bytes.empty() && decode(bytes, d);
        textures.push_back(std::move(d));
        return textures.back().ok ? &textures.back() : nullptr;
    };

    Array<Face> faces;
    Array<Tri> tris;
    Array<Mat> mats;
    Array<mat4> nodes;
    for (usize k = 0; k < order.size();) {
        const u32 group = sources[order[k]].group;
        tris.clear();
        mats.clear();
        for (; k < order.size() && sources[order[k]].group == group; ++k) {
            const ImpostorSource& src = sources[order[k]];
            if (!src.mesh || !src.mesh->header || src.mesh->skinned()) {
                ++st.skipped;
                continue;
            }
            const asset::MeshView& v = *src.mesh;
            nodes.resize(v.nodes.size());
            for (usize i = 0; i < v.nodes.size(); ++i)
                nodes[i] = v.nodes[i].parent < 0 ? node_local(v.nodes[i]) : nodes[static_cast<usize>(v.nodes[i].parent)] * node_local(v.nodes[i]);
            for (u32 s = 0; s < v.submeshes.size(); ++s) {
                const asset::MeshSubmesh& sm = v.submeshes[s];
                const asset::MaterialChunk& mc = src.material ? *src.material : sm.material;
                if ((mc.flags & asset::MaterialFlags::AlphaBlend) || (!src.material && v.material_ext(s).transmission > 0.0f)) {
                    ++st.skipped;
                    continue;
                }
                Mat m;
                m.color = vec4{mc.base_color[0], mc.base_color[1], mc.base_color[2], mc.base_color[3]} * src.tint;
                m.mask = (mc.flags & asset::MaterialFlags::AlphaMask) != 0;
                m.cutoff = mc.alpha_cutoff;
                const asset::Guid tg = mc.texture(asset::MaterialSlot::BaseColor);
                m.texture = find_texture(tg);
                if (!tg.is_null() && !m.texture) ++st.untextured;
                f32 rep[2];
                mc.uv_scale(rep);
                const u32 mat = static_cast<u32>(mats.size());
                mats.push_back(m);
                const mat4 place = src.world * (sm.node < nodes.size() ? nodes[sm.node] : mat4::identity());
                const auto vertex = [&](u32 local, vec3& p, vec2& uv) {
                    const asset::MeshVertex& q = v.vertices[sm.base_vertex + local];
                    p = transform_point(place, vec3{sm.center[0] + snorm(q.position[0]) * sm.extent[0],
                                                    sm.center[1] + snorm(q.position[1]) * sm.extent[1],
                                                    sm.center[2] + snorm(q.position[2]) * sm.extent[2]});
                    uv = vec2{(sm.uv_offset[0] + q.uv[0] / 65535.0f * sm.uv_scale[0]) * rep[0],
                              (sm.uv_offset[1] + q.uv[1] / 65535.0f * sm.uv_scale[1]) * rep[1]};
                };
                for (u32 i = 0; i + 2 < sm.index_count; i += 3) {
                    Tri t;
                    t.mat = mat;
                    for (u32 c = 0; c < 3; ++c) vertex(v.index(sm.first_index + i + c), t.p[c], t.uv[c]);
                    tris.push_back(t);
                }
            }
        }
        if (tris.empty()) continue;

        vec3 lo = tris[0].p[0], hi = lo;
        for (const Tri& t : tris)
            for (const vec3& p : t.p) {
                lo = min(lo, p);
                hi = max(hi, p);
            }
        st.bounds = st.groups ? merge(st.bounds, aabb3{lo, hi}) : aabb3{lo, hi};
        ++st.groups;
        const vec3 size = hi - lo, centre = (lo + hi) * 0.5f, half = size * 0.5f;
        const f32 width = max(size.x, size.z);
        const bool card = size.y > settings.card_ratio * width && width < settings.card_size;
        if (card) ++st.cards;
        else ++st.boxes;

        // Box: the four sides and the top. Cards: the +Z and +X views, placed through the centre.
        const vec3 dirs[5] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}, {0, 1, 0}};
        const u32 count = card ? 2u : 5u;
        for (u32 d = 0; d < count; ++d) {
            Face f;
            f.card = card;
            f.n = card ? (d == 0 ? vec3{0, 0, 1} : vec3{1, 0, 0}) : dirs[d];
            const vec3 fwd = -f.n;
            const vec3 up_ref = f.n.y > 0.5f ? vec3{0, 0, -1} : vec3{0, 1, 0};
            f.right = cross(fwd, up_ref);
            f.up = cross(f.right, fwd);
            f.hw = std::fabs(dot(half, f.right));
            f.hh = std::fabs(dot(half, f.up));
            f.eye = centre + f.n * std::fabs(dot(half, f.n));
            f.at = card ? centre : f.eye;
            if (f.hw < 1e-3f || f.hh < 1e-3f) continue;
            const f32 longest = 2.0f * max(f.hw, f.hh);
            const f32 scale = min(settings.texels_per_metre, static_cast<f32>(max(settings.max_face, 1u)) / longest);
            f.w = clamp(static_cast<u32>(std::ceil(2.0f * f.hw * scale - 1e-3f)), 1u, max(settings.max_face, 1u));
            f.h = clamp(static_cast<u32>(std::ceil(2.0f * f.hh * scale - 1e-3f)), 1u, max(settings.max_face, 1u));
            capture(f, tris.span(), mats.span());
            if (f.image.empty()) continue;
            faces.push_back(std::move(f));
        }
        st.triangles += static_cast<u32>(tris.size());
    }
    if (faces.empty()) {
        out.stats = st;
        return false;
    }

    // ---- shelf packing, tallest first ------------------------------------------------------------------------------
    u64 area = 0;
    u32 widest = 0;
    for (const Face& f : faces) {
        area += static_cast<u64>(f.w + 2 * kPad) * (f.h + 2 * kPad);
        widest = max(widest, f.w + 2 * kPad);
    }
    u32 aw = pow2_at_least(max(settings.atlas_width, widest));
    while (static_cast<u64>(aw) * aw < area + area / 4) aw <<= 1;  // square-ish: grow the width with the content
    Array<u32> pack;
    for (u32 i = 0; i < faces.size(); ++i) pack.push_back(i);
    std::stable_sort(pack.begin(), pack.end(), [&](u32 a, u32 b) {
        return faces[a].h != faces[b].h ? faces[a].h > faces[b].h : faces[a].w > faces[b].w;
    });
    u32 x = 0, y = 0, shelf = 0;
    for (u32 i : pack) {
        Face& f = faces[i];
        const u32 bw = f.w + 2 * kPad, bh = f.h + 2 * kPad;
        if (x + bw > aw) {
            x = 0;
            y += shelf;
            shelf = 0;
        }
        f.ax = x;
        f.ay = y;
        x += bw;
        shelf = max(shelf, bh);
    }
    const u32 ah = pow2_at_least(y + shelf);
    out.atlas_width = aw;
    out.atlas_height = ah;
    out.atlas.resize(static_cast<usize>(aw) * ah * 4, 0);
    for (const Face& f : faces) {
        const u32 bw = f.w + 2 * kPad, bh = f.h + 2 * kPad;
        for (u32 r = 0; r < bh; ++r)
            std::memcpy(&out.atlas[(static_cast<usize>(f.ay + r) * aw + f.ax) * 4], &f.image[static_cast<usize>(r) * bw * 4], static_cast<usize>(bw) * 4);
    }

    // ---- the mesh: boxes, then cards -------------------------------------------------------------------------------
    Array<asset::MeshVertex> vertices;
    Array<vec3> positions;
    Array<u32> indices;
    Array<asset::MeshSubmesh> subs;
    bool wide = false;
    for (int pass = 0; pass < 2; ++pass) {
        const bool cards = pass == 1;
        asset::MeshSubmesh sm{};
        sm.first_index = static_cast<u32>(indices.size());
        sm.base_vertex = static_cast<u32>(vertices.size());
        for (const Face& f : faces) {
            if (f.card != cards) continue;
            const u32 base = static_cast<u32>(vertices.size()) - sm.base_vertex;
            const f32 u0 = static_cast<f32>(f.ax + kPad) / static_cast<f32>(aw), u1 = static_cast<f32>(f.ax + kPad + f.w) / static_cast<f32>(aw);
            const f32 v0 = static_cast<f32>(f.ay + kPad) / static_cast<f32>(ah), v1 = static_cast<f32>(f.ay + kPad + f.h) / static_cast<f32>(ah);
            const vec3 r = f.right * f.hw, u = f.up * f.hh;
            const vec3 corner[4] = {f.at - r - u, f.at + r - u, f.at + r + u, f.at - r + u};
            const vec2 uv[4] = {{u0, v1}, {u1, v1}, {u1, v0}, {u0, v0}};
            f32 oct[2];
            oct_encode(f.n, oct);
            for (int c = 0; c < 4; ++c) {
                asset::MeshVertex q{};
                q.normal[0] = snorm16(oct[0]);
                q.normal[1] = snorm16(oct[1]);
                q.uv[0] = unorm16(uv[c].x);
                q.uv[1] = unorm16(uv[c].y);
                vertices.push_back(q);
                positions.push_back(corner[c]);
            }
            for (u32 i : {0u, 1u, 2u, 0u, 2u, 3u}) indices.push_back(base + i);
            ++st.quads;
        }
        sm.index_count = static_cast<u32>(indices.size()) - sm.first_index;
        sm.vertex_count = static_cast<u32>(vertices.size()) - sm.base_vertex;
        if (sm.index_count == 0) continue;
        wide = wide || sm.vertex_count > 0xFFFF;
        sm.uv_scale[0] = sm.uv_scale[1] = 1.0f;
        asset::MaterialChunk& m = sm.material;
        m.base_color[0] = m.base_color[1] = m.base_color[2] = m.base_color[3] = 1.0f;
        m.roughness = 1.0f;
        m.alpha_cutoff = 0.5f;
        m.flags = asset::MaterialFlags::AlphaMask | (cards ? asset::MaterialFlags::DoubleSided : 0u);
        m.textures[static_cast<u32>(asset::MaterialSlot::BaseColor)][0] = atlas_guid.hi;
        m.textures[static_cast<u32>(asset::MaterialSlot::BaseColor)][1] = atlas_guid.lo;
        subs.push_back(sm);
    }
    vec3 lo = positions[0], hi = positions[0];
    for (const vec3& p : positions) {
        lo = min(lo, p);
        hi = max(hi, p);
    }
    const vec3 centre = (lo + hi) * 0.5f, extent = max((hi - lo) * 0.5f, vec3{1e-4f, 1e-4f, 1e-4f});
    for (usize i = 0; i < vertices.size(); ++i) {
        vertices[i].position[0] = snorm16((positions[i].x - centre.x) / extent.x);
        vertices[i].position[1] = snorm16((positions[i].y - centre.y) / extent.y);
        vertices[i].position[2] = snorm16((positions[i].z - centre.z) / extent.z);
    }
    for (asset::MeshSubmesh& m : subs) {
        m.center[0] = centre.x;
        m.center[1] = centre.y;
        m.center[2] = centre.z;
        m.extent[0] = extent.x;
        m.extent[1] = extent.y;
        m.extent[2] = extent.z;
    }
    Array<byte> ib;
    if (wide) {
        ib.resize(indices.size() * 4);
        std::memcpy(ib.data(), indices.data(), ib.size());
    } else {
        ib.resize(indices.size() * 2);
        for (usize i = 0; i < indices.size(); ++i) {
            const u16 q = static_cast<u16>(indices[i]);
            std::memcpy(ib.data() + i * 2, &q, 2);
        }
    }
    asset::MeshNode node{};
    node.parent = -1;
    node.scale[0] = node.scale[1] = node.scale[2] = 1.0f;
    node.rotation[3] = 1.0f;
    asset::MeshData d;
    d.vertices = vertices.span();
    d.indices = ib.span();
    d.submeshes = subs.span();
    d.nodes = Span<const asset::MeshNode>(&node, 1);
    d.flags = wide ? asset::MeshFlags::Index32 : 0u;
    d.bounds_min[0] = lo.x;
    d.bounds_min[1] = lo.y;
    d.bounds_min[2] = lo.z;
    d.bounds_max[0] = hi.x;
    d.bounds_max[1] = hi.y;
    d.bounds_max[2] = hi.z;
    asset::write_mesh(d, out.mesh);

    st.atlas_width = aw;
    st.atlas_height = ah;
    out.stats = st;
    return true;
}

}  // namespace mce::cook

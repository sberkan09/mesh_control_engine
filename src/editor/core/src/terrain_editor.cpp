#include "mce/editor/terrain_editor.h"

#include <algorithm>
#include <cmath>

#include "mce/asset/formats.h"
#include "mce/cook/fs.h"
#include "mce/cook/image.h"
#include "mce/kit/kit3d.h"
#include "mce/log.h"

namespace mce::editor {

namespace {

constexpr const char* kStage = "editor.terrain";

void base64_append(Span<const u8> in, cook::String& out) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    usize i = 0;
    char quad[5] = {};
    for (; i + 2 < in.size(); i += 3) {
        const u32 v = (u32(in[i]) << 16) | (u32(in[i + 1]) << 8) | u32(in[i + 2]);
        for (int s = 0; s < 4; ++s) quad[s] = k[(v >> (18 - 6 * s)) & 63];
        out += StringView(quad, 4);
    }
    if (i < in.size()) {
        u32 v = u32(in[i]) << 16;
        if (i + 1 < in.size()) v |= u32(in[i + 1]) << 8;
        quad[0] = k[(v >> 18) & 63];
        quad[1] = k[(v >> 12) & 63];
        quad[2] = i + 1 < in.size() ? k[(v >> 6) & 63] : '=';
        quad[3] = '=';
        out += StringView(quad, 4);
    }
}

// A value in [0, 1) fixed per lattice point and seed.
f32 lattice(i32 x, i32 z, u32 seed) {
    u32 h = static_cast<u32>(x) * 0x8da6b343u ^ static_cast<u32>(z) * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return static_cast<f32>(h & 0xFFFFFFu) / 16777216.0f;
}

// Smooth value noise in [-1, 1] with cells of `cell` metres.
f32 value_noise(f32 x, f32 z, f32 cell, u32 seed) {
    const f32 fx = x / cell, fz = z / cell;
    const f32 x0 = std::floor(fx), z0 = std::floor(fz);
    const i32 ix = static_cast<i32>(x0), iz = static_cast<i32>(z0);
    f32 u = fx - x0, w = fz - z0;
    u = u * u * (3.0f - 2.0f * u);
    w = w * w * (3.0f - 2.0f * w);
    const f32 a = lattice(ix, iz, seed), b = lattice(ix + 1, iz, seed), c = lattice(ix, iz + 1, seed), d = lattice(ix + 1, iz + 1, seed);
    return 2.0f * ((a + (b - a) * u) * (1.0f - w) + (c + (d - c) * u) * w) - 1.0f;
}

}  // namespace

f32 terrain_brush_falloff(const TerrainBrush& b, f32 d) {
    if (b.radius <= 0.0f || d >= b.radius) return 0.0f;
    const f32 inner = math::clamp(b.hardness, 0.0f, 1.0f) * b.radius;
    if (d <= inner) return 1.0f;
    const f32 t = (d - inner) / (b.radius - inner);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

// ---- TerrainDocument ----

bool TerrainDocument::load(StringView source, Span<const byte> cooked, StringView file, cook::DiagList& diags) {
    cook::JsonDoc json;
    cook::JsonError err;
    if (!json.parse(source, err)) {
        diags.error(Errc::ParseError, file, err.line, err.col, kStage, err.message.view());
        return false;
    }
    const cook::JsonValue root = json.root();
    const cook::JsonValue fmt = root.get("format");
    if (!root.is_object() || !fmt.is_string() || fmt.as_string() != "mce-terrain") {
        diags.error(Errc::ParseError, file, root.line(), root.col(), kStage, "\"format\" must be \"mce-terrain\"");
        return false;
    }
    Result<asset::TerrainView> view = asset::TerrainView::read(cooked);
    if (!view) {
        diags.error(Errc::ParseError, file, 0, 0, kStage, "the cooked terrain does not read");
        return false;
    }
    const asset::TerrainView& v = view.value();
    TerrainDocument d;
    d.resolution = v.resolution();
    d.splat_resolution = v.splat_resolution();
    d.layer_count = v.header->layer_count;
    d.size = {v.header->size[0], v.header->size[1]};
    d.height_min = v.header->height_min;
    d.height_max = v.header->height_max;
    const u32 n = d.resolution;
    d.heights.resize(usize(n) * n);
    for (u32 z = 0; z < n; ++z)
        for (u32 x = 0; x < n; ++x) d.heights[usize(z) * n + x] = v.sample(x, z);
    d.holes.resize((usize(n) * n + 7) / 8, u8{0});
    for (usize i = 0; i < v.holes.size() && i < d.holes.size(); ++i) d.holes[i] = v.holes[i];
    if (d.layer_count > 0) {
        const u32 s = d.splat_resolution;
        d.weights.resize(usize(s) * s * d.layer_count);
        for (u32 z = 0; z < s; ++z)
            for (u32 x = 0; x < s; ++x)
                for (u32 l = 0; l < d.layer_count; ++l) d.weights[(usize(z) * s + x) * d.layer_count + l] = v.weight(x, z, l);
    }
    // Every key the brushes do not own, as written.
    d.splat_explicit_ = root.get("splat").valid();
    for (u32 i = 0; i < root.size(); ++i) {
        const StringView key = root.key_at(i);
        if (key == "heights" || key == "fill" || key == "holes" || key == "splat") continue;
        d.kept_ += "  ";
        cook::append_json_string(d.kept_, key);
        d.kept_ += ": ";
        cook::write_json(root.at(i), d.kept_, false);
        d.kept_ += ",\n";
    }
    *this = static_cast<TerrainDocument&&>(d);
    return true;
}

void TerrainDocument::serialize(cook::String& out) const {
    out += "{\n";
    out += kept_.view();
    Array<u8> bytes;
    bytes.resize(heights.size() * 2);
    const f32 span = height_max - height_min;
    for (usize i = 0; i < heights.size(); ++i) {
        const f32 t = math::clamp((heights[i] - height_min) / span, 0.0f, 1.0f);
        const u32 q = static_cast<u32>(std::lround(t * 65535.0f));
        bytes[i * 2] = static_cast<u8>(q & 0xFF);
        bytes[i * 2 + 1] = static_cast<u8>(q >> 8);
    }
    out += "  \"heights\": \"";
    base64_append(bytes.span(), out);
    out += "\"";
    if (splat_explicit_ && layer_count > 0) {
        out += ",\n  \"splat\": \"";
        base64_append(weights.span(), out);
        out += "\"";
    }
    bool any_hole = false;
    for (u8 b : holes) any_hole = any_hole || b != 0;
    if (any_hole) {
        out += ",\n  \"holes\": \"";
        base64_append(holes.span(), out);
        out += "\"";
    }
    out += "\n}\n";
}

math::vec2 TerrainDocument::spacing() const {
    return {size.x / static_cast<f32>(resolution - 1), size.y / static_cast<f32>(resolution - 1)};
}

bool TerrainDocument::hole(u32 x, u32 z) const {
    const usize i = usize(z) * resolution + x;
    return (holes[i / 8] >> (i % 8)) & 1u;
}

f32 TerrainDocument::height_at(f32 x, f32 z) const {
    const f32 last = static_cast<f32>(resolution - 1);
    const f32 fx = math::clamp((x / size.x + 0.5f) * last, 0.0f, last), fz = math::clamp((z / size.y + 0.5f) * last, 0.0f, last);
    const u32 x0 = std::min(static_cast<u32>(fx), resolution - 2), z0 = std::min(static_cast<u32>(fz), resolution - 2);
    const f32 u = fx - static_cast<f32>(x0), w = fz - static_cast<f32>(z0);
    const f32 a = height(x0, z0) + (height(x0 + 1, z0) - height(x0, z0)) * u;
    const f32 b = height(x0, z0 + 1) + (height(x0 + 1, z0 + 1) - height(x0, z0 + 1)) * u;
    return a + (b - a) * w;
}

u32 TerrainDocument::apply(const TerrainBrush& b, math::vec2 c, f32 scale) {
    if (!loaded() || b.radius <= 0.0f) return 0;
    // The sample box under the brush on a grid of `res` samples over the terrain.
    auto box = [&](u32 res, u32& x0, u32& x1, u32& z0, u32& z1) {
        const f32 last = static_cast<f32>(res - 1);
        auto lo = [&](f32 v, f32 extent) { return static_cast<u32>(math::clamp(std::floor((v / extent + 0.5f) * last), 0.0f, last)); };
        auto hi = [&](f32 v, f32 extent) { return static_cast<u32>(math::clamp(std::ceil((v / extent + 0.5f) * last), 0.0f, last)); };
        x0 = lo(c.x - b.radius, size.x);
        x1 = hi(c.x + b.radius, size.x);
        z0 = lo(c.y - b.radius, size.y);
        z1 = hi(c.y + b.radius, size.y);
        return c.x + b.radius >= -0.5f * size.x && c.x - b.radius <= 0.5f * size.x && c.y + b.radius >= -0.5f * size.y &&
               c.y - b.radius <= 0.5f * size.y;
    };
    auto pos = [&](u32 res, u32 x, u32 z) {
        const f32 last = static_cast<f32>(res - 1);
        return math::vec2{(static_cast<f32>(x) / last - 0.5f) * size.x, (static_cast<f32>(z) / last - 0.5f) * size.y};
    };
    u32 x0, x1, z0, z1;
    u32 changed = 0;
    if (b.op == TerrainBrushOp::Paint) {
        if (layer_count == 0 || b.layer >= layer_count || !box(splat_resolution, x0, x1, z0, z1)) return 0;
        const f32 amount = math::clamp(b.strength * scale, 0.0f, 1.0f);
        for (u32 z = z0; z <= z1; ++z)
            for (u32 x = x0; x <= x1; ++x) {
                const f32 w = terrain_brush_falloff(b, math::length(pos(splat_resolution, x, z) - c)) * amount;
                if (w <= 0.0f) continue;
                u8* s = &weights[(usize(z) * splat_resolution + x) * layer_count];
                u32 others = 0;
                for (u32 l = 0; l < layer_count; ++l)
                    if (l != b.layer) others += s[l];
                const f32 target = static_cast<f32>(s[b.layer]) + (255.0f - static_cast<f32>(s[b.layer])) * w;
                const f32 keep = others > 0 ? (255.0f - target) / static_cast<f32>(others) : 0.0f;
                u32 rest = 0;
                bool diff = false;
                for (u32 l = 0; l < layer_count; ++l) {
                    if (l == b.layer) continue;
                    const u8 nv = static_cast<u8>(std::min(255.0f, std::floor(static_cast<f32>(s[l]) * keep + 0.5f)));
                    diff = diff || nv != s[l];
                    s[l] = nv;
                    rest += nv;
                }
                const u8 own = static_cast<u8>(rest >= 255 ? 0 : 255 - rest);
                diff = diff || own != s[b.layer];
                s[b.layer] = own;
                changed += diff;
            }
        splat_explicit_ = true;
        return changed;
    }
    if (!box(resolution, x0, x1, z0, z1)) return 0;
    if (b.op == TerrainBrushOp::Hole || b.op == TerrainBrushOp::Fill) {
        const bool set = b.op == TerrainBrushOp::Hole;
        for (u32 z = z0; z <= z1; ++z)
            for (u32 x = x0; x <= x1; ++x) {
                if (math::length(pos(resolution, x, z) - c) > b.radius) continue;
                const usize i = usize(z) * resolution + x;
                const u8 bit = static_cast<u8>(1u << (i % 8));
                const u8 before = holes[i / 8];
                holes[i / 8] = set ? static_cast<u8>(before | bit) : static_cast<u8>(before & ~bit);
                changed += holes[i / 8] != before;
            }
        return changed;
    }
    // Height ops read a copy of the box (plus a ring for Smooth) so the dab does not feed on itself.
    const u32 bx0 = x0 > 0 ? x0 - 1 : 0, bz0 = z0 > 0 ? z0 - 1 : 0;
    const u32 bx1 = std::min(x1 + 1, resolution - 1), bz1 = std::min(z1 + 1, resolution - 1);
    const u32 bw = bx1 - bx0 + 1;
    Array<f32> src;
    src.resize(usize(bw) * (bz1 - bz0 + 1));
    for (u32 z = bz0; z <= bz1; ++z)
        for (u32 x = bx0; x <= bx1; ++x) src[usize(z - bz0) * bw + (x - bx0)] = height(x, z);
    auto at = [&](u32 x, u32 z) { return src[usize(z - bz0) * bw + (x - bx0)]; };
    const f32 pull = math::clamp(b.strength * scale, 0.0f, 1.0f);
    const f32 noise_cell = std::max(b.radius * 0.5f, 1e-3f);
    for (u32 z = z0; z <= z1; ++z)
        for (u32 x = x0; x <= x1; ++x) {
            const math::vec2 p = pos(resolution, x, z);
            const f32 w = terrain_brush_falloff(b, math::length(p - c));
            if (w <= 0.0f) continue;
            const f32 h = at(x, z);
            f32 nh = h;
            switch (b.op) {
                case TerrainBrushOp::Raise: nh = h + b.strength * scale * w; break;
                case TerrainBrushOp::Lower: nh = h - b.strength * scale * w; break;
                case TerrainBrushOp::Smooth: {
                    f32 sum = 0.0f;
                    u32 count = 0;
                    for (u32 zz = (z > bz0 ? z - 1 : z); zz <= std::min(z + 1, bz1); ++zz)
                        for (u32 xx = (x > bx0 ? x - 1 : x); xx <= std::min(x + 1, bx1); ++xx) {
                            sum += at(xx, zz);
                            ++count;
                        }
                    nh = h + (sum / static_cast<f32>(count) - h) * pull * w;
                    break;
                }
                case TerrainBrushOp::Flatten: nh = h + (b.target - h) * pull * w; break;
                case TerrainBrushOp::Noise: nh = h + value_noise(p.x, p.y, noise_cell, b.seed) * b.strength * scale * w; break;
                default: break;
            }
            nh = math::clamp(nh, height_min, height_max);
            if (nh != h) {
                heights[usize(z) * resolution + x] = nh;
                ++changed;
            }
        }
    return changed;
}

bool TerrainDocument::import_heightmap(Span<const byte> file, const TerrainHeightmapImport& o, cook::String& error) {
    if (!loaded()) {
        error += "no terrain is loaded";
        return false;
    }
    u32 w = 0, h = 0;
    Array<f32> src;  // 0..1, rows top first
    if (o.png) {
        Result<cook::Image16> img = cook::decode_png_grey16(file);
        if (!img) {
            error += "the PNG does not read: ";
            error += img.error().path.view();
            return false;
        }
        w = img.value().width;
        h = img.value().height;
        src.resize(usize(w) * h);
        for (usize i = 0; i < src.size(); ++i) src[i] = static_cast<f32>(img.value().grey[i]) / 65535.0f;
    } else {
        if (o.raw_bits != 8 && o.raw_bits != 16) {
            error += "raw_bits must be 8 or 16";
            return false;
        }
        const usize bytes = o.raw_bits / 8, n = file.size() / bytes;
        if (n == 0 || n * bytes != file.size()) {
            error += "the raw file's size is not a whole number of samples";
            return false;
        }
        w = o.raw_width;
        if (w == 0) {
            w = static_cast<u32>(std::lround(std::sqrt(static_cast<f64>(n))));
            if (usize(w) * w != n) {
                error += "the raw file is not square: give raw_width";
                return false;
            }
        }
        if (n % w != 0) {
            error += "the raw file's samples are not a whole number of raw_width rows";
            return false;
        }
        h = static_cast<u32>(n / w);
        src.resize(n);
        for (usize i = 0; i < n; ++i)
            src[i] = bytes == 1 ? static_cast<f32>(u8(file[i])) / 255.0f
                                : static_cast<f32>(u32(u8(file[2 * i])) | (u32(u8(file[2 * i + 1])) << 8)) / 65535.0f;
    }
    if (w < 2 || h < 2) {
        error += "the heightmap needs at least 2 x 2 samples";
        return false;
    }
    const f32 lo = o.low == o.high ? height_min : o.low, hi = o.low == o.high ? height_max : o.high;
    const u32 n = resolution;
    for (u32 z = 0; z < n; ++z)
        for (u32 x = 0; x < n; ++x) {
            const f32 fx = static_cast<f32>(x) / static_cast<f32>(n - 1) * static_cast<f32>(w - 1);
            const f32 fz = static_cast<f32>(z) / static_cast<f32>(n - 1) * static_cast<f32>(h - 1);
            const u32 x0 = std::min(static_cast<u32>(fx), w - 2), z0 = std::min(static_cast<u32>(fz), h - 2);
            const f32 u = fx - static_cast<f32>(x0), v = fz - static_cast<f32>(z0);
            const f32* r0 = src.data() + usize(z0) * w;
            const f32* r1 = r0 + w;
            const f32 a = r0[x0] + (r0[x0 + 1] - r0[x0]) * u, b = r1[x0] + (r1[x0 + 1] - r1[x0]) * u;
            heights[usize(z) * n + x] = math::clamp(lo + (a + (b - a) * v) * (hi - lo), height_min, height_max);
        }
    return true;
}

void TerrainDocument::generate(const TerrainGenerate& g) {
    if (!loaded()) return;
    const u32 n = resolution;
    const math::vec2 sp = spacing();
    const f32 feature = g.feature > 0.0f ? g.feature : 0.25f * std::max(size.x, size.y);
    const u32 octaves = std::clamp(g.octaves, 1u, 12u);
    const f32 ridged = math::clamp(g.ridged, 0.0f, 1.0f);
    // fBm in [-1, 1], then stretched over the range [low, high] of the heights.
    Array<f32> t;
    t.resize(usize(n) * n);
    f32 tmin = 1e30f, tmax = -1e30f;
    for (u32 z = 0; z < n; ++z)
        for (u32 x = 0; x < n; ++x) {
            const f32 px = -0.5f * size.x + static_cast<f32>(x) * sp.x, pz = -0.5f * size.y + static_cast<f32>(z) * sp.y;
            f32 sum = 0.0f, amp = 1.0f, total = 0.0f, cell = feature;
            for (u32 o = 0; o < octaves; ++o) {
                const f32 v = value_noise(px, pz, std::max(cell, 1e-3f), g.seed * 977u + o);
                sum += amp * ((1.0f - ridged) * v + ridged * (1.0f - 2.0f * std::fabs(v)));
                total += amp;
                amp *= g.gain;
                cell /= std::max(g.lacunarity, 1.01f);
            }
            const f32 v = sum / total;
            t[usize(z) * n + x] = v;
            tmin = std::min(tmin, v);
            tmax = std::max(tmax, v);
        }
    const f32 span = height_max - height_min;
    const f32 lo = math::clamp(g.low, 0.0f, 1.0f), hi = math::clamp(g.high, 0.0f, 1.0f);
    const f32 island = math::clamp(g.island, 0.0f, 1.0f);
    for (u32 z = 0; z < n; ++z)
        for (u32 x = 0; x < n; ++x) {
            f32 v = tmax > tmin ? (t[usize(z) * n + x] - tmin) / (tmax - tmin) : 0.5f;
            if (island > 0.0f) {
                // 1 inside, falling to 0 over the outer `island` share of the way to the edge.
                const f32 dx = static_cast<f32>(x) / static_cast<f32>(n - 1) * 2.0f - 1.0f;
                const f32 dz = static_cast<f32>(z) / static_cast<f32>(n - 1) * 2.0f - 1.0f;
                const f32 d = std::sqrt(dx * dx + dz * dz);
                const f32 e = math::clamp((d - (1.0f - island)) / island, 0.0f, 1.0f);
                v *= 1.0f - e * e * (3.0f - 2.0f * e);
            }
            heights[usize(z) * n + x] = height_min + (lo + v * (hi - lo)) * span;
        }

    // Thermal: where a neighbour is lower by more than the talus step, half the excess moves down
    // (shared out among the steep neighbours), so slopes settle toward talus_deg.
    const f32 talus = std::tan(math::radians(math::clamp(g.talus_deg, 1.0f, 89.0f)));
    const f32 step_x = talus * sp.x, step_z = talus * sp.y;
    Array<f32> delta;
    for (u32 pass = 0; pass < g.thermal; ++pass) {
        delta.clear();
        delta.resize(heights.size(), 0.0f);
        for (u32 z = 0; z < n; ++z)
            for (u32 x = 0; x < n; ++x) {
                const usize i = usize(z) * n + x;
                const f32 hgt = heights[i];
                usize nb[4];
                f32 ex[4];
                u32 k = 0;
                f32 excess = 0.0f, most = 0.0f;
                const auto consider = [&](usize j, f32 limit) {
                    const f32 d = hgt - heights[j] - limit;
                    if (d <= 0.0f) return;
                    nb[k] = j;
                    ex[k++] = d;
                    excess += d;
                    most = std::max(most, d);
                };
                if (x > 0) consider(i - 1, step_x);
                if (x + 1 < n) consider(i + 1, step_x);
                if (z > 0) consider(i - n, step_z);
                if (z + 1 < n) consider(i + n, step_z);
                if (k == 0) continue;
                const f32 moved = 0.25f * most;
                delta[i] -= moved;
                for (u32 j = 0; j < k; ++j) delta[nb[j]] += moved * ex[j] / excess;
            }
        for (usize i = 0; i < heights.size(); ++i) heights[i] += delta[i];
    }

    // Droplets: each starts at a random sample, runs downhill with some inertia, picks up silt
    // where it speeds up (up to a capacity set by its speed and the drop) and leaves it where it
    // slows or climbs. The 4 samples around it share each change by their bilinear weights.
    const f32 cell = 0.5f * (sp.x + sp.y);
    for (u32 d = 0; d < g.droplets; ++d) {
        f32 px = lattice(static_cast<i32>(d), 17, g.seed) * static_cast<f32>(n - 1);
        f32 pz = lattice(static_cast<i32>(d), 31, g.seed) * static_cast<f32>(n - 1);
        f32 dx = 0.0f, dz = 0.0f, speed = 1.0f, water = 1.0f, silt = 0.0f;
        for (u32 life = 0; life < 64; ++life) {
            const u32 x0 = std::min(static_cast<u32>(px), n - 2), z0 = std::min(static_cast<u32>(pz), n - 2);
            const f32 u = px - static_cast<f32>(x0), v = pz - static_cast<f32>(z0);
            const usize i00 = usize(z0) * n + x0, i10 = i00 + 1, i01 = i00 + n, i11 = i01 + 1;
            const f32 h00 = heights[i00], h10 = heights[i10], h01 = heights[i01], h11 = heights[i11];
            const f32 here = (h00 + (h10 - h00) * u) * (1.0f - v) + (h01 + (h11 - h01) * u) * v;
            const f32 gx = (h10 - h00) * (1.0f - v) + (h11 - h01) * v, gz = (h01 - h00) * (1.0f - u) + (h11 - h10) * u;
            dx = dx * 0.3f - gx * 0.7f;
            dz = dz * 0.3f - gz * 0.7f;
            const f32 len = std::sqrt(dx * dx + dz * dz);
            if (len < 1e-6f) break;
            dx /= len;
            dz /= len;
            const f32 nx = px + dx, nz = pz + dz;
            if (nx < 0.0f || nz < 0.0f || nx > static_cast<f32>(n - 1) || nz > static_cast<f32>(n - 1)) break;
            const u32 bx = std::min(static_cast<u32>(nx), n - 2), bz = std::min(static_cast<u32>(nz), n - 2);
            const f32 bu = nx - static_cast<f32>(bx), bv = nz - static_cast<f32>(bz);
            const usize j00 = usize(bz) * n + bx;
            const f32 there = (heights[j00] + (heights[j00 + 1] - heights[j00]) * bu) * (1.0f - bv) +
                              (heights[j00 + n] + (heights[j00 + n + 1] - heights[j00 + n]) * bu) * bv;
            const f32 drop = here - there;  // > 0 downhill
            const f32 capacity = std::max(drop / cell, 0.01f) * speed * water * 0.5f * cell;
            f32 change;  // > 0 deposits
            if (drop < 0.0f) change = std::min(-drop, silt);
            else if (silt > capacity) change = 0.3f * (silt - capacity);
            else change = -std::min(0.3f * (capacity - silt), drop);
            silt -= change;
            heights[i00] += change * (1.0f - u) * (1.0f - v);
            heights[i10] += change * u * (1.0f - v);
            heights[i01] += change * (1.0f - u) * v;
            heights[i11] += change * u * v;
            speed = std::sqrt(std::max(0.0f, speed * speed + drop / cell * 4.0f));
            water *= 0.98f;
            px = nx;
            pz = nz;
        }
        // What the droplet still carries settles where it stopped.
        const u32 sx = std::min(static_cast<u32>(px), n - 2), sz = std::min(static_cast<u32>(pz), n - 2);
        const f32 su = px - static_cast<f32>(sx), sv = pz - static_cast<f32>(sz);
        const usize s00 = usize(sz) * n + sx;
        heights[s00] += silt * (1.0f - su) * (1.0f - sv);
        heights[s00 + 1] += silt * su * (1.0f - sv);
        heights[s00 + n] += silt * (1.0f - su) * sv;
        heights[s00 + n + 1] += silt * su * sv;
    }
    for (f32& hgt : heights) hgt = math::clamp(hgt, height_min, height_max);
}

// ---- TerrainEditor ----

bool TerrainEditor::bind(EditorSession& session, EntityId entity) {
    const kit::Terrain3D* t = entity ? session.scene().get<kit::Terrain3D>(entity) : nullptr;
    const ProjectAsset* a = t && !t->terrain.guid.is_null() ? session.project().find(t->terrain.guid) : nullptr;
    if (!a || a->type != asset::ChunkType::Terrain) {
        unbind();
        return false;
    }
    if (entity == entity_ && t->terrain.guid == terrain_ && bound()) return true;
    asset::AssetSystem* assets = session.project().mounted();
    const asset::AssetKey key = assets ? assets->find(t->terrain.guid) : asset::kInvalidAssetKey;
    if (key == asset::kInvalidAssetKey) {
        unbind();
        return false;
    }
    Result<asset::AssetHandle<asset::Terrain>> h = assets->load_sync<asset::Terrain>(key);
    if (!h) {
        unbind();
        return false;
    }
    const cook::String path = session.project().asset_path(a->path.view());
    cook::String text;
    cook::DiagList diags;
    TerrainDocument d;
    const bool ok = cook::fs::read_text(path.view(), text) && d.load(text.view(), assets->resolve(h.value()), a->path.view(), diags);
    assets->release(h.value());
    if (!ok) {
        MCE_LOG_WARN("editor: terrain {} cannot be read", a->path.view());
        unbind();
        return false;
    }
    unbind();
    entity_ = entity;
    terrain_ = t->terrain.guid;
    path_ = path;
    doc_ = static_cast<TerrainDocument&&>(d);
    return true;
}

void TerrainEditor::unbind() {
    entity_ = kNoEntity;
    terrain_ = {};
    path_ = cook::String();
    doc_ = TerrainDocument{};
    stroking_ = changed_ = dirty_ = false;
    undo_.clear();
    redo_.clear();
}

bool TerrainEditor::raycast(const EditorScene& scene, math::vec3 origin, math::vec3 dir, math::vec3& world_hit,
                            math::vec2& local_hit) const {
    if (!bound()) return false;
    const WorldTransform wt = scene.world_transform(entity_);
    const math::quat inv = math::conjugate(wt.rotation);
    auto safe = [](f32 s) { return std::fabs(s) > 1e-6f ? s : 1e-6f; };
    const math::vec3 sc{safe(wt.scale.x), safe(wt.scale.y), safe(wt.scale.z)};
    const math::vec3 o = math::rotate(inv, origin - wt.position) / sc;
    const math::vec3 d = math::rotate(inv, dir) / sc;
    // The ray's span inside the terrain's box.
    const math::vec3 lo{-0.5f * doc_.size.x, doc_.height_min, -0.5f * doc_.size.y}, hi{0.5f * doc_.size.x, doc_.height_max, 0.5f * doc_.size.y};
    f32 t0 = 0.0f, t1 = 1e30f;
    for (int k = 0; k < 3; ++k) {
        const f32 ok = k == 0 ? o.x : (k == 1 ? o.y : o.z), dk = k == 0 ? d.x : (k == 1 ? d.y : d.z);
        const f32 lk = k == 0 ? lo.x : (k == 1 ? lo.y : lo.z), hk = k == 0 ? hi.x : (k == 1 ? hi.y : hi.z);
        if (std::fabs(dk) < 1e-12f) {
            if (ok < lk || ok > hk) return false;
            continue;
        }
        f32 a = (lk - ok) / dk, b = (hk - ok) / dk;
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
    }
    if (t0 > t1) return false;
    auto above = [&](f32 t) {
        const math::vec3 p = o + d * t;
        return p.y - doc_.height_at(p.x, p.z);
    };
    // March in steps of half a sample along the ground (or a 64th of the span for a steep ray).
    const math::vec2 sp = doc_.spacing();
    const f32 flat = std::sqrt(d.x * d.x + d.z * d.z);
    f32 step = flat > 1e-9f ? 0.5f * std::min(sp.x, sp.y) / flat : (t1 - t0);
    step = std::max(step, (t1 - t0) / 20000.0f);
    f32 prev_t = t0, prev = above(t0);
    if (prev < 0.0f) return false;  // starts under the ground
    for (f32 t = t0 + step;; t += step) {
        const bool last = t >= t1;
        if (last) t = t1;
        const f32 cur = above(t);
        if (cur <= 0.0f) {
            f32 a = prev_t, b = t;
            for (int i = 0; i < 24; ++i) {
                const f32 m = 0.5f * (a + b);
                (above(m) > 0.0f ? a : b) = m;
            }
            const math::vec3 p = o + d * b;
            local_hit = {p.x, p.z};
            world_hit = wt.position + math::rotate(wt.rotation, math::vec3{p.x * wt.scale.x, p.y * wt.scale.y, p.z * wt.scale.z});
            return true;
        }
        if (last) return false;
        prev_t = t;
        prev = cur;
    }
}

void TerrainEditor::brush_outline(const EditorScene& scene, math::vec2 local, f32 radius, Array<math::vec3>& points) const {
    points.clear();
    if (!bound()) return;
    const WorldTransform wt = scene.world_transform(entity_);
    constexpr u32 kSides = 48;
    for (u32 i = 0; i <= kSides; ++i) {
        const f32 a = static_cast<f32>(i) / static_cast<f32>(kSides) * 6.28318531f;
        const f32 x = local.x + radius * std::cos(a), z = local.y + radius * std::sin(a);
        const f32 y = doc_.height_at(x, z) + 0.05f;
        points.push_back(wt.position + math::rotate(wt.rotation, math::vec3{x * wt.scale.x, y * wt.scale.y, z * wt.scale.z}));
    }
}

TerrainEditor::Snapshot TerrainEditor::snapshot() const { return Snapshot{doc_.heights, doc_.weights, doc_.holes}; }

void TerrainEditor::restore(Snapshot& s) {
    doc_.heights = static_cast<Array<f32>&&>(s.heights);
    doc_.weights = static_cast<Array<u8>&&>(s.weights);
    doc_.holes = static_cast<Array<u8>&&>(s.holes);
}

void TerrainEditor::begin_stroke() {
    if (!bound() || stroking_) return;
    before_ = snapshot();
    stroking_ = true;
    changed_ = false;
}

u32 TerrainEditor::dab(math::vec2 local, f32 scale) {
    if (!stroking_) return 0;
    const u32 n = doc_.apply(brush, local, scale);
    if (n > 0) changed_ = dirty_ = true;
    return n;
}

Result<f64> TerrainEditor::save(EditorSession& session) {
    cook::String rel;
    session.project().relative_asset_path(path_.view(), rel);
    cook::String text;
    doc_.serialize(text);
    Result<HotReloadResult> r = session.write_asset(rel.view(), text.view());
    if (!r) return r.error();
    dirty_ = false;
    return r.value().seconds;
}

Result<f64> TerrainEditor::flush(EditorSession& session) {
    if (!dirty_) return 0.0;
    return save(session);
}

Result<f64> TerrainEditor::end_stroke(EditorSession& session) {
    if (!stroking_) return 0.0;
    stroking_ = false;
    if (!changed_) return 0.0;
    undo_.push_back(static_cast<Snapshot&&>(before_));
    before_ = Snapshot{};
    redo_.clear();
    // Bound the history: each entry is a whole terrain.
    const usize entry = undo_.back().bytes();
    while (undo_.size() > 1 && undo_.size() * entry > kMaxHistoryBytes) undo_.erase(0);
    if (!dirty_) return 0.0;  // flushed already
    return save(session);
}

bool TerrainEditor::undo(EditorSession& session) {
    if (stroking_ || undo_.empty()) return false;
    redo_.push_back(snapshot());
    restore(undo_.back());
    undo_.pop_back();
    return static_cast<bool>(save(session));
}

bool TerrainEditor::redo(EditorSession& session) {
    if (stroking_ || redo_.empty()) return false;
    undo_.push_back(snapshot());
    restore(redo_.back());
    redo_.pop_back();
    return static_cast<bool>(save(session));
}

}  // namespace mce::editor

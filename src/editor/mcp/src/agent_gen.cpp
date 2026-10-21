#include "agent_gen.h"

#include <cmath>
#include <cstring>

namespace mce::mcp::gen {

namespace {

constexpr f32 kPi = 3.14159265358979f;

f32 clamp01(f32 v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
f32 fract(f32 v) { return v - std::floor(v); }
f32 smooth(f32 t) { return t * t * (3.0f - 2.0f * t); }
f32 lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

u32 hash3(u64 seed, i32 x, i32 y, i32 z = 0) {
    u64 h = seed ^ (static_cast<u64>(static_cast<u32>(x)) * 0x9E3779B97F4A7C15ull);
    h ^= static_cast<u64>(static_cast<u32>(y)) * 0xC2B2AE3D27D4EB4Full;
    h ^= static_cast<u64>(static_cast<u32>(z)) * 0x165667B19E3779F9ull;
    h ^= h >> 31;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 29;
    return static_cast<u32>(h >> 32);
}
f32 hashf(u64 seed, i32 x, i32 y, i32 z = 0) { return static_cast<f32>(hash3(seed, x, y, z) >> 8) / static_cast<f32>(1u << 24); }

i32 wrap(i32 v, i32 period) {
    if (period <= 0) return v;
    const i32 m = v % period;
    return m < 0 ? m + period : m;
}

void set_px(cook::Image& img, u32 x, u32 y, u32 rgba) {
    u8* p = &img.rgba[(usize(y) * img.width + x) * 4];
    p[0] = static_cast<u8>(rgba >> 24);
    p[1] = static_cast<u8>(rgba >> 16);
    p[2] = static_cast<u8>(rgba >> 8);
    p[3] = static_cast<u8>(rgba);
}

cook::Image blank(u32 w, u32 h) {
    cook::Image img;
    img.width = w;
    img.height = h;
    img.rgba.resize(usize(w) * h * 4);
    for (u8& b : img.rgba) b = 0;
    return img;
}

}  // namespace

u64 Rng::next() {
    s += 0x9E3779B97F4A7C15ull;
    u64 z = s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
f32 Rng::unit() { return static_cast<f32>(next() >> 40) / static_cast<f32>(1u << 24); }
i32 Rng::range_i(i32 lo, i32 hi) { return lo + static_cast<i32>(next() % static_cast<u64>(hi - lo + 1)); }

// ---------------------------------------------------------------- color

Rgb hsv(f32 h, f32 s, f32 v) {
    h = fract(h) * 6.0f;
    const i32 i = static_cast<i32>(h);
    const f32 f = h - static_cast<f32>(i);
    const f32 p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (i) {
        case 0: return {v, t, p};
        case 1: return {q, v, p};
        case 2: return {p, v, t};
        case 3: return {p, q, v};
        case 4: return {t, p, v};
        default: return {v, p, q};
    }
}

u32 pack(Rgb c, u8 a) {
    auto ch = [](f32 v) { return static_cast<u32>(std::lround(clamp01(v) * 255.0f)); };
    return (ch(c.r) << 24) | (ch(c.g) << 16) | (ch(c.b) << 8) | a;
}

Rgb unpack(u32 rgba) {
    return {static_cast<f32>((rgba >> 24) & 255) / 255.0f, static_cast<f32>((rgba >> 16) & 255) / 255.0f,
            static_cast<f32>((rgba >> 8) & 255) / 255.0f};
}

Rgb mix(Rgb a, Rgb b, f32 t) { return {lerp(a.r, b.r, t), lerp(a.g, b.g, t), lerp(a.b, b.b, t)}; }

bool parse_hex(StringView s, u32& rgba) {
    if (!s.empty() && s[0] == '#') s = s.substr(1);
    if (s.size() != 6 && s.size() != 8) return false;
    u32 v = 0;
    for (const char c : s) {
        u32 d;
        if (c >= '0' && c <= '9') d = static_cast<u32>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<u32>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = static_cast<u32>(c - 'A' + 10);
        else return false;
        v = v * 16 + d;
    }
    rgba = s.size() == 6 ? (v << 8) | 0xFF : v;
    return true;
}

void to_hex(u32 rgba, cook::String& out) { cook::appendf(out, "#%06x", rgba >> 8); }

constexpr const char* kSchemes[] = {"analogous", "complementary", "triad", "pastel", "earth", "neon", "mono"};

bool palette_scheme_known(StringView scheme) {
    for (const char* s : kSchemes)
        if (scheme == s) return true;
    return false;
}

Array<u32> palette(u64 seed, u32 count, StringView scheme) {
    Rng rng(seed);
    const f32 base = rng.unit();
    Array<u32> out;
    for (u32 i = 0; i < count; ++i) {
        const f32 t = count > 1 ? static_cast<f32>(i) / static_cast<f32>(count - 1) : 0.5f;  // dark -> light
        f32 h = base, s = 0.6f, v = lerp(0.18f, 0.95f, t);
        if (scheme == "analogous") {
            h = base + (t - 0.5f) * 0.18f;
            s = lerp(0.75f, 0.45f, t);
        } else if (scheme == "complementary") {
            h = base + (i % 2 ? 0.5f : 0.0f);
            s = lerp(0.8f, 0.5f, t);
        } else if (scheme == "triad") {
            h = base + static_cast<f32>(i % 3) / 3.0f;
            s = lerp(0.75f, 0.5f, t);
        } else if (scheme == "pastel") {
            h = base + static_cast<f32>(i) * 0.61803f;
            s = 0.32f;
            v = lerp(0.72f, 0.98f, t);
        } else if (scheme == "earth") {
            h = 0.05f + base * 0.12f + (t - 0.5f) * 0.08f;
            s = lerp(0.55f, 0.3f, t);
            v = lerp(0.2f, 0.85f, t);
        } else if (scheme == "neon") {
            h = base + static_cast<f32>(i) * 0.61803f;
            s = i == 0 ? 0.6f : 0.95f;
            v = i == 0 ? 0.12f : lerp(0.8f, 1.0f, t);
        } else {  // mono
            s = lerp(0.5f, 0.15f, t);
        }
        h += rng.range(-0.015f, 0.015f);
        out.push_back(pack(hsv(h, clamp01(s), clamp01(v))));
    }
    return out;
}

// ---------------------------------------------------------------- pixel sprites

constexpr const char* kSpriteKinds[] = {"creature", "character", "ship", "item", "blob"};

bool sprite_kind_known(StringView kind) {
    for (const char* k : kSpriteKinds)
        if (kind == k) return true;
    return false;
}

cook::Image pixel_sprite(u64 seed, StringView kind, u32 w, u32 h, Span<const u32> colors) {
    Rng rng(seed);
    // 0 empty, 1 body. The left half (with the center column) is drawn, then mirrored.
    Array<u8> cell;
    cell.resize(usize(w) * h);
    for (u8& c : cell) c = 0;
    const u32 half = (w + 1) / 2;
    const f32 limb = rng.range(0.15f, 0.35f);
    const f32 head = rng.range(0.32f, 0.48f);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < half; ++x) {
            const f32 dx = static_cast<f32>(half - 1 - x) / static_cast<f32>(half);  // 0 at the center
            const f32 ny = h > 1 ? static_cast<f32>(y) / static_cast<f32>(h - 1) : 0.5f;
            f32 p = 0.03f;
            if (kind == "creature" || kind == "blob") {
                const f32 top = kind == "blob" ? 0.25f : 0.1f;
                const f32 cy = (top + 1.0f) * 0.5f, ry = (1.0f - top) * 0.5f;
                const f32 e = (dx / 0.9f) * (dx / 0.9f) + ((ny - cy) / ry) * ((ny - cy) / ry);
                p = e < 1.0f ? 0.95f - 0.45f * e : 0.03f;
                if (kind == "creature" && ny > 0.8f && dx > 0.25f && dx < 0.25f + limb) p = 0.85f;  // legs
                if (kind == "creature" && ny < 0.25f && dx > 0.3f && dx < 0.55f) p = 0.6f;          // ears or horns
            } else if (kind == "character") {
                if (ny < 0.38f) {
                    const f32 e = (dx / head) * (dx / head) + ((ny - 0.2f) / 0.2f) * ((ny - 0.2f) / 0.2f);
                    p = e < 1.0f ? 0.95f : 0.02f;
                } else if (ny < 0.72f) {
                    p = dx < 0.5f ? 0.95f : (dx < 0.5f + limb ? 0.55f : 0.02f);  // torso, arms
                } else {
                    p = dx > 0.12f && dx < 0.45f ? 0.92f : 0.02f;  // legs
                }
            } else if (kind == "ship") {
                const f32 width = (0.2f + 0.8f * ny) * 0.95f;
                p = dx < width ? 0.8f - 0.3f * (dx / (width + 0.01f)) : 0.03f;
                if (ny > 0.65f && dx > width * 0.6f && dx < width + 0.25f) p = 0.75f;  // wings
            } else {  // item
                const f32 d = dx + std::fabs(ny - 0.5f) * 1.25f;
                p = d < 0.62f ? 0.95f : (d < 0.75f ? 0.4f : 0.02f);
            }
            if (rng.unit() < p) cell[usize(y) * w + x] = 1;
        }
    }
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < half; ++x) cell[usize(y) * w + (w - 1 - x)] = cell[usize(y) * w + x];

    auto at = [&](i32 x, i32 y) -> u8 {
        if (x < 0 || y < 0 || x >= static_cast<i32>(w) || y >= static_cast<i32>(h)) return 0;
        return cell[usize(y) * w + static_cast<usize>(x)];
    };
    // Speckle off: a body cell with fewer than two body neighbours goes (twice, so thin strands
    // shrink back to the shape). Symmetric, so the mirror holds.
    for (u32 pass = 0; pass < 2; ++pass) {
        Array<u8> keep;
        keep.resize(cell.size());
        for (u32 y = 0; y < h; ++y) {
            for (u32 x = 0; x < w; ++x) {
                const i32 ix = static_cast<i32>(x), iy = static_cast<i32>(y);
                const u32 nb = u32(at(ix - 1, iy)) + at(ix + 1, iy) + at(ix, iy - 1) + at(ix, iy + 1);
                keep[usize(y) * w + x] = at(ix, iy) && nb >= 2 ? 1 : 0;
            }
        }
        cell = static_cast<Array<u8>&&>(keep);
    }
    const usize n = colors.size();
    cook::Image img = blank(w, h);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const i32 ix = static_cast<i32>(x), iy = static_cast<i32>(y);
            if (!at(ix, iy)) {
                if (at(ix - 1, iy) || at(ix + 1, iy) || at(ix, iy - 1) || at(ix, iy + 1)) set_px(img, x, y, colors[0] | 0xFF);
                continue;
            }
            // Shade: lighter at the top, a highlight where the top-left is open, darker bottom-right.
            const f32 ny = h > 1 ? static_cast<f32>(y) / static_cast<f32>(h - 1) : 0.5f;
            f32 t = 1.0f - ny * 0.7f;
            if (!at(ix - 1, iy - 1)) t += 0.25f;
            if (!at(ix + 1, iy + 1)) t -= 0.25f;
            const usize idx = 1 + static_cast<usize>(std::lround(clamp01(t) * static_cast<f32>(n - 2)));
            set_px(img, x, y, colors[idx < n ? idx : n - 1] | 0xFF);
        }
    }
    // Eyes for living things.
    if (kind == "creature" || kind == "character" || kind == "blob") {
        const u32 ey = kind == "character" ? static_cast<u32>(static_cast<f32>(h) * 0.18f) : static_cast<u32>(static_cast<f32>(h) * 0.42f);
        const u32 ex = half >= 3 ? half - 3 + (w % 2 ? 1 : 0) : 0;
        if (ey < h && ex < w && at(static_cast<i32>(ex), static_cast<i32>(ey))) {
            set_px(img, ex, ey, colors[0] | 0xFF);
            set_px(img, w - 1 - ex, ey, colors[0] | 0xFF);
        }
    }
    return img;
}

// ---------------------------------------------------------------- tile sets

struct TileStyle {
    const char* name;
    u32 fill[4];     // dark -> light
    u32 surface[3];  // the top layer (grass, snow, ...); 0: none
};
constexpr TileStyle kTileStyles[] = {
    {"grass", {0x4a2f1fFF, 0x6b4428FF, 0x8a5a35FF, 0xa06d42FF}, {0x2f6b2aFF, 0x4f9a36FF, 0x7cc44aFF}},
    {"stone", {0x3a3d45FF, 0x585c66FF, 0x7a7f8aFF, 0x9da2acFF}, {0, 0, 0}},
    {"brick", {0x5a2318FF, 0x8a3a26FF, 0xa8513aFF, 0xc9b9a8FF}, {0, 0, 0}},
    {"dirt", {0x3e2716FF, 0x5c3a21FF, 0x7a512eFF, 0x936a40FF}, {0, 0, 0}},
    {"sand", {0x9c7a45FF, 0xc29c5cFF, 0xdcbc7aFF, 0xeed59cFF}, {0, 0, 0}},
    {"snow", {0x4a3b35FF, 0x6a5650FF, 0x8a756cFF, 0xa0908aFF}, {0xa8c4dcFF, 0xd8e8f4FF, 0xffffffFF}},
    {"water", {0x123a6bFF, 0x1d5a9bFF, 0x2f7fc4FF, 0x6fb5e8FF}, {0, 0, 0}},
    {"wood", {0x4a2c16FF, 0x6e4322FF, 0x8f5a2fFF, 0xb07a45FF}, {0, 0, 0}},
    {"metal", {0x30353dFF, 0x4f5662FF, 0x737c8aFF, 0xa9b2c0FF}, {0, 0, 0}},
};

bool tile_style_known(StringView style) {
    for (const TileStyle& s : kTileStyles)
        if (style == s.name) return true;
    return false;
}

namespace {

// The fill pattern's palette index (0..3) at a pixel of a tile, `variant` 0..2.
u32 fill_index(StringView style, u64 seed, i32 x, i32 y, i32 t, i32 variant) {
    const f32 n = hashf(seed, x, y, variant);
    const f32 fx = static_cast<f32>(x) / static_cast<f32>(t), fy = static_cast<f32>(y) / static_cast<f32>(t);
    if (style == "brick") {
        const i32 row = y / (t / 4 > 0 ? t / 4 : 1);
        const i32 off = (row % 2) * (t / 4);
        const bool mortar = y % (t / 4 > 0 ? t / 4 : 1) == 0 || wrap(x + off, t / 2 > 0 ? t / 2 : 1) == 0;
        if (mortar) return 3;
        return n < 0.15f ? 0 : (n < 0.85f ? 1 : 2);
    }
    if (style == "wood") {
        const f32 grain = std::sin((fy * 6.0f + std::sin(fx * 3.0f + static_cast<f32>(variant)) * 0.4f) * kPi * 2.0f);
        if (y % (t / 2 > 0 ? t / 2 : 1) == 0) return 0;
        return grain > 0.6f ? 2 : (n < 0.08f ? 0 : 1);
    }
    if (style == "metal") {
        const bool edge = x == 0 || y == 0 || x == t - 1 || y == t - 1;
        const bool rivet = (x == 2 || x == t - 3) && (y == 2 || y == t - 3);
        if (rivet) return 3;
        if (edge) return 0;
        return fy + fx * 0.3f < 0.5f ? 2 : 1;
    }
    if (style == "water") {
        const f32 w = std::sin((fx * 2.0f + fy * 0.5f + static_cast<f32>(variant) * 0.33f) * kPi * 2.0f);
        return w > 0.85f ? 3 : (fy < 0.5f ? 2 : 1);
    }
    if (style == "stone") {
        // Cobbles: distance to hashed cell centres on a 2x2 grid.
        const i32 cells = 2;
        f32 best = 9.0f, second = 9.0f;
        for (i32 cy = -1; cy <= cells; ++cy)
            for (i32 cx = -1; cx <= cells; ++cx) {
                const f32 px = (static_cast<f32>(cx) + hashf(seed, wrap(cx, cells), wrap(cy, cells), 7 + variant)) / static_cast<f32>(cells);
                const f32 py = (static_cast<f32>(cy) + hashf(seed, wrap(cx, cells), wrap(cy, cells), 11 + variant)) / static_cast<f32>(cells);
                const f32 d = std::sqrt((fx - px) * (fx - px) + (fy - py) * (fy - py));
                if (d < best) {
                    second = best;
                    best = d;
                } else if (d < second) {
                    second = d;
                }
            }
        if (second - best < 0.06f) return 0;
        return best < 0.12f ? 3 : (n < 0.3f ? 1 : 2);
    }
    // grass, dirt, sand, snow: speckled.
    return n < 0.12f ? 0 : (n < 0.7f ? 1 : (n < 0.93f ? 2 : 3));
}

}  // namespace

cook::Image tileset(u64 seed, StringView style, u32 tile, Span<const u32> colors) {
    const TileStyle* st = &kTileStyles[0];
    for (const TileStyle& s : kTileStyles)
        if (style == s.name) st = &s;
    u32 fill[4], surf[3];
    for (u32 i = 0; i < 4; ++i) fill[i] = colors.size() >= 4 ? colors[i] | 0xFF : st->fill[i];
    for (u32 i = 0; i < 3; ++i) surf[i] = colors.size() >= 7 ? colors[4 + i] | 0xFF : st->surface[i];
    const bool has_surface = surf[0] != 0;
    const i32 t = static_cast<i32>(tile);
    cook::Image img = blank(tile * kTileCount, tile);
    Rng rng(seed);
    // The surface's lower edge, one column at a time (shared by top, top_left, top_right, platform).
    Array<i32> edge;
    for (i32 x = 0; x < t; ++x) edge.push_back(t / 4 + rng.range_i(0, t / 8 > 0 ? t / 8 : 1));
    auto put = [&](u32 index, i32 x, i32 y, u32 rgba) { set_px(img, index * tile + static_cast<u32>(x), static_cast<u32>(y), rgba); };
    for (u32 index = 0; index < kTileCount; ++index) {
        for (i32 y = 0; y < t; ++y) {
            for (i32 x = 0; x < t; ++x) {
                const i32 variant = index < 3 ? static_cast<i32>(index) : 0;
                u32 c = fill[fill_index(style, seed, x, y, t, variant)];
                const StringView name = kTileNames[index];
                const bool top = name == "top" || name == "top_left" || name == "top_right" || name == "platform";
                if (top) {
                    if (has_surface && y < edge[static_cast<usize>(x)]) {
                        const f32 n = hashf(seed, x, y, 31);
                        c = surf[y == 0 ? 2 : (n < 0.3f ? 0 : 1)];
                    } else if (!has_surface && y == 0) {
                        c = fill[3];  // a lit top edge
                    } else if (!has_surface && y == 1) {
                        c = fill[2];
                    }
                    if ((name == "top_left" || name == "platform") && x == 0) c = fill[0];
                    if ((name == "top_right" || name == "platform") && x == t - 1) c = fill[0];
                    if (name == "platform" && y >= t / 2) c = 0;
                    if (name == "platform" && y == t / 2 - 1 && c) c = fill[0];
                }
                if (name == "decoration") {
                    // A tuft, a pebble or a crystal on a transparent cell.
                    c = 0;
                    const i32 cx = t / 2, base = t - 1;
                    const i32 kind = static_cast<i32>(seed % 3);
                    if (kind == 0 && y > t / 2 && std::abs(x - cx) <= (base - y) / 2 + 1 && hashf(seed, x, y, 41) < 0.7f)
                        c = has_surface ? surf[1 + (x + y) % 2] : fill[2];
                    if (kind == 1 && (x - cx) * (x - cx) * 2 + (y - base) * (y - base) * 3 < t * t / 6) c = fill[(x + y) % 2 ? 2 : 3];
                    if (kind == 2 && std::abs(x - cx) * 3 + std::abs(y - (t * 3) / 4) < t / 2) c = fill[3];
                }
                put(index, x, y, c);
            }
        }
    }
    return img;
}

// ---------------------------------------------------------------- noise textures

constexpr const char* kNoiseKinds[] = {"value", "perlin", "fbm", "ridged", "worley", "sky"};

bool noise_kind_known(StringView kind) {
    for (const char* k : kNoiseKinds)
        if (kind == k) return true;
    return false;
}

namespace {

f32 value_noise(u64 seed, f32 x, f32 y, i32 px, i32 py) {
    const i32 x0 = static_cast<i32>(std::floor(x)), y0 = static_cast<i32>(std::floor(y));
    const f32 fx = smooth(x - static_cast<f32>(x0)), fy = smooth(y - static_cast<f32>(y0));
    auto v = [&](i32 ix, i32 iy) { return hashf(seed, wrap(ix, px), wrap(iy, py)); };
    return lerp(lerp(v(x0, y0), v(x0 + 1, y0), fx), lerp(v(x0, y0 + 1), v(x0 + 1, y0 + 1), fx), fy);
}

// Gradient noise in about -1..1.
f32 perlin(u64 seed, f32 x, f32 y, i32 px, i32 py) {
    const i32 x0 = static_cast<i32>(std::floor(x)), y0 = static_cast<i32>(std::floor(y));
    const f32 fx = x - static_cast<f32>(x0), fy = y - static_cast<f32>(y0);
    auto g = [&](i32 ix, i32 iy, f32 dx, f32 dy) {
        const f32 a = hashf(seed, wrap(ix, px), wrap(iy, py), 3) * 2.0f * kPi;
        return std::cos(a) * dx + std::sin(a) * dy;
    };
    const f32 u = smooth(fx), v = smooth(fy);
    const f32 n = lerp(lerp(g(x0, y0, fx, fy), g(x0 + 1, y0, fx - 1, fy), u), lerp(g(x0, y0 + 1, fx, fy - 1), g(x0 + 1, y0 + 1, fx - 1, fy - 1), u), v);
    return n * 1.414f;
}

f32 fbm(u64 seed, f32 x, f32 y, i32 px, i32 py, u32 octaves, bool ridged) {
    f32 sum = 0, amp = 0.5f, norm = 0;
    for (u32 o = 0; o < octaves; ++o) {
        const i32 m = 1 << o;
        f32 n = perlin(seed + o * 101, x * static_cast<f32>(m), y * static_cast<f32>(m), px > 0 ? px * m : 0, py > 0 ? py * m : 0);
        if (ridged) n = 1.0f - std::fabs(n) * 2.0f;
        sum += n * amp;
        norm += amp;
        amp *= 0.5f;
    }
    return sum / norm;
}

f32 worley(u64 seed, f32 x, f32 y, i32 px, i32 py) {
    const i32 x0 = static_cast<i32>(std::floor(x)), y0 = static_cast<i32>(std::floor(y));
    f32 best = 9.0f;
    for (i32 dy = -1; dy <= 1; ++dy)
        for (i32 dx = -1; dx <= 1; ++dx) {
            const i32 cx = x0 + dx, cy = y0 + dy;
            const f32 fx = static_cast<f32>(cx) + hashf(seed, wrap(cx, px), wrap(cy, py), 5);
            const f32 fy = static_cast<f32>(cy) + hashf(seed, wrap(cx, px), wrap(cy, py), 9);
            const f32 d = (x - fx) * (x - fx) + (y - fy) * (y - fy);
            if (d < best) best = d;
        }
    return std::sqrt(best);
}

Rgb ramp(Span<const u32> colors, f32 t) {
    t = clamp01(t);
    if (colors.empty()) return {t, t, t};
    if (colors.size() == 1) return mix({0, 0, 0}, unpack(colors[0]), t);
    const f32 f = t * static_cast<f32>(colors.size() - 1);
    const usize i = static_cast<usize>(f);
    if (i + 1 >= colors.size()) return unpack(colors[colors.size() - 1]);
    return mix(unpack(colors[i]), unpack(colors[i + 1]), f - static_cast<f32>(i));
}

}  // namespace

cook::Image noise_texture(u64 seed, const NoiseDesc& d) {
    cook::Image img = blank(d.width, d.height);
    const StringView kind = d.kind.view();
    if (kind == "sky") {
        const Rgb horizon = !d.colors.empty() ? unpack(d.colors[0]) : Rgb{0.78f, 0.86f, 0.95f};
        const Rgb zenith = d.colors.size() >= 2 ? unpack(d.colors[1]) : Rgb{0.18f, 0.42f, 0.82f};
        const Rgb cloud = d.colors.size() >= 3 ? unpack(d.colors[2]) : Rgb{1.0f, 1.0f, 1.0f};
        const Rgb ground = mix(horizon, {0.12f, 0.12f, 0.14f}, 0.6f);
        const i32 period = static_cast<i32>(std::lround(d.scale * 2.0f)) > 0 ? static_cast<i32>(std::lround(d.scale * 2.0f)) : 1;
        for (u32 y = 0; y < d.height; ++y) {
            const f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(d.height);
            const f32 elev = (0.5f - v) * 2.0f;  // 1 zenith, 0 horizon, -1 nadir
            for (u32 x = 0; x < d.width; ++x) {
                const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(d.width);
                Rgb c = elev >= 0 ? mix(horizon, zenith, std::pow(elev, 0.6f)) : mix(horizon, ground, std::pow(-elev, 0.4f));
                if (elev > 0.02f) {
                    if (d.clouds > 0) {
                        // Clouds thin out toward the zenith and the horizon's haze.
                        const f32 n = fbm(seed, u * static_cast<f32>(period), v * d.scale, period, 0, d.octaves, false) * 0.5f + 0.5f;
                        const f32 cover = clamp01((n - (1.0f - d.clouds)) * 3.0f) * clamp01(elev * 6.0f) * (1.0f - elev * 0.5f);
                        c = mix(c, cloud, cover);
                    }
                    if (d.stars > 0) {
                        const f32 s = hashf(seed, static_cast<i32>(x), static_cast<i32>(y), 77);
                        if (s > 1.0f - d.stars * 0.01f) c = mix(c, {1, 1, 1}, (s - (1.0f - d.stars * 0.01f)) / (d.stars * 0.01f) * 0.7f + 0.3f);
                    }
                }
                set_px(img, x, y, pack(c));
            }
        }
        return img;
    }
    const i32 period = d.tileable ? (static_cast<i32>(std::lround(d.scale)) > 0 ? static_cast<i32>(std::lround(d.scale)) : 1) : 0;
    const f32 scale = d.tileable ? static_cast<f32>(period) : d.scale;
    for (u32 y = 0; y < d.height; ++y) {
        for (u32 x = 0; x < d.width; ++x) {
            const f32 fx = static_cast<f32>(x) / static_cast<f32>(d.width) * scale;
            const f32 fy = static_cast<f32>(y) / static_cast<f32>(d.height) * scale;
            f32 t = 0;
            if (kind == "value") t = value_noise(seed, fx, fy, period, period);
            else if (kind == "perlin") t = perlin(seed, fx, fy, period, period) * 0.5f + 0.5f;
            else if (kind == "fbm") t = fbm(seed, fx, fy, period, period, d.octaves, false) * 0.5f + 0.5f;
            else if (kind == "ridged") t = fbm(seed, fx, fy, period, period, d.octaves, true) * 0.5f + 0.5f;
            else t = clamp01(worley(seed, fx, fy, period, period));
            set_px(img, x, y, pack(ramp(d.colors.span(), t)));
        }
    }
    return img;
}

// ---------------------------------------------------------------- sfxr

constexpr const char* kSfxPresets[] = {"pickup", "laser", "explosion", "powerup", "hit", "jump", "blip", "random"};

bool sfx_preset_known(StringView preset) {
    for (const char* p : kSfxPresets)
        if (preset == p) return true;
    return false;
}

SfxParams sfx_preset(u64 seed, StringView preset) {
    Rng r(seed);
    auto frnd = [&](f32 range) { return r.unit() * range; };
    auto rnd = [&](i32 n) { return r.range_i(0, n); };
    SfxParams p;
    if (preset == "pickup") {
        p.base_freq = 0.4f + frnd(0.5f);
        p.env_attack = 0.0f;
        p.env_sustain = frnd(0.1f);
        p.env_decay = 0.1f + frnd(0.4f);
        p.env_punch = 0.3f + frnd(0.3f);
        if (rnd(1)) {
            p.arp_speed = 0.5f + frnd(0.2f);
            p.arp_mod = 0.2f + frnd(0.4f);
        }
    } else if (preset == "laser") {
        p.wave = rnd(2);
        if (p.wave == 2 && rnd(1)) p.wave = rnd(1);
        p.base_freq = 0.5f + frnd(0.5f);
        p.freq_limit = p.base_freq - 0.2f - frnd(0.6f);
        if (p.freq_limit < 0.2f) p.freq_limit = 0.2f;
        p.freq_ramp = -0.15f - frnd(0.2f);
        if (rnd(2) == 0) {
            p.base_freq = 0.3f + frnd(0.6f);
            p.freq_limit = frnd(0.1f);
            p.freq_ramp = -0.35f - frnd(0.3f);
        }
        if (rnd(1)) {
            p.duty = frnd(0.5f);
            p.duty_ramp = frnd(0.2f);
        } else {
            p.duty = 0.4f + frnd(0.5f);
            p.duty_ramp = -frnd(0.7f);
        }
        p.env_attack = 0.0f;
        p.env_sustain = 0.1f + frnd(0.2f);
        p.env_decay = frnd(0.4f);
        if (rnd(1)) p.env_punch = frnd(0.3f);
        if (rnd(2) == 0) {
            p.pha_offset = frnd(0.2f);
            p.pha_ramp = -frnd(0.2f);
        }
        if (rnd(1)) p.hpf_freq = frnd(0.3f);
    } else if (preset == "explosion") {
        p.wave = 3;
        if (rnd(1)) {
            p.base_freq = 0.1f + frnd(0.4f);
            p.freq_ramp = -0.1f + frnd(0.4f);
        } else {
            p.base_freq = 0.2f + frnd(0.7f);
            p.freq_ramp = -0.2f - frnd(0.2f);
        }
        p.base_freq *= p.base_freq;
        if (rnd(4) == 0) p.freq_ramp = 0.0f;
        if (rnd(2) == 0) p.repeat_speed = 0.3f + frnd(0.5f);
        p.env_attack = 0.0f;
        p.env_sustain = 0.1f + frnd(0.3f);
        p.env_decay = frnd(0.5f);
        if (rnd(1) == 0) {
            p.pha_offset = -0.3f + frnd(0.9f);
            p.pha_ramp = -frnd(0.3f);
        }
        p.env_punch = 0.2f + frnd(0.6f);
        if (rnd(1)) {
            p.vib_strength = frnd(0.7f);
            p.vib_speed = frnd(0.6f);
        }
        if (rnd(2) == 0) {
            p.arp_speed = 0.6f + frnd(0.3f);
            p.arp_mod = 0.8f - frnd(1.6f);
        }
    } else if (preset == "powerup") {
        if (rnd(1)) p.wave = 1;
        else p.duty = frnd(0.6f);
        if (rnd(1)) {
            p.base_freq = 0.2f + frnd(0.3f);
            p.freq_ramp = 0.1f + frnd(0.4f);
            p.repeat_speed = 0.4f + frnd(0.4f);
        } else {
            p.base_freq = 0.2f + frnd(0.3f);
            p.freq_ramp = 0.05f + frnd(0.2f);
            if (rnd(1)) {
                p.vib_strength = frnd(0.7f);
                p.vib_speed = frnd(0.6f);
            }
        }
        p.env_attack = 0.0f;
        p.env_sustain = frnd(0.4f);
        p.env_decay = 0.1f + frnd(0.4f);
    } else if (preset == "hit") {
        p.wave = rnd(2);
        if (p.wave == 2) p.wave = 3;
        if (p.wave == 0) p.duty = frnd(0.6f);
        p.base_freq = 0.2f + frnd(0.6f);
        p.freq_ramp = -0.3f - frnd(0.4f);
        p.env_attack = 0.0f;
        p.env_sustain = frnd(0.1f);
        p.env_decay = 0.1f + frnd(0.2f);
        if (rnd(1)) p.hpf_freq = frnd(0.3f);
    } else if (preset == "jump") {
        p.wave = 0;
        p.duty = frnd(0.6f);
        p.base_freq = 0.3f + frnd(0.3f);
        p.freq_ramp = 0.1f + frnd(0.2f);
        p.env_attack = 0.0f;
        p.env_sustain = 0.1f + frnd(0.3f);
        p.env_decay = 0.1f + frnd(0.2f);
        if (rnd(1)) p.hpf_freq = frnd(0.3f);
        if (rnd(1)) p.lpf_freq = 1.0f - frnd(0.6f);
    } else if (preset == "blip") {
        p.wave = rnd(1);
        if (p.wave == 0) p.duty = frnd(0.6f);
        p.base_freq = 0.2f + frnd(0.4f);
        p.env_attack = 0.0f;
        p.env_sustain = 0.1f + frnd(0.1f);
        p.env_decay = frnd(0.2f);
        p.hpf_freq = 0.1f;
    } else {  // random, as sfxr's "randomize" (kept to sounds that end)
        p.base_freq = std::pow(frnd(2.0f) - 1.0f, 2.0f);
        if (rnd(1)) p.base_freq = std::pow(frnd(2.0f) - 1.0f, 3.0f) + 0.5f;
        p.freq_ramp = std::pow(frnd(2.0f) - 1.0f, 5.0f);
        if (p.base_freq > 0.7f && p.freq_ramp > 0.2f) p.freq_ramp = -p.freq_ramp;
        if (p.base_freq < 0.2f && p.freq_ramp < -0.05f) p.freq_ramp = -p.freq_ramp;
        p.freq_dramp = std::pow(frnd(2.0f) - 1.0f, 3.0f);
        p.duty = frnd(2.0f) - 1.0f;
        p.duty_ramp = std::pow(frnd(2.0f) - 1.0f, 3.0f);
        p.vib_strength = std::pow(frnd(2.0f) - 1.0f, 3.0f);
        p.vib_speed = frnd(2.0f) - 1.0f;
        p.env_attack = std::pow(frnd(2.0f) - 1.0f, 3.0f);
        p.env_sustain = std::pow(frnd(2.0f) - 1.0f, 2.0f);
        p.env_decay = frnd(2.0f) - 1.0f;
        p.env_punch = std::pow(frnd(0.8f), 2.0f);
        if (p.env_attack + p.env_sustain + p.env_decay < 0.2f) {
            p.env_sustain += 0.2f + frnd(0.3f);
            p.env_decay += 0.2f + frnd(0.3f);
        }
        p.lpf_resonance = frnd(2.0f) - 1.0f;
        p.lpf_freq = 1.0f - std::pow(frnd(1.0f), 3.0f);
        p.lpf_ramp = std::pow(frnd(2.0f) - 1.0f, 3.0f);
        if (p.lpf_freq < 0.1f && p.lpf_ramp < -0.05f) p.lpf_ramp = -p.lpf_ramp;
        p.hpf_freq = std::pow(frnd(1.0f), 5.0f);
        p.hpf_ramp = std::pow(frnd(2.0f) - 1.0f, 5.0f);
        p.pha_offset = std::pow(frnd(2.0f) - 1.0f, 3.0f);
        p.pha_ramp = std::pow(frnd(2.0f) - 1.0f, 3.0f);
        p.repeat_speed = frnd(2.0f) - 1.0f;
        p.arp_speed = frnd(2.0f) - 1.0f;
        p.arp_mod = frnd(2.0f) - 1.0f;
        p.wave = rnd(3);
        p.env_attack = std::fabs(p.env_attack);
        p.env_sustain = std::fabs(p.env_sustain);
        p.env_decay = std::fabs(p.env_decay);
        p.repeat_speed = std::fabs(p.repeat_speed);
        p.arp_speed = std::fabs(p.arp_speed);
    }
    return p;
}

Array<i16> synth_sfx(const SfxParams& p, u64 seed) {
    Rng rng(seed ^ 0x5F3759DFull);
    // State, reset as sfxr's ResetSample.
    i32 phase = 0;
    f64 fperiod = 0, fmaxperiod = 0, fslide = 0, fdslide = 0;
    i32 period = 0;
    f32 square_duty = 0, square_slide = 0;
    f64 arp_mod = 0;
    i32 arp_time = 0, arp_limit = 0;
    f32 fltp = 0, fltdp = 0, fltw = 0, fltw_d = 0, fltdmp = 0, fltphp = 0, flthp = 0, flthp_d = 0;
    f32 vib_phase = 0, vib_speed = 0, vib_amp = 0;
    f32 env_vol = 0;
    i32 env_stage = 0, env_time = 0, env_length[3] = {0, 0, 0};
    f32 fphase = 0, fdphase = 0;
    i32 iphase = 0, ipp = 0;
    f32 phaser[1024];
    f32 noise[32];
    i32 rep_time = 0, rep_limit = 0;

    auto reset = [&](bool restart) {
        if (!restart) phase = 0;
        fperiod = 100.0 / (static_cast<f64>(p.base_freq) * p.base_freq + 0.001);
        period = static_cast<i32>(fperiod);
        fmaxperiod = 100.0 / (static_cast<f64>(p.freq_limit) * p.freq_limit + 0.001);
        fslide = 1.0 - std::pow(static_cast<f64>(p.freq_ramp), 3.0) * 0.01;
        fdslide = -std::pow(static_cast<f64>(p.freq_dramp), 3.0) * 0.000001;
        square_duty = 0.5f - p.duty * 0.5f;
        square_slide = -p.duty_ramp * 0.00005f;
        arp_mod = p.arp_mod >= 0.0f ? 1.0 - std::pow(static_cast<f64>(p.arp_mod), 2.0) * 0.9 : 1.0 + std::pow(static_cast<f64>(p.arp_mod), 2.0) * 10.0;
        arp_time = 0;
        arp_limit = static_cast<i32>(std::pow(1.0f - p.arp_speed, 2.0f) * 20000 + 32);
        if (p.arp_speed == 1.0f) arp_limit = 0;
        if (restart) return;
        fltp = 0;
        fltdp = 0;
        fltw = std::pow(p.lpf_freq, 3.0f) * 0.1f;
        fltw_d = 1.0f + p.lpf_ramp * 0.0001f;
        fltdmp = 5.0f / (1.0f + std::pow(p.lpf_resonance, 2.0f) * 20.0f) * (0.01f + fltw);
        if (fltdmp > 0.8f) fltdmp = 0.8f;
        fltphp = 0;
        flthp = std::pow(p.hpf_freq, 2.0f) * 0.1f;
        flthp_d = 1.0f + p.hpf_ramp * 0.0003f;
        vib_phase = 0;
        vib_speed = std::pow(p.vib_speed, 2.0f) * 0.01f;
        vib_amp = p.vib_strength * 0.5f;
        env_vol = 0;
        env_stage = 0;
        env_time = 0;
        env_length[0] = static_cast<i32>(p.env_attack * p.env_attack * 100000.0f);
        env_length[1] = static_cast<i32>(p.env_sustain * p.env_sustain * 100000.0f);
        env_length[2] = static_cast<i32>(p.env_decay * p.env_decay * 100000.0f);
        fphase = std::pow(p.pha_offset, 2.0f) * 1020.0f;
        if (p.pha_offset < 0.0f) fphase = -fphase;
        fdphase = std::pow(p.pha_ramp, 2.0f) * 1.0f;
        if (p.pha_ramp < 0.0f) fdphase = -fdphase;
        iphase = std::abs(static_cast<i32>(fphase));
        ipp = 0;
        for (f32& v : phaser) v = 0;
        for (f32& v : noise) v = rng.unit() * 2.0f - 1.0f;
        rep_time = 0;
        rep_limit = static_cast<i32>(std::pow(1.0f - p.repeat_speed, 2.0f) * 20000 + 32);
        if (p.repeat_speed == 0.0f) rep_limit = 0;
    };
    reset(false);

    Array<i16> out;
    constexpr usize kMaxSamples = kSampleRate * 4;  // sfxr sounds are short; never run away
    bool playing = true;
    while (playing && out.size() < kMaxSamples) {
        ++rep_time;
        if (rep_limit != 0 && rep_time >= rep_limit) {
            rep_time = 0;
            reset(true);
        }
        ++arp_time;
        if (arp_limit != 0 && arp_time >= arp_limit) {
            arp_limit = 0;
            fperiod *= arp_mod;
        }
        fslide += fdslide;
        fperiod *= fslide;
        if (fperiod > fmaxperiod) {
            fperiod = fmaxperiod;
            if (p.freq_limit > 0.0f) playing = false;
        }
        f64 rfperiod = fperiod;
        if (vib_amp > 0.0f) {
            vib_phase += vib_speed;
            rfperiod = fperiod * (1.0 + std::sin(vib_phase) * vib_amp);
        }
        period = static_cast<i32>(rfperiod);
        if (period < 8) period = 8;
        square_duty += square_slide;
        if (square_duty < 0.0f) square_duty = 0.0f;
        if (square_duty > 0.5f) square_duty = 0.5f;
        ++env_time;
        if (env_time > env_length[env_stage]) {
            env_time = 0;
            ++env_stage;
            if (env_stage == 3) break;
        }
        const f32 len = static_cast<f32>(env_length[env_stage] > 0 ? env_length[env_stage] : 1);
        if (env_stage == 0) env_vol = static_cast<f32>(env_time) / len;
        if (env_stage == 1) env_vol = 1.0f + (1.0f - static_cast<f32>(env_time) / len) * 2.0f * p.env_punch;
        if (env_stage == 2) env_vol = 1.0f - static_cast<f32>(env_time) / len;
        fphase += fdphase;
        iphase = std::abs(static_cast<i32>(fphase));
        if (iphase > 1023) iphase = 1023;
        if (flthp_d != 0.0f) {
            flthp *= flthp_d;
            if (flthp < 0.00001f) flthp = 0.00001f;
            if (flthp > 0.1f) flthp = 0.1f;
        }
        f32 ssample = 0;
        for (i32 si = 0; si < 8; ++si) {  // 8x supersampling
            f32 sample = 0;
            ++phase;
            if (phase >= period) {
                phase %= period;
                if (p.wave == 3)
                    for (f32& v : noise) v = rng.unit() * 2.0f - 1.0f;
            }
            const f32 fp = static_cast<f32>(phase) / static_cast<f32>(period);
            switch (p.wave) {
                case 0: sample = fp < square_duty ? 0.5f : -0.5f; break;
                case 1: sample = 1.0f - fp * 2.0f; break;
                case 2: sample = std::sin(fp * 2.0f * kPi); break;
                default: sample = noise[phase * 32 / period]; break;
            }
            const f32 pp = fltp;
            fltw *= fltw_d;
            if (fltw < 0.0f) fltw = 0.0f;
            if (fltw > 0.1f) fltw = 0.1f;
            if (p.lpf_freq != 1.0f) {
                fltdp += (sample - fltp) * fltw;
                fltdp -= fltdp * fltdmp;
            } else {
                fltp = sample;
                fltdp = 0.0f;
            }
            fltp += fltdp;
            fltphp += fltp - pp;
            fltphp -= fltphp * flthp;
            sample = fltphp;
            phaser[ipp & 1023] = sample;
            sample += phaser[(ipp - iphase + 1024) & 1023];
            ipp = (ipp + 1) & 1023;
            ssample += sample * env_vol;
        }
        ssample = ssample / 8.0f * p.volume;
        if (ssample > 1.0f) ssample = 1.0f;
        if (ssample < -1.0f) ssample = -1.0f;
        out.push_back(static_cast<i16>(std::lround(ssample * 32767.0f)));
    }
    return out;
}

// ---------------------------------------------------------------- music

struct Scale {
    const char* name;
    i32 steps[7];
    u32 count;
};
constexpr Scale kScales[] = {
    {"major", {0, 2, 4, 5, 7, 9, 11}, 7},      {"minor", {0, 2, 3, 5, 7, 8, 10}, 7}, {"dorian", {0, 2, 3, 5, 7, 9, 10}, 7},
    {"pentatonic", {0, 2, 4, 7, 9, 0, 0}, 5}, {"blues", {0, 3, 5, 6, 7, 10, 0}, 6},
};

bool music_scale_known(StringView scale) {
    for (const Scale& s : kScales)
        if (scale == s.name) return true;
    return false;
}

namespace {

// The MIDI note of scale degree `d` (any integer) above `base`.
i32 degree_note(const Scale& s, i32 base, i32 d) {
    const i32 n = static_cast<i32>(s.count);
    const i32 oct = d >= 0 ? d / n : -((-d + n - 1) / n);
    return base + s.steps[wrap(d, n)] + 12 * oct;
}

f32 note_hz(i32 midi) { return 440.0f * std::pow(2.0f, static_cast<f32>(midi - 69) / 12.0f); }

// Adds a note into a looping buffer (tails wrap around, so the loop is seamless).
void add_note(Array<f32>& buf, usize start, f32 hz, f32 seconds, f32 gain, i32 wave, f32 decay) {
    const usize n = static_cast<usize>(seconds * static_cast<f32>(kSampleRate));
    const usize total = buf.size();
    f32 ph = 0;
    const f32 dph = hz / static_cast<f32>(kSampleRate);
    for (usize i = 0; i < n; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(kSampleRate);
        const f32 env = std::fmin(1.0f, t * 200.0f) * std::exp(-t * decay) * (i + 256 > n ? static_cast<f32>(n - i) / 256.0f : 1.0f);
        f32 s;
        switch (wave) {
            case 0: s = ph < 0.5f ? 0.5f : -0.5f; break;                        // square
            case 1: s = ph < 0.25f ? 0.5f : -0.5f; break;                       // narrow pulse
            case 2: s = 4.0f * std::fabs(ph - 0.5f) - 1.0f; break;             // triangle
            default: s = std::sin(ph * 2.0f * kPi); break;                      // sine
        }
        buf[(start + i) % total] += s * env * gain;
        ph += dph;
        if (ph >= 1.0f) ph -= 1.0f;
    }
}

}  // namespace

Array<i16> music_loop(u64 seed, const MusicDesc& d) {
    Rng rng(seed);
    const Scale* sc = &kScales[0];
    for (const Scale& s : kScales)
        if (d.scale.view() == s.name) sc = &s;
    const usize step = static_cast<usize>(std::lround(static_cast<f64>(kSampleRate) * 60.0 / static_cast<f64>(d.bpm) / 4.0));  // a 16th
    const usize total = step * 16 * d.bars;
    Array<f32> buf;
    buf.resize(total);
    for (f32& v : buf) v = 0;
    const f32 sixteenth = static_cast<f32>(step) / static_cast<f32>(kSampleRate);

    // Chords: one per bar from a progression of scale degrees.
    static constexpr i32 kProgressions[][4] = {{0, 4, 5, 3}, {0, 5, 3, 4}, {5, 3, 0, 4}, {0, 3, 4, 0}, {0, 2, 3, 4}, {0, 6, 5, 4}};
    const i32* prog = kProgressions[rng.range_i(0, 5)];
    const i32 base = 48 + static_cast<i32>(d.root % 12);  // C3 + root
    const i32 bass_pattern = rng.range_i(0, 2);
    i32 melody = 7 + rng.range_i(0, 4);  // a degree two octaves up-ish
    for (u32 bar = 0; bar < d.bars; ++bar) {
        const i32 chord = prog[bar % 4];
        const usize bar0 = bar * 16 * step;
        // Pad: the triad, held for the bar, soft.
        for (i32 k = 0; k < 3; ++k)
            add_note(buf, bar0, note_hz(degree_note(*sc, base + 12, chord + k * 2)), sixteenth * 16.0f, 0.06f, 2, 0.6f);
        // Bass: root on the beats (and eighths in the busier patterns).
        for (u32 s = 0; s < 16; ++s) {
            const bool on = bass_pattern == 0 ? s % 4 == 0 : bass_pattern == 1 ? s % 2 == 0 : (s % 4 == 0 || s % 8 == 6);
            if (!on) continue;
            const i32 note = degree_note(*sc, base - 12, chord + ((s % 8 == 4 && bass_pattern != 0) ? 4 : 0));
            add_note(buf, bar0 + s * step, note_hz(note), sixteenth * 1.8f, 0.22f, 2, 6.0f);
        }
        // Melody: a random walk on eighths, landing on chord tones on the beats.
        for (u32 s = 0; s < 16; s += 2) {
            if (rng.unit() < 0.3f) continue;  // rests
            if (s % 4 == 0) {
                const i32 tones[3] = {chord, chord + 2, chord + 4};
                i32 best = tones[0] + 7;
                for (const i32 t : tones)
                    for (const i32 o : {0, 7, 14})
                        if (std::abs(t + o - melody) < std::abs(best - melody)) best = t + o;
                melody = best;
            } else {
                melody += rng.range_i(-2, 2);
            }
            if (melody < 4) melody = 4;
            if (melody > 16) melody = 16;
            const f32 len = sixteenth * (rng.unit() < 0.3f ? 3.5f : 1.7f);
            add_note(buf, bar0 + s * step, note_hz(degree_note(*sc, base, melody)), len, 0.12f, 1, 4.0f);
        }
        if (!d.drums) continue;
        // Drums: kick on 1 and 3, snare on 2 and 4, hats on eighths.
        for (u32 s = 0; s < 16; ++s) {
            const usize at = bar0 + s * step;
            if (s % 8 == 0) {
                const usize n = static_cast<usize>(0.18f * static_cast<f32>(kSampleRate));
                f32 ph = 0;
                for (usize i = 0; i < n; ++i) {
                    const f32 t = static_cast<f32>(i) / static_cast<f32>(kSampleRate);
                    ph += (50.0f + 120.0f * std::exp(-t * 30.0f)) / static_cast<f32>(kSampleRate);
                    buf[(at + i) % total] += std::sin(ph * 2.0f * kPi) * std::exp(-t * 18.0f) * 0.5f;
                }
            }
            if (s % 8 == 4) {
                const usize n = static_cast<usize>(0.12f * static_cast<f32>(kSampleRate));
                for (usize i = 0; i < n; ++i) {
                    const f32 t = static_cast<f32>(i) / static_cast<f32>(kSampleRate);
                    buf[(at + i) % total] += (rng.unit() * 2.0f - 1.0f) * std::exp(-t * 25.0f) * 0.22f;
                }
            }
            if (s % 2 == 0) {
                const usize n = static_cast<usize>(0.03f * static_cast<f32>(kSampleRate));
                f32 prev = 0;
                for (usize i = 0; i < n; ++i) {
                    const f32 t = static_cast<f32>(i) / static_cast<f32>(kSampleRate);
                    const f32 w = rng.unit() * 2.0f - 1.0f;
                    buf[(at + i) % total] += (w - prev) * std::exp(-t * 120.0f) * 0.06f;  // a crude high-pass
                    prev = w;
                }
            }
        }
    }
    f32 peak = 1e-6f;
    for (const f32 v : buf) peak = std::fmax(peak, std::fabs(v));
    const f32 gain = d.volume / peak;
    Array<i16> out;
    out.resize(total);
    for (usize i = 0; i < total; ++i) out[i] = static_cast<i16>(std::lround(clamp01(buf[i] * gain * 0.5f + 0.5f) * 65535.0f - 32768.0f));
    return out;
}

void wav_bytes(Span<const i16> samples, u32 rate, Array<byte>& out) {
    const u32 data = static_cast<u32>(samples.size() * 2);
    out.resize(44 + data);
    byte* p = out.data();
    auto u32le = [&](usize at, u32 v) {
        for (u32 i = 0; i < 4; ++i) p[at + i] = static_cast<byte>((v >> (i * 8)) & 0xFF);
    };
    auto u16le = [&](usize at, u16 v) {
        p[at] = static_cast<byte>(v & 0xFF);
        p[at + 1] = static_cast<byte>(v >> 8);
    };
    std::memcpy(p, "RIFF", 4);
    u32le(4, 36 + data);
    std::memcpy(p + 8, "WAVEfmt ", 8);
    u32le(16, 16);
    u16le(20, 1);  // PCM
    u16le(22, 1);  // mono
    u32le(24, rate);
    u32le(28, rate * 2);
    u16le(32, 2);
    u16le(34, 16);
    std::memcpy(p + 36, "data", 4);
    u32le(40, data);
    for (usize i = 0; i < samples.size(); ++i) u16le(44 + i * 2, static_cast<u16>(samples[i]));
}

}  // namespace mce::mcp::gen

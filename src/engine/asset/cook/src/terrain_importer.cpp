// .mcterrain -> CHNK_TERRAIN: a heightmap terrain (M-TERRAIN 1). The editor's brushes and the MCP
// terrain tools rewrite `heights`; everything else is set when the terrain is made.
//
// {
//   "format": "mce-terrain", "version": 1, "guid": "<uuid>", "name": "island",
//   "resolution": 257,            // samples per side: 2^n + 1, 33..4097
//   "chunk": 32,                  // quads per side of a finest LOD node: 8, 16, ... 256 (at most resolution - 1)
//   "size": [256, 256],           // metres along x and z, centred on the entity
//   "height_range": [0, 40],      // metres a sample of 0 and of 65535 stand for
//   "heights": "<base64>",        // resolution² little-endian u16, z rows of x columns; omitted = flat
//   "fill": 0,                    // metres of a flat terrain (no "heights"); default: the range's low end
//   "layers": [                   // splat layers (M-TERRAIN 2), at most 8, painted by "splat"
//     { "material": "guid:<uuid>",          // a .mcmat: its maps, base color, roughness, metallic
//       "tile": 4,                          // metres per repeat of its textures (default 4)
//       "triplanar": false,                 // also project along x and z on steep ground
//       "height_strength": 1,               // how far its albedo brightness lifts it over neighbours
//       "auto": { "height": [lo, hi], "slope": [0, 30] } }   // with no "splat": where it grows
//   ],
//   "splat_resolution": 257,      // weight samples per side: 2^n + 1, 33..resolution (default min(resolution, 1025))
//   "splat": "<base64>",          // splat_resolution² × layer count u8 weights, sample by sample;
//                                 // omitted = the layers' "auto" rules, else all layer 0
//   "holes": "<base64>"           // (M-TERRAIN 3) a bit per height sample, sample i at bit i % 8 of byte
//                                 // i / 8: triangles touching a set sample are neither drawn nor collided
// }

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "importer_common.h"
#include "mce/asset/formats.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.terrain";
constexpr u32 kTerrainFormatVersion = 1;

int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

// Standard or URL-safe alphabet, padding optional; false on any other character.
bool base64_decode(StringView in, Array<byte>& out) {
    out.clear();
    out.reserve(in.size() / 4 * 3 + 3);
    u32 acc = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=') break;
        const int v = base64_value(c);
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<u32>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<byte>((acc >> bits) & 0xFF));
        }
    }
    return true;
}

bool is_resolution(f64 n, f64 max) {
    const u32 r = static_cast<u32>(n);
    return n >= asset::kTerrainMinResolution && n <= max && f64(r) == n && ((r - 1) & (r - 2)) == 0;
}

// An "auto" rule's weight at height h (metres) and slope (degrees): 1 inside its ranges, fading over a
// soft edge outside them.
struct AutoRule {
    bool any = false;
    f32 height[2] = {-1e9f, 1e9f};
    f32 slope[2] = {0.0f, 90.0f};
    f32 height_edge = 1.0f;
    f32 weight(f32 h, f32 deg) const {
        auto band = [](f32 v, f32 lo, f32 hi, f32 edge) {
            const f32 d = std::min(v - lo, hi - v);
            return std::clamp(d / edge + 0.5f, 0.0f, 1.0f);
        };
        return band(h, height[0], height[1], height_edge) * band(deg, slope[0], slope[1], 4.0f);
    }
};

class TerrainImporter final : public IImporter {
public:
    StringView id() const override { return "terrain"; }
    u32 version() const override { return 1; }
    ChunkType type() const override { return ChunkType::Terrain; }
    bool native() const override { return true; }
    bool handles_extension(StringView ext) const override { return ext == ".mcterrain"; }

    void scan_dependencies(const ImportInput& in, Array<DepRef>& out, DiagList& diags) const override {
        const JsonValue layers = in.settings.get("layers");
        if (!layers.is_array()) return;  // cook() reports the wrong type
        for (u32 i = 0; i < layers.size(); ++i) {
            const JsonValue m = layers.at(i).is_object() ? layers.at(i).get("material") : JsonValue();
            if (!m.valid() || m.is_null()) continue;
            Guid g;
            if (!m.is_string() || !parse_guid_ref(m.as_string(), g)) {
                detail::error_at(diags, Errc::ParseError, in.display_path, m, kStage, "a layer's 'material' must be \"guid:<uuid>\" or null");
                continue;
            }
            DepRef d;
            d.guid = g;
            d.expected = ChunkType::Material;
            d.line = m.line();
            d.col = m.col();
            char where[48];
            std::snprintf(where, sizeof where, "layers[%u].material", i);
            d.where.assign(where);
            out.push_back(d);
        }
    }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        const JsonValue doc = in.settings;
        const StringView file = in.display_path;
        auto fail = [&](JsonValue at, StringView msg) {
            detail::error_at(diags, Errc::ParseError, file, at.valid() ? at : doc, kStage, msg);
            return false;
        };
        if (!detail::check_keys(doc, {"format", "version", "guid", "name", "resolution", "chunk", "size", "height_range", "heights", "fill", "layers",
                                      "splat_resolution", "splat", "holes"},
                                file, kStage, diags))
            return false;
        const JsonValue fmt = doc.get("format");
        if (!fmt.is_string() || fmt.as_string() != "mce-terrain") return fail(fmt, "\"format\" must be \"mce-terrain\"");
        const JsonValue ver = doc.get("version");
        if (!ver.is_integer() || ver.as_number() != kTerrainFormatVersion) {
            String msg;
            appendf(msg, "unsupported terrain version (this cook reads version %u)", kTerrainFormatVersion);
            detail::error_at(diags, Errc::VersionTooNew, file, ver.valid() ? ver : doc, kStage, msg);
            return false;
        }

        asset::TerrainHeader h{};
        const JsonValue res = doc.get("resolution");
        const f64 n = res.is_integer() ? res.as_number() : 0.0;
        const u32 resolution = static_cast<u32>(n);
        if (!is_resolution(n, asset::kTerrainMaxResolution))
            return fail(res, "'resolution' must be 2^n + 1 samples per side, 33 to 4097 (33, 65, 129, 257, 513, 1025, ...)");
        h.resolution = resolution;

        const JsonValue chunk = doc.get("chunk");
        h.chunk = 32;
        if (chunk.valid()) {
            const f64 c = chunk.is_integer() ? chunk.as_number() : 0.0;
            h.chunk = static_cast<u32>(c);
            if (c < 8 || c > 256 || (h.chunk & (h.chunk - 1)) != 0) return fail(chunk, "'chunk' must be 8, 16, 32, 64, 128 or 256");
        }
        if (h.chunk > resolution - 1) h.chunk = resolution - 1;

        auto pair = [&](const char* key, f64 lo_min, f64 hi_max, f32 (&dst)[2]) {
            const JsonValue v = doc.get(key);
            if (!v.is_array() || v.size() != 2 || !v.at(0).is_number() || !v.at(1).is_number()) {
                String msg;
                appendf(msg, "'%s' must be [a, b] in metres", key);
                return fail(v, msg.view());
            }
            const f64 a = v.at(0).as_number(), b = v.at(1).as_number();
            if (!std::isfinite(a) || !std::isfinite(b) || a < lo_min || b > hi_max) {
                String msg;
                appendf(msg, "'%s' is out of range", key);
                return fail(v, msg.view());
            }
            dst[0] = static_cast<f32>(a);
            dst[1] = static_cast<f32>(b);
            return true;
        };
        if (!pair("size", 0.0, 1e6, h.size)) return false;
        if (h.size[0] <= 0.0f || h.size[1] <= 0.0f) return fail(doc.get("size"), "'size' must be positive");
        f32 range[2] = {0.0f, 1.0f};
        if (!pair("height_range", -1e5, 1e5, range)) return false;
        if (range[1] <= range[0]) return fail(doc.get("height_range"), "'height_range' must be [low, high] with high > low");
        h.height_min = range[0];
        h.height_max = range[1];

        Array<u16> heights;
        heights.resize(usize(resolution) * resolution, u16{0});
        const JsonValue data = doc.get("heights");
        const JsonValue fill = doc.get("fill");
        if (data.valid()) {
            if (fill.valid()) return fail(fill, "'fill' is for a flat terrain: drop it or 'heights'");
            Array<byte> bytes;
            if (!data.is_string() || !base64_decode(data.as_string(), bytes)) return fail(data, "'heights' must be a base64 string");
            if (bytes.size() != heights.size() * 2) {
                String msg;
                appendf(msg, "'heights' holds %zu bytes; a %u x %u terrain needs %zu (little-endian u16 per sample)", bytes.size(), resolution,
                        resolution, heights.size() * 2);
                return fail(data, msg.view());
            }
            for (usize i = 0; i < heights.size(); ++i)
                heights[i] = static_cast<u16>(static_cast<u32>(bytes[i * 2]) | (static_cast<u32>(bytes[i * 2 + 1]) << 8));
        } else if (fill.valid()) {
            if (!fill.is_number() || fill.as_number() < h.height_min || fill.as_number() > h.height_max)
                return fail(fill, "'fill' must be a height inside 'height_range'");
            const f64 t = (fill.as_number() - h.height_min) / (f64(h.height_max) - h.height_min);
            const u16 v = static_cast<u16>(std::lround(t * 65535.0));
            for (u16& s : heights) s = v;
        }

        Array<asset::TerrainLayer> layers;
        Array<u8> splat;
        if (!cook_layers(doc, file, h, heights.span(), layers, splat, diags)) return false;

        Array<u8> holes;
        if (const JsonValue hv = doc.get("holes"); hv.valid()) {
            Array<byte> bytes;
            if (!hv.is_string() || !base64_decode(hv.as_string(), bytes)) return fail(hv, "'holes' must be a base64 string");
            const usize need = (usize(resolution) * resolution + 7) / 8;
            if (bytes.size() != need) {
                String msg;
                appendf(msg, "'holes' holds %zu bytes; a %u x %u terrain needs %zu (a bit per sample)", bytes.size(), resolution, resolution, need);
                return fail(hv, msg.view());
            }
            holes.resize(need);
            bool any = false;
            for (usize i = 0; i < need; ++i) any |= (holes[i] = static_cast<u8>(bytes[i])) != 0;
            if (any) h.flags |= asset::TerrainFlags::kHoles;
            else holes.clear();
        }

        Array<byte> payload;
        asset::write_terrain(h, heights.span(), payload, layers.span(), splat.span(), holes.span());
        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::Terrain;
        d.version = asset::chunk_version(asset::TERRAIN_VERSION, 0);
        d.guid = in.guid;
        out.add(d, payload.span());
        return true;
    }

private:
    // "layers", "splat_resolution" and "splat" (or the layers' "auto" rules) into the header's layer
    // fields, the layer table and the two RGBA8 weight planes.
    static bool cook_layers(JsonValue doc, StringView file, asset::TerrainHeader& h, Span<const u16> heights,
                            Array<asset::TerrainLayer>& layers, Array<u8>& splat, DiagList& diags) {
        auto fail = [&](JsonValue at, StringView msg) {
            detail::error_at(diags, Errc::ParseError, file, at.valid() ? at : doc, kStage, msg);
            return false;
        };
        const JsonValue list = doc.get("layers");
        if (!list.valid()) {
            if (doc.get("splat").valid() || doc.get("splat_resolution").valid())
                return fail(doc.get("splat").valid() ? doc.get("splat") : doc.get("splat_resolution"), "a splat map needs 'layers'");
            return true;
        }
        if (!list.is_array() || list.size() == 0 || list.size() > asset::kTerrainMaxLayers)
            return fail(list, "'layers' must be an array of 1 to 8 layers");
        const u32 count = list.size();
        Array<AutoRule> rules;
        bool any_rule = false;
        for (u32 i = 0; i < count; ++i) {
            const JsonValue l = list.at(i);
            if (!l.is_object()) return fail(l, "a layer must be an object");
            if (!detail::check_keys(l, {"material", "tile", "triplanar", "height_strength", "auto"}, file, kStage, diags)) return false;
            asset::TerrainLayer t{};
            Guid g;
            if (const JsonValue m = l.get("material"); m.is_string() && parse_guid_ref(m.as_string(), g)) {
                t.material[0] = g.hi;
                t.material[1] = g.lo;
            }
            f64 tile = 4.0, strength = 1.0;
            bool triplanar = false;
            if (!detail::read_number(l, "tile", 4.0, tile, file, kStage, diags) ||
                !detail::read_number(l, "height_strength", 1.0, strength, file, kStage, diags) ||
                !detail::read_bool(l, "triplanar", false, triplanar, file, kStage, diags))
                return false;
            if (!(tile > 0.0 && tile < 1e6)) return fail(l.get("tile"), "'tile' must be metres per repeat, above 0");
            if (!(strength >= 0.0 && strength <= 100.0)) return fail(l.get("height_strength"), "'height_strength' must be 0 to 100");
            t.tile = static_cast<f32>(tile);
            t.height_strength = static_cast<f32>(strength);
            if (triplanar) t.flags |= asset::TerrainLayerFlags::kTriplanar;
            layers.push_back(t);

            AutoRule r;
            r.height_edge = std::max(1.0f, 0.02f * (h.height_max - h.height_min));
            if (const JsonValue a = l.get("auto"); a.valid()) {
                if (!a.is_object()) return fail(a, "'auto' must be an object");
                if (!detail::check_keys(a, {"height", "slope"}, file, kStage, diags)) return false;
                r.any = any_rule = true;
                auto range = [&](const char* key, f32 (&dst)[2], f64 lo, f64 hi) {
                    const JsonValue v = a.get(key);
                    if (!v.valid()) return true;
                    if (!v.is_array() || v.size() != 2 || !v.at(0).is_number() || !v.at(1).is_number() ||
                        !(v.at(0).as_number() <= v.at(1).as_number()) || v.at(0).as_number() < lo || v.at(1).as_number() > hi) {
                        String msg;
                        appendf(msg, "'auto.%s' must be [low, high]%s", key, key[0] == 's' ? " in degrees, 0 to 90" : " in metres");
                        return fail(v, msg.view());
                    }
                    dst[0] = static_cast<f32>(v.at(0).as_number());
                    dst[1] = static_cast<f32>(v.at(1).as_number());
                    return true;
                };
                if (!range("height", r.height, -1e5, 1e5) || !range("slope", r.slope, 0.0, 90.0)) return false;
            }
            rules.push_back(r);
        }

        u32 sres = std::min(h.resolution, 1025u);
        if (const JsonValue sr = doc.get("splat_resolution"); sr.valid()) {
            if (!sr.is_integer() || !is_resolution(sr.as_number(), h.resolution))
                return fail(sr, "'splat_resolution' must be 2^n + 1 weight samples per side, 33 up to 'resolution'");
            sres = static_cast<u32>(sr.as_number());
        }
        h.layer_count = count;
        h.splat_resolution = sres;
        const usize n = usize(sres) * sres;
        splat.resize(n * 8, u8{0});
        auto put = [&](usize sample, u32 layer, u8 w) { splat[(layer / 4) * n * 4 + sample * 4 + layer % 4] = w; };

        if (const JsonValue data = doc.get("splat"); data.valid()) {
            Array<byte> bytes;
            if (!data.is_string() || !base64_decode(data.as_string(), bytes)) return fail(data, "'splat' must be a base64 string");
            if (bytes.size() != n * count) {
                String msg;
                appendf(msg, "'splat' holds %zu bytes; %u x %u samples of %u layers need %zu (one u8 weight per layer per sample)", bytes.size(),
                        sres, sres, count, n * count);
                return fail(data, msg.view());
            }
            for (usize i = 0; i < n; ++i)
                for (u32 l = 0; l < count; ++l) put(i, l, static_cast<u8>(bytes[i * count + l]));
            return true;
        }
        if (!any_rule) {
            for (usize i = 0; i < n; ++i) put(i, 0, 255);
            return true;
        }
        // The "auto" rules over the heights: each weight sample takes the height and slope of the
        // terrain at its position; a sample no rule claims goes to layer 0.
        const u32 res = h.resolution;
        const f32 dx = h.size[0] / f32(res - 1), dz = h.size[1] / f32(res - 1), scale = (h.height_max - h.height_min) / 65535.0f;
        auto height = [&](i32 x, i32 z) {
            x = std::clamp(x, 0, i32(res) - 1);
            z = std::clamp(z, 0, i32(res) - 1);
            return h.height_min + f32(heights[usize(z) * res + usize(x)]) * scale;
        };
        for (u32 sz = 0; sz < sres; ++sz)
            for (u32 sx = 0; sx < sres; ++sx) {
                const i32 x = i32(std::lround(f64(sx) * (res - 1) / (sres - 1))), z = i32(std::lround(f64(sz) * (res - 1) / (sres - 1)));
                const f32 gx = (height(x + 1, z) - height(x - 1, z)) / (2.0f * dx), gz = (height(x, z + 1) - height(x, z - 1)) / (2.0f * dz);
                const f32 deg = std::atan(std::sqrt(gx * gx + gz * gz)) * 57.29578f, hh = height(x, z);
                f32 w[asset::kTerrainMaxLayers] = {}, sum = 0.0f;
                for (u32 l = 0; l < count; ++l) {
                    w[l] = rules[l].any ? rules[l].weight(hh, deg) : 0.0f;
                    sum += w[l];
                }
                if (sum < 1.0f) w[0] += 1.0f - sum, sum = std::max(sum, 1.0f);
                for (u32 l = 0; l < count; ++l) put(usize(sz) * sres + sx, l, static_cast<u8>(std::lround(w[l] / sum * 255.0f)));
            }
        return true;
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_terrain_importer() { return mem::make_unique<TerrainImporter>(mem::heap()); }

}  // namespace mce::cook

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
//   "fill": 0                     // metres of a flat terrain (no "heights"); default: the range's low end
// }

#include <cmath>
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

class TerrainImporter final : public IImporter {
public:
    StringView id() const override { return "terrain"; }
    u32 version() const override { return 1; }
    ChunkType type() const override { return ChunkType::Terrain; }
    bool native() const override { return true; }
    bool handles_extension(StringView ext) const override { return ext == ".mcterrain"; }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        const JsonValue doc = in.settings;
        const StringView file = in.display_path;
        auto fail = [&](JsonValue at, StringView msg) {
            detail::error_at(diags, Errc::ParseError, file, at.valid() ? at : doc, kStage, msg);
            return false;
        };
        if (!detail::check_keys(doc, {"format", "version", "guid", "name", "resolution", "chunk", "size", "height_range", "heights", "fill"},
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
        if (n < asset::kTerrainMinResolution || n > asset::kTerrainMaxResolution || ((resolution - 1) & (resolution - 2)) != 0)
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

        Array<byte> payload;
        asset::write_terrain(h, heights.span(), payload);
        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::Terrain;
        d.version = asset::chunk_version(asset::TERRAIN_VERSION, 0);
        d.guid = in.guid;
        out.add(d, payload.span());
        return true;
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_terrain_importer() { return mem::make_unique<TerrainImporter>(mem::heap()); }

}  // namespace mce::cook

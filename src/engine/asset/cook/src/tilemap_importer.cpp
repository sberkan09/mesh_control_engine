// .mctilemap -> CHNK_TILE: one tile layer over a tileset texture (FR-R2D-115, ADR-0030 §8). The
// editor's Tilemap panel is the writer; the tileset GUID is a dependency (a missing or
// non-texture reference is a cook error naming file:line:col).
//
// {
//   "format": "mce-tilemap", "version": 1, "guid": "<uuid>", "name": "level1",
//   "tileset": "guid:<texture uuid>", "columns": 8, "rows": 8,   // tileset cells
//   "width": 32, "height": 18, "tile_size": 1,                    // tiles, metres per tile
//   "tiles": [ 0, 3, 3, ... ]   // width x height, row 0 = bottom; 0 = empty, n = cell n-1
// }

#include <cstring>

#include "importer_common.h"
#include "mce/asset/formats.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.tilemap";
constexpr u32 kTilemapFormatVersion = 1;
constexpr u32 kMaxSide = 4096;

bool read_count(JsonValue doc, StringView key, u32 def, u32 max, u32& out, StringView file, DiagList& diags) {
    f64 v = def;
    if (!detail::read_number(doc, key, def, v, file, kStage, diags)) return false;
    if (v < 1.0 || v > static_cast<f64>(max) || v != static_cast<f64>(static_cast<u32>(v))) {
        String msg;
        appendf(msg, "'%.*s' must be a whole number from 1 to %u", static_cast<int>(key.size()), key.data(), max);
        detail::error_at(diags, Errc::ParseError, file, doc.get(key).valid() ? doc.get(key) : doc, kStage, msg);
        return false;
    }
    out = static_cast<u32>(v);
    return true;
}

class TilemapImporter final : public IImporter {
public:
    StringView id() const override { return "tilemap"; }
    u32 version() const override { return 1; }
    ChunkType type() const override { return ChunkType::Tilemap; }
    bool native() const override { return true; }
    bool handles_extension(StringView ext) const override { return ext == ".mctilemap"; }

    void scan_dependencies(const ImportInput& in, Array<DepRef>& out, DiagList& diags) const override {
        const JsonValue ts = in.settings.get("tileset");
        if (!ts.valid() || ts.is_null()) return;
        Guid g;
        if (!ts.is_string() || !parse_guid_ref(ts.as_string(), g)) {
            detail::error_at(diags, Errc::ParseError, in.display_path, ts, kStage, "'tileset' must be \"guid:<uuid>\" or null");
            return;
        }
        DepRef d;
        d.guid = g;
        d.expected = ChunkType::Texture;
        d.line = ts.line();
        d.col = ts.col();
        d.where.assign("tileset");
        out.push_back(d);
    }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        const JsonValue doc = in.settings;
        const StringView file = in.display_path;
        const u32 errors_before = diags.error_count();
        if (!detail::check_keys(doc, {"format", "version", "guid", "name", "tileset", "columns", "rows", "width", "height", "tile_size", "tiles"},
                                file, kStage, diags))
            return false;
        const JsonValue fmt = doc.get("format");
        if (!fmt.is_string() || fmt.as_string() != "mce-tilemap") {
            detail::error_at(diags, Errc::ParseError, file, fmt.valid() ? fmt : doc, kStage, "\"format\" must be \"mce-tilemap\"");
            return false;
        }
        const JsonValue ver = doc.get("version");
        if (!ver.is_integer() || ver.as_number() != kTilemapFormatVersion) {
            String msg;
            appendf(msg, "unsupported tilemap version (this cook reads version %u)", kTilemapFormatVersion);
            detail::error_at(diags, Errc::VersionTooNew, file, ver.valid() ? ver : doc, kStage, msg);
            return false;
        }
        asset::TilemapHeader h{};
        f64 tile_size = 1.0;
        read_count(doc, "width", 1, kMaxSide, h.width, file, diags);
        read_count(doc, "height", 1, kMaxSide, h.height, file, diags);
        read_count(doc, "columns", 1, 256, h.columns, file, diags);
        read_count(doc, "rows", 1, 256, h.rows, file, diags);
        detail::read_number(doc, "tile_size", 1.0, tile_size, file, kStage, diags);
        if (!(tile_size > 0.0)) detail::error_at(diags, Errc::ParseError, file, doc.get("tile_size"), kStage, "'tile_size' must be > 0");
        h.tile_size = static_cast<f32>(tile_size);
        if (const JsonValue ts = doc.get("tileset"); ts.is_string()) {
            Guid g;
            if (parse_guid_ref(ts.as_string(), g)) {
                h.tileset[0] = g.hi;
                h.tileset[1] = g.lo;
            }
        }
        if (diags.error_count() != errors_before) return false;

        const usize count = usize(h.width) * h.height;
        const u32 cells = h.columns * h.rows;
        Array<byte> payload;
        payload.resize(sizeof(h) + count * sizeof(u32), byte{0});
        std::memcpy(payload.data(), &h, sizeof(h));
        u32* tiles = reinterpret_cast<u32*>(payload.data() + sizeof(h));
        const JsonValue arr = doc.get("tiles");
        if (arr.valid()) {
            if (!arr.is_array() || arr.size() != count) {
                String msg;
                appendf(msg, "'tiles' must be an array of width x height = %zu numbers", count);
                detail::error_at(diags, Errc::ParseError, file, arr, kStage, msg);
                return false;
            }
            for (u32 i = 0; i < arr.size(); ++i) {
                const JsonValue t = arr.at(i);
                if (!t.is_integer() || t.as_number() < 0 || t.as_number() > cells) {
                    String msg;
                    appendf(msg, "tile must be 0 (empty) to %u (the tileset's cell count)", cells);
                    detail::error_at(diags, Errc::ParseError, file, t, kStage, msg);
                    return false;
                }
                tiles[i] = static_cast<u32>(t.as_number());
            }
        }

        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::Tilemap;
        d.version = asset::chunk_version(asset::TILEMAP_VERSION, 0);
        d.guid = in.guid;
        out.add(d, payload.span());
        return true;
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_tilemap_importer() { return mem::make_unique<TilemapImporter>(mem::heap()); }

}  // namespace mce::cook

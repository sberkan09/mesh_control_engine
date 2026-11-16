#include "mce/editor/tilemap_editor.h"

#include <cmath>
#include <limits>
#include <utility>

#include "mce/cook/cook.h"
#include "mce/cook/fs.h"
#include "mce/cook/importer.h"
#include "mce/cook/json.h"
#include "mce/kit/components.h"
#include "mce/log.h"

namespace mce::editor {

namespace {

constexpr const char* kStage = "editor.tilemap";
constexpr u32 kMaxSide = 4096;  // the cook's limit

bool read_u32(cook::JsonValue root, StringView key, u32 max, u32& out, StringView file, cook::DiagList& diags) {
    const cook::JsonValue v = root.get(key);
    if (!v.is_integer() || v.as_number() < 1.0 || v.as_number() > static_cast<f64>(max)) {
        cook::String msg;
        cook::appendf(msg, "'%.*s' must be a whole number from 1 to %u", static_cast<int>(key.size()), key.data(), max);
        const cook::JsonValue at = v.valid() ? v : root;
        diags.error(Errc::ParseError, file, at.line(), at.col(), kStage, msg.view());
        return false;
    }
    out = static_cast<u32>(v.as_number());
    return true;
}

}  // namespace

// ---- TilemapDocument ----

TilemapDocument TilemapDocument::make(asset::Guid guid, StringView name, asset::Guid tileset, u32 columns, u32 rows,
                                      u32 width, u32 height, f32 tile_size) {
    TilemapDocument d;
    d.guid = guid;
    d.name = cook::String(name);
    d.tileset = tileset;
    d.columns = columns < 1 ? 1 : columns;
    d.rows = rows < 1 ? 1 : rows;
    d.width = width < 1 ? 1 : (width > kMaxSide ? kMaxSide : width);
    d.height = height < 1 ? 1 : (height > kMaxSide ? kMaxSide : height);
    d.tile_size = tile_size > 0.0f ? tile_size : 1.0f;
    d.tiles.resize(usize(d.width) * d.height, 0u);
    return d;
}

bool TilemapDocument::parse(StringView text, StringView file, cook::DiagList& diags) {
    cook::JsonDoc json;
    cook::JsonError err;
    if (!json.parse(text, err)) {
        diags.error(Errc::ParseError, file, err.line, err.col, kStage, err.message.view());
        return false;
    }
    const cook::JsonValue root = json.root();
    const cook::JsonValue fmt = root.get("format");
    if (!root.is_object() || !fmt.is_string() || fmt.as_string() != "mce-tilemap") {
        diags.error(Errc::ParseError, file, root.line(), root.col(), kStage, "\"format\" must be \"mce-tilemap\"");
        return false;
    }
    const u32 before = diags.error_count();
    TilemapDocument d;
    if (const cook::JsonValue g = root.get("guid"); g.is_string()) {
        if (Result<asset::Guid> r = asset::parse_guid(g.as_string())) d.guid = r.value();
    }
    if (const cook::JsonValue n = root.get("name"); n.is_string()) d.name = cook::String(n.as_string());
    if (const cook::JsonValue t = root.get("tileset"); t.is_string()) cook::parse_guid_ref(t.as_string(), d.tileset);
    read_u32(root, "columns", 256, d.columns, file, diags);
    read_u32(root, "rows", 256, d.rows, file, diags);
    read_u32(root, "width", kMaxSide, d.width, file, diags);
    read_u32(root, "height", kMaxSide, d.height, file, diags);
    if (const cook::JsonValue ts = root.get("tile_size"); ts.is_number()) {
        // A size that does not fit an f32 (or underflows to 0) would write back as "inf" or 0, which no reader takes.
        const f32 size = static_cast<f32>(ts.as_number());
        if (size > 0.0f && size <= std::numeric_limits<f32>::max()) d.tile_size = size;
    }
    if (diags.error_count() != before) return false;
    d.tiles.resize(usize(d.width) * d.height, 0u);
    if (const cook::JsonValue arr = root.get("tiles"); arr.valid()) {
        if (!arr.is_array() || arr.size() != d.tiles.size()) {
            diags.error(Errc::ParseError, file, arr.line(), arr.col(), kStage, "'tiles' must hold width x height numbers");
            return false;
        }
        for (u32 i = 0; i < arr.size(); ++i) {
            const cook::JsonValue t = arr.at(i);
            if (!t.is_integer() || t.as_number() < 0.0 || t.as_number() > static_cast<f64>(d.cells())) {
                diags.error(Errc::ParseError, file, t.line(), t.col(), kStage, "tile out of the tileset's range");
                return false;
            }
            d.tiles[i] = static_cast<u32>(t.as_number());
        }
    }
    *this = static_cast<TilemapDocument&&>(d);
    return true;
}

void TilemapDocument::serialize(cook::String& out) const {
    out += "{\n  \"format\": \"mce-tilemap\",\n  \"version\": 1,\n  \"guid\": \"";
    out += asset::to_string(guid).view();
    out += "\",\n  \"name\": ";
    cook::append_json_string(out, name.view());
    out += ",\n  \"tileset\": ";
    if (tileset.is_null()) {
        out += "null";
    } else {
        out += "\"guid:";
        out += asset::to_string(tileset).view();
        out += "\"";
    }
    cook::appendf(out, ",\n  \"columns\": %u,\n  \"rows\": %u,\n  \"width\": %u,\n  \"height\": %u,\n  \"tile_size\": ",
                  columns, rows, width, height);
    cook::append_json_number(out, static_cast<f64>(tile_size));
    out += ",\n  \"tiles\": [";
    for (u32 y = 0; y < height; ++y) {
        out += y == 0 ? "\n    " : ",\n    ";
        for (u32 x = 0; x < width; ++x) cook::appendf(out, x == 0 ? "%u" : ", %u", tiles[usize(y) * width + x]);
    }
    out += "\n  ]\n}\n";
}

bool TilemapDocument::set(i32 x, i32 y, u32 tile) {
    if (!contains(x, y) || tile > cells()) return false;
    u32& t = tiles[usize(y) * width + u32(x)];
    if (t == tile) return false;
    t = tile;
    return true;
}

u32 TilemapDocument::fill_rect(i32 x0, i32 y0, i32 x1, i32 y1, u32 tile) {
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    const i32 xmax = static_cast<i32>(width) - 1, ymax = static_cast<i32>(height) - 1;
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > xmax ? xmax : x1;
    y1 = y1 > ymax ? ymax : y1;
    u32 changed = 0;
    for (i32 y = y0; y <= y1; ++y)
        for (i32 x = x0; x <= x1; ++x) changed += set(x, y, tile) ? 1u : 0u;
    return changed;
}

// ---- TilemapEditor ----

Result<asset::Guid> TilemapEditor::create(EditorSession& session, EntityId entity, StringView rel_path, asset::Guid tileset,
                                          u32 columns, u32 rows, u32 width, u32 height, f32 tile_size) {
    EditorScene& scene = session.scene();
    if (!scene.doc().find(entity)) return Error(Errc::NotFound, kStage, "no such entity");
    if (rel_path.size() < 10 || rel_path.substr(rel_path.size() - 10) != ".mctilemap")
        return Error(Errc::InvalidArgument, kStage, "a tilemap file ends in .mctilemap");
    if (!session.project().is_open()) return Error(Errc::InvalidArgument, kStage, "no project is open");
    u64 size = 0;
    i64 mtime = 0;
    if (cook::fs::stat(session.project().asset_path(rel_path).view(), size, mtime)) return Error(Errc::AlreadyExists, kStage, rel_path);

    const usize slash = rel_path.rfind('/');
    StringView name = slash == StringView::npos ? rel_path : rel_path.substr(slash + 1);
    name = name.substr(0, name.size() - 10);
    const TilemapDocument doc = TilemapDocument::make(cook::generate_guid(), name, tileset, columns, rows, width, height, tile_size);
    cook::String text;
    doc.serialize(text);
    Result<HotReloadResult> r = session.write_asset(rel_path, text.view());
    if (!r) return r.error();
    if (!session.project().find(doc.guid)) return Error(Errc::ParseError, kStage, "the new tilemap did not cook (see the console)");

    // One undo entry: the layer component and its map reference.
    scene.begin_edit("Create tilemap");
    if (!scene.get<kit::TilemapLayer>(entity)) scene.add_component(entity, refl::type_of<kit::TilemapLayer>());
    kit::TilemapLayer layer = *scene.get<kit::TilemapLayer>(entity);
    layer.map.guid = doc.guid;
    scene.set(entity, layer);
    scene.end_edit();
    return doc.guid;
}

bool TilemapEditor::bind(EditorSession& session, EntityId entity) {
    const kit::TilemapLayer* layer = entity ? session.scene().get<kit::TilemapLayer>(entity) : nullptr;
    const ProjectAsset* a = layer && !layer->map.guid.is_null() ? session.project().find(layer->map.guid) : nullptr;
    if (!a || a->type != asset::ChunkType::Tilemap) {
        unbind();
        return false;
    }
    if (entity == entity_ && layer->map.guid == map_) return true;
    const cook::String path = session.project().asset_path(a->path.view());
    cook::String text;
    cook::DiagList diags;
    TilemapDocument d;
    if (!cook::fs::read_text(path.view(), text) || !d.parse(text.view(), a->path.view(), diags)) {
        MCE_LOG_WARN("editor: tilemap {} cannot be read", a->path.view());
        unbind();
        return false;
    }
    unbind();
    entity_ = entity;
    map_ = layer->map.guid;
    path_ = path;
    doc_ = static_cast<TilemapDocument&&>(d);
    return true;
}

void TilemapEditor::unbind() {
    entity_ = kNoEntity;
    map_ = {};
    path_ = cook::String();
    doc_ = TilemapDocument{};
    stroking_ = changed_ = false;
    undo_.clear();
    redo_.clear();
}

bool TilemapEditor::tile_at(const EditorScene& scene, math::vec2 world, i32& x, i32& y) const {
    if (!bound()) return false;
    const WorldTransform w = scene.world_transform(entity_);
    x = static_cast<i32>(std::floor((world.x - w.position.x) / doc_.tile_size));
    y = static_cast<i32>(std::floor((world.y - w.position.y) / doc_.tile_size));
    return doc_.contains(x, y);
}

void TilemapEditor::tile_rect(const EditorScene& scene, i32 x, i32 y, math::vec2& lo, math::vec2& hi) const {
    const WorldTransform w = scene.world_transform(entity_);
    lo = {w.position.x + static_cast<f32>(x) * doc_.tile_size, w.position.y + static_cast<f32>(y) * doc_.tile_size};
    hi = {lo.x + doc_.tile_size, lo.y + doc_.tile_size};
}

void TilemapEditor::lines(const EditorScene& scene, Array<OverlayLine>& out) const {
    if (!bound()) return;
    math::vec2 lo, hi;
    tile_rect(scene, 0, 0, lo, hi);
    const f32 ts = doc_.tile_size;
    const f32 w = static_cast<f32>(doc_.width) * ts, h = static_cast<f32>(doc_.height) * ts;
    const math::Color grid{0.55f, 0.85f, 0.55f, 0.25f}, edge{0.55f, 0.85f, 0.55f, 0.8f};
    if (doc_.width <= 256 && doc_.height <= 256) {
        for (u32 x = 1; x < doc_.width; ++x) {
            const f32 gx = lo.x + static_cast<f32>(x) * ts;
            out.push_back(OverlayLine{{gx, lo.y}, {gx, lo.y + h}, grid, 1.0f});
        }
        for (u32 y = 1; y < doc_.height; ++y) {
            const f32 gy = lo.y + static_cast<f32>(y) * ts;
            out.push_back(OverlayLine{{lo.x, gy}, {lo.x + w, gy}, grid, 1.0f});
        }
    }
    const math::vec2 q[4] = {lo, {lo.x + w, lo.y}, {lo.x + w, lo.y + h}, {lo.x, lo.y + h}};
    quad_lines(q, edge, 1.5f, out);
}

void TilemapEditor::begin_stroke() {
    if (!bound() || stroking_) return;
    before_ = doc_.tiles;
    stroking_ = true;
    changed_ = false;
}

bool TilemapEditor::paint(i32 x, i32 y, u32 tile) {
    if (!stroking_) return false;
    const bool c = doc_.set(x, y, tile);
    changed_ = changed_ || c;
    return c;
}

u32 TilemapEditor::fill(i32 x0, i32 y0, i32 x1, i32 y1, u32 tile) {
    if (!stroking_) return 0;
    const u32 n = doc_.fill_rect(x0, y0, x1, y1, tile);
    changed_ = changed_ || n > 0;
    return n;
}

Result<f64> TilemapEditor::end_stroke(EditorSession& session) {
    if (!stroking_) return 0.0;
    stroking_ = false;
    if (!changed_) return 0.0;
    undo_.push_back(static_cast<Array<u32>&&>(before_));
    before_ = Array<u32>();
    redo_.clear();
    // Bound the history: each entry is a whole grid.
    const usize entry = doc_.tiles.size() * sizeof(u32);
    while (undo_.size() > 1 && undo_.size() * entry > kMaxHistoryBytes) undo_.erase(0);
    Result<HotReloadResult> r = session.write_asset(rel_path(session).view(), text().view());
    if (!r) return r.error();
    return r.value().seconds;
}

bool TilemapEditor::undo(EditorSession& session) {
    if (stroking_ || undo_.empty()) return false;
    redo_.push_back(static_cast<Array<u32>&&>(doc_.tiles));
    doc_.tiles = static_cast<Array<u32>&&>(undo_.back());
    undo_.pop_back();
    return static_cast<bool>(session.write_asset(rel_path(session).view(), text().view()));
}

bool TilemapEditor::redo(EditorSession& session) {
    if (stroking_ || redo_.empty()) return false;
    undo_.push_back(static_cast<Array<u32>&&>(doc_.tiles));
    doc_.tiles = static_cast<Array<u32>&&>(redo_.back());
    redo_.pop_back();
    return static_cast<bool>(session.write_asset(rel_path(session).view(), text().view()));
}

cook::String TilemapEditor::rel_path(const EditorSession& session) const {
    cook::String rel;
    session.project().relative_asset_path(path_.view(), rel);
    return rel;
}

cook::String TilemapEditor::text() const {
    cook::String t;
    doc_.serialize(t);
    return t;
}

}  // namespace mce::editor

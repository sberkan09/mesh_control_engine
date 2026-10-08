#pragma once
// Tilemap authoring (ROADMAP M6, FR-EDT-120 / FR-R2D-115, editor-tools.md §2.7, ADR-0030 §8).
//
//   TilemapDocument  the .mctilemap text asset: a width x height grid of tile indices over a
//                    tileset texture cut into columns x rows cells (0 = empty, n = cell n-1).
//   TilemapEditor    what the Tilemap panel and the viewport's paint tool drive: binds to the
//                    selected entity's TilemapLayer, paints single tiles and rectangle fills in
//                    strokes, and on stroke end saves the asset and hot-reloads it, so the cook
//                    re-bakes CHNK_TILE and the viewport shows it (no restart). Strokes undo and
//                    redo here (the tiles live in the asset, not in the scene document).
// Headless: tested in CI; the panel draws it.

#include "mce/cook/string.h"
#include "mce/editor/editor_session.h"
#include "mce/editor/preview_renderer.h"
#include "mce/math/math.h"

namespace mce::editor {

class TilemapDocument {
public:
    asset::Guid guid;
    cook::String name;
    asset::Guid tileset;  // texture
    u32 columns = 1, rows = 1;
    u32 width = 1, height = 1;
    f32 tile_size = 1.0f;
    Array<u32> tiles;  // width x height, row 0 = bottom

    static TilemapDocument make(asset::Guid guid, StringView name, asset::Guid tileset, u32 columns, u32 rows, u32 width,
                                u32 height, f32 tile_size);
    bool parse(StringView text, StringView file, cook::DiagList& diags);
    // One row of tiles per line, bottom row first (the chunk's order).
    void serialize(cook::String& out) const;

    u32 cells() const { return columns * rows; }
    bool contains(i32 x, i32 y) const { return x >= 0 && y >= 0 && u32(x) < width && u32(y) < height; }
    u32 at(i32 x, i32 y) const { return contains(x, y) ? tiles[usize(y) * width + u32(x)] : 0; }
    // False when outside the map, out of the tileset's range, or unchanged.
    bool set(i32 x, i32 y, u32 tile);
    // Fills the rectangle between two corners (inclusive, clipped to the map); tiles changed.
    u32 fill_rect(i32 x0, i32 y0, i32 x1, i32 y1, u32 tile);
};

class TilemapEditor {
public:
    static constexpr usize kMaxHistoryBytes = 32 * 1024 * 1024;

    // Creates a new .mctilemap at `rel_path` (under assets/), cooks it, and gives `entity` a
    // TilemapLayer drawing it (one scene undo entry). Returns the new asset's GUID.
    static Result<asset::Guid> create(EditorSession& session, EntityId entity, StringView rel_path, asset::Guid tileset,
                                      u32 columns, u32 rows, u32 width, u32 height, f32 tile_size);

    // Follows the selection: binds to `entity`'s TilemapLayer map (re-reading the asset when it
    // changed). False when the entity has none, or the asset cannot be read.
    bool bind(EditorSession& session, EntityId entity);
    bool bound() const { return !map_.is_null(); }
    void unbind();
    EntityId entity() const { return entity_; }
    asset::Guid map() const { return map_; }
    const TilemapDocument& doc() const { return doc_; }

    // The tile under a scene point (the map's tile (0, 0) corner sits at the entity's position).
    bool tile_at(const EditorScene& scene, math::vec2 world, i32& x, i32& y) const;
    // The map's tile (x, y) rectangle in scene space (bottom-left, top-right).
    void tile_rect(const EditorScene& scene, i32 x, i32 y, math::vec2& lo, math::vec2& hi) const;

    // The map's outline and tile grid as viewport overlay lines (outline only past 256 tiles a side).
    void lines(const EditorScene& scene, Array<OverlayLine>& out) const;

    // A stroke groups paints and fills into one undo step and one save.
    void begin_stroke();
    bool paint(i32 x, i32 y, u32 tile);
    u32 fill(i32 x0, i32 y0, i32 x1, i32 y1, u32 tile);
    // Saves and hot-reloads when the stroke changed anything; returns the reload's wall time.
    Result<f64> end_stroke(EditorSession& session);
    bool in_stroke() const { return stroking_; }

    bool undo(EditorSession& session);
    bool redo(EditorSession& session);
    bool can_undo() const { return !undo_.empty(); }
    bool can_redo() const { return !redo_.empty(); }

    u32 brush = 1;  // the tile the paint tool lays (0 erases)

private:
    cook::String rel_path(const EditorSession& session) const;
    cook::String text() const;

    EntityId entity_ = kNoEntity;
    asset::Guid map_;
    cook::String path_;  // absolute
    TilemapDocument doc_;
    Array<u32> before_;  // tiles at stroke start
    bool stroking_ = false;
    bool changed_ = false;
    Array<Array<u32>> undo_, redo_;  // whole tile grids (bounded by kMaxHistoryBytes)
};

}  // namespace mce::editor

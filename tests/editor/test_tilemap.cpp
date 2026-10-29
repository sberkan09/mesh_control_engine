// Tilemap panel core (ROADMAP M6, FR-EDT-120 / FR-R2D-115, ADR-0030 §8), headless: a new
// .mctilemap is created and cooked, an entity draws it through TilemapLayer, paint strokes and
// rectangle fills save and re-bake the asset in the live mount in under a second, strokes undo
// and redo, and the cook reports a bad tilemap with file:line.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "cook_test_util.h"
#include "mce/asset/formats.h"
#include "mce/editor/editor_session.h"
#include "mce/editor/tilemap_editor.h"
#include "mce/kit/components.h"

using namespace mce;
using namespace mce::editor;

namespace {

constexpr const char* kCrate = "5ce4e000-0000-4000-8000-000000000001";

asset::Guid crate() { return asset::parse_guid(kCrate).value(); }

std::string open_demo(EditorSession& session, const char* name) {
    const std::string root = cook_test::fresh_dir(name);
    std::filesystem::copy(std::string(MCE_SOURCE_DIR) + "/samples/scene_demo/assets", root + "/assets",
                          std::filesystem::copy_options::recursive);
    REQUIRE(session.open_project(StringView(root.data(), root.size())));
    return root;
}

// The tiles the live mount serves for `map` right now.
Array<u32> mounted_tiles(EditorSession& session, asset::Guid map) {
    Array<u32> out;
    asset::AssetSystem* assets = session.project().mounted();
    const asset::AssetKey key = assets ? assets->find(map) : asset::kInvalidAssetKey;
    if (key == asset::kInvalidAssetKey) return out;
    Result<asset::AssetHandle<asset::Tilemap>> h = assets->load_sync<asset::Tilemap>(key);
    if (!h) return out;
    if (Result<asset::TilemapView> v = asset::TilemapView::read(assets->resolve(h.value())))
        for (u32 t : v.value().tiles) out.push_back(t);
    assets->release(h.value());
    return out;
}

}  // namespace

TEST_CASE("tilemap: the document round-trips; set and fill clip to the map and the tileset") {
    TilemapDocument d = TilemapDocument::make(crate(), "level", crate(), 4, 2, 6, 3, 0.5f);
    CHECK(d.tiles.size() == 18);
    CHECK(d.set(0, 0, 3));
    CHECK_FALSE(d.set(0, 0, 3));   // unchanged
    CHECK_FALSE(d.set(6, 0, 1));   // outside
    CHECK_FALSE(d.set(1, 1, 9));   // beyond the 8 cells
    CHECK(d.fill_rect(4, 2, 9, -3, 2) == 6);  // corners in any order, clipped to x 4..5, y 0..2
    CHECK(d.at(5, 2) == 2);
    cook::String text;
    d.serialize(text);
    TilemapDocument back;
    cook::DiagList diags;
    REQUIRE(back.parse(text.view(), "level.mctilemap", diags));
    REQUIRE(back.tiles.size() == d.tiles.size());
    bool same = true;
    for (usize i = 0; i < d.tiles.size(); ++i) same = same && back.tiles[i] == d.tiles[i];
    CHECK(same);
    CHECK(back.tileset == crate());
    CHECK(back.tile_size == 0.5f);
    CHECK_FALSE(back.parse("{ \"format\": \"mce-tilemap\", \"width\": 0 }", "x.mctilemap", diags));
    CHECK(diags.error_count() > 0);
}

TEST_CASE("tilemap: a tile size that does not fit a float keeps the default and the file reads back") {
    TilemapDocument d;
    cook::DiagList diags;
    REQUIRE(d.parse(R"({ "format": "mce-tilemap", "columns": 1, "rows": 1, "width": 2, "height": 1, "tile_size": 1e300 })", "big.mctilemap", diags));
    CHECK(d.tile_size == 1.0f);
    REQUIRE(d.parse(R"({ "format": "mce-tilemap", "columns": 1, "rows": 1, "width": 2, "height": 1, "tile_size": 1e-300 })", "tiny.mctilemap", diags));
    CHECK(d.tile_size == 1.0f);
    cook::String text;
    d.serialize(text);
    TilemapDocument back;
    CHECK(back.parse(text.view(), "back.mctilemap", diags));
}

TEST_CASE("tilemap: create, paint, fill -> saved and re-baked in the live mount; strokes undo and redo") {
    EditorSession session;
    open_demo(session, "tilemap_paint");
    EditorScene& s = session.scene();
    const EntityId e = s.create_entity("Tiles");
    LocalTransform t;
    t.position = {-2.0f, 1.0f, 0.0f};
    s.set(e, t);
    s.clear_undo();

    Result<asset::Guid> map = TilemapEditor::create(session, e, "maps/level1.mctilemap", crate(), 2, 2, 8, 4, 0.5f);
    REQUIRE(map);
    CHECK(s.undo_count() == 1);
    CHECK(s.get<kit::TilemapLayer>(e)->map.guid == map.value());
    CHECK(mounted_tiles(session, map.value()).size() == 32);
    CHECK(TilemapEditor::create(session, e, "maps/level1.mctilemap", crate(), 2, 2, 8, 4, 0.5f).error().code ==
          Errc::AlreadyExists);

    TilemapEditor ed;
    REQUIRE(ed.bind(session, e));
    i32 x = 0, y = 0;
    CHECK(ed.tile_at(s, {-1.2f, 1.6f}, x, y));  // (0.8, 0.6) from the corner at 0.5 m tiles
    CHECK(x == 1);
    CHECK(y == 1);
    CHECK_FALSE(ed.tile_at(s, {-2.1f, 1.0f}, x, y));

    // A resident handle goes stale on re-bake: that is what makes the kit rebuild its tilemap.
    asset::AssetSystem* assets = session.project().mounted();
    Result<asset::AssetHandle<asset::Tilemap>> held = assets->load_sync<asset::Tilemap>(assets->find(map.value()));
    REQUIRE(held);

    ed.begin_stroke();
    CHECK(ed.paint(1, 1, 3));
    CHECK(ed.paint(2, 1, 3));
    Result<f64> took = ed.end_stroke(session);
    REQUIRE(took);
    CHECK(took.value() < 1.0);
    MESSAGE("tilemap stroke save + re-bake: " << took.value() * 1000.0 << " ms");
    Array<u32> live = mounted_tiles(session, map.value());
    REQUIRE(live.size() == 32);
    CHECK(live[1 * 8 + 1] == 3);
    CHECK(live[1 * 8 + 2] == 3);
    CHECK_FALSE(session.project().mounted()->is_alive(held.value()));
    session.project().mounted()->release(held.value());

    ed.begin_stroke();
    CHECK(ed.fill(0, 0, 7, 0, 4) == 8);  // the bottom row
    REQUIRE(ed.end_stroke(session));
    CHECK(mounted_tiles(session, map.value())[7] == 4);

    // An empty stroke saves nothing and adds no undo step.
    ed.begin_stroke();
    CHECK_FALSE(ed.paint(0, 0, 4));
    REQUIRE(ed.end_stroke(session));

    REQUIRE(ed.undo(session));
    CHECK(mounted_tiles(session, map.value())[7] == 0);
    CHECK(mounted_tiles(session, map.value())[9] == 3);
    REQUIRE(ed.undo(session));
    CHECK(mounted_tiles(session, map.value())[9] == 0);
    CHECK_FALSE(ed.can_undo());
    REQUIRE(ed.redo(session));
    REQUIRE(ed.redo(session));
    CHECK(mounted_tiles(session, map.value())[7] == 4);
    CHECK(mounted_tiles(session, map.value())[10] == 3);

    // The scene saves only the reference; the tiles live in the asset.
    REQUIRE(session.save_scene_as("scenes/tiles.mcscene"));
    std::ifstream f(std::string(session.scene().path()));
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(text.find("TilemapLayer") != std::string::npos);
    CHECK(text.find(std::string(asset::to_string(map.value()).view())) != std::string::npos);

    // The scene cooks (the TilemapLayer reference resolves to a Tilemap) and plays.
    CHECK(session.cook().diags.error_count() == 0);
    REQUIRE(session.enter_play());
    session.stop_play();
}

TEST_CASE("tilemap: a tile beyond the tileset is a cook error with file:line") {
    EditorSession session;
    const std::string root = open_demo(session, "tilemap_bad");
    std::filesystem::create_directories(root + "/assets/maps");
    {
        std::ofstream f(root + "/assets/maps/bad.mctilemap", std::ios::binary | std::ios::trunc);
        f << "{ \"format\": \"mce-tilemap\", \"version\": 1, \"guid\": \"5ce4e000-0000-4000-8000-0000000000a1\",\n"
             "  \"tileset\": \"guid:" << kCrate << "\", \"columns\": 2, \"rows\": 1, \"width\": 2, \"height\": 1,\n"
             "  \"tiles\": [1, 7] }\n";
    }
    session.hot_reload();
    bool found = false;
    for (const cook::Diagnostic& d : session.project().last_report().diags.all())
        if (d.file.view().find("bad.mctilemap") != StringView::npos && d.line == 3) found = true;
    CHECK(found);
}

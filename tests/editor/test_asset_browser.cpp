// The asset browser (ROADMAP M6, ADR-0030 §7), headless: the catalog as folders and filtered
// entries, what a drop makes (a sprite, a prefab instance), and GUID-stable rename / move — the
// scene that uses the asset still resolves it, and an open scene that moves keeps its document.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "cook_test_util.h"
#include "mce/editor/asset_browser.h"
#include "mce/editor/editor_session.h"
#include "mce/kit/components.h"

using namespace mce;
using namespace mce::editor;

namespace {

constexpr const char* kCrate = "5ce4e000-0000-4000-8000-000000000001";

std::string browser_project(const char* name) {
    const std::string root = cook_test::fresh_dir(name);
    std::filesystem::copy(std::string(MCE_SOURCE_DIR) + "/samples/scene_demo/assets", root + "/assets",
                          std::filesystem::copy_options::recursive);
    std::filesystem::create_directories(root + "/assets/prefabs/props");
    std::ofstream(root + "/assets/prefabs/props/crate_stack.mcprefab")
        << R"({ "format": "mce-prefab", "version": 1, "guid": "5ce4e000-0000-4000-8000-0000000000f3", "name": "crate_stack",
  "entities": [ { "name": "Crate", "components": { "LocalTransform": {}, "Sprite": { "texture": "guid:)" << kCrate
        << R"(" } } } ] })";
    return root;
}

const ProjectAsset* by_path(const EditorProject& p, StringView rel) {
    for (const ProjectAsset& a : p.assets())
        if (a.path.view() == rel) return &a;
    return nullptr;
}

}  // namespace

TEST_CASE("asset browser: folders, the current folder's entries, search and type filter") {
    const std::string root = browser_project("browser_model");
    EditorSession session;
    REQUIRE(session.open_project(StringView(root.data(), root.size())));
    AssetBrowserModel m;
    m.refresh(session.project());
    Array<StringView> top;
    m.subfolders(StringView(), top);
    REQUIRE(top.size() == 4);  // fonts, prefabs, scenes, textures
    CHECK(top[0] == "fonts");
    Array<StringView> sub;
    m.subfolders("prefabs", sub);
    REQUIRE(sub.size() == 1);
    CHECK(sub[0] == "prefabs/props");

    Array<const ProjectAsset*> list;
    m.entries(session.project(), list);
    CHECK(list.empty());  // nothing directly under assets/
    m.set_folder("textures");
    m.entries(session.project(), list);
    REQUIRE(list.size() == 3);
    CHECK(list[0]->path.view() == "textures/ball.png");

    list.clear();
    m.set_filter("CRATE");  // case-insensitive, every folder
    m.entries(session.project(), list);
    CHECK(list.size() == 2);  // crate.png and crate_stack.mcprefab
    list.clear();
    m.set_type("Prefab");
    m.entries(session.project(), list);
    REQUIRE(list.size() == 1);
    CHECK(asset_file_name(list[0]->path.view()) == "crate_stack.mcprefab");
    CHECK(asset_folder(list[0]->path.view()) == "prefabs/props");
}

TEST_CASE("asset browser: dropping a texture makes a sprite, a prefab an instance; one undo step each") {
    const std::string root = browser_project("browser_drop");
    EditorSession session;
    REQUIRE(session.open_project(StringView(root.data(), root.size())));
    EditorScene& s = session.scene();
    s.clear_undo();
    const ProjectAsset* crate = by_path(session.project(), "textures/crate.png");
    const ProjectAsset* stack = by_path(session.project(), "prefabs/props/crate_stack.mcprefab");
    REQUIRE(crate);
    REQUIRE(stack);
    CHECK(asset_drop_kind(crate->type) == AssetDrop::Sprite);
    CHECK(asset_drop_kind(stack->type) == AssetDrop::PrefabInstance);

    const EntityId sprite = drop_asset(s, *crate, {2, 3});
    REQUIRE(sprite != kNoEntity);
    CHECK(s.doc().find(sprite)->name.view() == "crate");
    REQUIRE(s.get<kit::Sprite>(sprite));
    CHECK(s.get<kit::Sprite>(sprite)->texture.guid == crate->guid);
    CHECK(s.get<LocalTransform>(sprite)->position.y == 3.0f);
    CHECK(s.selection().anchor() == sprite);
    CHECK(s.undo_count() == 1);

    const EntityId inst = drop_asset(s, *stack, {0, 0}, sprite);  // dropped on a hierarchy row: a child
    REQUIRE(inst != kNoEntity);
    CHECK(s.doc().find(inst)->prefab == stack->guid);
    CHECK(s.parent_of(inst) == sprite);
    CHECK(s.ghost_count() == 1);  // the prefab's content shows in the preview
    CHECK(s.undo_count() == 2);
    REQUIRE(s.undo());
    CHECK_FALSE(s.doc().find(inst));
    CHECK(s.ghost_count() == 0);
    CHECK(s.verify_world());

    const ProjectAsset* scene = by_path(session.project(), "scenes/demo.mcscene");
    REQUIRE(scene);
    CHECK(asset_drop_kind(scene->type) == AssetDrop::OpenScene);
    CHECK(drop_asset(s, *scene, {0, 0}) == kNoEntity);
}

TEST_CASE("asset browser: rename / move keeps the GUID; users still resolve it; the open scene follows") {
    const std::string root = browser_project("browser_rename");
    EditorSession session;
    REQUIRE(session.open_project(StringView(root.data(), root.size())));
    REQUIRE(session.open_scene("scenes/demo.mcscene"));
    const asset::Guid crate = by_path(session.project(), "textures/crate.png")->guid;

    REQUIRE(session.rename_asset(crate, "textures/props/wooden_crate.png"));
    CHECK(std::filesystem::exists(root + "/assets/textures/props/wooden_crate.png"));
    CHECK(std::filesystem::exists(root + "/assets/textures/props/wooden_crate.png.mceasset"));  // the sidecar moved too
    CHECK_FALSE(std::filesystem::exists(root + "/assets/textures/crate.png"));
    const ProjectAsset* moved = session.project().find(crate);
    REQUIRE(moved);
    CHECK(moved->path.view() == "textures/props/wooden_crate.png");
    // The demo scene references the crate by GUID: it still cooks and plays.
    session.cook();
    CHECK(session.project().last_report().failed == 0);
    REQUIRE(session.enter_play());
    session.stop_play();

    // Refused: another extension, a path outside assets/, an existing file.
    CHECK_FALSE(session.rename_asset(crate, "textures/crate.jpg"));
    CHECK_FALSE(session.rename_asset(crate, "../crate.png"));
    CHECK_FALSE(session.rename_asset(crate, "textures/ball.png"));

    // The open scene's own file moves: the document stays, unsaved edits included, and saves there.
    session.scene().create_entity("Unsaved");
    const asset::Guid demo = by_path(session.project(), "scenes/demo.mcscene")->guid;
    REQUIRE(session.rename_asset(demo, "scenes/levels/level1.mcscene"));
    const std::string now(session.scene().path().data(), session.scene().path().size());
    CHECK(now.find("scenes/levels/level1.mcscene") != std::string::npos);
    CHECK(session.scene().dirty());
    REQUIRE(session.scene().save(session.scene().path()));
    CHECK(std::filesystem::exists(root + "/assets/scenes/levels/level1.mcscene"));
    CHECK_FALSE(std::filesystem::exists(root + "/assets/scenes/demo.mcscene"));
}

// Prefab content in the viewport (ROADMAP M6, ADR-0030 §6), headless: a prefab instance in the
// open scene shows its prefab's entities in the preview world as ghosts placed at the instance,
// they follow the instance, the instance's components override the prefab root, the document
// never sees them, and a prefab edited on disk shows up after hot reload.

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include "cook_test_util.h"
#include "mce/ecs/hierarchy.h"
#include "mce/editor/editor_session.h"
#include "mce/kit/components.h"

using namespace mce;
using namespace mce::editor;

namespace {

constexpr const char* kStack = "5ce4e000-0000-4000-8000-0000000000f1";
constexpr const char* kCrate = "5ce4e000-0000-4000-8000-000000000001";

void put(const std::string& path, const std::string& text) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << text;
}

std::string stack_prefab(f32 top_y) {
    return std::string(R"({ "format": "mce-prefab", "version": 1, "guid": ")") + kStack + R"(", "name": "stack",
  "entities": [
    { "name": "Bottom", "components": {
        "LocalTransform": { "position": [0, 0, 0] },
        "Sprite": { "texture": "guid:)" + kCrate + R"(" } } },
    { "name": "Top", "components": {
        "LocalTransform": { "position": [0, )" + std::to_string(top_y) + R"(, 0] },
        "ChildOf": { "parent": "$0" },
        "Sprite": { "texture": "guid:)" + kCrate + R"(", "order": 1 } } }
  ] })";
}

std::string instance_scene() {
    return std::string(R"({ "format": "mce-scene", "version": 1, "guid": "5ce4e000-0000-4000-8000-0000000000f2",
  "name": "inst", "entities": [
    { "name": "Stack A", "prefab": "guid:)") + kStack + R"(", "components": {
        "LocalTransform": { "position": [3, 0, 0] } } }
  ] })";
}

bool close(f32 a, f32 b) { return std::fabs(a - b) <= 1e-4f; }

// The ghosts' WorldTransforms, ordered by y.
void ghost_positions(EditorScene& s, Array<math::vec3>& out) {
    out.clear();
    const Entity inst = s.runtime_entity(s.doc().at(0).id);
    s.world().query<const WorldTransform, const kit::Sprite>().each([&](Entity e, const WorldTransform& t, const kit::Sprite&) {
        if (e != inst) out.push_back(t.position);
    });
    if (out.size() == 2 && out[0].y > out[1].y) std::swap(out[0], out[1]);
}

}  // namespace

TEST_CASE("prefab preview: an instance shows its prefab's entities, placed at the instance") {
    const std::string root = cook_test::fresh_dir("prefab_preview");
    std::filesystem::copy(std::string(MCE_SOURCE_DIR) + "/samples/scene_demo/assets", root + "/assets",
                          std::filesystem::copy_options::recursive);
    put(root + "/assets/prefabs/stack.mcprefab", stack_prefab(1.0f));
    put(root + "/assets/scenes/inst.mcscene", instance_scene());

    EditorSession session;
    REQUIRE(session.open_project(StringView(root.data(), root.size())));
    REQUIRE(session.open_scene("scenes/inst.mcscene"));
    EditorScene& s = session.scene();
    CHECK(s.doc().size() == 1);  // the document holds only the instance
    CHECK(s.ghost_count() == 2);
    CHECK(s.verify_world());
    Array<math::vec3> at;
    ghost_positions(s, at);
    REQUIRE(at.size() == 2);
    CHECK(close(at[0].x, 3.0f));
    CHECK(close(at[0].y, 0.0f));
    CHECK(close(at[1].x, 3.0f));
    CHECK(close(at[1].y, 1.0f));

    // The ghosts follow the instance; undo puts them back.
    const EntityId inst = s.doc().at(0).id;
    LocalTransform t = *s.get<LocalTransform>(inst);
    t.position.x = -2.0f;
    s.set(inst, t);
    ghost_positions(s, at);
    REQUIRE(at.size() == 2);
    CHECK(close(at[1].x, -2.0f));
    CHECK(s.ghost_count() == 2);
    REQUIRE(s.undo());
    ghost_positions(s, at);
    CHECK(close(at[1].x, 3.0f));
    CHECK(s.verify_world());

    // Saving writes only the instance.
    cook::String text;
    s.serialize(text);
    CHECK(text.view().find("Bottom") == StringView::npos);

    // The prefab edited on disk: after the hot reload the ghosts show the new layout.
    put(root + "/assets/prefabs/stack.mcprefab", stack_prefab(2.5f));
    const auto mt = std::filesystem::last_write_time(root + "/assets/prefabs/stack.mcprefab");
    std::filesystem::last_write_time(root + "/assets/prefabs/stack.mcprefab", mt + std::chrono::seconds(2));
    session.tick_hot_reload(0.0, 0.0);
    session.tick_hot_reload(1.0, 0.0);
    ghost_positions(s, at);
    REQUIRE(at.size() == 2);
    CHECK(close(at[1].y, 2.5f));
    CHECK(s.verify_world());
}

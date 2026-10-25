// M-SCENEDIFF: what changed between two versions of a scene, by entity id and reflected field.

#include <doctest/doctest.h>

#include <string_view>

#include "mce/ecs/transform.h"
#include "mce/editor/editor_scene.h"
#include "mce/editor/scene_diff.h"
#include "mce/kit/components.h"

using namespace mce;
using namespace mce::editor;

namespace {

// A copy of the scene as it is now: serialized and parsed back, so later edits to `s` leave it untouched.
SceneDocument snapshot_of(const EditorScene& s) {
    cook::String text;
    s.doc().serialize(text);
    SceneDocument copy;
    cook::DiagList diags;
    REQUIRE(copy.parse(text.view(), "snapshot.mcscene", diags));
    return copy;
}

const EntityDiff* find(const SceneDiff& d, EntityId id) {
    for (const EntityDiff& e : d.entities)
        if (e.id == id) return &e;
    return nullptr;
}

bool has(const cook::String& text, std::string_view what) {
    return std::string_view(text.c_str()).find(what) != std::string_view::npos;
}

}  // namespace

TEST_CASE("an unchanged scene has an empty diff") {
    EditorScene s;
    s.create_entity("A");
    s.create_entity("B");
    const SceneDocument before = snapshot_of(s);
    const SceneDiff d = diff_scenes(before, s.doc());
    CHECK(d.empty());
    CHECK(format_diff(d).empty());
}

TEST_CASE("added, removed and renamed entities are reported by id") {
    EditorScene s;
    const EntityId a = s.create_entity("A");
    const EntityId b = s.create_entity("B");
    const SceneDocument before = snapshot_of(s);

    const EntityId c = s.create_entity("C");
    REQUIRE(s.destroy_entity(a));
    REQUIRE(s.rename_entity(b, "Bee"));

    const SceneDiff d = diff_scenes(before, s.doc());
    REQUIRE(find(d, c) != nullptr);
    CHECK(find(d, c)->kind == DiffKind::Added);
    REQUIRE(find(d, a) != nullptr);
    CHECK(find(d, a)->kind == DiffKind::Removed);
    CHECK(find(d, a)->name.view() == "A");
    REQUIRE(find(d, b) != nullptr);
    CHECK(find(d, b)->kind == DiffKind::Changed);
    CHECK(find(d, b)->old_name.view() == "B");
    CHECK(find(d, b)->name.view() == "Bee");

    const cook::String text = format_diff(d);
    CHECK(has(text, "+ C (id "));
    CHECK(has(text, "- A (id "));
    CHECK(has(text, "renamed from \"B\""));
}

TEST_CASE("component changes name the field; added and removed components are reported") {
    EditorScene s;
    const EntityId a = s.create_entity("A");
    const SceneDocument before = snapshot_of(s);

    LocalTransform t = *s.get<LocalTransform>(a);
    t.position.x = 3.0f;
    REQUIRE(s.set(a, t));
    REQUIRE(s.add_component(a, refl::type_of<kit::Sprite>()));

    const SceneDiff d = diff_scenes(before, s.doc());
    const EntityDiff* e = find(d, a);
    REQUIRE(e != nullptr);
    bool moved = false, sprite_added = false;
    for (const ComponentChange& c : e->components) {
        if (c.op == ComponentOp::Changed && c.fields != 0) moved = true;
        if (c.op == ComponentOp::Added) sprite_added = true;
    }
    CHECK(moved);
    CHECK(sprite_added);
    const cook::String text = format_diff(d);
    CHECK(has(text, ".position"));
    CHECK(has(text, "Sprite"));

    // The other way round, the same component is reported as removed.
    const SceneDiff back = diff_scenes(s.doc(), before);
    bool sprite_removed = false;
    for (const ComponentChange& c : find(back, a)->components) sprite_removed = sprite_removed || c.op == ComponentOp::Removed;
    CHECK(sprite_removed);
}

TEST_CASE("a reorder is reported once, without per-entity noise") {
    EditorScene s;
    const EntityId a = s.create_entity("A");
    s.create_entity("B");
    s.create_entity("C");
    const SceneDocument before = snapshot_of(s);
    REQUIRE(s.move_entity(a, 2));

    const SceneDiff d = diff_scenes(before, s.doc());
    CHECK(d.reordered);
    CHECK(d.entities.empty());
    CHECK(has(format_diff(d), "~ entity order"));
}

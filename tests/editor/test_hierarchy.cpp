// The editor's ChildOf tree (ROADMAP M6, ADR-0030 §5), headless: re-parenting keeps where an
// entity sits, cycles are refused, children are placed and picked where their parent puts them,
// deleting a parent deletes its subtree (and undo brings it back), the gizmo drags children in
// scene space, and the play world gets the runtime hierarchy.

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <string>

#include "cook_test_util.h"
#include "mce/ecs/hierarchy.h"
#include "mce/editor/editor_session.h"
#include "mce/editor/gizmo.h"
#include "mce/editor/viewport2d.h"
#include "mce/kit/components.h"

using namespace mce;
using namespace mce::editor;

namespace {

bool close(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }

EntityId place(EditorScene& s, const char* name, math::vec2 p, f32 angle = 0.0f, f32 scale = 1.0f) {
    const EntityId id = s.create_entity(name);
    LocalTransform t;
    t.position = {p.x, p.y, 0.0f};
    t.rotation = rotation_z(angle);
    t.scale = {scale, scale, 1.0f};
    s.set(id, t);
    return id;
}

std::string text_of(const EditorScene& s) {
    cook::String t;
    s.serialize(t);
    return std::string(t.view().data(), t.view().size());
}

constexpr f32 kQuarter = 1.5707963f;

}  // namespace

TEST_CASE("hierarchy: set_parent keeps the scene placement, orders the file, and undoes in one step") {
    EditorScene s;
    const EntityId child = place(s, "Child", {3, 1});
    const EntityId other = place(s, "Other", {0, 0});
    const EntityId parent = place(s, "Parent", {1, 1}, kQuarter, 2.0f);
    s.clear_undo();
    const std::string before = text_of(s);

    REQUIRE(s.set_parent(child, parent));
    CHECK(s.parent_of(child) == parent);
    CHECK(s.undo_count() == 1);
    CHECK(s.undo_label() == "Reparent");
    const WorldTransform w = s.world_transform(child);
    CHECK(close(w.position.x, 3.0f));  // still where it was
    CHECK(close(w.position.y, 1.0f));
    CHECK(close(angle_z(w.rotation), 0.0f));
    CHECK(close(w.scale.x, 1.0f));
    const LocalTransform* local = s.get<LocalTransform>(child);
    CHECK(close(local->position.y, -1.0f));  // (2,0) in the scene = (0,-1) under a 90°, x2 parent
    CHECK(close(local->scale.x, 0.5f));
    CHECK(s.doc().index_of(child) == s.doc().index_of(parent) + 1);  // the file reads parent-first
    CHECK(s.verify_world());
    // The preview world draws the composed placement.
    const WorldTransform* live = s.world().try_get<WorldTransform>(s.runtime_entity(child));
    REQUIRE(live);
    CHECK(close(live->position.x, 3.0f));

    Array<EntityId> roots, kids;
    s.children_of(kNoEntity, roots);
    s.children_of(parent, kids);
    CHECK(roots.size() == 2);
    REQUIRE(kids.size() == 1);
    CHECK(kids[0] == child);

    // Cycles and no-ops are refused.
    CHECK_FALSE(s.set_parent(parent, child));
    CHECK_FALSE(s.set_parent(child, child));
    CHECK_FALSE(s.set_parent(child, parent));
    CHECK(s.is_ancestor(parent, child));

    // Moving the parent carries the child; its local transform is untouched.
    LocalTransform tp = *s.get<LocalTransform>(parent);
    tp.position.x += 10.0f;
    s.set(parent, tp);
    CHECK(close(s.world_transform(child).position.x, 13.0f));
    math::vec2 q[4];
    REQUIRE(entity_quad(s, child, q));
    CHECK(pick_entity(s, {13.0f, 1.0f}) == child);

    s.undo();
    s.undo();
    CHECK(text_of(s) == before);
    CHECK(s.verify_world());

    // Unparent: back to a root at the same place.
    REQUIRE(s.redo());
    REQUIRE(s.set_parent(child, kNoEntity));
    CHECK(s.get<ChildOf>(child) == nullptr);
    CHECK(close(s.get<LocalTransform>(child)->position.x, 3.0f));
    (void)other;
}

TEST_CASE("hierarchy: deleting a parent deletes its subtree; undo restores all of it") {
    EditorScene s;
    const EntityId root = place(s, "Root", {0, 0});
    const EntityId mid = place(s, "Mid", {1, 0});
    const EntityId leaf = place(s, "Leaf", {2, 0});
    const EntityId keep = place(s, "Keep", {5, 0});
    REQUIRE(s.set_parent(mid, root));
    REQUIRE(s.set_parent(leaf, mid));
    s.clear_undo();
    const std::string before = text_of(s);
    s.begin_edit("Delete");
    REQUIRE(s.destroy_entity(root));
    s.end_edit();
    CHECK(s.doc().size() == 1);
    CHECK(s.doc().find(keep));
    CHECK(s.verify_world());
    REQUIRE(s.undo());
    CHECK(text_of(s) == before);
    CHECK(s.verify_world());
    CHECK(s.parent_of(leaf) == mid);
}

TEST_CASE("hierarchy: the gizmo drags children in scene space and skips a selected ancestor's children") {
    EditorScene s;
    const EntityId parent = place(s, "Parent", {0, 0}, kQuarter, 2.0f);
    const EntityId child = place(s, "Child", {2, 0});
    REQUIRE(s.set_parent(child, parent));

    TransformGizmo g;
    s.selection().set(child);
    math::vec2 pivot;
    REQUIRE(TransformGizmo::pivot(s, pivot));
    CHECK(close(pivot.x, 2.0f));
    REQUIRE(g.begin(s, GizmoPart::X, {2, 0}));
    g.update(s, {3, 0}, 0.0f);  // +1 m along the scene's X
    g.end();
    CHECK(close(s.world_transform(child).position.x, 3.0f));
    CHECK(close(s.world_transform(child).position.y, 0.0f));
    CHECK(close(s.get<LocalTransform>(child)->position.y, -1.5f));  // parent space: rotated, x2

    // Parent and child selected: only the parent moves, the child follows it.
    s.selection().set(parent);
    s.selection().toggle(child);
    const LocalTransform child_local = *s.get<LocalTransform>(child);
    REQUIRE(g.begin(s, GizmoPart::XY, {0, 0}));
    g.update(s, {1, 1}, 0.0f);
    g.end();
    CHECK(s.get<LocalTransform>(child)->position.y == child_local.position.y);
    CHECK(close(s.world_transform(child).position.x, 4.0f));
    CHECK(close(s.world_transform(child).position.y, 1.0f));
}

TEST_CASE("hierarchy: the play world composes WorldTransform through the runtime hierarchy") {
    const std::string root = cook_test::fresh_dir("hierarchy_play");
    std::filesystem::copy(std::string(MCE_SOURCE_DIR) + "/samples/scene_demo/assets", root + "/assets",
                          std::filesystem::copy_options::recursive);
    EditorSession session;
    REQUIRE(session.open_project(StringView(root.data(), root.size())));
    REQUIRE(session.open_scene("scenes/demo.mcscene"));
    EditorScene& s = session.scene();
    const EntityId parent = place(s, "Parent", {4, 0}, kQuarter);
    const EntityId child = place(s, "Child", {4, 2});
    REQUIRE(s.set_parent(child, parent));
    REQUIRE(session.enter_play());
    World& w = *session.play().world();
    bool found = false;
    w.query<const ChildOf, const WorldTransform>().each([&](const ChildOf& c, const WorldTransform& t) {
        found = !c.parent.is_null();
        CHECK(close(t.position.x, 4.0f));
        CHECK(close(t.position.y, 2.0f));
    });
    CHECK(found);
    CHECK(ecs::hierarchy_installed(w));
    session.stop_play();
}

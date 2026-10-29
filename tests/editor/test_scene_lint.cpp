// M-SCENELINT: mistakes a scene document carries without failing to load.

#include <doctest/doctest.h>

#include <algorithm>
#include <limits>
#include <string_view>

#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/editor/editor_scene.h"
#include "mce/editor/scene_lint.h"
#include "mce/kit/anim3d.h"

using namespace mce;
using namespace mce::editor;

namespace {

bool has(const Array<LintFinding>& all, EntityId id, LintKind kind) {
    for (const LintFinding& f : all)
        if (f.id == id && f.kind == kind) return true;
    return false;
}

void set_transform(EditorScene& s, EntityId id, const LocalTransform& t) {
    s.add_component(id, refl::type_of<LocalTransform>());
    (void)s.set(id, t);  // false when the value is already there; the checks below read the document
}

}  // namespace

TEST_CASE("a tidy scene has no findings") {
    EditorScene s;
    const EntityId room = s.create_entity("Room");
    const EntityId wall = s.create_entity("Wall");
    s.set_parent(wall, room);
    set_transform(s, wall, LocalTransform{});
    CHECK(lint_scene(s.doc()).empty());
    CHECK(format_lint(lint_scene(s.doc()).span()).empty());
}

TEST_CASE("siblings that share a name are reported, the same name under different parents is not") {
    EditorScene s;
    const EntityId room_a = s.create_entity("A");
    const EntityId room_b = s.create_entity("B");
    const EntityId first = s.create_entity("Wall");
    const EntityId second = s.create_entity("Wall");
    const EntityId elsewhere = s.create_entity("Wall");
    s.set_parent(first, room_a);
    s.set_parent(second, room_a);
    s.set_parent(elsewhere, room_b);
    const Array<LintFinding> f = lint_scene(s.doc());
    CHECK(has(f, second, LintKind::DuplicateSiblingName));
    CHECK_FALSE(has(f, first, LintKind::DuplicateSiblingName));
    CHECK_FALSE(has(f, elsewhere, LintKind::DuplicateSiblingName));
    const std::string_view text = format_lint(f.span()).c_str();
    CHECK(text.find("duplicate name among its siblings") != std::string_view::npos);
}

TEST_CASE("empty names, zero scale and a transform that is not a number") {
    EditorScene s;
    const EntityId unnamed = s.create_entity("");
    const EntityId flat = s.create_entity("Flat");
    const EntityId broken = s.create_entity("Broken");
    LocalTransform t;
    t.scale = {1.0f, 0.0f, 1.0f};
    set_transform(s, flat, t);
    LocalTransform nan_t;
    nan_t.position.x = std::numeric_limits<f32>::quiet_NaN();
    set_transform(s, broken, nan_t);
    const Array<LintFinding> f = lint_scene(s.doc());
    CHECK(has(f, unnamed, LintKind::EmptyName));
    CHECK(has(f, flat, LintKind::ZeroScale));
    CHECK(has(f, broken, LintKind::NonFiniteTransform));
    CHECK_FALSE(has(f, broken, LintKind::ZeroScale));
}

TEST_CASE("a parent the scene lacks is reported") {
    EditorScene s;
    const EntityId orphan = s.create_entity("Orphan");
    s.add_component(orphan, refl::type_of<ChildOf>());
    REQUIRE(s.set(orphan, ChildOf{doc_entity_ref(999)}));
    CHECK(has(lint_scene(s.doc()), orphan, LintKind::MissingParent));
}

TEST_CASE("a reference to an entity the scene lacks is reported; a null one and a present one are not") {
    EditorScene s;
    const EntityId hand = s.create_entity("Hand");
    const EntityId gun = s.create_entity("Gun");
    s.add_component(gun, refl::type_of<kit::AttachToBone>());
    CHECK_FALSE(has(lint_scene(s.doc()), gun, LintKind::DanglingRef));  // a null target

    kit::AttachToBone a;
    a.target = doc_entity_ref(hand);
    REQUIRE(s.set(gun, a));
    CHECK_FALSE(has(lint_scene(s.doc()), gun, LintKind::DanglingRef));

    a.target = doc_entity_ref(999);
    REQUIRE(s.set(gun, a));
    const Array<LintFinding> f = lint_scene(s.doc());
    CHECK(has(f, gun, LintKind::DanglingRef));
    CHECK_FALSE(has(f, gun, LintKind::MissingParent));  // a parent is reported as MissingParent, not twice
    CHECK(std::string_view(format_lint(f.span()).c_str()).find("Gun (id ") != std::string_view::npos);
}

TEST_CASE("a loop of parents is reported once, on its lowest id; a chain into the loop and a tidy tree are not") {
    EditorScene s;
    const EntityId a = s.create_entity("A");
    const EntityId b = s.create_entity("B");
    const EntityId c = s.create_entity("C");
    const EntityId tail = s.create_entity("Tail");  // hangs below the loop, not in it
    const EntityId solo = s.create_entity("Solo");
    for (EntityId e : {a, b, c, tail}) s.add_component(e, refl::type_of<ChildOf>());
    REQUIRE(s.set(a, ChildOf{doc_entity_ref(c)}));
    REQUIRE(s.set(b, ChildOf{doc_entity_ref(a)}));
    REQUIRE(s.set(c, ChildOf{doc_entity_ref(b)}));
    REQUIRE(s.set(tail, ChildOf{doc_entity_ref(c)}));
    const Array<LintFinding> f = lint_scene(s.doc());
    usize cycles = 0;
    for (const LintFinding& x : f) cycles += x.kind == LintKind::ParentCycle ? 1 : 0;
    CHECK(cycles == 1);
    CHECK(has(f, std::min({a, b, c}), LintKind::ParentCycle));
    CHECK_FALSE(has(f, tail, LintKind::ParentCycle));
    CHECK_FALSE(has(f, solo, LintKind::ParentCycle));
    CHECK(std::string_view(format_lint(f.span()).c_str()).find("parents loop") != std::string_view::npos);
}

TEST_CASE("fix_scene_lint breaks a parent loop with one change and a second run finds nothing") {
    EditorScene s;
    const EntityId a = s.create_entity("A");
    const EntityId b = s.create_entity("B");
    s.add_component(a, refl::type_of<ChildOf>());
    s.add_component(b, refl::type_of<ChildOf>());
    REQUIRE(s.set(a, ChildOf{doc_entity_ref(b)}));
    REQUIRE(s.set(b, ChildOf{doc_entity_ref(a)}));
    SceneDocument doc = s.doc();
    CHECK(fix_scene_lint(doc) == 1);
    CHECK(lint_scene(doc).empty());
    CHECK(fix_scene_lint(doc) == 0);
    // The lowest id became a root; the other keeps its parent.
    const EntityId low = std::min(a, b), high = std::max(a, b);
    CHECK(doc.find(low)->find(refl::type_of<ChildOf>()) == nullptr);
    CHECK(doc.find(high)->find(refl::type_of<ChildOf>()) != nullptr);
}

TEST_CASE("fix_scene_lint repairs every kind and leaves a clean scene") {
    EditorScene s;
    const EntityId room = s.create_entity("Room");
    const EntityId first = s.create_entity("Wall");
    const EntityId second = s.create_entity("Wall");
    const EntityId third = s.create_entity("Wall");
    const EntityId unnamed = s.create_entity("");
    const EntityId flat = s.create_entity("Flat");
    const EntityId broken = s.create_entity("Broken");
    const EntityId orphan = s.create_entity("Orphan");
    const EntityId gun = s.create_entity("Gun");
    s.set_parent(first, room);
    s.set_parent(second, room);
    s.set_parent(third, room);
    LocalTransform flat_t;
    flat_t.scale = {1.0f, 0.0f, 1.0f};
    set_transform(s, flat, flat_t);
    LocalTransform nan_t;
    nan_t.position.x = std::numeric_limits<f32>::quiet_NaN();
    nan_t.scale = {2.0f, 0.0f, 2.0f};
    set_transform(s, broken, nan_t);
    s.add_component(orphan, refl::type_of<ChildOf>());
    REQUIRE(s.set(orphan, ChildOf{doc_entity_ref(999)}));
    s.add_component(gun, refl::type_of<kit::AttachToBone>());
    kit::AttachToBone a;
    a.target = doc_entity_ref(998);
    REQUIRE(s.set(gun, a));
    REQUIRE_FALSE(lint_scene(s.doc()).empty());

    SceneDocument doc = s.doc();
    const usize fixed = fix_scene_lint(doc);
    CHECK(fixed >= 7);
    CHECK(lint_scene(doc).empty());
    CHECK(doc.find(first)->name.view() == "Wall");
    CHECK(doc.find(second)->name.view() == "Wall (2)");
    CHECK(doc.find(third)->name.view() == "Wall (3)");
    CHECK(doc.find(unnamed)->name.view().size() > 0);
    CHECK(doc.find(orphan)->find(refl::type_of<ChildOf>()) == nullptr);
    const auto* fixed_flat = static_cast<const LocalTransform*>(doc.find(flat)->find(refl::type_of<LocalTransform>())->data());
    CHECK(fixed_flat->scale.y == 1.0f);
    const auto* fixed_broken = static_cast<const LocalTransform*>(doc.find(broken)->find(refl::type_of<LocalTransform>())->data());
    CHECK(fixed_broken->position.x == 0.0f);
    CHECK(fixed_broken->scale.x == 2.0f);  // a finite axis is kept
    CHECK(fixed_broken->scale.y == 1.0f);
    const auto* fixed_gun = static_cast<const kit::AttachToBone*>(doc.find(gun)->find(refl::type_of<kit::AttachToBone>())->data());
    CHECK(fixed_gun->target.is_null());

    CHECK(fix_scene_lint(doc) == 0);  // idempotent
}

TEST_CASE("fix_scene_lint on a tidy scene changes nothing") {
    EditorScene s;
    s.create_entity("A");
    s.create_entity("B");
    SceneDocument doc = s.doc();
    cook::String before, after;
    doc.serialize(before);
    CHECK(fix_scene_lint(doc) == 0);
    doc.serialize(after);
    CHECK(before.view() == after.view());
}

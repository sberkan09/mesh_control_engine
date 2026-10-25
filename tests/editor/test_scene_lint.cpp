// M-SCENELINT: mistakes a scene document carries without failing to load.

#include <doctest/doctest.h>

#include <limits>
#include <string_view>

#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/editor/editor_scene.h"
#include "mce/editor/scene_lint.h"

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
    REQUIRE(s.set(id, t));
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

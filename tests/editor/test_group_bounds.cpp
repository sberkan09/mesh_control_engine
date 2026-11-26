// M-MEASURE 1: group_bounds measures where a group of entities sits.

#include <doctest/doctest.h>

#include "mce/ecs/transform.h"
#include "mce/editor/group_bounds.h"

using namespace mce;
using namespace mce::editor;

namespace {

EntityId place(EditorScene& s, const char* name, f32 x, f32 y) {
    const EntityId id = s.create_entity(name);
    LocalTransform t;
    t.position = {x, y, 0.5f};
    s.set(id, t);
    return id;
}

}  // namespace

TEST_CASE("group_bounds gives the box, middle, size and diagonal of a group") {
    EditorScene s;
    const EntityId a = place(s, "A", -1.0f, 2.0f), b = place(s, "B", 5.0f, 6.0f), c = place(s, "C", 1.0f, 3.0f);
    const EntityId ids[] = {a, b, c, b};  // b twice counts once
    const GroupBounds g = group_bounds(s, Span<const EntityId>(ids, 4));
    CHECK(g.count == 3);
    CHECK(g.min_x == doctest::Approx(-1.0f));
    CHECK(g.max_x == doctest::Approx(5.0f));
    CHECK(g.min_y == doctest::Approx(2.0f));
    CHECK(g.max_y == doctest::Approx(6.0f));
    CHECK(g.center_x == doctest::Approx(2.0f));
    CHECK(g.center_y == doctest::Approx(4.0f));
    CHECK(g.width == doctest::Approx(6.0f));
    CHECK(g.height == doctest::Approx(4.0f));
    CHECK(g.diagonal == doctest::Approx(std::sqrt(52.0f)));
}

TEST_CASE("group_bounds uses world positions and skips entities without a transform") {
    EditorScene s;
    const EntityId parent = place(s, "P", 10.0f, 0.0f);
    const EntityId child = place(s, "C", 12.0f, 1.0f);
    s.set_parent(child, parent);  // keeps its world place
    const EntityId ids[] = {child, kNoEntity};
    const GroupBounds g = group_bounds(s, Span<const EntityId>(ids, 2));
    CHECK(g.count == 1);
    CHECK(g.center_x == doctest::Approx(12.0f));
    CHECK(g.diagonal == doctest::Approx(0.0f));
    const GroupBounds none = group_bounds(s, Span<const EntityId>());
    CHECK(none.count == 0);
    CHECK(none.width == 0.0f);
}

TEST_CASE("group_bounds changes nothing and makes no undo entry") {
    EditorScene s;
    const EntityId a = place(s, "A", 1.0f, 1.0f);
    const EntityId ids[] = {a};
    group_bounds(s, Span<const EntityId>(ids, 1));
    CHECK(s.world_transform(a).position.x == doctest::Approx(1.0f));
    CHECK(s.undo());  // the one undo step is the placement above, not a measurement
    CHECK(s.world_transform(a).position.x != doctest::Approx(1.0f));
}

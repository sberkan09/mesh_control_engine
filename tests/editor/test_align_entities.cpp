// M-ALIGN: align and distribute entities on one axis.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/editor/align_entities.h"

using namespace mce;
using namespace mce::editor;

namespace {

bool close(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }

EntityId place(EditorScene& s, const char* name, f32 x, f32 y) {
    const EntityId id = s.create_entity(name);
    LocalTransform t;
    t.position = {x, y, 0.5f};
    s.set(id, t);
    return id;
}

f32 world_x(const EditorScene& s, EntityId id) { return s.world_transform(id).position.x; }
f32 world_y(const EditorScene& s, EntityId id) { return s.world_transform(id).position.y; }

}  // namespace

TEST_CASE("align_entities moves one axis to the lowest, middle or highest position") {
    EditorScene s;
    const EntityId a = place(s, "A", 1.0f, 5.0f);
    const EntityId b = place(s, "B", 4.0f, -2.0f);
    const EntityId c = place(s, "C", 10.0f, 3.0f);
    const EntityId ids[] = {a, b, c};
    const Span<const EntityId> all(ids, 3);

    CHECK(align_entities(s, all, AlignAxis::X, AlignMode::Min));
    CHECK(close(world_x(s, a), 1.0f));
    CHECK(close(world_x(s, b), 1.0f));
    CHECK(close(world_x(s, c), 1.0f));
    CHECK(close(world_y(s, b), -2.0f));  // the other axis stays
    CHECK(s.get<LocalTransform>(b)->position.z == 0.5f);

    CHECK_FALSE(align_entities(s, all, AlignAxis::X, AlignMode::Min));  // already aligned: nothing changes

    CHECK(align_entities(s, all, AlignAxis::Y, AlignMode::Center));  // (-2 + 5) / 2
    CHECK(close(world_y(s, a), 1.5f));
    CHECK(close(world_y(s, c), 1.5f));

    CHECK_FALSE(align_entities(s, all, AlignAxis::Y, AlignMode::Max));  // all at 1.5 already
    CHECK(close(world_y(s, b), 1.5f));

    const EntityId one[] = {a};
    CHECK_FALSE(align_entities(s, Span<const EntityId>(one, 1), AlignAxis::X, AlignMode::Max));  // needs two
}

TEST_CASE("align_entities is one undo entry and respects a parent's transform") {
    EditorScene s;
    const EntityId parent = place(s, "Parent", 100.0f, 0.0f);
    const EntityId a = place(s, "A", 0.0f, 0.0f);
    const EntityId b = place(s, "B", 6.0f, 0.0f);
    REQUIRE(s.set_parent(a, parent));  // A keeps its world x of 0 (local -100)
    const EntityId ids[] = {a, b};
    CHECK(align_entities(s, Span<const EntityId>(ids, 2), AlignAxis::X, AlignMode::Max));
    CHECK(close(world_x(s, a), 6.0f));
    CHECK(close(world_x(s, b), 6.0f));
    CHECK(s.undo());
    CHECK(close(world_x(s, a), 0.0f));
    CHECK(close(world_x(s, b), 6.0f));
}

TEST_CASE("distribute_entities spaces the middle ones evenly") {
    EditorScene s;
    const EntityId a = place(s, "A", 0.0f, 0.0f);
    const EntityId b = place(s, "B", 9.0f, 0.0f);  // far right of its neighbours' even spot
    const EntityId c = place(s, "C", 2.0f, 0.0f);
    const EntityId d = place(s, "D", 12.0f, 0.0f);
    const EntityId ids[] = {a, b, c, d};
    CHECK(distribute_entities(s, Span<const EntityId>(ids, 4), AlignAxis::X));
    CHECK(close(world_x(s, a), 0.0f));  // the outer two stay
    CHECK(close(world_x(s, d), 12.0f));
    CHECK(close(world_x(s, c), 4.0f));  // sorted by position: c then b
    CHECK(close(world_x(s, b), 8.0f));

    CHECK_FALSE(distribute_entities(s, Span<const EntityId>(ids, 4), AlignAxis::X));  // already even
    CHECK_FALSE(distribute_entities(s, Span<const EntityId>(ids, 2), AlignAxis::X));  // needs three
}

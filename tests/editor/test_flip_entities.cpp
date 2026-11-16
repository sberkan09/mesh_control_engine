// M-FLIP: mirror entities across the middle of the selection.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/editor/flip_entities.h"

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

TEST_CASE("flip_entities mirrors positions across the middle and negates the scale, twice restores") {
    EditorScene s;
    const EntityId a = place(s, "A", 0.0f, 1.0f);
    const EntityId b = place(s, "B", 4.0f, 7.0f);
    const EntityId c = place(s, "C", 1.0f, 2.0f);
    const EntityId ids[] = {a, b, c};
    const Span<const EntityId> all(ids, 3);

    CHECK(flip_entities(s, all, AlignAxis::X));
    CHECK(close(world_x(s, a), 4.0f));
    CHECK(close(world_x(s, b), 0.0f));
    CHECK(close(world_x(s, c), 3.0f));
    CHECK(close(world_y(s, b), 7.0f));  // the other axis stays
    CHECK(s.get<LocalTransform>(a)->scale.x == -1.0f);
    CHECK(s.get<LocalTransform>(a)->scale.y == 1.0f);

    CHECK(flip_entities(s, all, AlignAxis::X));
    CHECK(close(world_x(s, a), 0.0f));
    CHECK(close(world_x(s, c), 1.0f));
    CHECK(s.get<LocalTransform>(a)->scale.x == 1.0f);

    CHECK(flip_entities(s, all, AlignAxis::Y));  // 1..7: y -> 8 - y
    CHECK(close(world_y(s, a), 7.0f));
    CHECK(close(world_y(s, c), 6.0f));
    CHECK(s.get<LocalTransform>(a)->scale.y == -1.0f);
}

TEST_CASE("flip_entities flips one entity in place and is one undo entry") {
    EditorScene s;
    const EntityId a = place(s, "A", 3.0f, 3.0f);
    const EntityId b = place(s, "B", 5.0f, 3.0f);
    const EntityId one[] = {a};
    CHECK(flip_entities(s, Span<const EntityId>(one, 1), AlignAxis::X));
    CHECK(close(world_x(s, a), 3.0f));
    CHECK(s.get<LocalTransform>(a)->scale.x == -1.0f);
    CHECK(s.undo());
    CHECK(s.get<LocalTransform>(a)->scale.x == 1.0f);

    const EntityId two[] = {a, b};
    CHECK(flip_entities(s, Span<const EntityId>(two, 2), AlignAxis::X));
    CHECK(s.undo());
    CHECK(close(world_x(s, a), 3.0f));
    CHECK(close(world_x(s, b), 5.0f));
    CHECK(s.get<LocalTransform>(b)->scale.x == 1.0f);
}

TEST_CASE("flip_entities respects a parent's transform and skips entities without one") {
    EditorScene s;
    const EntityId parent = place(s, "Parent", 100.0f, 0.0f);
    const EntityId a = place(s, "A", 0.0f, 0.0f);
    const EntityId b = place(s, "B", 6.0f, 0.0f);
    REQUIRE(s.set_parent(a, parent));  // A keeps its world x of 0
    const EntityId ids[] = {a, b, kNoEntity};
    CHECK(flip_entities(s, Span<const EntityId>(ids, 3), AlignAxis::X));
    CHECK(close(world_x(s, a), 6.0f));
    CHECK(close(world_x(s, b), 0.0f));

    const EntityId none[] = {kNoEntity};
    CHECK_FALSE(flip_entities(s, Span<const EntityId>(none, 1), AlignAxis::X));
}

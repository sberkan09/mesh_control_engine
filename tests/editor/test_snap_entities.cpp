// M-SNAPSEL: Edit > Snap to grid rounds the selection's world X and Y to the grid step.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/editor/snap_entities.h"

using namespace mce;
using namespace mce::editor;

namespace {

bool close(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }

EntityId place(EditorScene& s, const char* name, f32 x, f32 y) {
    const EntityId id = s.create_entity(name);
    LocalTransform t;
    t.position = {x, y, 0.37f};
    s.set(id, t);
    return id;
}

f32 world_x(const EditorScene& s, EntityId id) { return s.world_transform(id).position.x; }
f32 world_y(const EditorScene& s, EntityId id) { return s.world_transform(id).position.y; }

}  // namespace

TEST_CASE("snap_entities rounds X and Y to the step, keeps Z, and is one undo entry") {
    EditorScene s;
    const EntityId a = place(s, "A", 0.26f, 1.74f);
    const EntityId b = place(s, "B", 3.0f, -0.9f);  // x is already on the grid
    const EntityId c = place(s, "C", 7.7f, 7.7f);   // not in the list
    const EntityId ids[] = {a, b, a};

    CHECK(snap_entities(s, Span<const EntityId>(ids, 3), 0.5f) == 2);  // a once, b
    CHECK(close(world_x(s, a), 0.5f));
    CHECK(close(world_y(s, a), 1.5f));
    CHECK(close(world_x(s, b), 3.0f));
    CHECK(close(world_y(s, b), -1.0f));
    CHECK(close(s.get<LocalTransform>(a)->position.z, 0.37f));
    CHECK(close(world_x(s, c), 7.7f));

    CHECK(snap_entities(s, Span<const EntityId>(ids, 3), 0.5f) == 0);  // already on the grid: no undo entry
    CHECK(s.undo());
    CHECK(close(world_x(s, a), 0.26f));
    CHECK(close(world_y(s, b), -0.9f));
}

TEST_CASE("snap_entities snaps world positions under a parent and ignores a bad step") {
    EditorScene s;
    const EntityId parent = place(s, "Parent", 0.3f, 0.0f);
    const EntityId a = place(s, "A", 0.3f, 0.0f);
    REQUIRE(s.set_parent(a, parent));  // A keeps its world x of 0.3 (local 0)
    const EntityId ids[] = {a, kNoEntity};
    CHECK(snap_entities(s, Span<const EntityId>(ids, 2), 0.25f) == 1);
    CHECK(close(world_x(s, a), 0.25f));

    CHECK(snap_entities(s, Span<const EntityId>(ids, 2), 0.0f) == 0);
    CHECK(snap_entities(s, Span<const EntityId>(ids, 2), -1.0f) == 0);
}

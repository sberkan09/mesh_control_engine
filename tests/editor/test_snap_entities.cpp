// M-SNAPSEL: Edit > Snap to grid rounds the selection's world X and Y to the grid step.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

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

// Found by the fuzz below: a parent that snapped carried an on-grid child off the grid, because only the off-grid
// entities were placed.
TEST_CASE("snap_entities puts an on-grid child back after its parent snaps") {
    EditorScene s;
    const EntityId p = place(s, "P", 0.3f, 0.0f);
    const EntityId c = place(s, "C", 5.0f, 0.0f);  // world 5.3 once parented below
    REQUIRE(s.set_parent(c, p));
    const EntityId ids[] = {c, p};
    // c sits at world 5.3 (off the grid) in this setup; move it to world 6.0 exactly, then snap both.
    WorldTransform w = s.world_transform(c);
    w.position.x = 6.0f;
    (void)s.set(c, s.local_for_world(c, w));
    CHECK(snap_entities(s, Span<const EntityId>(ids, 2), 1.0f) == 1);  // only the parent was off the grid
    CHECK(close(world_x(s, p), 0.0f));
    CHECK(close(world_x(s, c), 6.0f));
}

TEST_CASE("snap_entities fuzz: every selected entity ends on the grid, whatever the order") {
    for (u32 seed = 1; seed <= 200; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        EditorScene s;
        std::vector<EntityId> all;
        for (u32 i = 0, n = 2 + static_cast<u32>(rng() % 7); i < n; ++i)
            all.push_back(place(s, "E", static_cast<f32>(rng() % 400) / 10.0f - 20.0f,
                                static_cast<f32>(rng() % 400) / 10.0f - 20.0f));
        for (u32 i = 0, k = static_cast<u32>(rng() % 6); i < k; ++i)
            (void)s.set_parent(all[rng() % all.size()], all[rng() % all.size()]);
        std::vector<EntityId> pick;
        for (EntityId id : all)
            if (rng() % 4 != 0) pick.push_back(id);
        std::shuffle(pick.begin(), pick.end(), rng);
        const f32 step = 0.5f + static_cast<f32>(rng() % 4);
        const auto on_grid = [&](f32 v) {
            const f32 r = v / step;
            return std::fabs(r - std::round(r)) < 1e-3f;
        };
        usize off = 0;
        for (EntityId id : pick) {
            const WorldTransform w = s.world_transform(id);
            off += (!on_grid(w.position.x) || !on_grid(w.position.y)) ? 1 : 0;
        }
        const usize n = snap_entities(s, Span<const EntityId>(pick.data(), pick.size()), step);
        CHECK(n == off);
        for (EntityId id : pick) {
            const WorldTransform w = s.world_transform(id);
            CHECK(on_grid(w.position.x));
            CHECK(on_grid(w.position.y));
            CHECK(close(w.position.z, 0.37f));
        }
    }
}

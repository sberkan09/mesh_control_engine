// M-FLIP: mirror entities across the middle of the selection.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <vector>

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

// Found by the fuzz below: with a child listed before its parent, the parent moved afterwards and took the child
// along, so the child ended where it started.
TEST_CASE("flip_entities handles a child listed before its parent") {
    EditorScene s;
    const EntityId p = place(s, "P", 0.0f, 0.0f);
    const EntityId c = place(s, "C", 10.0f, 0.0f);
    REQUIRE(s.set_parent(c, p));
    const EntityId ids[] = {c, p};
    CHECK(flip_entities(s, Span<const EntityId>(ids, 2), AlignAxis::X));
    CHECK(close(world_x(s, p), 10.0f));
    CHECK(close(world_x(s, c), 0.0f));
}

// M-FLIP2: with parents and children both in the selection, in any order, every selected entity ends where the mirror
// puts it in the world, and flipping twice restores the scene.
TEST_CASE("flip_entities fuzz: nested selections land on the mirrored world position in any order") {
    for (u32 seed = 1; seed <= 200; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        EditorScene s;
        std::vector<EntityId> all;
        for (u32 i = 0, n = 2 + static_cast<u32>(rng() % 7); i < n; ++i)
            all.push_back(place(s, "E", static_cast<f32>(static_cast<int>(rng() % 21) - 10),
                                static_cast<f32>(static_cast<int>(rng() % 21) - 10)));
        for (u32 i = 0, k = static_cast<u32>(rng() % 8); i < k; ++i)
            (void)s.set_parent(all[rng() % all.size()], all[rng() % all.size()]);

        std::vector<EntityId> pick;
        for (EntityId id : all)
            if (rng() % 3 != 0) pick.push_back(id);
        std::shuffle(pick.begin(), pick.end(), rng);
        if (pick.empty()) continue;
        const AlignAxis axis = rng() % 2 == 0 ? AlignAxis::X : AlignAxis::Y;
        const auto coord = [&](EntityId id) {
            const WorldTransform w = s.world_transform(id);
            return axis == AlignAxis::X ? w.position.x : w.position.y;
        };
        f32 lo = coord(pick[0]), hi = lo;
        std::map<EntityId, f32> before;
        for (EntityId id : pick) {
            before[id] = coord(id);
            lo = std::min(lo, before[id]);
            hi = std::max(hi, before[id]);
        }
        std::map<EntityId, f32> all_before;
        for (EntityId id : all) all_before[id] = coord(id);

        CHECK(flip_entities(s, Span<const EntityId>(pick.data(), pick.size()), axis));
        for (EntityId id : pick) CHECK(close(coord(id), lo + hi - before[id], 1e-3f));

        CHECK(flip_entities(s, Span<const EntityId>(pick.data(), pick.size()), axis));
        for (EntityId id : pick) CHECK(close(coord(id), before[id], 1e-3f));
    }
}

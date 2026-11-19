// M-DUPOFFSET: duplicate the selection and shift the copies, as one undo entry.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <utility>
#include <vector>

#include "mce/ecs/transform.h"
#include "mce/editor/duplicate_offset.h"

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

TEST_CASE("duplicate_offset copies the entities, shifts the copies and leaves the originals") {
    EditorScene s;
    const EntityId a = place(s, "A", 0.0f, 0.0f);
    const EntityId b = place(s, "B", 2.0f, 1.0f);
    const usize before = s.doc().size();
    const EntityId ids[] = {a, b};
    Array<EntityId> made;

    CHECK(duplicate_offset(s, Span<const EntityId>(ids, 2), 5.0f, -1.0f, &made) == 2);
    REQUIRE(made.size() == 2);
    CHECK(s.doc().size() == before + 2);
    CHECK(close(world_x(s, a), 0.0f));
    CHECK(close(world_x(s, b), 2.0f));
    CHECK(close(world_x(s, made[0]), 5.0f));
    CHECK(close(world_y(s, made[0]), -1.0f));
    CHECK(close(world_x(s, made[1]), 7.0f));
    CHECK(close(world_y(s, made[1]), 0.0f));

    CHECK(s.undo());  // one entry: the copies and the shift go together
    CHECK(s.doc().size() == before);
    CHECK(close(world_x(s, a), 0.0f));
}

TEST_CASE("duplicate_offset moves a copied child once, with its copied parent") {
    EditorScene s;
    const EntityId parent = place(s, "Parent", 0.0f, 0.0f);
    const EntityId child = place(s, "Child", 1.0f, 0.0f);
    REQUIRE(s.set_parent(child, parent));
    const EntityId ids[] = {parent, child};  // Edit > Duplicate copies exactly the listed entities
    Array<EntityId> made;

    CHECK(duplicate_offset(s, Span<const EntityId>(ids, 2), 10.0f, 0.0f, &made) == 2);
    f32 parent_x = -1.0f, child_x = -1.0f;
    for (EntityId id : made) {
        if (s.parent_of(id) == kNoEntity) parent_x = world_x(s, id);
        else child_x = world_x(s, id);
    }
    CHECK(close(parent_x, 10.0f));
    CHECK(close(child_x, 11.0f));  // 10 for the parent plus its own 1, not 20 + 1
}

TEST_CASE("duplicate_offset with nothing to duplicate changes nothing") {
    EditorScene s;
    place(s, "A", 0.0f, 0.0f);
    const usize before = s.doc().size();
    const EntityId none[] = {kNoEntity};
    CHECK(duplicate_offset(s, Span<const EntityId>(none, 1), 1.0f, 1.0f) == 0);
    CHECK(duplicate_offset(s, Span<const EntityId>(), 1.0f, 1.0f) == 0);
    CHECK(s.doc().size() == before);
}

// Found by the fuzz below: a copy of a child whose parent was not selected lands at the root, so it started from its
// local position instead of the original's world position.
TEST_CASE("duplicate_offset shifts a copy from the original's world position when its parent is not copied") {
    EditorScene s;
    const EntityId parent = place(s, "Parent", 10.0f, 0.0f);
    const EntityId child = place(s, "Child", 1.0f, 2.0f);
    REQUIRE(s.set_parent(child, parent));  // world stays (1, 2)
    const EntityId ids[] = {child};
    Array<EntityId> made;
    CHECK(duplicate_offset(s, Span<const EntityId>(ids, 1), 3.0f, 0.0f, &made) == 1);
    REQUIRE(made.size() == 1);
    CHECK(close(world_x(s, made[0]), 4.0f));
    CHECK(close(world_y(s, made[0]), 2.0f));
}

TEST_CASE("duplicate_offset fuzz: every copy sits at its original's world position plus the offset") {
    for (u32 seed = 1; seed <= 200; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        EditorScene s;
        std::vector<EntityId> all;
        for (u32 i = 0, n = 2 + static_cast<u32>(rng() % 6); i < n; ++i)
            all.push_back(place(s, "E", static_cast<f32>(rng() % 40) - 20.0f, static_cast<f32>(rng() % 40) - 20.0f));
        for (u32 i = 0, k = static_cast<u32>(rng() % 5); i < k; ++i)
            (void)s.set_parent(all[rng() % all.size()], all[rng() % all.size()]);
        std::vector<EntityId> pick;
        for (EntityId id : all)
            if (rng() % 3 != 0) pick.push_back(id);
        std::shuffle(pick.begin(), pick.end(), rng);
        if (pick.empty()) continue;
        const f32 dx = static_cast<f32>(rng() % 9) - 4.0f;
        const f32 dy = static_cast<f32>(rng() % 9) - 4.0f;
        std::vector<std::pair<f32, f32>> want;
        want.reserve(pick.size());
        for (EntityId id : pick) {
            const WorldTransform w = s.world_transform(id);
            want.emplace_back(w.position.x + dx, w.position.y + dy);
        }
        Array<EntityId> made;
        const usize n = duplicate_offset(s, Span<const EntityId>(pick.data(), pick.size()), dx, dy, &made);
        CHECK(n == pick.size());
        std::vector<std::pair<f32, f32>> got;
        got.reserve(made.size());
        for (EntityId id : made) got.emplace_back(world_x(s, id), world_y(s, id));
        std::sort(want.begin(), want.end());
        std::sort(got.begin(), got.end());
        REQUIRE(got.size() == want.size());
        for (usize i = 0; i < got.size(); ++i) {
            CHECK(close(got[i].first, want[i].first));
            CHECK(close(got[i].second, want[i].second));
        }
    }
}

// M-RESETXFORM: Edit > Reset transform puts the local transform back to the identity.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include "mce/ecs/transform.h"
#include "mce/editor/reset_transform.h"

using namespace mce;
using namespace mce::editor;

namespace {

EntityId place(EditorScene& s, const char* name, f32 x, f32 y) {
    const EntityId id = s.create_entity(name);
    LocalTransform t;
    t.position = {x, y, 2.0f};
    t.rotation = rotation_z(0.7f);
    t.scale = {3.0f, 4.0f, 1.0f};
    s.set(id, t);
    return id;
}

// Same tolerance as reset_transforms: set_parent keeps the world position, and the rounding of that
// (fused multiply-add on clang release) leaves a local transform a hair away from the exact identity.
bool near(f32 a, f32 b) { return std::fabs(a - b) <= 1e-6f; }

bool is_identity(const LocalTransform& t) {
    return near(t.position.x, 0.0f) && near(t.position.y, 0.0f) && near(t.position.z, 0.0f) &&
           near(t.rotation.x, 0.0f) && near(t.rotation.y, 0.0f) && near(t.rotation.z, 0.0f) &&
           near(t.rotation.w, 1.0f) && near(t.scale.x, 1.0f) && near(t.scale.y, 1.0f) && near(t.scale.z, 1.0f);
}

}  // namespace

TEST_CASE("reset_transforms puts position, rotation and scale back, in one undo entry") {
    EditorScene s;
    const EntityId a = place(s, "A", 5.0f, 6.0f);
    const EntityId b = place(s, "B", -1.0f, 2.0f);
    const EntityId c = place(s, "C", 9.0f, 9.0f);
    const EntityId ids[] = {a, b, a};  // a twice: counted once

    CHECK(reset_transforms(s, Span<const EntityId>(ids, 3)) == 2);
    CHECK(is_identity(*s.get<LocalTransform>(a)));
    CHECK(is_identity(*s.get<LocalTransform>(b)));
    CHECK(s.get<LocalTransform>(c)->position.x == 9.0f);  // not in the list

    s.undo();
    CHECK(s.get<LocalTransform>(a)->position.x == 5.0f);
    CHECK(s.get<LocalTransform>(b)->position.x == -1.0f);
}

TEST_CASE("reset_transforms skips identities and entities without a transform, with no undo entry") {
    EditorScene s;
    const EntityId a = place(s, "A", 1.0f, 1.0f);
    const EntityId ids[] = {a};
    CHECK(reset_transforms(s, Span<const EntityId>(ids, 1)) == 1);
    CHECK(reset_transforms(s, Span<const EntityId>(ids, 1)) == 0);  // already the identity

    const EntityId bare = s.create_entity("Bare");
    s.set(bare, LocalTransform{});
    s.undo();  // the set above; leaves the entity without a LocalTransform
    const EntityId none[] = {bare, kNoEntity};
    CHECK(reset_transforms(s, Span<const EntityId>(none, 2)) == 0);
}

TEST_CASE("reset_transforms fuzz: the selection ends at identity, the rest is untouched, the count is exact") {
    for (u32 seed = 1; seed <= 200; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        EditorScene s;
        std::vector<EntityId> all;
        for (u32 i = 0, n = 2 + static_cast<u32>(rng() % 6); i < n; ++i) {
            all.push_back(place(s, "E", static_cast<f32>(rng() % 10), 1.0f));
            if (rng() % 4 == 0) (void)s.set(all.back(), LocalTransform{});  // some already at the identity
        }
        for (u32 i = 0, k = static_cast<u32>(rng() % 5); i < k; ++i)
            (void)s.set_parent(all[rng() % all.size()], all[rng() % all.size()]);
        std::vector<EntityId> pick;
        for (EntityId id : all)
            if (rng() % 2 == 0) pick.push_back(id);
        if (!pick.empty()) pick.push_back(pick.front());  // a repeated id counts once
        std::shuffle(pick.begin(), pick.end(), rng);
        usize want = 0;
        std::vector<EntityId> seen;
        for (EntityId id : pick) {
            if (std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
            seen.push_back(id);
            want += is_identity(*s.get<LocalTransform>(id)) ? 0 : 1;
        }
        std::vector<LocalTransform> before;
        before.reserve(all.size());
        for (EntityId id : all) before.push_back(*s.get<LocalTransform>(id));
        CHECK(reset_transforms(s, Span<const EntityId>(pick.data(), pick.size())) == want);
        for (usize i = 0; i < all.size(); ++i) {
            const bool picked = std::find(pick.begin(), pick.end(), all[i]) != pick.end();
            const LocalTransform* t = s.get<LocalTransform>(all[i]);
            if (picked)
                CHECK(is_identity(*t));
            else
                CHECK(t->position.x == before[i].position.x);
        }
    }
}

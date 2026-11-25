// M-SCATTER 1: jitter_entities nudges the selection's position, angle and size by bounded random amounts.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include "mce/ecs/transform.h"
#include "mce/editor/jitter_entities.h"

using namespace mce;
using namespace mce::editor;

namespace {

constexpr f32 kPi = 3.14159265358979f;

EntityId place(EditorScene& s, const char* name, f32 x, f32 y) {
    const EntityId id = s.create_entity(name);
    LocalTransform t;
    t.position = {x, y, 0.5f};
    s.set(id, t);
    return id;
}

Span<const EntityId> span_of(const std::vector<EntityId>& v) { return Span<const EntityId>(v.data(), v.size()); }

JitterParams all_ranges(u32 seed) {
    JitterParams p;
    p.move_x = 2.0f;
    p.move_y = 1.0f;
    p.rotate_degrees = 30.0f;
    p.scale_min = 0.8f;
    p.scale_max = 1.25f;
    p.seed = seed;
    return p;
}

}  // namespace

TEST_CASE("jitter_entities keeps every change inside its range and leaves Z alone") {
    EditorScene s;
    std::vector<EntityId> ids;
    for (u32 i = 0; i < 40; ++i) ids.push_back(place(s, "T", static_cast<f32>(i) * 3.0f, 10.0f));
    CHECK(jitter_entities(s, span_of(ids), all_ranges(7)) == 40);
    bool moved = false;
    for (u32 i = 0; i < 40; ++i) {
        const WorldTransform w = s.world_transform(ids[i]);
        CHECK(std::fabs(w.position.x - static_cast<f32>(i) * 3.0f) <= 2.0f + 1e-4f);
        CHECK(std::fabs(w.position.y - 10.0f) <= 1.0f + 1e-4f);
        CHECK(std::fabs(w.position.z - 0.5f) <= 1e-5f);
        CHECK(std::fabs(angle_z(w.rotation)) <= 30.0f * kPi / 180.0f + 1e-4f);
        CHECK(w.scale.x >= 0.8f - 1e-4f);
        CHECK(w.scale.x <= 1.25f + 1e-4f);
        CHECK(std::fabs(w.scale.x - w.scale.y) <= 1e-6f);  // one factor for all axes
        moved = moved || std::fabs(w.position.x - static_cast<f32>(i) * 3.0f) > 1e-3f;
    }
    CHECK(moved);
}

TEST_CASE("jitter_entities gives the same result for the same seed and another for another seed") {
    EditorScene a, b, c;
    std::vector<EntityId> ia, ib, ic;
    for (u32 i = 0; i < 10; ++i) {
        ia.push_back(place(a, "T", static_cast<f32>(i), 0.0f));
        ib.push_back(place(b, "T", static_cast<f32>(i), 0.0f));
        ic.push_back(place(c, "T", static_cast<f32>(i), 0.0f));
    }
    jitter_entities(a, span_of(ia), all_ranges(3));
    jitter_entities(b, span_of(ib), all_ranges(3));
    jitter_entities(c, span_of(ic), all_ranges(4));
    bool differs = false;
    for (u32 i = 0; i < 10; ++i) {
        CHECK(a.world_transform(ia[i]).position.x == b.world_transform(ib[i]).position.x);
        CHECK(a.world_transform(ia[i]).scale.x == b.world_transform(ib[i]).scale.x);
        differs = differs || a.world_transform(ia[i]).position.x != c.world_transform(ic[i]).position.x;
    }
    CHECK(differs);
}

TEST_CASE("jitter_entities is one undo entry and refuses what it cannot do") {
    EditorScene s;
    const EntityId a = place(s, "A", 1.0f, 1.0f);
    const EntityId b = place(s, "B", 2.0f, 2.0f);
    const std::vector<EntityId> ids = {a, b, a};
    CHECK(jitter_entities(s, span_of(ids), all_ranges(1)) == 2);  // a once
    CHECK(s.undo());
    CHECK(s.world_transform(a).position.x == 1.0f);
    CHECK(s.world_transform(b).position.y == 2.0f);

    JitterParams zero;  // all ranges zero
    CHECK(jitter_entities(s, span_of(ids), zero) == 0);
    JitterParams bad = all_ranges(1);
    bad.scale_min = 2.0f;  // above the max
    CHECK_FALSE(jitter_params_valid(bad));
    CHECK(jitter_entities(s, span_of(ids), bad) == 0);
    bad = all_ranges(1);
    bad.move_x = -1.0f;
    CHECK(jitter_entities(s, span_of(ids), bad) == 0);
    bad = all_ranges(1);
    bad.rotate_degrees = std::nanf("");
    CHECK(jitter_entities(s, span_of(ids), bad) == 0);
    CHECK(jitter_entities(s, Span<const EntityId>(), all_ranges(1)) == 0);
    CHECK(s.world_transform(a).position.x == 1.0f);  // none of the refusals moved anything
    CHECK(s.world_transform(b).position.x == 2.0f);
}

TEST_CASE("jitter_entities works in world space under a parent, and a moved parent carries its child") {
    EditorScene s;
    const EntityId p = place(s, "P", 10.0f, 0.0f);
    const EntityId c = place(s, "C", 11.0f, 0.0f);
    REQUIRE(s.set_parent(c, p));  // keeps the world position: 11
    const std::vector<EntityId> only_child = {c};
    JitterParams m;
    m.move_x = 0.5f;
    m.seed = 9;
    CHECK(jitter_entities(s, span_of(only_child), m) == 1);
    CHECK(std::fabs(s.world_transform(c).position.x - 11.0f) <= 0.5f + 1e-4f);
    CHECK(std::fabs(s.world_transform(p).position.x - 10.0f) <= 1e-5f);  // the parent did not move

    const f32 child_before = s.world_transform(c).position.x - s.world_transform(p).position.x;
    const std::vector<EntityId> only_parent = {p};
    CHECK(jitter_entities(s, span_of(only_parent), m) == 1);
    CHECK(std::fabs(s.world_transform(c).position.x - s.world_transform(p).position.x - child_before) <= 1e-4f);
}

TEST_CASE("jitter_entities fuzz: every selected entity stays within its ranges, whatever the hierarchy") {
    for (u32 seed = 1; seed <= 200; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        EditorScene s;
        std::vector<EntityId> all;
        for (u32 i = 0, n = 2 + static_cast<u32>(rng() % 7); i < n; ++i)
            all.push_back(place(s, "E", static_cast<f32>(rng() % 400) / 10.0f - 20.0f,
                                static_cast<f32>(rng() % 400) / 10.0f - 20.0f));
        for (u32 i = 0, k = static_cast<u32>(rng() % 5); i < k; ++i)
            (void)s.set_parent(all[rng() % all.size()], all[rng() % all.size()]);
        // Only the roots are selected, so no parent carries a selected child: each one's world change is its own.
        std::vector<EntityId> pick;
        for (EntityId id : all)
            if (s.get<LocalTransform>(id) && s.parent_of(id) == kNoEntity && rng() % 4 != 0) pick.push_back(id);
        std::shuffle(pick.begin(), pick.end(), rng);
        std::vector<WorldTransform> before;
        before.reserve(pick.size());
        for (EntityId id : pick) before.push_back(s.world_transform(id));
        const JitterParams p = all_ranges(static_cast<u32>(rng()));
        CHECK(jitter_entities(s, span_of(pick), p) == pick.size());
        for (usize i = 0; i < pick.size(); ++i) {
            const WorldTransform w = s.world_transform(pick[i]);
            CHECK(std::fabs(w.position.x - before[i].position.x) <= p.move_x + 1e-3f);
            CHECK(std::fabs(w.position.y - before[i].position.y) <= p.move_y + 1e-3f);
            const f32 k = w.scale.x / before[i].scale.x;
            CHECK(k >= p.scale_min - 1e-3f);
            CHECK(k <= p.scale_max + 1e-3f);
        }
    }
}

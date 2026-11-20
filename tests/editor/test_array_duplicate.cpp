// M-LAYOUTTOOLS 1: copy the selection into a grid as one undo entry.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

#include "mce/ecs/transform.h"
#include "mce/editor/array_duplicate.h"

using namespace mce;
using namespace mce::editor;

namespace {

bool close(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }

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

TEST_CASE("array_duplicate fills a grid, row by row, and the originals stay in cell (0, 0)") {
    EditorScene s;
    const EntityId a = place(s, "A", 1.0f, 2.0f);
    const EntityId ids[] = {a};
    Array<EntityId> made;
    CHECK(array_duplicate(s, Span<const EntityId>(ids, 1), 3, 2, 10.0f, -4.0f, &made) == 5);
    REQUIRE(made.size() == 5);
    CHECK(close(world_x(s, a), 1.0f));
    CHECK(close(world_y(s, a), 2.0f));
    usize k = 0;
    for (u32 j = 0; j < 2; ++j)
        for (u32 i = 0; i < 3; ++i) {
            if (i == 0 && j == 0) continue;
            CHECK(close(world_x(s, made[k]), 1.0f + 10.0f * static_cast<f32>(i)));
            CHECK(close(world_y(s, made[k]), 2.0f - 4.0f * static_cast<f32>(j)));
            ++k;
        }
}

TEST_CASE("array_duplicate is one undo entry") {
    EditorScene s;
    const EntityId a = place(s, "A", 0.0f, 0.0f);
    const EntityId b = place(s, "B", 1.0f, 0.0f);
    const EntityId ids[] = {a, b};
    const usize before = s.doc().size();
    const usize undo_before = s.undo_count();
    CHECK(array_duplicate(s, Span<const EntityId>(ids, 2), 2, 2, 5.0f, 5.0f) == 6);
    CHECK(s.doc().size() == before + 6);
    CHECK(s.undo_count() == undo_before + 1);
    CHECK(s.undo());
    CHECK(s.doc().size() == before);
}

TEST_CASE("array_duplicate keeps a copied parent and child together in every cell") {
    EditorScene s;
    const EntityId parent = place(s, "Parent", 0.0f, 0.0f);
    const EntityId child = place(s, "Child", 1.0f, 0.0f);
    REQUIRE(s.set_parent(child, parent));
    const EntityId ids[] = {child, parent};
    Array<EntityId> made;
    CHECK(array_duplicate(s, Span<const EntityId>(ids, 2), 3, 1, 10.0f, 0.0f, &made) == 4);
    std::vector<f32> parents, children;
    for (EntityId id : made) (s.parent_of(id) == kNoEntity ? parents : children).push_back(world_x(s, id));
    std::sort(parents.begin(), parents.end());
    std::sort(children.begin(), children.end());
    REQUIRE(parents.size() == 2);
    REQUIRE(children.size() == 2);
    CHECK(close(parents[0], 10.0f));
    CHECK(close(parents[1], 20.0f));
    CHECK(close(children[0], 11.0f));
    CHECK(close(children[1], 21.0f));
}

TEST_CASE("array_duplicate refuses what it cannot do and leaves no undo entry") {
    EditorScene s;
    const EntityId a = place(s, "A", 0.0f, 0.0f);
    const EntityId ids[] = {a};
    const Span<const EntityId> one(ids, 1);
    const usize before = s.doc().size();
    const usize undo_before = s.undo_count();
    const f32 inf = std::numeric_limits<f32>::infinity();
    CHECK(array_duplicate(s, one, 1, 1, 1.0f, 1.0f) == 0);
    CHECK(array_duplicate(s, one, 0, 3, 1.0f, 1.0f) == 0);
    CHECK(array_duplicate(s, one, 3, 0, 1.0f, 1.0f) == 0);
    CHECK(array_duplicate(s, one, 2, 2, inf, 0.0f) == 0);
    CHECK(array_duplicate(s, one, 2, 2, 0.0f, std::nanf("")) == 0);
    CHECK(array_duplicate(s, one, 100, 100, 1.0f, 1.0f) == 0);
    CHECK(array_duplicate(s, one, 4098, 1, 1.0f, 0.0f) == 0);  // 4097 copies, one over the limit
    const EntityId none[] = {kNoEntity};
    CHECK(array_duplicate(s, Span<const EntityId>(none, 1), 2, 2, 1.0f, 1.0f) == 0);
    CHECK(array_duplicate(s, Span<const EntityId>(), 2, 2, 1.0f, 1.0f) == 0);
    CHECK(s.doc().size() == before);
    CHECK(s.undo_count() == undo_before);
}

TEST_CASE("array_duplicate limit counts every copied entity") {
    EditorScene s;
    const EntityId a = place(s, "A", 0.0f, 0.0f);
    const EntityId b = place(s, "B", 0.0f, 0.0f);
    const EntityId ids[] = {a, b};
    const Span<const EntityId> two(ids, 2);
    const usize before = s.doc().size();
    CHECK(array_duplicate(s, two, 2050, 1, 1.0f, 0.0f) == 0);  // 2049 cells x 2 = 4098
    CHECK(s.doc().size() == before);
    CHECK(array_duplicate(s, two, 2049, 1, 1.0f, 0.0f) == 4096);  // exactly the limit
    CHECK(s.doc().size() == before + 4096);
}

TEST_CASE("array_duplicate fuzz: every copy lands on its grid cell, whatever the selection order") {
    for (u32 seed = 1; seed <= 200; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        EditorScene s;
        std::vector<EntityId> all;
        for (u32 i = 0, n = 2 + static_cast<u32>(rng() % 5); i < n; ++i)
            all.push_back(place(s, "E", static_cast<f32>(static_cast<int>(rng() % 41) - 20),
                                static_cast<f32>(static_cast<int>(rng() % 41) - 20)));
        for (u32 i = 0, k = static_cast<u32>(rng() % 4); i < k; ++i)
            (void)s.set_parent(all[rng() % all.size()], all[rng() % all.size()]);
        std::vector<EntityId> pick;
        for (EntityId id : all)
            if (rng() % 3 != 0) pick.push_back(id);
        if (pick.empty()) pick.push_back(all[0]);
        std::shuffle(pick.begin(), pick.end(), rng);
        const u32 cols = 1 + static_cast<u32>(rng() % 4), rows = 1 + static_cast<u32>(rng() % 4);
        const f32 dx = static_cast<f32>(static_cast<int>(rng() % 21) - 10);
        const f32 dy = static_cast<f32>(static_cast<int>(rng() % 21) - 10);

        std::vector<std::pair<f32, f32>> expect;
        for (u32 j = 0; j < rows; ++j)
            for (u32 i = 0; i < cols; ++i) {
                if (i == 0 && j == 0) continue;
                for (EntityId id : pick)
                    expect.push_back(
                        {world_x(s, id) + static_cast<f32>(i) * dx, world_y(s, id) + static_cast<f32>(j) * dy});
            }
        const usize before = s.doc().size();
        Array<EntityId> made;
        const usize n = array_duplicate(s, Span<const EntityId>(pick.data(), pick.size()), cols, rows, dx, dy, &made);
        if (cols == 1 && rows == 1) {
            CHECK(n == 0);
            continue;
        }
        // A listed entity that appears twice is copied once per listing, so the model counts per listing too only
        // when the picks are unique; the shuffle above never repeats an id.
        REQUIRE(n == expect.size());
        CHECK(s.doc().size() == before + n);
        std::vector<std::pair<f32, f32>> got;
        for (EntityId id : made) got.push_back({world_x(s, id), world_y(s, id)});
        const auto less = [](const auto& l, const auto& r) {
            return l.first != r.first ? l.first < r.first : l.second < r.second;
        };
        std::sort(got.begin(), got.end(), less);
        std::sort(expect.begin(), expect.end(), less);
        REQUIRE(got.size() == expect.size());
        for (usize i = 0; i < got.size(); ++i) {
            CHECK(close(got[i].first, expect[i].first));
            CHECK(close(got[i].second, expect[i].second));
        }
        CHECK(s.undo());
        CHECK(s.doc().size() == before);
    }
}

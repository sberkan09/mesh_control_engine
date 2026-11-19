// M-ALIGN: align and distribute entities on one axis.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

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

// Found by the fuzz below: with a child listed before its parent, the parent moved afterwards and carried the child
// off the line.
TEST_CASE("align_entities puts a child on the line when it is listed before its parent") {
    EditorScene s;
    const EntityId p = place(s, "P", 0.0f, 0.0f);
    const EntityId c = place(s, "C", 10.0f, 0.0f);
    const EntityId other = place(s, "O", 4.0f, 0.0f);
    REQUIRE(s.set_parent(c, p));
    const EntityId ids[] = {c, p, other};
    CHECK(align_entities(s, Span<const EntityId>(ids, 3), AlignAxis::X, AlignMode::Max));
    CHECK(close(world_x(s, p), 10.0f));
    CHECK(close(world_x(s, c), 10.0f));
    CHECK(close(world_x(s, other), 10.0f));
}

// M-ALIGN2: parents and children together in the selection, in any order, still end where the command says.
namespace {

struct Tree {
    EditorScene s;
    std::vector<EntityId> all;
    std::vector<EntityId> pick;  // shuffled, a random subset
};

void build(Tree& t, std::mt19937& rng, usize min_pick) {
    for (u32 i = 0, n = 3 + static_cast<u32>(rng() % 6); i < n; ++i)
        t.all.push_back(place(t.s, "E", static_cast<f32>(static_cast<int>(rng() % 41) - 20),
                              static_cast<f32>(static_cast<int>(rng() % 41) - 20)));
    for (u32 i = 0, k = 1 + static_cast<u32>(rng() % 6); i < k; ++i)
        (void)t.s.set_parent(t.all[rng() % t.all.size()], t.all[rng() % t.all.size()]);
    for (EntityId id : t.all)
        if (rng() % 4 != 0) t.pick.push_back(id);
    std::shuffle(t.pick.begin(), t.pick.end(), rng);
    while (t.pick.size() < min_pick) t.pick.push_back(t.all[rng() % t.all.size()]);
}

}  // namespace

TEST_CASE("align_entities fuzz: every selected entity ends on the target, whatever the order") {
    for (u32 seed = 1; seed <= 200; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        Tree t;
        build(t, rng, 2);
        const AlignAxis axis = rng() % 2 == 0 ? AlignAxis::X : AlignAxis::Y;
        const AlignMode mode = static_cast<AlignMode>(rng() % 3);
        const auto coord = [&](EntityId id) { return axis == AlignAxis::X ? world_x(t.s, id) : world_y(t.s, id); };
        std::vector<EntityId> uniq = t.pick;
        std::sort(uniq.begin(), uniq.end());
        uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
        if (uniq.size() < 2) continue;
        f32 lo = coord(uniq[0]), hi = lo;
        for (EntityId id : uniq) lo = std::min(lo, coord(id)), hi = std::max(hi, coord(id));
        const f32 target = mode == AlignMode::Min ? lo : mode == AlignMode::Max ? hi : (lo + hi) * 0.5f;
        (void)align_entities(t.s, Span<const EntityId>(t.pick.data(), t.pick.size()), axis, mode);
        for (EntityId id : uniq) CHECK(close(coord(id), target, 1e-3f));
    }
}

TEST_CASE("distribute_entities fuzz: the selected entities end evenly spread, whatever the order") {
    for (u32 seed = 1; seed <= 200; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        Tree t;
        build(t, rng, 3);
        const AlignAxis axis = rng() % 2 == 0 ? AlignAxis::X : AlignAxis::Y;
        const auto coord = [&](EntityId id) { return axis == AlignAxis::X ? world_x(t.s, id) : world_y(t.s, id); };
        std::vector<EntityId> uniq = t.pick;
        std::sort(uniq.begin(), uniq.end());
        uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
        if (uniq.size() < 3) continue;
        std::vector<std::pair<f32, EntityId>> sorted;
        sorted.reserve(uniq.size());
        for (EntityId id : uniq) sorted.push_back({coord(id), id});
        std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        const f32 lo = sorted.front().first;
        const f32 step = (sorted.back().first - lo) / static_cast<f32>(sorted.size() - 1);
        (void)distribute_entities(t.s, Span<const EntityId>(t.pick.data(), t.pick.size()), axis);
        // Equal positions have no defined order, so compare the set of final positions with the slots, and check
        // that the order along the axis is kept wherever the start positions differ.
        std::vector<f32> finals;
        finals.reserve(sorted.size());
        for (const auto& e : sorted) finals.push_back(coord(e.second));
        std::sort(finals.begin(), finals.end());
        for (usize i = 0; i < finals.size(); ++i) CHECK(close(finals[i], lo + step * static_cast<f32>(i), 1e-3f));
        for (usize i = 0; i < sorted.size(); ++i)
            for (usize j = 0; j < sorted.size(); ++j)
                if (sorted[i].first + 1e-4f < sorted[j].first)
                    CHECK(coord(sorted[i].second) <= coord(sorted[j].second) + 1e-3f);
    }
}

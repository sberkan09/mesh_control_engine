// M-SELRELATED 1: parents, children, descendants and siblings of a selection.

#include <doctest/doctest.h>

#include <random>
#include <set>
#include <string>
#include <vector>

#include "mce/ecs/hierarchy.h"
#include "mce/editor/select_related.h"

using namespace mce;
using namespace mce::editor;

namespace {

// The same entities, in any order (the document order of siblings is the scene's business, not this test's).
bool is(const Array<EntityId>& a, std::initializer_list<EntityId> want) {
    if (a.size() != want.size()) return false;
    for (const EntityId w : want) {
        bool found = false;
        for (const EntityId x : a) found = found || x == w;
        if (!found) return false;
    }
    return true;
}

}  // namespace

TEST_CASE("select_related walks the ChildOf tree") {
    EditorScene s;
    const EntityId room = s.create_entity("Room");
    const EntityId wall = s.create_entity("Wall");
    const EntityId torch = s.create_entity("Torch");
    const EntityId lamp = s.create_entity("Lamp");
    const EntityId player = s.create_entity("Player");
    REQUIRE(s.set_parent(wall, room));
    REQUIRE(s.set_parent(lamp, room));
    REQUIRE(s.set_parent(torch, wall));

    const EntityId room_only[] = {room};
    const EntityId wall_only[] = {wall};
    const EntityId torch_only[] = {torch};
    const EntityId both[] = {wall, lamp};

    CHECK(is(select_related(s, Span<const EntityId>(wall_only, 1), Relation::Parent), {room}));
    CHECK(select_related(s, Span<const EntityId>(room_only, 1), Relation::Parent).empty());  // a root has no parent
    CHECK(is(select_related(s, Span<const EntityId>(both, 2), Relation::Parent), {room}));   // once, not twice

    CHECK(is(select_related(s, Span<const EntityId>(room_only, 1), Relation::Children), {wall, lamp}));
    CHECK(select_related(s, Span<const EntityId>(torch_only, 1), Relation::Children).empty());  // a leaf

    CHECK(is(select_related(s, Span<const EntityId>(room_only, 1), Relation::Descendants), {wall, lamp, torch}));

    CHECK(is(select_related(s, Span<const EntityId>(wall_only, 1), Relation::Siblings), {wall, lamp}));
    CHECK(is(select_related(s, Span<const EntityId>(room_only, 1), Relation::Siblings), {room, player}));  // roots
}

// Found by the fuzz below: the siblings of an id that is not in the scene came back as "the roots".
TEST_CASE("select_related: an id that is gone has no siblings") {
    EditorScene s;
    const EntityId a = s.create_entity("A");
    (void)a;
    const EntityId gone[] = {static_cast<EntityId>(900)};
    CHECK(select_related(s, Span<const EntityId>(gone, 1), Relation::Siblings).empty());
}

// M-SELRELATED2: every relation equals a plain scan of the parent links (missing, itself, loops), each entity once.
TEST_CASE("select_related fuzz: the four relations equal a plain scan on any parent links") {
    u32 looped = 0;
    for (u32 seed = 1; seed <= 200; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        EditorScene s;
        std::vector<EntityId> ids;
        for (u32 i = 0, n = 1 + static_cast<u32>(rng() % 9); i < n; ++i)
            ids.push_back(s.create_entity("E" + std::to_string(i)));
        for (u32 i = 0, k = static_cast<u32>(rng() % 12); i < k; ++i) {
            const EntityId id = ids[rng() % ids.size()];
            s.add_component(id, refl::type_of<ChildOf>());
            s.set(id, ChildOf{doc_entity_ref(rng() % 5 == 0 ? static_cast<EntityId>(900) : ids[rng() % ids.size()])});
        }
        // The plain parent of each entity: kNoEntity for a root or a link to nothing.
        const auto parent = [&](EntityId id) { return s.parent_of(id); };
        std::vector<EntityId> from;
        for (u32 i = 0, n = static_cast<u32>(rng() % 4); i < n; ++i)
            from.push_back(rng() % 8 == 0 ? static_cast<EntityId>(900) : ids[rng() % ids.size()]);
        const Span<const EntityId> sel(from.data(), from.size());

        const auto as_set = [](const Array<EntityId>& a, bool& unique) {
            std::set<EntityId> st;
            for (EntityId x : a) st.insert(x);
            unique = st.size() == a.size();
            return st;
        };
        const auto exists = [&](EntityId id) { return s.doc().find(id) != nullptr; };
        bool unique = false;

        std::set<EntityId> want;
        for (EntityId f : from)
            if (exists(f) && parent(f) != kNoEntity) want.insert(parent(f));
        CHECK(as_set(select_related(s, sel, Relation::Parent), unique) == want);
        CHECK(unique);

        want.clear();
        for (EntityId f : from)
            for (EntityId x : ids)
                if (exists(f) && parent(x) == f) want.insert(x);
        CHECK(as_set(select_related(s, sel, Relation::Children), unique) == want);
        CHECK(unique);

        want.clear();
        for (EntityId f : from) {
            if (!exists(f)) continue;
            std::vector<EntityId> todo{f};
            std::set<EntityId> seen;
            while (!todo.empty()) {
                const EntityId cur = todo.back();
                todo.pop_back();
                for (EntityId x : ids)
                    if (parent(x) == cur && seen.insert(x).second) todo.push_back(x);
            }
            if (seen.count(f)) ++looped;
            want.insert(seen.begin(), seen.end());
        }
        CHECK(as_set(select_related(s, sel, Relation::Descendants), unique) == want);
        CHECK(unique);

        want.clear();
        for (EntityId f : from)
            for (EntityId x : ids)
                // The roots' group also holds the entities of a ChildOf cycle (they have no root above them).
                if (exists(f) && (parent(x) == parent(f) || (parent(f) == kNoEntity && s.is_ancestor(x, x))))
                    want.insert(x);
        CHECK(as_set(select_related(s, sel, Relation::Siblings), unique) == want);
        CHECK(unique);
    }
    CHECK(looped > 5);
}

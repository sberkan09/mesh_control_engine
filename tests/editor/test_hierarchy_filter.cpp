// M-HIERFILTER 1: the Hierarchy search box.

#include <doctest/doctest.h>

#include <random>
#include <string>
#include <vector>

#include "mce/ecs/hierarchy.h"
#include "mce/editor/hierarchy_filter.h"

using namespace mce;
using namespace mce::editor;

TEST_CASE("HierarchyFilter name_matches") {
    CHECK(HierarchyFilter::name_matches("Torch Left", ""));
    CHECK(HierarchyFilter::name_matches("Torch Left", "torch"));
    CHECK(HierarchyFilter::name_matches("Torch Left", "LEFT tor"));  // every word, any order, any case
    CHECK_FALSE(HierarchyFilter::name_matches("Torch Left", "torch right"));
    CHECK(HierarchyFilter::name_matches("Torch Left", "  torch   "));
    CHECK_FALSE(HierarchyFilter::name_matches("", "x"));
}

TEST_CASE("HierarchyFilter shows the matches and their ancestors") {
    EditorScene s;
    const EntityId room = s.create_entity("Room");
    const EntityId wall = s.create_entity("Wall");
    const EntityId torch = s.create_entity("Torch");
    const EntityId player = s.create_entity("Player");
    REQUIRE(s.set_parent(wall, room));
    REQUIRE(s.set_parent(torch, wall));

    HierarchyFilter f;
    f.rebuild(s, "");
    CHECK_FALSE(f.active());
    CHECK(f.visible(s.doc(), player));  // inactive: everything shows

    f.rebuild(s, "torch");
    CHECK(f.active());
    CHECK(f.match_count() == 1);
    CHECK(f.is_match(s.doc(), torch));
    CHECK(f.visible(s.doc(), wall));  // ancestors lead to the hit
    CHECK(f.visible(s.doc(), room));
    CHECK_FALSE(f.is_match(s.doc(), wall));
    CHECK_FALSE(f.visible(s.doc(), player));

    f.rebuild(s, "nothing here");
    CHECK(f.active());
    CHECK(f.match_count() == 0);
    CHECK_FALSE(f.visible(s.doc(), room));
}

// M-HIERFILTER2: the flags equal a plain model on any parent links (missing, itself, loops).
TEST_CASE("HierarchyFilter fuzz: matches and their ancestors equal a plain walk") {
    static const char* const kNames[] = {"Torch", "torch left", "Wall", "Room Torch", "", "Door", "TORCH"};
    static const char* const kQueries[] = {"torch", "TORCH left", "wall door", "o", "left  torch", "zzz", "  ", ""};
    u32 looped = 0;
    for (u32 seed = 1; seed <= 200; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        EditorScene s;
        std::vector<EntityId> ids;
        for (u32 i = 0, n = static_cast<u32>(rng() % 10); i < n; ++i) ids.push_back(s.create_entity(kNames[rng() % 7]));
        if (!ids.empty())
            for (u32 i = 0, k = static_cast<u32>(rng() % 12); i < k; ++i) {
                const EntityId id = ids[rng() % ids.size()];
                s.add_component(id, refl::type_of<ChildOf>());
                s.set(id,
                      ChildOf{doc_entity_ref(rng() % 5 == 0 ? static_cast<EntityId>(900) : ids[rng() % ids.size()])});
            }
        const std::string query = kQueries[rng() % 8];
        HierarchyFilter f;
        f.rebuild(s, query);
        const bool blank = query.find_first_not_of(' ') == std::string::npos;
        CHECK(f.active() == !blank);
        if (blank) {
            for (EntityId id : ids) CHECK(f.visible(s.doc(), id));
            continue;
        }
        // The model: an entity is visible when it matches or sits on the parent chain of a match.
        std::vector<bool> match(ids.size(), false), shown(ids.size(), false);
        usize count = 0;
        for (usize i = 0; i < ids.size(); ++i) {
            const DocEntity* e = s.doc().find(ids[i]);
            const StringView nm = e->name.view();
            match[i] = HierarchyFilter::name_matches(nm, StringView(query.data(), query.size()));
            count += match[i] ? 1 : 0;
        }
        for (usize i = 0; i < ids.size(); ++i) {
            if (!match[i]) continue;
            shown[i] = true;
            std::vector<EntityId> seen;
            bool loop = false;
            for (EntityId p = s.parent_of(ids[i]); p != kNoEntity && s.doc().find(p) != nullptr; p = s.parent_of(p)) {
                bool dup = false;
                for (EntityId q : seen) dup = dup || q == p;
                if (dup) {
                    loop = true;
                    break;
                }
                seen.push_back(p);
                for (usize j = 0; j < ids.size(); ++j)
                    if (ids[j] == p) shown[j] = true;
            }
            looped += loop ? 1 : 0;
        }
        CHECK(f.match_count() == count);
        for (usize i = 0; i < ids.size(); ++i) {
            CHECK(f.visible(s.doc(), ids[i]) == shown[i]);
            CHECK(f.is_match(s.doc(), ids[i]) == match[i]);
        }
    }
    CHECK(looped > 5);
}

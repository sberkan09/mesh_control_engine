// M-HIERFILTER 1: the Hierarchy search box.

#include <doctest/doctest.h>

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

// M-SELALL 1: select all and invert the selection. M-SELMATCH: select by name.

#include <doctest/doctest.h>

#include "mce/editor/select_all.h"

using namespace mce;
using namespace mce::editor;

TEST_CASE("select_all and invert_selection") {
    EditorScene s;
    CHECK(select_all(s).empty());
    CHECK(invert_selection(s).empty());

    const EntityId a = s.create_entity("A");
    const EntityId b = s.create_entity("B");
    const EntityId c = s.create_entity("C");

    const Array<EntityId> all = select_all(s);
    REQUIRE(all.size() == 3);
    CHECK(all[0] == a);  // document order
    CHECK(all[1] == b);
    CHECK(all[2] == c);

    CHECK(invert_selection(s).size() == 3);  // nothing selected: everything

    s.selection().set(b);
    const Array<EntityId> rest = invert_selection(s);
    REQUIRE(rest.size() == 2);
    CHECK(rest[0] == a);
    CHECK(rest[1] == c);

    s.selection().set(a);
    s.selection().toggle(b);
    s.selection().toggle(c);
    CHECK(invert_selection(s).empty());  // all selected: none
}

TEST_CASE("select_matching returns the named entities in document order") {
    EditorScene s;
    const EntityId a = s.create_entity("Wall North");
    s.create_entity("Player");
    const EntityId c = s.create_entity("wall south");
    CHECK(select_matching(s, "").empty());  // blank: nothing (the filter box shows everything, selecting would not)
    CHECK(select_matching(s, "  ").empty());
    CHECK(select_matching(s, "nothing").empty());

    const Array<EntityId> walls = select_matching(s, "WALL");
    REQUIRE(walls.size() == 2);
    CHECK(walls[0] == a);
    CHECK(walls[1] == c);
    CHECK(select_matching(s, "wall south").size() == 1);
}

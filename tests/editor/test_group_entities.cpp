// M-GROUP: group the selection under a new parent, and ungroup.

#include <doctest/doctest.h>

#include "mce/editor/group_entities.h"

using namespace mce;
using namespace mce::editor;

TEST_CASE("group_entities wraps the top-level entities in a new parent") {
    EditorScene s;
    const EntityId a = s.create_entity("A");
    const EntityId b = s.create_entity("B");
    const EntityId c = s.create_entity("C");
    const EntityId other = s.create_entity("Other");
    REQUIRE(s.set_parent(c, b));  // C is under B

    const EntityId ids[] = {a, b, c};  // C has an ancestor in the list: it stays under B
    const EntityId g = group_entities(s, Span<const EntityId>(ids, 3));
    REQUIRE(g != kNoEntity);
    CHECK(s.parent_of(a) == g);
    CHECK(s.parent_of(b) == g);
    CHECK(s.parent_of(c) == b);
    CHECK(s.parent_of(g) == kNoEntity);
    CHECK(s.parent_of(other) == kNoEntity);

    CHECK(s.undo());  // one entry
    CHECK(s.doc().find(g) == nullptr);
    CHECK(s.parent_of(a) == kNoEntity);
    CHECK(s.parent_of(c) == b);

    CHECK(group_entities(s, Span<const EntityId>()) == kNoEntity);
}

TEST_CASE("group_entities puts the group under the first entity's parent") {
    EditorScene s;
    const EntityId root = s.create_entity("Root");
    const EntityId x = s.create_entity("X");
    REQUIRE(s.set_parent(x, root));
    const EntityId ids[] = {x};
    const EntityId g = group_entities(s, Span<const EntityId>(ids, 1));
    CHECK(s.parent_of(g) == root);
    CHECK(s.parent_of(x) == g);
}

TEST_CASE("ungroup_entities frees the children and deletes the parent") {
    EditorScene s;
    const EntityId root = s.create_entity("Root");
    const EntityId g = s.create_entity("G");
    const EntityId k1 = s.create_entity("K1");
    const EntityId k2 = s.create_entity("K2");
    const EntityId leaf = s.create_entity("Leaf");
    REQUIRE(s.set_parent(g, root));
    REQUIRE(s.set_parent(k1, g));
    REQUIRE(s.set_parent(k2, g));

    const EntityId leaf_only[] = {leaf};
    CHECK(ungroup_entities(s, Span<const EntityId>(leaf_only, 1)).empty());  // no children: nothing changes
    CHECK(s.doc().find(leaf) != nullptr);

    const EntityId ids[] = {g};
    const Array<EntityId> freed = ungroup_entities(s, Span<const EntityId>(ids, 1));
    REQUIRE(freed.size() == 2);
    CHECK(s.doc().find(g) == nullptr);
    CHECK(s.parent_of(k1) == root);  // moved to the group's parent
    CHECK(s.parent_of(k2) == root);

    CHECK(s.undo());  // one entry
    CHECK(s.doc().find(g) != nullptr);
    CHECK(s.parent_of(k1) == g);
}

// M-COPYPATH: hierarchy paths of entities.

#include <doctest/doctest.h>

#include "mce/editor/entity_path.h"

using namespace mce;
using namespace mce::editor;

TEST_CASE("entity_path joins the names from the root down") {
    EditorScene s;
    const EntityId room = s.create_entity("Room");
    const EntityId wall = s.create_entity("Wall");
    const EntityId torch = s.create_entity("Torch");
    const EntityId player = s.create_entity("Player");
    REQUIRE(s.set_parent(wall, room));
    REQUIRE(s.set_parent(torch, wall));

    CHECK(entity_path(s, torch).view() == "Room/Wall/Torch");
    CHECK(entity_path(s, wall).view() == "Room/Wall");
    CHECK(entity_path(s, room).view() == "Room");
    CHECK(entity_path(s, player).view() == "Player");
    CHECK(entity_path(s, 9999).empty());  // not in the scene
    CHECK(entity_path(s, kNoEntity).empty());
}

TEST_CASE("entity_paths lists one path per line in the order given") {
    EditorScene s;
    const EntityId a = s.create_entity("A");
    const EntityId b = s.create_entity("B");
    REQUIRE(s.set_parent(b, a));
    const EntityId ids[] = {b, 9999, a};  // the missing one is skipped
    CHECK(entity_paths(s, Span<const EntityId>(ids, 3)).view() == "A/B\nA");
    CHECK(entity_paths(s, Span<const EntityId>()).empty());
}

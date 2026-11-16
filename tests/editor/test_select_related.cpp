// M-SELRELATED 1: parents, children, descendants and siblings of a selection.

#include <doctest/doctest.h>

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

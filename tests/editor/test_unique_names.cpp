// M-UNIQUENAMES: numbered suffixes for entities that share a name.

#include <doctest/doctest.h>

#include "mce/editor/unique_names.h"

using namespace mce;
using namespace mce::editor;

namespace {

StringView name_of(const EditorScene& s, EntityId id) { return s.doc().find(id)->name.view(); }

Array<EntityId> everyone(const EditorScene& s) {
    Array<EntityId> ids;
    for (const DocEntity& e : s.doc().entities()) ids.push_back(e.id);
    return ids;
}

}  // namespace

TEST_CASE("make_names_unique numbers the later holders of a name") {
    EditorScene s;
    const EntityId a = s.create_entity("Wall");
    const EntityId b = s.create_entity("Wall");
    const EntityId c = s.create_entity("Wall");
    const EntityId d = s.create_entity("Lamp");
    const Array<EntityId> ids = everyone(s);

    CHECK(make_names_unique(s, ids.span()) == 2);
    CHECK(name_of(s, a) == "Wall");  // the first keeps it
    CHECK(name_of(s, b) == "Wall (2)");
    CHECK(name_of(s, c) == "Wall (3)");
    CHECK(name_of(s, d) == "Lamp");

    CHECK(make_names_unique(s, ids.span()) == 0);  // already unique: nothing changes
    CHECK(s.undo());                               // one entry for the whole pass
    CHECK(name_of(s, b) == "Wall");
    CHECK(name_of(s, c) == "Wall");
}

TEST_CASE("make_names_unique respects names outside the list and replaces an old suffix") {
    EditorScene s;
    const EntityId keep = s.create_entity("Door (2)");
    const EntityId first = s.create_entity("Door");
    const EntityId again = s.create_entity("Door (2)");  // taken by `keep`, which is not in the list
    const EntityId list[] = {first, again};
    CHECK(make_names_unique(s, Span<const EntityId>(list, 2)) == 1);
    CHECK(name_of(s, keep) == "Door (2)");
    CHECK(name_of(s, first) == "Door");
    CHECK(name_of(s, again) == "Door (3)");  // "Door (2)" is taken: the suffix is replaced, not stacked
}

TEST_CASE("make_names_unique keeps long names within the name limit") {
    EditorScene s;
    const char* long_name =
        "012345678901234567890123456789012345678901234567890123456789012";  // 63 characters, the most a name holds
    const EntityId a = s.create_entity(long_name);
    const EntityId b = s.create_entity(long_name);
    const Array<EntityId> ids = everyone(s);
    CHECK(make_names_unique(s, ids.span()) == 1);
    CHECK(name_of(s, a).size() == 63);
    CHECK(name_of(s, b).size() == 63);
    CHECK(name_of(s, b)[60] == '(');
    CHECK(name_of(s, a) != name_of(s, b));
}

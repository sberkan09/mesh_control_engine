// M-COPYPATH: hierarchy paths of entities.

#include <doctest/doctest.h>

#include <random>
#include <string>
#include <vector>

#include "mce/ecs/hierarchy.h"
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

// M-ENTITYPATH2: a path names each entity once, whatever the parent links are.
namespace {

usize segments(const cook::String& p) {
    if (p.empty()) return 0;
    usize n = 1;
    for (char c : std::string(p.view().data(), p.view().size())) n += (c == '/');
    return n;
}

}  // namespace

TEST_CASE("entity_path of a loop of parents names every entity once") {
    EditorScene s;
    const EntityId a = s.create_entity("A");
    const EntityId b = s.create_entity("B");
    const EntityId c = s.create_entity("C");
    for (EntityId id : {a, b, c}) s.add_component(id, refl::type_of<ChildOf>());
    s.set(a, ChildOf{doc_entity_ref(b)});
    s.set(b, ChildOf{doc_entity_ref(c)});
    s.set(c, ChildOf{doc_entity_ref(a)});
    CHECK(segments(entity_path(s, a)) <= 3);
    CHECK(segments(entity_path(s, b)) <= 3);
}

TEST_CASE("entity_path fuzz: the segments follow the parent chain on any links") {
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
        for (EntityId id : ids) {
            // The model: walk up while the parent is in the scene, stop at a repeat.
            std::vector<EntityId> chain;
            bool loop = false;
            for (EntityId cur = id; cur != kNoEntity && s.doc().find(cur) != nullptr; cur = s.parent_of(cur)) {
                bool seen = false;
                for (EntityId c : chain) seen = seen || c == cur;
                if (seen) {
                    loop = true;
                    break;
                }
                chain.push_back(cur);
            }
            looped += loop ? 1 : 0;
            const cook::String p = entity_path(s, id);
            CHECK(segments(p) <= s.doc().size());
            std::string want;
            for (usize i = chain.size(); i > 0; --i) {
                if (i != chain.size()) want += '/';
                want += std::string(s.doc().find(chain[i - 1])->name.view().data(),
                                    s.doc().find(chain[i - 1])->name.view().size());
            }
            if (!loop) CHECK(std::string(p.view().data(), p.view().size()) == want);
        }
    }
    CHECK(looped > 20);
}

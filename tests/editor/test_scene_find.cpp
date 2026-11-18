// M-SCENEFIND: which entities of a scene document match a question.

#include <doctest/doctest.h>

#include <cctype>
#include <random>
#include <string>
#include <string_view>

#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/editor/editor_scene.h"
#include "mce/editor/scene_find.h"
#include "mce/kit/components.h"

using namespace mce;
using namespace mce::editor;

namespace {

struct Fixture {
    EditorScene s;
    EntityId room, wall, torch, other;
    Fixture() {
        room = s.create_entity("Room");
        wall = s.create_entity("Wall");
        torch = s.create_entity("Torch");
        other = s.create_entity("Hall Torch");
        s.set_parent(wall, room);
        s.set_parent(torch, wall);
        s.add_component(torch, refl::type_of<kit::Sprite>());
        s.add_component(wall, refl::type_of<kit::Sprite>());
    }
};

}  // namespace

TEST_CASE("an empty query matches every entity, in file order") {
    Fixture f;
    const Array<FindHit> hits = find_entities(f.s.doc(), {});
    REQUIRE(hits.size() == 4);
    CHECK(hits[0].id == f.room);
    CHECK(hits[3].id == f.other);
}

TEST_CASE("a name matches a part of it, ignoring case") {
    Fixture f;
    FindQuery q;
    q.name = "TORCH";
    const Array<FindHit> hits = find_entities(f.s.doc(), q);
    REQUIRE(hits.size() == 2);
    CHECK(hits[0].id == f.torch);
    CHECK(hits[1].id == f.other);
    q.name = "nothing like it";
    CHECK(find_entities(f.s.doc(), q).empty());
}

TEST_CASE("a component and a name must both hold; the hit carries its hierarchy path") {
    Fixture f;
    FindQuery q;
    q.name = "torch";
    q.component = &refl::type_of<kit::Sprite>();
    const Array<FindHit> hits = find_entities(f.s.doc(), q);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].id == f.torch);
    CHECK(hits[0].path.view() == "Room/Wall/Torch");
    CHECK(hits[0].name.view() == "Torch");

    q.name = {};
    CHECK(find_entities(f.s.doc(), q).size() == 2);  // Wall and Torch carry a Sprite
}

TEST_CASE("prefab instances only: none in a scene made of plain entities") {
    Fixture f;
    FindQuery q;
    q.prefab_instances_only = true;
    CHECK(find_entities(f.s.doc(), q).empty());
}

TEST_CASE("the report lists one path per hit, or says there are none") {
    Fixture f;
    FindQuery q;
    q.name = "torch";
    const Array<FindHit> hits = find_entities(f.s.doc(), q);
    const cook::String text = format_hits(hits.span());
    const std::string_view v = text.c_str();
    CHECK(v.find("Room/Wall/Torch (id ") != std::string_view::npos);
    CHECK(v.find("\nHall Torch (id ") != std::string_view::npos);

    q.name = "zzz";
    const Array<FindHit> none = find_entities(f.s.doc(), q);
    CHECK(std::string_view(format_hits(none.span()).c_str()) == "no matches");
}

// M-SCENEFIND2 step 1: random scenes, including parents that are missing, the entity itself or a loop. Each query is
// checked against a plain scan of the document: the same ids in file order, every hit's path ends in its name, and a
// loop of parents does not hang the path walk.
TEST_CASE("find_entities fuzz: the hits equal a plain scan, in file order, on any parent links") {
    const refl::TypeRecord& sprite = refl::type_of<kit::Sprite>();
    const refl::TypeRecord& transform = refl::type_of<LocalTransform>();
    u32 nonempty = 0;
    for (u32 seed = 1; seed <= 200; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        EditorScene s;
        const char* const names[] = {"Wall", "wall", "WALL 2", "Torch", "Hall Torch", "Room", "", "a/b", "Ünï"};
        for (u32 i = 0, n = 1 + static_cast<u32>(rng() % 10); i < n; ++i) s.create_entity(names[rng() % 9]);
        const auto any = [&]() { return s.doc().at(rng() % s.doc().size()).id; };
        for (u32 i = 0, k = static_cast<u32>(rng() % 12); i < k; ++i) {
            const EntityId id = any();
            switch (rng() % 4) {
                case 0:
                    s.add_component(id, sprite);
                    break;
                case 1:
                    s.remove_component(id, transform);
                    break;
                case 2:
                    s.add_component(id, refl::type_of<ChildOf>());
                    s.set(id, ChildOf{doc_entity_ref(rng() % 5 == 0 ? static_cast<EntityId>(900)
                                                                    : any())});  // itself, a loop, or nothing
                    break;
                default:
                    s.set_parent(id, any());
                    break;
            }
        }
        const char* const parts[] = {"", "wall", "WALL", "all", "torch", "x", "a/b", "Ünï"};
        for (const char* part : parts) {
            for (int comp = 0; comp < 3; ++comp) {
                FindQuery q;
                q.name = StringView(part);
                q.component = comp == 0 ? nullptr : (comp == 1 ? &sprite : &transform);
                const Array<FindHit> hits = find_entities(s.doc(), q);

                std::string needle = part;
                for (char& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                usize at = 0;
                for (const DocEntity& e : s.doc().entities()) {
                    std::string hay(e.name.view().data(), e.name.view().size());
                    for (char& c : hay) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    const bool want = hay.find(needle) != std::string::npos &&
                                      (q.component == nullptr || e.find(*q.component) != nullptr);
                    if (!want) continue;
                    REQUIRE(at < hits.size());
                    CHECK(hits[at].id == e.id);
                    const std::string path(hits[at].path.view().data(), hits[at].path.view().size());
                    const std::string name(e.name.view().data(), e.name.view().size());
                    CHECK(path.size() >= name.size());
                    CHECK(path.compare(path.size() - name.size(), name.size(), name) == 0);
                    ++at;
                }
                CHECK(hits.size() == at);
                if (at > 0) ++nonempty;
            }
        }
        FindQuery prefab;
        prefab.prefab_instances_only = true;
        CHECK(find_entities(s.doc(), prefab).empty());
    }
    CHECK(nonempty > 400);
}

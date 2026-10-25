// M-SCENEFIND: which entities of a scene document match a question.

#include <doctest/doctest.h>

#include <string_view>

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

// M-SCENESTATS: what a scene document holds at a glance.

#include <doctest/doctest.h>

#include <string_view>

#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/editor/editor_scene.h"
#include "mce/editor/scene_stats.h"
#include "mce/kit/components.h"

using namespace mce;
using namespace mce::editor;

namespace {

u32 uses_of(const SceneStats& s, std::string_view type) {
    for (const ComponentUse& u : s.components)
        if (type == u.type->name) return u.entities;
    return 0;
}

void place(EditorScene& s, EntityId id, f32 x, f32 y) {
    s.add_component(id, refl::type_of<LocalTransform>());
    LocalTransform t;
    t.position = {x, y, 0.0f};
    (void)s.set(id, t);
}

}  // namespace

TEST_CASE("an empty scene has empty stats") {
    EditorScene s;
    const SceneStats st = scene_stats(s.doc());
    CHECK(st.entities == 0);
    CHECK(st.roots == 0);
    CHECK(st.max_depth == 0);
    CHECK_FALSE(st.has_bounds);
    CHECK(st.components.empty());
}

TEST_CASE("entities, roots, depth and the components in use are counted") {
    EditorScene s;
    const EntityId room = s.create_entity("Room");
    const EntityId wall = s.create_entity("Wall");
    const EntityId torch = s.create_entity("Torch");
    const EntityId lone = s.create_entity("Lone");
    s.set_parent(wall, room);
    s.set_parent(torch, wall);
    place(s, room, 0, 0);
    place(s, wall, 4, -2);
    place(s, torch, 1, 7);
    s.add_component(wall, refl::type_of<kit::Sprite>());
    s.add_component(torch, refl::type_of<kit::Sprite>());
    (void)lone;

    const SceneStats st = scene_stats(s.doc());
    CHECK(st.entities == 4);
    CHECK(st.roots == 2);  // Room and Lone
    CHECK(st.max_depth == 2);  // Torch under Wall under Room
    CHECK(st.prefab_instances == 0);
    CHECK(uses_of(st, "Sprite") == 2);
    CHECK(uses_of(st, "LocalTransform") == 4);  // new entities start with one
    CHECK(st.has_bounds);
    CHECK(st.min.x == 0.0f);
    CHECK(st.min.y == -2.0f);
    CHECK(st.max.x == 4.0f);
    CHECK(st.max.y == 7.0f);
    // The most used component is listed first.
    REQUIRE(st.components.size() >= 2);
    CHECK(st.components[0].entities >= st.components[1].entities);
}

TEST_CASE("the report names the facts") {
    EditorScene s;
    const EntityId a = s.create_entity("A");
    place(s, a, 1, 2);
    const cook::String text = format_stats(scene_stats(s.doc()));
    const std::string_view v = text.c_str();
    CHECK(v.find("entities: 1 (1 root, depth 0)") != std::string_view::npos);
    CHECK(v.find("bounds: (1, 2, 0) to (1, 2, 0)") != std::string_view::npos);
    CHECK(v.find("LocalTransform: 1") != std::string_view::npos);
}

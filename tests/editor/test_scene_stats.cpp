// M-SCENESTATS: what a scene document holds at a glance.

#include <doctest/doctest.h>

#include <algorithm>
#include <random>
#include <string>
#include <string_view>
#include <utility>

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

// Found by the fuzz below: the depth walk's guard allowed one step too many, so a loop of parents in a scene of 3
// entities reported depth 4. A chain of n entities is n - 1 deep.
TEST_CASE("a loop of parents reports a depth below the entity count") {
    EditorScene s;
    const EntityId a = s.create_entity("A");
    const EntityId b = s.create_entity("B");
    const EntityId c = s.create_entity("C");
    for (const auto& link : {std::pair{a, b}, std::pair{b, c}, std::pair{c, a}}) {
        s.add_component(link.first, refl::type_of<ChildOf>());
        REQUIRE(s.set(link.first, ChildOf{doc_entity_ref(link.second)}));
    }
    CHECK(scene_stats(s.doc()).max_depth == 2);
    EditorScene chain;
    const EntityId x = chain.create_entity("X");
    const EntityId y = chain.create_entity("Y");
    const EntityId z = chain.create_entity("Z");
    chain.set_parent(y, x);
    chain.set_parent(z, y);
    CHECK(scene_stats(chain.doc()).max_depth == 2);  // a real chain keeps its full depth
}

// M-SCENESTATS2 step 1: random scenes, including parents that are missing, the entity itself or a loop. The counts
// equal a plain scan of the document; the depth equals a walk of the parent chains when the scene has no loop, and any
// scene (a loop included) gets its stats at all, with a depth below the entity count.
TEST_CASE("scene_stats fuzz: the numbers equal a plain scan on any parent links") {
    const refl::TypeRecord& sprite = refl::type_of<kit::Sprite>();
    u32 looped = 0;
    for (u32 seed = 1; seed <= 200; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        EditorScene s;
        for (u32 i = 0, n = static_cast<u32>(rng() % 11); i < n; ++i) s.create_entity("E" + std::to_string(i));
        const auto any = [&]() { return s.doc().at(rng() % s.doc().size()).id; };
        if (s.doc().size() > 0)
            for (u32 i = 0, k = static_cast<u32>(rng() % 14); i < k; ++i) {
                const EntityId id = any();
                switch (rng() % 6) {
                    case 0:
                        s.add_component(id, sprite);
                        break;
                    case 1:
                        place(s, id, static_cast<f32>(static_cast<int>(rng() % 21) - 10),
                              static_cast<f32>(static_cast<int>(rng() % 21) - 10));
                        break;
                    case 2:
                        s.add_component(id, refl::type_of<ChildOf>());
                        s.set(id, ChildOf{doc_entity_ref(rng() % 5 == 0 ? static_cast<EntityId>(900) : any())});
                        break;
                    case 3:
                        s.remove_component(id, refl::type_of<LocalTransform>());
                        break;
                    case 4:
                        s.add_component(id, refl::type_of<ChildOf>());
                        s.set(id, ChildOf{doc_entity_ref(any())});  // any entity: itself or a loop
                        break;
                    default:
                        s.set_parent(id, any());
                        break;
                }
            }
        const SceneDocument& doc = s.doc();
        const SceneStats st = scene_stats(doc);
        CHECK(st.entities == doc.size());

        const auto parent_of = [&](const DocEntity& e) -> const DocEntity* {
            const DocComponent* c = e.find(refl::type_of<ChildOf>());
            if (c == nullptr) return nullptr;
            const EntityId p = doc_entity_id(static_cast<const ChildOf*>(c->data())->parent);
            return p == e.id ? nullptr : doc.find(p);
        };
        u32 roots = 0, depth = 0, sprites = 0;
        bool loop = false, any_pos = false;
        math::vec3 lo{0, 0, 0}, hi{0, 0, 0};
        for (const DocEntity& e : doc.entities()) {
            if (parent_of(e) == nullptr) ++roots;
            u32 d = 0;
            for (const DocEntity* up = parent_of(e); up != nullptr; up = parent_of(*up)) {
                if (++d > doc.size()) {
                    loop = true;
                    break;
                }
            }
            if (d <= doc.size()) depth = d > depth ? d : depth;
            if (e.find(sprite) != nullptr) ++sprites;
            if (const DocComponent* t = e.find(refl::type_of<LocalTransform>())) {
                const math::vec3 p = static_cast<const LocalTransform*>(t->data())->position;
                if (!any_pos) {
                    lo = hi = p;
                    any_pos = true;
                } else {
                    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
                }
            }
        }
        CHECK(st.roots == roots);
        CHECK(uses_of(st, "Sprite") == sprites);
        CHECK(st.has_bounds == any_pos);
        if (any_pos) {
            CHECK(st.min.x == lo.x);
            CHECK(st.min.y == lo.y);
            CHECK(st.max.x == hi.x);
            CHECK(st.max.y == hi.y);
        }
        if (loop) {
            ++looped;
            CHECK(st.max_depth + 1 <= doc.size());  // a chain of n entities is n - 1 deep: a loop never reports more
        } else {
            CHECK(st.max_depth == depth);
        }
        for (usize i = 1; i < st.components.size(); ++i)
            CHECK(st.components[i - 1].entities >= st.components[i].entities);
    }
    CHECK(looped > 5);  // the generator does build loops
}

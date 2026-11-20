// M-STACKLINT: entities that are exact copies of each other.

#include <doctest/doctest.h>

#include <algorithm>
#include <map>
#include <random>
#include <tuple>
#include <vector>

#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/editor/array_duplicate.h"
#include "mce/editor/editor_scene.h"
#include "mce/editor/stacked_entities.h"

using namespace mce;
using namespace mce::editor;

namespace {

EntityId place(EditorScene& s, const char* name, f32 x, f32 y) {
    const EntityId id = s.create_entity(name);
    LocalTransform t;
    t.position = {x, y, 0.0f};
    s.set(id, t);
    return id;
}

}  // namespace

TEST_CASE("find_stacked reports copies on one spot and ignores names") {
    EditorScene s;
    const EntityId a = place(s, "Crate", 1.0f, 2.0f);
    const EntityId b = place(s, "Crate 2", 1.0f, 2.0f);  // another name: still a copy
    place(s, "Elsewhere", 5.0f, 2.0f);
    const EntityId d = place(s, "Crate 3", 1.0f, 2.0f);
    const Array<StackedGroup> g = find_stacked(s.doc());
    REQUIRE(g.size() == 1);
    REQUIRE(g[0].ids.size() == 3);
    CHECK(g[0].ids[0] == a);
    CHECK(g[0].ids[1] == b);
    CHECK(g[0].ids[2] == d);
    const cook::String text = format_stacked(s.doc(), Span<const StackedGroup>(g.data(), g.size()));
    CHECK(text.view().find("Crate (id") != StringView::npos);
    CHECK(text.view().find("x3") != StringView::npos);
}

TEST_CASE("find_stacked tells different parents and different components apart, and finds nothing in a tidy scene") {
    EditorScene s;
    const EntityId p1 = place(s, "P1", 0.0f, 0.0f);
    const EntityId p2 = place(s, "P2", 9.0f, 0.0f);
    const EntityId a = place(s, "A", 1.0f, 1.0f);
    const EntityId b = place(s, "B", 1.0f, 1.0f);
    REQUIRE(s.set_parent(a, p1));  // the same world spot, but not the same local value or parent
    REQUIRE(s.set_parent(b, p2));
    CHECK(find_stacked(s.doc()).empty());
    const EntityId c = place(s, "C", 4.0f, 4.0f);
    const EntityId d = place(s, "D", 4.0f, 4.0f);
    s.add_component(d, refl::type_of<ChildOf>());  // one more component than C
    (void)c;
    CHECK(find_stacked(s.doc()).empty());
}

TEST_CASE("find_stacked catches an array duplicate with no step") {
    EditorScene s;
    const EntityId a = place(s, "Lamp", 0.0f, 0.0f);
    const EntityId ids[] = {a};
    REQUIRE(array_duplicate(s, Span<const EntityId>(ids, 1), 3, 1, 0.0f, 0.0f) == 2);
    const Array<StackedGroup> g = find_stacked(s.doc());
    REQUIRE(g.size() == 1);
    CHECK(g[0].ids.size() == 3);
    CHECK(g[0].ids[0] == a);
}

TEST_CASE("find_stacked fuzz: the groups match a plain model over the document's values") {
    for (u32 seed = 1; seed <= 200; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        EditorScene s;
        std::vector<EntityId> all;
        for (u32 i = 0, n = 2 + static_cast<u32>(rng() % 10); i < n; ++i)
            all.push_back(place(s, "E", static_cast<f32>(rng() % 3), static_cast<f32>(rng() % 2)));
        for (u32 i = 0, k = static_cast<u32>(rng() % 4); i < k; ++i)
            (void)s.set_parent(all[rng() % all.size()], all[rng() % all.size()]);
        if (rng() % 3 == 0) s.destroy_entity(all[rng() % all.size()]);

        // The model: entities with equal (local position, parent) are copies.
        std::map<std::tuple<f32, f32, f32, EntityId>, std::vector<EntityId>> model;
        for (const DocEntity& e : s.doc().entities()) {
            const DocComponent* t = e.find(refl::type_of<LocalTransform>());
            if (t == nullptr) continue;
            const math::vec3 p = static_cast<const LocalTransform*>(t->data())->position;
            const DocComponent* c = e.find(refl::type_of<ChildOf>());
            const EntityId parent =
                c == nullptr ? kNoEntity : doc_entity_id(static_cast<const ChildOf*>(c->data())->parent);
            model[{p.x, p.y, p.z, parent}].push_back(e.id);
        }
        std::vector<std::vector<EntityId>> want;
        for (auto& kv : model)
            if (kv.second.size() >= 2) want.push_back(kv.second);
        std::sort(want.begin(), want.end());

        std::vector<std::vector<EntityId>> got;
        for (const StackedGroup& g : find_stacked(s.doc()))
            got.push_back(std::vector<EntityId>(g.ids.begin(), g.ids.end()));
        std::sort(got.begin(), got.end());
        CHECK(got == want);
    }
}

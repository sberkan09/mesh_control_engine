// M-GROUP: group the selection under a new parent, and ungroup.

#include <doctest/doctest.h>

#include <cmath>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "mce/ecs/transform.h"
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

// M-GROUPFUZZ: random trees, random selections (a repeated id, an id that was never there, a parent with its child),
// checked against a plain parent map instead of the scene's own queries.
namespace {

struct Model {
    std::map<EntityId, EntityId> parent;  // kNoEntity: a root
    std::vector<EntityId> order;
};

bool near(f32 a, f32 b) { return std::fabs(a - b) < 1e-3f; }

}  // namespace

TEST_CASE("group and ungroup fuzz: parents match a plain model and nothing moves in the scene") {
    u32 grouped = 0, ungrouped = 0;
    for (u32 seed = 1; seed <= 200; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        EditorScene s;
        std::vector<EntityId> ids;
        for (u32 i = 0, n = 2 + static_cast<u32>(rng() % 9); i < n; ++i) {
            const EntityId id = s.create_entity("E" + std::to_string(i));
            s.add_component(id, refl::type_of<LocalTransform>());
            LocalTransform t;
            t.position = {static_cast<f32>(static_cast<int>(rng() % 21) - 10),
                          static_cast<f32>(static_cast<int>(rng() % 21) - 10), 0.0f};
            (void)s.set(id, t);
            ids.push_back(id);
        }
        for (u32 i = 0, k = static_cast<u32>(rng() % 10); i < k; ++i)
            (void)s.set_parent(ids[rng() % ids.size()], rng() % 4 == 0 ? kNoEntity : ids[rng() % ids.size()]);

        for (u32 round = 0; round < 4; ++round) {
            Model m;
            std::vector<EntityId> all;
            for (usize i = 0; i < s.doc().size(); ++i) all.push_back(s.doc().at(i).id);
            for (EntityId id : all) m.parent[id] = s.parent_of(id);
            std::map<EntityId, WorldTransform> world;
            for (EntityId id : all) world[id] = s.world_transform(id);

            Array<EntityId> pick;
            for (u32 i = 0, n = static_cast<u32>(rng() % 5); i < n; ++i)
                pick.push_back(rng() % 8 == 0 ? static_cast<EntityId>(900) : all[rng() % all.size()]);
            const Span<const EntityId> sel(pick.data(), pick.size());
            const usize undo_before = s.undo_count();

            if (rng() % 2 == 0) {
                // The model: the top-level entities of the selection (no ancestor of theirs in it), first-seen order.
                const auto ancestor_in_model = [&](EntityId a, EntityId id) {
                    for (EntityId p = m.parent[id], guard = 0; p != kNoEntity && guard < 64; p = m.parent[p], ++guard)
                        if (p == a) return true;
                    return false;
                };
                std::vector<EntityId> top;
                for (EntityId id : pick) {
                    if (m.parent.count(id) == 0) continue;
                    bool has = false;
                    for (EntityId o : pick) has = has || (o != id && m.parent.count(o) && ancestor_in_model(o, id));
                    bool seen = false;
                    for (EntityId t : top) seen = seen || t == id;
                    if (!has && !seen) top.push_back(id);
                }
                const EntityId g = group_entities(s, sel);
                if (top.empty()) {
                    CHECK(g == kNoEntity);
                    CHECK(s.undo_count() == undo_before);
                    continue;
                }
                ++grouped;
                REQUIRE(g != kNoEntity);
                CHECK(s.undo_count() == undo_before + 1);
                CHECK(s.parent_of(g) == m.parent[top[0]]);
                for (EntityId t : top) CHECK(s.parent_of(t) == g);
                for (EntityId id : all) {
                    bool is_top = false;
                    for (EntityId t : top) is_top = is_top || t == id;
                    if (!is_top) CHECK(s.parent_of(id) == m.parent[id]);
                    const WorldTransform now = s.world_transform(id);
                    CHECK(near(now.position.x, world[id].position.x));
                    CHECK(near(now.position.y, world[id].position.y));
                }
            } else {
                // The model of the spec: in selection order, a live entity with children hands them to its parent
                // and goes; one without children, a repeat, or an unknown id does nothing.
                std::map<EntityId, EntityId> mp = m.parent;
                std::vector<EntityId> gone;
                for (EntityId id : pick) {
                    if (mp.count(id) == 0) continue;
                    bool has_kids = false;
                    for (auto& kv : mp) has_kids = has_kids || kv.second == id;
                    if (!has_kids) continue;
                    const EntityId up = mp[id];
                    for (auto& kv : mp)
                        if (kv.second == id) kv.second = up;
                    mp.erase(id);
                    gone.push_back(id);
                }
                const Array<EntityId> freed = ungroup_entities(s, sel);
                if (gone.empty()) {
                    CHECK(freed.empty());
                    CHECK(s.undo_count() == undo_before);
                    continue;
                }
                ++ungrouped;
                CHECK(s.undo_count() == undo_before + 1);
                for (EntityId id : gone) CHECK(s.doc().find(id) == nullptr);
                CHECK(s.doc().size() == mp.size());
                for (auto& kv : mp) {
                    CHECK(s.parent_of(kv.first) == kv.second);
                    const WorldTransform now = s.world_transform(kv.first);
                    CHECK(near(now.position.x, world[kv.first].position.x));
                    CHECK(near(now.position.y, world[kv.first].position.y));
                }
            }
            CHECK(s.verify_world());
        }
    }
    CHECK(grouped > 100);  // the generator reaches both commands
    CHECK(ungrouped > 100);
}

// M-DUPOFFSET: duplicate the selection and shift the copies, as one undo entry.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/editor/duplicate_offset.h"

using namespace mce;
using namespace mce::editor;

namespace {

bool close(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }

EntityId place(EditorScene& s, const char* name, f32 x, f32 y) {
    const EntityId id = s.create_entity(name);
    LocalTransform t;
    t.position = {x, y, 0.5f};
    s.set(id, t);
    return id;
}

f32 world_x(const EditorScene& s, EntityId id) { return s.world_transform(id).position.x; }
f32 world_y(const EditorScene& s, EntityId id) { return s.world_transform(id).position.y; }

}  // namespace

TEST_CASE("duplicate_offset copies the entities, shifts the copies and leaves the originals") {
    EditorScene s;
    const EntityId a = place(s, "A", 0.0f, 0.0f);
    const EntityId b = place(s, "B", 2.0f, 1.0f);
    const usize before = s.doc().size();
    const EntityId ids[] = {a, b};
    Array<EntityId> made;

    CHECK(duplicate_offset(s, Span<const EntityId>(ids, 2), 5.0f, -1.0f, &made) == 2);
    REQUIRE(made.size() == 2);
    CHECK(s.doc().size() == before + 2);
    CHECK(close(world_x(s, a), 0.0f));
    CHECK(close(world_x(s, b), 2.0f));
    CHECK(close(world_x(s, made[0]), 5.0f));
    CHECK(close(world_y(s, made[0]), -1.0f));
    CHECK(close(world_x(s, made[1]), 7.0f));
    CHECK(close(world_y(s, made[1]), 0.0f));

    CHECK(s.undo());  // one entry: the copies and the shift go together
    CHECK(s.doc().size() == before);
    CHECK(close(world_x(s, a), 0.0f));
}

TEST_CASE("duplicate_offset moves a copied child once, with its copied parent") {
    EditorScene s;
    const EntityId parent = place(s, "Parent", 0.0f, 0.0f);
    const EntityId child = place(s, "Child", 1.0f, 0.0f);
    REQUIRE(s.set_parent(child, parent));
    const EntityId ids[] = {parent, child};  // Edit > Duplicate copies exactly the listed entities
    Array<EntityId> made;

    CHECK(duplicate_offset(s, Span<const EntityId>(ids, 2), 10.0f, 0.0f, &made) == 2);
    f32 parent_x = -1.0f, child_x = -1.0f;
    for (EntityId id : made) {
        if (s.parent_of(id) == kNoEntity) parent_x = world_x(s, id);
        else child_x = world_x(s, id);
    }
    CHECK(close(parent_x, 10.0f));
    CHECK(close(child_x, 11.0f));  // 10 for the parent plus its own 1, not 20 + 1
}

TEST_CASE("duplicate_offset with nothing to duplicate changes nothing") {
    EditorScene s;
    place(s, "A", 0.0f, 0.0f);
    const usize before = s.doc().size();
    const EntityId none[] = {kNoEntity};
    CHECK(duplicate_offset(s, Span<const EntityId>(none, 1), 1.0f, 1.0f) == 0);
    CHECK(duplicate_offset(s, Span<const EntityId>(), 1.0f, 1.0f) == 0);
    CHECK(s.doc().size() == before);
}

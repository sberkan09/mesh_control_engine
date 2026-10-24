// M-RESETXFORM: Edit > Reset transform puts the local transform back to the identity.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/editor/reset_transform.h"

using namespace mce;
using namespace mce::editor;

namespace {

EntityId place(EditorScene& s, const char* name, f32 x, f32 y) {
    const EntityId id = s.create_entity(name);
    LocalTransform t;
    t.position = {x, y, 2.0f};
    t.rotation = rotation_z(0.7f);
    t.scale = {3.0f, 4.0f, 1.0f};
    s.set(id, t);
    return id;
}

bool is_identity(const LocalTransform& t) {
    return t.position.x == 0.0f && t.position.y == 0.0f && t.position.z == 0.0f && t.rotation.x == 0.0f &&
           t.rotation.y == 0.0f && t.rotation.z == 0.0f && t.rotation.w == 1.0f && t.scale.x == 1.0f &&
           t.scale.y == 1.0f && t.scale.z == 1.0f;
}

}  // namespace

TEST_CASE("reset_transforms puts position, rotation and scale back, in one undo entry") {
    EditorScene s;
    const EntityId a = place(s, "A", 5.0f, 6.0f);
    const EntityId b = place(s, "B", -1.0f, 2.0f);
    const EntityId c = place(s, "C", 9.0f, 9.0f);
    const EntityId ids[] = {a, b, a};  // a twice: counted once

    CHECK(reset_transforms(s, Span<const EntityId>(ids, 3)) == 2);
    CHECK(is_identity(*s.get<LocalTransform>(a)));
    CHECK(is_identity(*s.get<LocalTransform>(b)));
    CHECK(s.get<LocalTransform>(c)->position.x == 9.0f);  // not in the list

    s.undo();
    CHECK(s.get<LocalTransform>(a)->position.x == 5.0f);
    CHECK(s.get<LocalTransform>(b)->position.x == -1.0f);
}

TEST_CASE("reset_transforms skips identities and entities without a transform, with no undo entry") {
    EditorScene s;
    const EntityId a = place(s, "A", 1.0f, 1.0f);
    const EntityId ids[] = {a};
    CHECK(reset_transforms(s, Span<const EntityId>(ids, 1)) == 1);
    CHECK(reset_transforms(s, Span<const EntityId>(ids, 1)) == 0);  // already the identity

    const EntityId bare = s.create_entity("Bare");
    s.set(bare, LocalTransform{});
    s.undo();  // the set above; leaves the entity without a LocalTransform
    const EntityId none[] = {bare, kNoEntity};
    CHECK(reset_transforms(s, Span<const EntityId>(none, 2)) == 0);
}

// M-RADIAL 1: radial_duplicate copies the selection around a centre point.

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "mce/ecs/transform.h"
#include "mce/editor/radial_duplicate.h"

using namespace mce;
using namespace mce::editor;

namespace {

constexpr f32 kPi = 3.14159265358979f;

EntityId place(EditorScene& s, const char* name, f32 x, f32 y) {
    const EntityId id = s.create_entity(name);
    LocalTransform t;
    t.position = {x, y, 0.5f};
    s.set(id, t);
    return id;
}

}  // namespace

TEST_CASE("radial_duplicate closes a ring of equal steps about the centre") {
    EditorScene s;
    const EntityId a = place(s, "Pillar", 5.0f, 0.0f);
    RadialParams p;
    p.count = 4;
    Array<EntityId> made;
    const EntityId ids[] = {a};
    CHECK(radial_duplicate(s, Span<const EntityId>(ids, 1), p, &made) == 3);
    REQUIRE(made.size() == 3);
    const f32 want[3][2] = {{0.0f, 5.0f}, {-5.0f, 0.0f}, {0.0f, -5.0f}};
    for (usize i = 0; i < 3; ++i) {
        const WorldTransform w = s.world_transform(made[i]);
        CHECK(std::fabs(w.position.x - want[i][0]) < 1e-3f);
        CHECK(std::fabs(w.position.y - want[i][1]) < 1e-3f);
        CHECK(std::fabs(w.position.z - 0.5f) < 1e-5f);
    }
    CHECK(std::fabs(angle_z(s.world_transform(made[0]).rotation) - kPi / 2.0f) < 1e-3f);
    CHECK(s.world_transform(a).position.x == doctest::Approx(5.0f));  // the original stays
}

TEST_CASE("radial_duplicate spreads an arc with both ends filled and can keep the heading") {
    EditorScene s;
    const EntityId a = place(s, "Torch", 2.0f, 1.0f);
    RadialParams p;
    p.count = 3;
    p.center_x = 1.0f;
    p.center_y = 1.0f;
    p.total_degrees = 180.0f;
    p.turn = false;
    Array<EntityId> made;
    const EntityId ids[] = {a};
    REQUIRE(radial_duplicate(s, Span<const EntityId>(ids, 1), p, &made) == 2);
    const WorldTransform mid = s.world_transform(made[0]), end = s.world_transform(made[1]);
    CHECK(std::fabs(mid.position.x - 1.0f) < 1e-3f);
    CHECK(std::fabs(mid.position.y - 2.0f) < 1e-3f);
    CHECK(std::fabs(end.position.x - 0.0f) < 1e-3f);
    CHECK(std::fabs(end.position.y - 1.0f) < 1e-3f);
    CHECK(std::fabs(angle_z(end.rotation)) < 1e-5f);
}

TEST_CASE("radial_duplicate refuses bad input without an undo entry and is one undo step otherwise") {
    EditorScene s;
    const EntityId a = place(s, "A", 1.0f, 0.0f);
    const EntityId ids[] = {a};
    const Span<const EntityId> sp(ids, 1);
    const usize before = s.doc().entities().size();
    RadialParams p;
    p.count = 1;
    CHECK(radial_duplicate(s, sp, p) == 0);
    p.count = 5000;
    CHECK(radial_duplicate(s, sp, p) == 0);
    p.count = 4;
    p.total_degrees = 0.0f;
    CHECK(radial_duplicate(s, sp, p) == 0);
    p.total_degrees = 360.0f;
    p.center_x = NAN;
    CHECK(radial_duplicate(s, sp, p) == 0);
    p.center_x = 0.0f;
    CHECK(radial_duplicate(s, Span<const EntityId>(), p) == 0);
    CHECK(s.doc().entities().size() == before);
    CHECK(radial_duplicate(s, sp, p) == 3);
    CHECK(s.doc().entities().size() == before + 3);
    CHECK(s.undo());
    CHECK(s.doc().entities().size() == before);
}

TEST_CASE("radial_duplicate keeps a copied child under its copied parent") {
    EditorScene s;
    const EntityId parent = place(s, "Parent", 4.0f, 0.0f);
    const EntityId child = place(s, "Child", 5.0f, 0.0f);
    s.set_parent(child, parent);
    RadialParams p;
    p.count = 2;
    Array<EntityId> made;
    const EntityId ids[] = {parent, child};
    REQUIRE(radial_duplicate(s, Span<const EntityId>(ids, 2), p, &made) == 2);
    const EntityId cp = s.parent_of(made[1]) == made[0] ? made[0] : made[1];
    const EntityId cc = cp == made[0] ? made[1] : made[0];
    CHECK(s.parent_of(cc) == cp);
    CHECK(std::fabs(s.world_transform(cp).position.x + 4.0f) < 1e-3f);
    CHECK(std::fabs(s.world_transform(cc).position.x + 5.0f) < 1e-3f);
}

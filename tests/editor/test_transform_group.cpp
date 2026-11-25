// M-GROUPXFORM 1: transform_group turns and resizes the selection about a pivot.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/editor/transform_group.h"

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

TEST_CASE("transform_group turns a pair about the middle of the group") {
    EditorScene s;
    const EntityId a = place(s, "A", 0.0f, 0.0f), b = place(s, "B", 4.0f, 0.0f);
    const EntityId ids[] = {a, b};
    GroupTransformParams p;
    p.degrees = 90.0f;
    CHECK(transform_group(s, Span<const EntityId>(ids, 2), p) == 2);
    const WorldTransform wa = s.world_transform(a), wb = s.world_transform(b);
    CHECK(std::fabs(wa.position.x - 2.0f) < 1e-3f);
    CHECK(std::fabs(wa.position.y + 2.0f) < 1e-3f);
    CHECK(std::fabs(wb.position.x - 2.0f) < 1e-3f);
    CHECK(std::fabs(wb.position.y - 2.0f) < 1e-3f);
    CHECK(std::fabs(angle_z(wb.rotation) - kPi / 2.0f) < 1e-3f);
    CHECK(std::fabs(wb.position.z - 0.5f) < 1e-5f);
}

TEST_CASE("transform_group scales distances and size about a chosen pivot") {
    EditorScene s;
    const EntityId a = place(s, "A", 3.0f, 1.0f);
    const EntityId ids[] = {a};
    GroupTransformParams p;
    p.scale = 2.0f;
    p.use_pivot = true;
    p.pivot_x = 1.0f;
    p.pivot_y = 1.0f;
    CHECK(transform_group(s, Span<const EntityId>(ids, 1), p) == 1);
    const WorldTransform w = s.world_transform(a);
    CHECK(std::fabs(w.position.x - 5.0f) < 1e-3f);
    CHECK(std::fabs(w.position.y - 1.0f) < 1e-3f);
    CHECK(std::fabs(w.scale.x - 2.0f) < 1e-4f);
    CHECK(std::fabs(w.scale.z - 2.0f) < 1e-4f);
}

TEST_CASE("transform_group refuses bad input and is one undo step") {
    EditorScene s;
    const EntityId a = place(s, "A", 1.0f, 0.0f), b = place(s, "B", 2.0f, 0.0f);
    const EntityId ids[] = {a, b};
    const Span<const EntityId> sp(ids, 2);
    GroupTransformParams p;
    CHECK(transform_group(s, sp, p) == 0);  // no turn, no resize
    p.degrees = 45.0f;
    p.scale = 0.0f;
    CHECK(transform_group(s, sp, p) == 0);
    p.scale = 1.0f;
    p.degrees = NAN;
    CHECK(transform_group(s, sp, p) == 0);
    CHECK(transform_group(s, Span<const EntityId>(), GroupTransformParams{30.0f}) == 0);
    CHECK(s.world_transform(a).position.x == doctest::Approx(1.0f));
    CHECK(transform_group(s, sp, GroupTransformParams{180.0f}) == 2);
    CHECK(s.world_transform(a).position.x == doctest::Approx(2.0f).epsilon(1e-3));
    CHECK(s.undo());
    CHECK(s.world_transform(a).position.x == doctest::Approx(1.0f));
    CHECK(s.world_transform(b).position.x == doctest::Approx(2.0f));
}

TEST_CASE("transform_group carries a listed child consistently with its listed parent") {
    EditorScene s;
    const EntityId parent = place(s, "P", 0.0f, 0.0f);
    const EntityId child = place(s, "C", 2.0f, 0.0f);
    s.set_parent(child, parent);
    const EntityId ids[] = {child, parent};
    GroupTransformParams p;
    p.degrees = 90.0f;
    p.use_pivot = true;
    CHECK(transform_group(s, Span<const EntityId>(ids, 2), p) == 2);
    CHECK(std::fabs(s.world_transform(child).position.x) < 1e-3f);
    CHECK(std::fabs(s.world_transform(child).position.y - 2.0f) < 1e-3f);
    CHECK(std::fabs(s.world_transform(parent).position.x) < 1e-3f);
}

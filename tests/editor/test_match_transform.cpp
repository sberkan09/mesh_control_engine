// M-MATCHXFORM: copy the anchor's position, rotation or scale onto the other selected entities.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/editor/match_transform.h"

using namespace mce;
using namespace mce::editor;

namespace {

bool close(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }

EntityId place(EditorScene& s, const char* name, f32 x, f32 y, f32 scale) {
    const EntityId id = s.create_entity(name);
    LocalTransform t;
    t.position = {x, y, 0.5f};
    t.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
    t.scale = {scale, scale, 1.0f};
    s.set(id, t);
    return id;
}

}  // namespace

TEST_CASE("match_transforms copies position, scale or rotation from the anchor, one undo entry each") {
    EditorScene s;
    const EntityId anchor = place(s, "Anchor", 4.0f, 5.0f, 2.0f);
    const EntityId b = place(s, "B", 0.0f, 0.0f, 1.0f);
    const EntityId c = place(s, "C", 1.0f, 1.0f, 1.0f);
    LocalTransform ra = *s.get<LocalTransform>(anchor);
    ra.rotation = rotation_z(0.9f);
    s.set(anchor, ra);
    const EntityId ids[] = {anchor, b, c, b};
    const Span<const EntityId> sel(ids, 4);

    CHECK(match_transforms(s, sel, anchor, MatchPart::Position) == 2);  // b once, c
    CHECK(close(s.world_transform(b).position.x, 4.0f));
    CHECK(close(s.world_transform(c).position.y, 5.0f));
    CHECK(s.get<LocalTransform>(b)->scale.x == 1.0f);  // only the position
    CHECK(match_transforms(s, sel, anchor, MatchPart::Position) == 0);  // already matched

    CHECK(match_transforms(s, sel, anchor, MatchPart::Scale) == 2);
    CHECK(s.get<LocalTransform>(c)->scale.x == 2.0f);
    CHECK(close(s.world_transform(c).position.x, 4.0f));

    CHECK(match_transforms(s, sel, anchor, MatchPart::Rotation) == 2);
    CHECK(close(s.get<LocalTransform>(b)->rotation.z, std::sin(0.45f)));

    CHECK(s.undo());  // the rotation only
    CHECK(close(s.get<LocalTransform>(b)->rotation.w, 1.0f));
    CHECK(s.get<LocalTransform>(b)->scale.x == 2.0f);
}

TEST_CASE("match_transforms respects a parent when matching position and ignores a missing anchor") {
    EditorScene s;
    const EntityId parent = place(s, "Parent", 100.0f, 0.0f, 1.0f);
    const EntityId a = place(s, "A", 3.0f, 0.0f, 1.0f);
    const EntityId b = place(s, "B", 0.0f, 0.0f, 1.0f);
    REQUIRE(s.set_parent(b, parent));  // B keeps its world x of 0 (local -100)
    const EntityId ids[] = {a, b};
    CHECK(match_transforms(s, Span<const EntityId>(ids, 2), a, MatchPart::Position) == 1);
    CHECK(close(s.world_transform(b).position.x, 3.0f));

    CHECK(match_transforms(s, Span<const EntityId>(ids, 2), kNoEntity, MatchPart::Position) == 0);
}

// Rotate / scale gizmos and multi-selection (ROADMAP M6, ADR-0030 §4), headless: the ring and
// scale handles' hit areas, a group turned and scaled about the anchor, snapping, the clamp that
// keeps a scale from flipping, cancel, one undo step per drag, Shift range select and the marquee.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/editor/editor_scene.h"
#include "mce/editor/gizmo.h"
#include "mce/editor/viewport2d.h"

using namespace mce;
using namespace mce::editor;

namespace {

bool close(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }

EntityId place(EditorScene& s, const char* name, math::vec2 p) {
    const EntityId id = s.create_entity(name);
    LocalTransform t;
    t.position = {p.x, p.y, 0.25f};
    s.set(id, t);
    return id;
}

}  // namespace

TEST_CASE("gizmo: rotate ring hit area; the group turns about the anchor; snap and cancel") {
    EditorScene s;
    const EntityId a = place(s, "A", {1, 1});
    const EntityId b = place(s, "B", {3, 1});
    s.selection().set(a);
    s.selection().toggle(b);
    s.clear_undo();

    EditorCamera2D cam;
    const rhi::Extent2D size{800, 600};
    math::vec2 pivot;
    REQUIRE(TransformGizmo::pivot(s, pivot));
    const math::vec2 o = cam.world_to_screen(pivot, size);
    CHECK(TransformGizmo::hit(cam, size, pivot, o + math::vec2{TransformGizmo::kRingPx, 0}, GizmoMode::Rotate) == GizmoPart::Ring);
    CHECK(TransformGizmo::hit(cam, size, pivot, o, GizmoMode::Rotate) == GizmoPart::None);  // no move square
    CHECK(TransformGizmo::hit(cam, size, pivot, o, GizmoMode::Scale) == GizmoPart::XY);

    TransformGizmo g;
    g.set_mode(GizmoMode::Rotate);
    CHECK_FALSE(g.begin(s, GizmoPart::XY, {2, 1}));  // the rotate tool has only the ring
    Array<OverlayLine> ring;
    g.lines(cam, pivot, GizmoPart::Ring, ring);
    CHECK(ring.size() == 48);

    s.begin_edit("Rotate");
    REQUIRE(g.begin(s, GizmoPart::Ring, {2, 1}));  // grab to the right of the pivot
    g.set_mode(GizmoMode::Scale);                   // ignored mid-drag
    CHECK(g.mode() == GizmoMode::Rotate);
    g.update(s, {1, 2}, 0.0f);  // ...and sweep a quarter turn counter-clockwise
    CHECK(close(g.angle(), 1.5707963f));
    CHECK(close(angle_z(s.get<LocalTransform>(a)->rotation), 1.5707963f));
    CHECK(close(s.get<LocalTransform>(a)->position.x, 1.0f));  // the anchor is the pivot
    CHECK(close(s.get<LocalTransform>(b)->position.x, 1.0f));  // B swings from (3,1) to (1,3)
    CHECK(close(s.get<LocalTransform>(b)->position.y, 3.0f));
    CHECK(s.get<LocalTransform>(b)->position.z == 0.25f);
    CHECK(close(angle_z(s.get<LocalTransform>(b)->rotation), 1.5707963f));

    g.update(s, {2, 1.45f}, 1.0f);  // ~24 degrees snaps to 30
    CHECK(close(g.angle(), 2.0f * TransformGizmo::kRotateSnap));
    g.end();
    s.end_edit();
    CHECK(s.undo_count() == 1);
    CHECK(s.undo_label() == "Rotate");

    // Cancel restores rotation and position.
    const LocalTransform before = *s.get<LocalTransform>(b);
    s.begin_edit("Rotate");
    REQUIRE(g.begin(s, GizmoPart::Ring, {2, 1}));
    g.update(s, {0, 1}, 0.0f);
    g.cancel(s);
    s.end_edit();
    CHECK(s.undo_count() == 1);  // a cancelled drag leaves no entry
    CHECK(s.get<LocalTransform>(b)->position.x == before.position.x);
    CHECK(s.get<LocalTransform>(b)->rotation.z == before.rotation.z);
    s.undo();
    CHECK(close(angle_z(s.get<LocalTransform>(b)->rotation), 0.0f));
    CHECK(s.get<LocalTransform>(b)->position.x == 3.0f);
}

TEST_CASE("gizmo: scale along an axis and uniformly; offsets scale with it; snap; no flip") {
    EditorScene s;
    const EntityId a = place(s, "A", {0, 0});
    const EntityId b = place(s, "B", {2, 1});
    s.selection().set(a);
    s.selection().toggle(b);
    TransformGizmo g;
    g.set_mode(GizmoMode::Scale);

    REQUIRE(g.begin(s, GizmoPart::X, {1, 0}));
    g.update(s, {3, 5}, 0.0f);  // x3 along X; the Y of the mouse is ignored
    CHECK(close(s.get<LocalTransform>(a)->scale.x, 3.0f));
    CHECK(close(s.get<LocalTransform>(a)->scale.y, 1.0f));
    CHECK(close(s.get<LocalTransform>(b)->position.x, 6.0f));  // offset from the pivot x3
    CHECK(close(s.get<LocalTransform>(b)->position.y, 1.0f));
    g.update(s, {1.23f, 0}, 1.0f);  // 1.23 snaps to 1.2
    CHECK(close(s.get<LocalTransform>(a)->scale.x, 1.2f));
    g.update(s, {-1, 0}, 0.0f);  // through the pivot: clamped, never flipped or zero
    CHECK(s.get<LocalTransform>(a)->scale.x == TransformGizmo::kMinScale);
    g.cancel(s);
    CHECK(s.get<LocalTransform>(a)->scale.x == 1.0f);
    CHECK(s.get<LocalTransform>(b)->position.x == 2.0f);

    REQUIRE(g.begin(s, GizmoPart::XY, {1, 1}));
    g.update(s, {0.5f, 0.5f}, 0.0f);  // half the distance: x0.5 uniform
    CHECK(close(g.factor().x, 0.5f));
    CHECK(close(s.get<LocalTransform>(b)->scale.y, 0.5f));
    CHECK(close(s.get<LocalTransform>(b)->position.x, 1.0f));
    CHECK(close(s.get<LocalTransform>(b)->position.y, 0.5f));
    g.end();
}

TEST_CASE("selection: Shift range select in document order and the viewport marquee") {
    EditorScene s;
    EntityId ids[5];
    for (int i = 0; i < 5; ++i) ids[i] = place(s, "E", {static_cast<f32>(i) * 3.0f, 0});

    s.select_range(ids[2]);  // nothing selected: just the target
    CHECK(s.selection().items().size() == 1);
    s.selection().set(ids[3]);
    s.select_range(ids[1]);  // backwards from the anchor
    CHECK(s.selection().anchor() == ids[3]);
    CHECK(s.selection().items().size() == 3);
    CHECK(s.selection().contains(ids[1]));
    CHECK(s.selection().contains(ids[2]));
    CHECK_FALSE(s.selection().contains(ids[4]));
    s.select_range(ids[4]);  // a new range replaces the old one...
    CHECK(s.selection().items().size() == 2);
    CHECK_FALSE(s.selection().contains(ids[1]));
    s.selection().toggle(ids[0]);
    s.select_range(ids[3], true);  // ...unless additive (Ctrl+Shift)
    CHECK(s.selection().contains(ids[0]));
    CHECK(s.selection().contains(ids[4]));

    Array<EntityId> hit;
    entities_in_rect(s, math::aabb2{{2.0f, -1.0f}, {7.0f, 1.0f}}, hit);  // quads are 1 m wide
    REQUIRE(hit.size() == 2);
    CHECK(hit[0] == ids[1]);
    CHECK(hit[1] == ids[2]);
    hit.clear();
    entities_in_rect(s, math::aabb2{{0.0f, 5.0f}, {20.0f, 6.0f}}, hit);
    CHECK(hit.empty());
}

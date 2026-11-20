// Rotate / scale gizmos and multi-selection (ROADMAP M6, ADR-0030 §4), headless: the ring and
// scale handles' hit areas, a group turned and scaled about the anchor, snapping, the clamp that
// keeps a scale from flipping, cancel, one undo step per drag, Shift range select and the marquee.

#include <doctest/doctest.h>

#include <cmath>
#include <random>
#include <vector>

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

namespace {

// A random tree of entities (some turned and scaled) and a random selection of them.
struct Forest {
    EditorScene s;
    std::vector<EntityId> all, pick;
    void build(std::mt19937& rng, bool turned) {
        for (u32 i = 0, n = 2 + static_cast<u32>(rng() % 6); i < n; ++i) {
            const EntityId id = s.create_entity("E");
            LocalTransform t;
            t.position = {static_cast<f32>(rng() % 200) / 10.0f - 10.0f, static_cast<f32>(rng() % 200) / 10.0f - 10.0f,
                          0.25f};
            if (turned) {
                t.rotation = rotation_z(static_cast<f32>(rng() % 628) / 100.0f);
                const f32 sc = 0.5f + static_cast<f32>(rng() % 30) / 10.0f;
                t.scale = {sc, sc, 1.0f};
            }
            s.set(id, t);
            all.push_back(id);
        }
        for (u32 i = 0, k = static_cast<u32>(rng() % 5); i < k; ++i)
            (void)s.set_parent(all[rng() % all.size()], all[rng() % all.size()]);
        for (EntityId id : all)
            if (rng() % 3 != 0) pick.push_back(id);
        if (pick.empty()) pick.push_back(all[0]);
        s.selection().set(pick[0]);
        for (usize i = 1; i < pick.size(); ++i) s.selection().toggle(pick[i]);
        s.clear_undo();
    }
};

}  // namespace

TEST_CASE("gizmo fuzz: a move drag shifts every selected entity by the drag, and cancel restores them") {
    for (u32 seed = 1; seed <= 200; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        Forest f;
        f.build(rng, true);
        std::vector<math::vec3> before;
        std::vector<LocalTransform> locals;
        for (EntityId id : f.pick) {
            before.push_back(f.s.world_transform(id).position);
            locals.push_back(*f.s.get<LocalTransform>(id));
        }
        math::vec2 pivot;
        REQUIRE(TransformGizmo::pivot(f.s, pivot));
        const math::vec2 d{static_cast<f32>(rng() % 200) / 10.0f - 10.0f,
                           static_cast<f32>(rng() % 200) / 10.0f - 10.0f};
        TransformGizmo g;
        f.s.begin_edit("Move");
        REQUIRE(g.begin(f.s, GizmoPart::XY, pivot));
        g.update(f.s, pivot + d, 0.0f);
        for (usize i = 0; i < f.pick.size(); ++i) {
            const math::vec3 now = f.s.world_transform(f.pick[i]).position;
            CHECK(close(now.x, before[i].x + d.x, 1e-3f));
            CHECK(close(now.y, before[i].y + d.y, 1e-3f));
            CHECK(close(now.z, before[i].z, 1e-3f));
        }
        g.cancel(f.s);
        g.end();
        f.s.end_edit();
        for (usize i = 0; i < f.pick.size(); ++i) {
            const LocalTransform& l = *f.s.get<LocalTransform>(f.pick[i]);
            CHECK(close(l.position.x, locals[i].position.x, 1e-3f));
            CHECK(close(l.position.y, locals[i].position.y, 1e-3f));
        }
    }
}

TEST_CASE("gizmo fuzz: a rotate drag turns every selected entity about the pivot") {
    for (u32 seed = 1; seed <= 200; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        Forest f;
        f.build(rng, true);
        std::vector<math::vec3> before;
        for (EntityId id : f.pick) before.push_back(f.s.world_transform(id).position);
        math::vec2 pivot;
        REQUIRE(TransformGizmo::pivot(f.s, pivot));
        const f32 a = static_cast<f32>(rng() % 628) / 100.0f - 3.14f;
        const math::vec2 grab = pivot + math::vec2{5.0f, 0.0f};
        TransformGizmo g;
        g.set_mode(GizmoMode::Rotate);
        f.s.begin_edit("Rotate");
        REQUIRE(g.begin(f.s, GizmoPart::Ring, grab));
        g.update(f.s, pivot + math::vec2{5.0f * std::cos(a), 5.0f * std::sin(a)}, 0.0f);
        g.end();
        f.s.end_edit();
        const f32 c = std::cos(g.angle()), sn = std::sin(g.angle());
        for (usize i = 0; i < f.pick.size(); ++i) {
            const math::vec3 now = f.s.world_transform(f.pick[i]).position;
            const f32 rx = before[i].x - pivot.x, ry = before[i].y - pivot.y;
            CHECK(close(now.x, pivot.x + rx * c - ry * sn, 2e-3f));
            CHECK(close(now.y, pivot.y + rx * sn + ry * c, 2e-3f));
        }
    }
}

TEST_CASE("gizmo fuzz: a uniform scale drag scales every selected entity's offset from the pivot") {
    for (u32 seed = 1; seed <= 200; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        Forest f;
        f.build(rng, false);
        std::vector<math::vec3> before;
        for (EntityId id : f.pick) before.push_back(f.s.world_transform(id).position);
        math::vec2 pivot;
        REQUIRE(TransformGizmo::pivot(f.s, pivot));
        const f32 ratio = 0.5f + static_cast<f32>(rng() % 30) / 10.0f;
        TransformGizmo g;
        g.set_mode(GizmoMode::Scale);
        f.s.begin_edit("Scale");
        REQUIRE(g.begin(f.s, GizmoPart::XY, pivot + math::vec2{4.0f, 0.0f}));
        g.update(f.s, pivot + math::vec2{4.0f * ratio, 0.0f}, 0.0f);
        g.end();
        f.s.end_edit();
        const f32 k = g.factor().x;
        for (usize i = 0; i < f.pick.size(); ++i) {
            const math::vec3 now = f.s.world_transform(f.pick[i]).position;
            CHECK(close(now.x, pivot.x + (before[i].x - pivot.x) * k, 2e-3f));
            CHECK(close(now.y, pivot.y + (before[i].y - pivot.y) * k, 2e-3f));
        }
    }
}

// M-VIEWS 2, 4: kit::collect_views — which cameras are views, in which order, at which pixels —
// split-screen cells and the small-view quality cuts.

#include <doctest/doctest.h>

#include "mce/ecs/transform.h"
#include "mce/kit/components.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/views.h"

using namespace mce;

namespace {

kit::CameraView view_at(f32 x, f32 y, f32 w, f32 h, i32 order = 0) {
    kit::CameraView v;
    v.viewport = {x, y, w, h};
    v.order = order;
    return v;
}

rhi::Extent2D texture_size(World&, Entity, void*) { return {256, 128}; }

}  // namespace

TEST_CASE("views: no CameraView keeps the single-view path") {
    World w;
    w.spawn_with(LocalTransform{}, kit::Camera3DComponent{});
    Array<kit::ViewEntry> out;
    CHECK_FALSE(kit::collect_views(w, {1280, 720}, out));
    CHECK(out.empty());
    kit::CameraView off;
    off.enabled = 0;
    w.spawn_with(LocalTransform{}, kit::Camera3DComponent{}, off);
    CHECK_FALSE(kit::collect_views(w, {1280, 720}, out));
}

TEST_CASE("views: split halves share their edge; the primary without a view is full screen") {
    World w;
    kit::Camera3DComponent second;
    second.primary = 0;
    const Entity a = w.spawn_with(LocalTransform{}, kit::Camera3DComponent{}, view_at(0, 0, 0.5f, 1));
    const Entity b = w.spawn_with(LocalTransform{}, second, view_at(0.5f, 0, 0.5f, 1));
    const Entity c = w.spawn_with(LocalTransform{}, second);  // not primary, no view: not drawn
    Array<kit::ViewEntry> out;
    REQUIRE(kit::collect_views(w, {1281, 721}, out));
    REQUIRE(out.size() == 2);
    CHECK(out[0].camera == a);
    CHECK(out[1].camera == b);
    CHECK(out[0].rect.x == 0);
    CHECK(out[0].rect.x + i32(out[0].rect.width) == out[1].rect.x);  // no gap, no overlap
    CHECK(out[1].rect.x + i32(out[1].rect.width) == 1281);
    CHECK(out[0].rect.height == 721);
    (void)c;

    // A minimap camera over a primary that has no CameraView of its own.
    World m;
    const Entity main = m.spawn_with(LocalTransform{}, kit::Camera3DComponent{});
    const Entity mini = m.spawn_with(LocalTransform{}, second, view_at(0.75f, 0.0f, 0.25f, 0.25f, 1));
    REQUIRE(kit::collect_views(m, {800, 600}, out));
    REQUIRE(out.size() == 2);
    CHECK(out[0].camera == main);
    CHECK(out[0].rect.width == 800);
    CHECK(out[1].camera == mini);
    CHECK(out[1].rect.x == 600);
    CHECK(out[1].rect.width == 200);
    CHECK(out[1].rect.height == 150);
    CHECK(out[1].is_3d);
}

TEST_CASE("views: textures first, then order, a 3D camera before a 2D one, then entity") {
    World w;
    const Entity rt = w.spawn();
    kit::CameraView into = view_at(0, 0, 1, 1, 5);
    into.target = rt;
    kit::Camera3DComponent second;
    second.primary = 0;
    const Entity hud = w.spawn_with(LocalTransform{}, kit::Camera2DComponent{}, view_at(0, 0, 1, 1, 0));
    const Entity world3d = w.spawn_with(LocalTransform{}, second, view_at(0, 0, 1, 1, 0));
    const Entity top = w.spawn_with(LocalTransform{}, second, view_at(0, 0, 0.5f, 0.5f, -1));
    const Entity tex = w.spawn_with(LocalTransform{}, second, into);
    Array<kit::ViewEntry> out;
    REQUIRE(kit::collect_views(w, {640, 360}, out));
    REQUIRE(out.size() == 3);  // no target-size callback: the texture view is left out
    CHECK(out[0].camera == top);
    CHECK(out[1].camera == world3d);
    CHECK(out[2].camera == hud);
    CHECK_FALSE(out[2].is_3d);
    REQUIRE(kit::collect_views(w, {640, 360}, out, &texture_size));
    REQUIRE(out.size() == 4);
    CHECK(out[0].camera == tex);
    CHECK(out[0].target == rt);
    CHECK(out[0].rect.width == 256);
    CHECK(out[0].rect.height == 128);
}

TEST_CASE("views: view_rect clamps and rounds") {
    const rhi::Rect2D r = kit::view_rect({-0.5f, 0.25f, 1.0f, 2.0f}, {100, 40});
    CHECK(r.x == 0);
    CHECK(r.width == 50);
    CHECK(r.y == 10);
    CHECK(r.height == 30);
    CHECK(kit::view_rect({0.5f, 0.5f, 0.0f, 0.5f}, {100, 40}).width == 0);
}

// ---- M-VIEWS 4: split-screen ---------------------------------------------------------------------

namespace {

kit::CameraView player(u32 split, i32 order = 0) {
    kit::CameraView v;
    v.split = split;
    v.order = order;
    return v;
}

bool same(const math::vec4& a, const math::vec4& b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }

}  // namespace

TEST_CASE("views: split_cell lays out 1 to 4 players") {
    using kit::SplitLayout;
    CHECK(same(kit::split_cell(SplitLayout::Auto, 1, 0), {0, 0, 1, 1}));
    CHECK(same(kit::split_cell(SplitLayout::Auto, 2, 1), {0.5f, 0, 0.5f, 1}));       // landscape: side by side
    CHECK(same(kit::split_cell(SplitLayout::Auto, 2, 1, 0.5f), {0, 0.5f, 1, 0.5f}));  // portrait: stacked
    CHECK(same(kit::split_cell(SplitLayout::Auto, 3, 0), {0, 0, 1, 0.5f}));          // player 1 on top
    CHECK(same(kit::split_cell(SplitLayout::Auto, 3, 2), {0.5f, 0.5f, 0.5f, 0.5f}));
    CHECK(same(kit::split_cell(SplitLayout::Auto, 4, 3), {0.5f, 0.5f, 0.5f, 0.5f}));
    CHECK(same(kit::split_cell(SplitLayout::Rows, 2, 1), {0, 0.5f, 1, 0.5f}));
    CHECK(same(kit::split_cell(SplitLayout::Columns, 4, 2), {0.5f, 0, 0.25f, 1}));
    CHECK(same(kit::split_cell(SplitLayout::Grid, 3, 2), {0, 0.5f, 0.5f, 0.5f}));  // the fourth cell stays empty
    CHECK(same(kit::split_cell(SplitLayout::Grid, 2, 1), {0.5f, 0, 0.5f, 1}));
    CHECK(same(kit::split_cell(SplitLayout::Auto, 4, 7), {0, 0, 1, 1}));  // out of range: the window
}

TEST_CASE("views: split players get their cells, reflow when one leaves, and share them with a HUD camera") {
    World w;
    kit::Camera3DComponent cam3;
    cam3.primary = 0;
    const Entity p1 = w.spawn_with(LocalTransform{}, kit::Camera3DComponent{}, player(1));
    const Entity p3 = w.spawn_with(LocalTransform{}, cam3, player(3));
    Array<kit::ViewEntry> out;
    REQUIRE(kit::collect_views(w, {1280, 720}, out));
    REQUIRE(out.size() == 2);  // players 1 and 3: a two-way split
    CHECK(out[0].camera == p1);
    CHECK(out[0].rect.x == 0);
    CHECK(out[0].rect.width == 640);
    CHECK(out[1].camera == p3);
    CHECK(out[1].rect.x == 640);
    CHECK(out[1].split == 3);
    CHECK(out[1].cell.x == 640);

    // Player 2 joins: three-way, 1 on top, 2 and 3 below.
    const Entity p2 = w.spawn_with(LocalTransform{}, cam3, player(2));
    REQUIRE(kit::collect_views(w, {1280, 720}, out));
    REQUIRE(out.size() == 3);
    CHECK(out[0].camera == p1);
    CHECK(out[0].rect.width == 1280);
    CHECK(out[0].rect.height == 360);
    CHECK(out[1].camera == p2);
    CHECK(out[1].rect.y == 360);
    CHECK(out[1].rect.x == 0);
    CHECK(out[2].camera == p3);
    CHECK(out[2].rect.x == 640);

    // Player 2's minimap: a 2D camera in the top-right quarter of their cell, over their 3D view.
    kit::Camera2DComponent c2;
    c2.primary = false;
    kit::CameraView mini = player(2, 1);
    mini.viewport = {0.75f, 0.0f, 0.25f, 0.25f};
    const Entity map = w.spawn_with(LocalTransform{}, c2, mini);
    REQUIRE(kit::collect_views(w, {1280, 720}, out));
    REQUIRE(out.size() == 4);
    CHECK(out[3].camera == map);
    CHECK(out[3].rect.x == 480);
    CHECK(out[3].rect.y == 360);
    CHECK(out[3].rect.width == 160);
    CHECK(out[3].rect.height == 90);

    // Player 1 leaves: 2 and 3 take the halves, the layout follows the players left.
    w.destroy(p1);
    REQUIRE(kit::collect_views(w, {1280, 720}, out));
    REQUIRE(out.size() == 3);
    CHECK(out[0].camera == p2);
    CHECK(out[0].rect.x == 0);
    CHECK(out[0].rect.height == 720);
    CHECK(out[1].camera == p3);
    CHECK(out[1].rect.x == 640);

    // A divider: interior edges move in, the window's edges stay.
    kit::SplitScreen s;
    s.layout = kit::SplitLayout::Rows;
    s.divider = 4;
    w.spawn_with(s);
    REQUIRE(kit::collect_views(w, {1280, 720}, out));
    CHECK(out[0].rect.y == 0);
    CHECK(out[0].rect.height == 358);
    CHECK(out[1].rect.y == 362);
    CHECK(out[1].rect.height == 358);
    CHECK(out[1].rect.width == 1280);
}

TEST_CASE("views: scale_view_quality cuts a small view's costs and leaves a full one alone") {
    render::MeshStore meshes;
    auto scene_with_everything = [&](render::Scene3D& s) {
        s.begin(render::Camera3D{}, {640, 360});
        render::ShadowSettings sh;
        sh.cascades = 4;
        s.set_shadows(sh);
        render::ScreenSpaceReflections ssr;
        ssr.enabled = true;
        s.set_reflections(ssr);
        render::Volumetrics vol;
        vol.enabled = true;
        s.set_volumetrics(vol);
        render::PostFx post;
        post.dof.enabled = true;
        post.motion_blur.enabled = true;
        s.set_post(post);
    };
    render::Scene3D full(meshes, mem::heap());
    scene_with_everything(full);
    kit::scale_view_quality(full, 1.0f);
    CHECK(full.shadow_settings().cascades == 4);
    CHECK(full.reflections().enabled);

    render::Scene3D half(meshes, mem::heap());
    scene_with_everything(half);
    kit::scale_view_quality(half, 0.5f);
    CHECK(half.shadow_settings().cascades == 2);
    CHECK(half.reflections().enabled);
    CHECK(half.volumetrics().enabled);

    render::Scene3D quarter(meshes, mem::heap());
    scene_with_everything(quarter);
    kit::scale_view_quality(quarter, 0.25f);
    CHECK(quarter.shadow_settings().cascades == 2);
    CHECK_FALSE(quarter.reflections().enabled);
    CHECK_FALSE(quarter.volumetrics().enabled);
    CHECK_FALSE(quarter.post().dof.enabled);
    CHECK_FALSE(quarter.post().motion_blur.enabled);
}

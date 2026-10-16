// M-VIEWS 2: kit::collect_views — which cameras are views, in which order, at which pixels.

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

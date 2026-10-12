// #136: kit::ViewInfo maps screen pixels, normalized touches and window-point mouse positions to
// the world through the primary camera's resolved view.

#include <doctest/doctest.h>

#include <utility>

#include "mce/kit/components.h"
#include "mce/render/camera2d.h"

using namespace mce;
using math::vec2;

namespace {

kit::ViewInfo view_for(const render::Camera2D& cam, u32 w, u32 h, f32 ppm = 48.0f) {
    kit::ViewInfo v;
    v.framebuffer_width = w;
    v.framebuffer_height = h;
    v.window_width = w / 2;  // a 2x HiDPI window
    v.window_height = h / 2;
    const render::View2D view = render::compute_view_2d(cam, {w, h}, ppm);
    v.view_center = view.center;
    v.view_pixels_per_meter = view.pixels_per_meter;
    v.view_rotation = cam.rotation;
    return v;
}

}  // namespace

TEST_CASE("view info: screen centre and corners map to the visible world rect") {
    render::Camera2D cam;
    cam.position = vec2(10, 5);
    cam.pixel_snap = false;
    cam.fit_height = 18.0f;
    for (auto [w, h] : {std::pair<u32, u32>{1280, 720}, {1080, 2400}}) {
        const kit::ViewInfo v = view_for(cam, w, h);
        const f32 fw = static_cast<f32>(w), fh = static_cast<f32>(h);
        const render::View2D view = render::compute_view_2d(cam, {w, h}, 48.0f);
        const vec2 c = v.screen_to_world({0.5f * fw, 0.5f * fh});
        CHECK(c.x == doctest::Approx(10.0f));
        CHECK(c.y == doctest::Approx(5.0f));
        const vec2 tl = v.screen_to_world({0.0f, 0.0f});  // top-left pixel -> min x, max y (Y up)
        CHECK(tl.x == doctest::Approx(view.world_bounds.min.x));
        CHECK(tl.y == doctest::Approx(view.world_bounds.max.y));
        const vec2 br = v.touch_to_world({1.0f, 1.0f});
        CHECK(br.x == doctest::Approx(view.world_bounds.max.x));
        CHECK(br.y == doctest::Approx(view.world_bounds.min.y));
        const vec2 m = v.mouse_to_world({0.25f * fw, 0.25f * fh});  // window points are half the pixels
        const vec2 p = v.screen_to_world({0.5f * fw, 0.5f * fh});
        CHECK(m.x == doctest::Approx(p.x));
        CHECK(m.y == doctest::Approx(p.y));
    }
}

TEST_CASE("view info: a rotated camera turns the mapping with it") {
    render::Camera2D cam;
    cam.pixel_snap = false;
    cam.rotation = 1.5707963f;  // 90 degrees CCW: screen right is world up
    const kit::ViewInfo v = view_for(cam, 1000, 1000, 100.0f);
    const vec2 r = v.screen_to_world({600.0f, 500.0f});  // 1 m right of centre on screen
    CHECK(r.x == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(r.y == doctest::Approx(1.0f));
}

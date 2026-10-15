// M-ACCESS step 2: the color-vision correction's reference math, the access.* convars and how
// apply_accessibility folds them into the 2D and 3D post chains. CPU only; the GPU passes are held
// to this reference in render_gl (test_render_gl.cpp, test_render3d_gl.cpp).

#include <doctest/doctest.h>

#include <cmath>

#include "mce/convar.h"
#include "mce/core.h"
#include "mce/render/accessibility.h"
#include "mce/render/renderer.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::render;
using math::vec3;

namespace {

ConVar& cvar(const char* name) {
    ConVar* cv = ConVar::find(StringId(name));
    REQUIRE(cv != nullptr);
    return *cv;
}

struct ResetAccess {
    ~ResetAccess() {
        for (const char* n : {"access.colorblind", "access.colorblind_strength", "access.text_scale", "access.high_contrast",
                              "access.motion_scale"})
            cvar(n).reset();
    }
};

f32 dist(vec3 a, vec3 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }

}  // namespace

TEST_CASE("M-ACCESS: color-vision correction keeps greys and moves the colors a viewer confuses") {
    for (ColorVision cv : {ColorVision::Protanopia, ColorVision::Deuteranopia, ColorVision::Tritanopia}) {
        INFO(color_vision_name(cv));
        // Greys: every simulation row sums to ~1, so there is no error to move.
        for (f32 g : {0.0f, 0.2f, 0.5f, 1.0f}) CHECK(dist(correct_color_vision(cv, 1.0f, {g, g, g}), {g, g, g}) < 0.01f);
        // Strength 0 and None are the identity.
        const vec3 c{0.8f, 0.3f, 0.1f};
        CHECK(dist(correct_color_vision(cv, 0.0f, c), c) == 0.0f);
        CHECK(dist(correct_color_vision(ColorVision::None, 1.0f, c), c) == 0.0f);
        // Output stays in [0, 1].
        const vec3 o = correct_color_vision(cv, 1.0f, {1.0f, 0.0f, 0.0f});
        CHECK(o.x >= 0.0f);
        CHECK(o.y <= 1.0f);
        CHECK(o.z <= 1.0f);
    }
    // Red and green look alike to a protanope; corrected, they are further apart as that viewer
    // sees them.
    const vec3 red{0.8f, 0.2f, 0.1f}, green{0.3f, 0.5f, 0.1f};
    for (ColorVision cv : {ColorVision::Protanopia, ColorVision::Deuteranopia}) {
        INFO(color_vision_name(cv));
        const f32 before = dist(simulate_color_vision(cv, red), simulate_color_vision(cv, green));
        const f32 after = dist(simulate_color_vision(cv, correct_color_vision(cv, 1.0f, red)),
                               simulate_color_vision(cv, correct_color_vision(cv, 1.0f, green)));
        CHECK(after > before * 1.2f);
    }
    // Blue and green for a tritanope.
    const vec3 blue{0.1f, 0.3f, 0.8f}, teal{0.1f, 0.5f, 0.4f};
    const f32 before = dist(simulate_color_vision(ColorVision::Tritanopia, blue), simulate_color_vision(ColorVision::Tritanopia, teal));
    const f32 after = dist(simulate_color_vision(ColorVision::Tritanopia, correct_color_vision(ColorVision::Tritanopia, 1.0f, blue)),
                           simulate_color_vision(ColorVision::Tritanopia, correct_color_vision(ColorVision::Tritanopia, 1.0f, teal)));
    CHECK(after > before * 1.2f);
}

TEST_CASE("M-ACCESS: the access.* convars, clamped, and the post chains they change") {
    ResetAccess reset;
    AccessibilitySettings s = accessibility_settings();
    CHECK(s.color_vision == ColorVision::None);
    CHECK(s.text_scale == 1.0f);
    CHECK(s.motion_scale == 1.0f);
    CHECK_FALSE(s.high_contrast);
    for (const char* n : {"access.colorblind", "access.colorblind_strength", "access.text_scale", "access.high_contrast",
                          "access.motion_scale"})
        CHECK((cvar(n).flags() & CVarFlags::Saved) != 0);  // a settings menu may bind and keep them

    cvar("access.colorblind").set_int(2);
    cvar("access.colorblind_strength").set_float(3.0f);
    cvar("access.text_scale").set_float(10.0f);
    cvar("access.motion_scale").set_float(-1.0f);
    cvar("access.high_contrast").set_bool(true);
    s = accessibility_settings();
    CHECK(s.color_vision == ColorVision::Deuteranopia);
    CHECK(s.color_vision_strength == 1.0f);
    CHECK(s.text_scale == 3.0f);
    CHECK(s.motion_scale == 0.0f);
    CHECK(s.high_contrast);
    cvar("access.colorblind").set_int(9);  // out of range: off
    CHECK(accessibility_settings().color_vision == ColorVision::None);

    // 2D: the correction alone makes the pass worth running; motion 0 drops the aberration.
    AccessibilitySettings a;
    a.color_vision = ColorVision::Protanopia;
    a.motion_scale = 0.5f;
    Post2D p;
    CHECK_FALSE(p.any());
    p.chromatic_aberration = true;
    p.aberration = 0.01f;
    apply_accessibility(a, p);
    CHECK(p.any());
    CHECK(p.color_vision == ColorVision::Protanopia);
    CHECK(p.aberration == doctest::Approx(0.005f));
    a.motion_scale = 0.0f;
    apply_accessibility(a, p);
    CHECK_FALSE(p.chromatic_aberration);
    a.color_vision_strength = 0.0f;  // no strength: no pass
    Post2D q;
    apply_accessibility(a, q);
    CHECK_FALSE(q.any());

    // 3D: the correction goes to the tonemap pass, motion blur scales down and turns off at 0.
    a = {};
    a.color_vision = ColorVision::Tritanopia;
    a.motion_scale = 0.25f;
    PostFx fx;
    fx.motion_blur.enabled = true;
    fx.motion_blur.intensity = 0.8f;
    apply_accessibility(a, fx);
    CHECK(fx.color_vision == ColorVision::Tritanopia);
    CHECK(fx.motion_blur.intensity == doctest::Approx(0.2f));
    CHECK(fx.motion_blur.enabled);
    a.motion_scale = 0.0f;
    apply_accessibility(a, fx);
    CHECK_FALSE(fx.motion_blur.enabled);

    const ShakeOffset shake = scale_shake({{0.4f, -0.2f}, 0.1f}, 0.5f);
    CHECK(shake.translation.x == doctest::Approx(0.2f));
    CHECK(shake.translation.y == doctest::Approx(-0.1f));
    CHECK(shake.rotation == doctest::Approx(0.05f));
}

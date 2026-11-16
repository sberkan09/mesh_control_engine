// M-REFLECT2 48a / 48b: the SSR blur stats default off, and the pure blur radius the main pass repeats as shader text.

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "mce/render/forward3d.h"
#include "mce/render/ssr_blur.h"

using namespace mce;
using namespace mce::render;

TEST_CASE("Forward3DStats: the SSR blur and motion bools default to false") {
    const Forward3DStats st{};
    CHECK_FALSE(st.ssr_blur);
    CHECK_FALSE(st.ssr_motion);
    CHECK_FALSE(st.ssr);
}

TEST_CASE("ssr_blur_radius: zero at and below the threshold, monotone, capped") {
    CHECK(ssr_blur_radius(0.0f, 0.0f) == 0.0f);
    CHECK(ssr_blur_radius(kSsrBlurMinRough, 0.0f) == 0.0f);
    CHECK(ssr_blur_radius(0.05f, 0.0f) == 0.0f);
    CHECK(ssr_blur_radius(0.101f, 0.0f) > 0.0f);
    CHECK(ssr_blur_radius(0.101f, 0.0f) < 0.01f);  // continuous at the threshold: no jump to a visible radius
    f32 prev = 0.0f;
    for (int i = 9; i <= 100; ++i) {
        const f32 r = ssr_blur_radius(f32(i) / 100.0f, 0.0f);
        CHECK(r >= prev);
        CHECK(r <= kSsrBlurMax);
        prev = r;
    }
    CHECK(ssr_blur_radius(1.0f, 0.0f) == kSsrBlurMax);
    CHECK(ssr_blur_radius(5.0f, 0.0f) == kSsrBlurMax);
    // The ladder of the GL test (half-size texels): 0.2 -> 0.72, 0.4 -> 3.6, 0.5 -> 5.76.
    CHECK(ssr_blur_radius(0.2f, 0.0f) == doctest::Approx(24.0f * (0.04f - 0.01f)));
    CHECK(ssr_blur_radius(0.4f, 0.0f) == doctest::Approx(24.0f * (0.16f - 0.01f)));
    CHECK(ssr_blur_radius(0.5f, 0.0f) == doctest::Approx(24.0f * (0.25f - 0.01f)));
}

TEST_CASE("ssr_blur_radius: the bump term widens, never below the threshold") {
    CHECK(ssr_blur_radius(0.2f, 0.1f) > ssr_blur_radius(0.2f, 0.0f));
    CHECK(ssr_blur_radius(0.2f, 0.2f) > ssr_blur_radius(0.2f, 0.1f));
    CHECK(ssr_blur_radius(0.05f, 1.0f) == 0.0f);  // a mirror below the threshold stays one tap
    CHECK(ssr_blur_radius(0.2f, -1.0f) == ssr_blur_radius(0.2f, 0.0f));
    CHECK(ssr_blur_radius(0.3f, 10.0f) == kSsrBlurMax);
}

TEST_CASE("ssr_blur_radius: NaN and negative inputs give 0") {
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    CHECK(ssr_blur_radius(nan, 0.0f) == 0.0f);
    CHECK(ssr_blur_radius(-0.5f, 0.0f) == 0.0f);
    CHECK(ssr_blur_radius(-0.5f, 1.0f) == 0.0f);
    CHECK(ssr_blur_radius(0.3f, nan) == ssr_blur_radius(0.3f, 0.0f));
}

TEST_CASE("ssr_bump_term: zero for a flat map, grows as the dot falls") {
    CHECK(ssr_bump_term(1.0f) == 0.0f);
    CHECK(ssr_bump_term(2.0f) == 0.0f);
    CHECK(ssr_bump_term(0.99f) > 0.0f);
    CHECK(ssr_bump_term(0.9f) > ssr_bump_term(0.99f));
    CHECK(ssr_bump_term(0.0f) == doctest::Approx(kSsrBumpGain));
    CHECK(ssr_bump_term(-1.0f) == doctest::Approx(2.0f * kSsrBumpGain));
    CHECK(ssr_bump_term(-3.0f) == ssr_bump_term(-1.0f));
    CHECK(ssr_bump_term(std::numeric_limits<f32>::quiet_NaN()) == 0.0f);
}

// M-LENSRAIN 42a: the LensRain setting's defaults and the Forward3DStats fields; nothing reads them yet.

#include <doctest/doctest.h>

#include "mce/render/forward3d.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::render;

TEST_CASE("LensRain defaults") {
    const LensRain r;
    CHECK_FALSE(r.enabled);
    CHECK(r.amount == 0.0f);
    CHECK(r.density == 1.0f);
    CHECK(r.size == 1.0f);
    CHECK(r.refraction == 1.0f);
    CHECK(r.blur == 0.35f);
    CHECK(r.trail == 0.5f);
    CHECK(r.fall_speed == 0.25f);
    CHECK(r.seed == 1u);
}

TEST_CASE("PostFx carries a disabled lens rain, Forward3DStats a zero one") {
    const PostFx fx{};
    CHECK_FALSE(fx.rain.enabled);
    CHECK(fx.rain.amount == 0.0f);
    const Forward3DStats st{};
    CHECK_FALSE(st.lens_rain);
    CHECK(st.lens_rain_amount == 0.0f);
}

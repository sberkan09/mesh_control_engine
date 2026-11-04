// M-LIGHTCOOKIE 40a: the light cookie data's defaults (PointLight, SunCookie, DirectionalLight), the Forward3D desc and stats fields and the
// storage bank; nothing reads them yet, so a default light is the light it was.

#include <doctest/doctest.h>

#include "mce/render/clusters.h"
#include "mce/render/forward3d.h"
#include "mce/render/scene3d.h"
#include "mce/rhi/types.h"

using namespace mce;
using namespace mce::render;

TEST_CASE("PointLight carries no cookie by default") {
    const PointLight l;
    CHECK(l.cookie == ~0u);
    CHECK(l.cookie_strength == 1.0f);
    CHECK(l.cookie_roll == 0.0f);
    CHECK(l.cookie_edge == 0.0f);
    // The members before the cookie keep their values.
    CHECK(l.radius == 5.0f);
    CHECK(l.spot_outer == 0.0f);
    CHECK_FALSE(l.cast_shadows);
}

TEST_CASE("SunCookie defaults and DirectionalLight carries an empty one") {
    const SunCookie c;
    CHECK(c.texture == ~0u);
    CHECK(c.strength == 1.0f);
    CHECK(c.size == 20.0f);
    CHECK(c.offset.x == 0.0f);
    CHECK(c.offset.y == 0.0f);
    CHECK(c.roll == 0.0f);
    const DirectionalLight sun;
    CHECK(sun.cookie.texture == ~0u);
    CHECK(sun.intensity == 2.0f);
    CHECK(sun.shadows);
}

TEST_CASE("Forward3D light cookie desc flag and stats") {
    const Forward3DDesc desc{};
    CHECK(desc.light_cookies);
    const Forward3DStats st{};
    CHECK(st.cookie_lights == 0u);
    CHECK(st.cookie_layers == 0u);
    CHECK_FALSE(st.sun_cookie);
}

TEST_CASE("The light cookie storage bank is its own number") {
    using namespace rhi::bank;
    static_assert(kLightCookies != kClusters && kLightCookies != kLightPool && kLightCookies != kLightIndices && kLightCookies != kJointPalette &&
                  kLightCookies != kClusterCull && kLightCookies != kClusterBounds && kLightCookies != kClusterCounter &&
                  kLightCookies != kLocalShadows && kLightCookies != kExposure && kLightCookies != kInstances && kLightCookies != kInstanceVisible &&
                  kLightCookies != kInstanceArgs && kLightCookies != kDecals && kLightCookies != kParticles && kLightCookies != kObjects);
    CHECK(kLightCookies == 2u);
}

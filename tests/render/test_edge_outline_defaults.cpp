// M-OUTLINE 39a: the EdgeOutline setting's defaults and the Forward3D desc and stats fields; nothing reads them yet.

#include <doctest/doctest.h>

#include "mce/render/forward3d.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::render;

TEST_CASE("EdgeOutline defaults") {
    const EdgeOutline o;
    CHECK_FALSE(o.enabled);
    CHECK(o.color.x == 0.02f);
    CHECK(o.color.y == 0.02f);
    CHECK(o.color.z == 0.025f);
    CHECK(o.opacity == 1.0f);
    CHECK(o.thickness == 1.0f);
    CHECK(o.depth_threshold == 0.05f);
    CHECK(o.normal_threshold == 0.4f);
    CHECK(o.depth_weight == 1.0f);
    CHECK(o.normal_weight == 1.0f);
    CHECK(o.fade_start == 30.0f);
    CHECK(o.fade_end == 80.0f);
    CHECK(o.fade_end >= o.fade_start);
    CHECK(o.sky_silhouette);
    CHECK(o.fog_follow);
}

TEST_CASE("PostFx carries a disabled outline, Forward3D a default-on desc flag and zero stats") {
    const PostFx fx{};
    CHECK_FALSE(fx.outline.enabled);
    const Forward3DDesc desc{};
    CHECK(desc.edge_outline);
    const Forward3DStats st{};
    CHECK_FALSE(st.edge_outline);
    CHECK(st.edge_outline_taps == 0u);
}

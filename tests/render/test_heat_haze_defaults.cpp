// M-HEATHAZE 43a: the HeatHaze setting's defaults and the Forward3DStats field; nothing reads them yet.

#include <doctest/doctest.h>

#include "mce/render/forward3d.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::render;

TEST_CASE("HeatHazeVolume defaults") {
    const HeatHazeVolume v;
    CHECK(v.center.x == 0.0f);
    CHECK(v.center.y == 0.0f);
    CHECK(v.center.z == 0.0f);
    CHECK(v.radius == 1.0f);
    CHECK(v.strength == 0.5f);
    CHECK(v.rise == 0.6f);
    CHECK(v.scale == 1.0f);
    CHECK(v.softness == 0.5f);
}

TEST_CASE("PostFx carries an empty heat haze, Forward3DStats a zero one") {
    static_assert(kHeatHazeMaxVolumes == 8);
    const HeatHaze h;
    CHECK(h.count == 0u);
    for (u32 i = 0; i < kHeatHazeMaxVolumes; ++i) CHECK(h.volumes[i].radius == 1.0f);
    const PostFx fx{};
    CHECK(fx.haze.count == 0u);
    const Forward3DStats st{};
    CHECK(st.heat_haze_volumes == 0u);
}

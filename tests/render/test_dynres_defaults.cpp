// M-DYNRES 46a: the DynamicResolution setting's defaults, the Forward3DDesc floor and the Forward3DStats fields; nothing reads them yet.

#include <doctest/doctest.h>

#include "mce/render/forward3d.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::render;

TEST_CASE("DynamicResolution defaults") {
    const DynamicResolution d;
    CHECK_FALSE(d.enabled);
    CHECK(d.target_ms == 14.0f);
    CHECK(d.min_scale == 0.6f);
    CHECK(d.max_scale == 1.0f);
    CHECK(d.headroom == 0.15f);
    for (u8 b : d.pad_) CHECK(b == 0);
}

TEST_CASE("PostFx carries the default (off) dynamic resolution, Forward3DStats and Forward3DDesc the neutral ones") {
    const PostFx fx{};
    CHECK_FALSE(fx.dynres.enabled);
    CHECK(fx.dynres.target_ms == 14.0f);
    CHECK(fx.dynres.max_scale == 1.0f);
    const Forward3DStats st{};
    CHECK(st.render_scale == 1.0f);
    CHECK_FALSE(st.dynres);
    CHECK(st.dynres_cost_ms == 0.0f);
    const Forward3DDesc desc{};
    CHECK(desc.render_scale == 1.0f);
    CHECK(desc.dynres_floor == 0.6f);
}

// M-SUNSHAFTS 45a: the SunShafts setting's defaults and the Forward3DStats fields; nothing reads them yet.

#include <doctest/doctest.h>

#include "mce/render/forward3d.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::render;

TEST_CASE("SunShafts defaults") {
    const SunShafts s;
    CHECK_FALSE(s.enabled);
    CHECK(s.intensity == 1.0f);
    CHECK(s.length == 0.85f);
    CHECK(s.decay == 0.97f);
    CHECK(s.threshold == 1.0f);
    CHECK(s.cap == 1.5f);
    CHECK(s.visibility == 1.0f);
    CHECK(s.scatter_m == 60.0f);
    for (u8 b : s.pad_) CHECK(b == 0);
}

TEST_CASE("PostFx carries the default (off) sun shafts setting, Forward3DStats a zero one") {
    const PostFx fx{};
    CHECK_FALSE(fx.shafts.enabled);
    CHECK(fx.shafts.intensity == 1.0f);
    CHECK(fx.shafts.scatter_m == 60.0f);
    const Forward3DStats st{};
    CHECK_FALSE(st.sun_shafts);
    CHECK(st.sun_shafts_strength == 0.0f);
}

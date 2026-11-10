// M-CLOUDSHADOW 51a: the CloudShadow setting's defaults and the Forward3DStats fields; nothing reads them yet.

#include <doctest/doctest.h>

#include "mce/render/forward3d.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::render;

TEST_CASE("CloudShadow defaults") {
    const CloudShadow s;
    CHECK_FALSE(s.enabled);
    CHECK(s.strength == 0.6f);
    CHECK(s.softness == 0.3f);
    for (u8 b : s.pad_) CHECK(b == 0);
}

TEST_CASE("PostFx carries the default (off) cloud shadow setting, Forward3DStats a zero one") {
    const PostFx fx{};
    CHECK_FALSE(fx.cloud_shadow.enabled);
    CHECK(fx.cloud_shadow.strength == 0.6f);
    CHECK(fx.cloud_shadow.softness == 0.3f);
    const Forward3DStats st{};
    CHECK_FALSE(st.cloud_shadow);
    CHECK(st.cloud_shadow_strength == 0.0f);
}

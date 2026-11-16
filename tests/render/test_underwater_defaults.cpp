// M-UNDERWATER 44a: the Underwater setting's defaults and the Forward3DStats fields; nothing reads them yet.

#include <doctest/doctest.h>

#include "mce/render/forward3d.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::render;

TEST_CASE("Underwater defaults") {
    const Underwater u;
    CHECK(u.enabled);
    CHECK(u.strength == 1.0f);
    CHECK(u.wobble == 0.5f);
    CHECK(u.fog_scale == 1.0f);
    CHECK(u.band == 0.0f);
    for (u8 b : u.pad_) CHECK(b == 0);
}

TEST_CASE("PostFx carries the default underwater setting, Forward3DStats a zero one") {
    const PostFx fx{};
    CHECK(fx.underwater.enabled);
    CHECK(fx.underwater.strength == 1.0f);
    CHECK(fx.underwater.band == 0.0f);
    const Forward3DStats st{};
    CHECK_FALSE(st.underwater);
    CHECK(st.underwater_submersion == 0.0f);
}

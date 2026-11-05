// M-TRANSLUCENT 41a: the Forward3D desc flag and the stats counter; nothing reads them yet.

#include <doctest/doctest.h>

#include "mce/render/forward3d.h"

using namespace mce;
using namespace mce::render;

TEST_CASE("Forward3D translucency desc flag and stats") {
    const Forward3DDesc desc{};
    CHECK(desc.translucency);
    const Forward3DStats st{};
    CHECK(st.translucent_draws == 0u);
}

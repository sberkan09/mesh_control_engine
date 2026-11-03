// M-LENSFLARE 38a: the LensFlare setting's defaults and the Forward3DStats fields; nothing reads them yet.

#include <doctest/doctest.h>

#include "mce/render/forward3d.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::render;

TEST_CASE("LensFlare defaults") {
    const LensFlare f;
    CHECK_FALSE(f.enabled);
    CHECK(f.intensity == 1.0f);
    CHECK(f.visibility == 1.0f);
    CHECK(f.halo == 1.0f);
    CHECK(f.halo_radius == 0.25f);
    CHECK(f.ghost_count == 4u);
    CHECK(f.ghost_intensity == 1.0f);
    CHECK(f.ghost_spacing == 1.0f);
    CHECK(f.ghost_chroma == 0.5f);
    CHECK(f.starburst == 0.0f);
    CHECK(f.starburst_rays == 6u);
    CHECK(f.occlusion_radius == 0.015f);
    CHECK(f.edge_fade == 0.1f);
    CHECK(kLensFlareMaxGhosts == 6u);
    CHECK(f.ghost_count <= kLensFlareMaxGhosts);
}

TEST_CASE("PostFx carries a disabled flare, Forward3DStats a zero one") {
    const PostFx fx{};
    CHECK_FALSE(fx.flare.enabled);
    const Forward3DStats st{};
    CHECK_FALSE(st.lens_flare);
    CHECK(st.lens_flare_strength == 0.0f);
}

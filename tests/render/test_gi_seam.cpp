// M-GI2 50b: the GiVolume carrier fields, their clamp and the volume texture's size.

#include <doctest/doctest.h>

#include <limits>

#include "mce/render/forward3d.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::render;

TEST_CASE("gi2: the carrier defaults leave every existing volume unchanged") {
    const GiVolume gi;
    CHECK_FALSE(gi.visibility);
    CHECK_FALSE(gi.relocate);
    CHECK_FALSE(gi.follow);
    CHECK(gi.max_offset == doctest::Approx(0.4f));
    CHECK(gi_texels_per_probe(gi) == 4);
    const rhi::Extent2D es = gi_volume_size(gi);
    CHECK(es.width == gi.counts[0] * 4);
    CHECK(es.height == gi.counts[1] * gi.counts[2]);
}

TEST_CASE("gi2: any of the three features widens a probe to 10 texels") {
    GiVolume a;
    a.visibility = true;
    GiVolume b;
    b.relocate = true;
    GiVolume c;
    c.follow = true;
    for (const GiVolume* g : {&a, &b, &c}) {
        CHECK(gi_texels_per_probe(*g) == 10);
        CHECK(gi_volume_size(*g).width == g->counts[0] * 10);
        CHECK(gi_volume_size(*g).height == g->counts[1] * g->counts[2]);
    }
}

TEST_CASE("gi2: apply_quality clamps max_offset and clears follow without updates") {
    const QualitySettings q = quality_settings(QualityTier::High);
    GiVolume gi;
    gi.max_offset = 2.0f;
    apply_quality(q, gi);
    CHECK(gi.max_offset == doctest::Approx(0.45f));
    gi.max_offset = -1.0f;
    apply_quality(q, gi);
    CHECK(gi.max_offset == doctest::Approx(0.0f));
    gi.max_offset = std::numeric_limits<f32>::quiet_NaN();
    apply_quality(q, gi);
    CHECK(gi.max_offset == doctest::Approx(0.4f));

    GiVolume frozen;
    frozen.follow = true;
    frozen.updates = false;
    apply_quality(q, frozen);
    CHECK_FALSE(frozen.follow);
    GiVolume live;
    live.follow = true;
    apply_quality(q, live);
    CHECK(live.follow);
}

TEST_CASE("gi2: the new stats start at zero") {
    const Forward3DStats st;
    CHECK_FALSE(st.gi_visibility);
    CHECK(st.gi_relocated == 0);
    CHECK(st.gi_scrolled == 0);
    CHECK(st.gi_vis_draws == 0);
}

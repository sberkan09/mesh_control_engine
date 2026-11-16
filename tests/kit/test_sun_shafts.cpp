// SunShafts3D (M-SUNSHAFTS 45d): the component defaults match the render setting, the explicit pad is zero, and the
// pure sun_shafts_visibility = clamp(1 - cloud_follow * cover, 0, 1), with the cover clamped to 0..1, NaN inputs as 0
// (the cover) or the default 0.5 (cloud_follow), exactly 1 for a clear sky or cloud_follow 0.

#include <doctest/doctest.h>

#include <bit>
#include <limits>

#include "mce/kit/sun_shafts.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();
constexpr f32 kInf = std::numeric_limits<f32>::infinity();

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }

}  // namespace

TEST_CASE("sun shafts: component defaults match the render setting") {
    const SunShafts3D c;
    const render::SunShafts r;
    CHECK(c.enabled == 1u);
    CHECK(same_bits(c.intensity, r.intensity));
    CHECK(same_bits(c.length, r.length));
    CHECK(same_bits(c.decay, r.decay));
    CHECK(same_bits(c.threshold, r.threshold));
    CHECK(same_bits(c.cap, r.cap));
    CHECK(same_bits(c.scatter_m, r.scatter_m));
    CHECK(same_bits(c.cloud_follow, 0.5f));
    CHECK(same_bits(c.threshold, 1.0f));  // the measured default (M-SUNSHAFTS 45c): the disk passes, the sky does not
    CHECK(c.pad_[0] == 0u);
    CHECK(c.pad_[1] == 0u);
    CHECK(c.pad_[2] == 0u);
}

TEST_CASE("sun shafts: a clear sky or cloud_follow 0 gives exactly 1") {
    SunShafts3D c;
    CHECK(same_bits(sun_shafts_visibility(c, 0.0f), 1.0f));
    c.cloud_follow = 0.0f;
    CHECK(same_bits(sun_shafts_visibility(c, 1.0f), 1.0f));
}

TEST_CASE("sun shafts: the cover dims by cloud_follow and the result stays in 0..1") {
    SunShafts3D c;
    CHECK(sun_shafts_visibility(c, 0.5f) == doctest::Approx(0.75f));
    CHECK(sun_shafts_visibility(c, 1.0f) == doctest::Approx(0.5f));
    c.cloud_follow = 1.0f;
    CHECK(same_bits(sun_shafts_visibility(c, 1.0f), 0.0f));
    CHECK(sun_shafts_visibility(c, 0.25f) == doctest::Approx(0.75f));
    c.cloud_follow = 4.0f;  // clamped to 1
    CHECK(same_bits(sun_shafts_visibility(c, 1.0f), 0.0f));
    c.cloud_follow = -1.0f;  // clamped to 0
    CHECK(same_bits(sun_shafts_visibility(c, 1.0f), 1.0f));
    c.cloud_follow = 1.0f;
    CHECK(same_bits(sun_shafts_visibility(c, 3.0f), 0.0f));   // the cover clamps to 1
    CHECK(same_bits(sun_shafts_visibility(c, -2.0f), 1.0f));  // and to 0
    // monotone in the cover
    f32 prev = 2.0f;
    for (int i = 0; i <= 20; ++i) {
        const f32 v = sun_shafts_visibility(SunShafts3D{}, f32(i) / 20.0f);
        CHECK(v <= prev);
        prev = v;
    }
}

TEST_CASE("sun shafts: NaN and infinite inputs stay finite") {
    SunShafts3D c;
    CHECK(same_bits(sun_shafts_visibility(c, kNaN), 1.0f));  // NaN cover counts as a clear sky
    CHECK(same_bits(sun_shafts_visibility(c, kInf), 0.5f));  // clamped to a full cover
    c.cloud_follow = kNaN;                                   // the default 0.5
    CHECK(sun_shafts_visibility(c, 1.0f) == doctest::Approx(0.5f));
}

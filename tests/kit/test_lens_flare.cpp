// LensFlare3D (M-LENSFLARE 38d): the component defaults and the pure lens_flare_visibility = 1 - cloud_dim * max(cloud, rain),
// clamped to 0..1, NaN inputs as 0, exactly 1 for clear air or cloud_dim 0.

#include <doctest/doctest.h>

#include <bit>
#include <limits>

#include "mce/kit/lens_flare.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();
constexpr f32 kInf = std::numeric_limits<f32>::infinity();

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }

}  // namespace

TEST_CASE("lens flare: component defaults match the render setting") {
    const LensFlare3D f;
    CHECK(f.enabled == 1u);
    CHECK(same_bits(f.intensity, 1.0f));
    CHECK(same_bits(f.halo, 1.0f));
    CHECK(same_bits(f.halo_radius, 0.25f));
    CHECK(f.ghost_count == 4u);
    CHECK(same_bits(f.ghost_intensity, 1.0f));
    CHECK(same_bits(f.ghost_spacing, 1.0f));
    CHECK(same_bits(f.ghost_chroma, 0.5f));
    CHECK(same_bits(f.starburst, 0.0f));
    CHECK(f.starburst_rays == 6u);
    CHECK(same_bits(f.occlusion_radius, 0.015f));
    CHECK(same_bits(f.edge_fade, 0.1f));
    CHECK(same_bits(f.cloud_dim, 1.0f));
    CHECK(f.pad_[0] == 0u);
    CHECK(f.pad_[1] == 0u);
    CHECK(f.pad_[2] == 0u);
}

TEST_CASE("lens flare: clear air gives exactly 1") {
    const LensFlare3D f;
    CHECK(same_bits(lens_flare_visibility(f, 0.0f, 0.0f), 1.0f));
}

TEST_CASE("lens flare: cloud cover and rain dim by the larger of the two") {
    const LensFlare3D f;
    CHECK(lens_flare_visibility(f, 0.25f, 0.0f) == doctest::Approx(0.75f));
    CHECK(lens_flare_visibility(f, 0.0f, 0.5f) == doctest::Approx(0.5f));
    CHECK(lens_flare_visibility(f, 0.25f, 0.5f) == doctest::Approx(0.5f));
    CHECK(lens_flare_visibility(f, 0.5f, 0.25f) == doctest::Approx(0.5f));
    CHECK(same_bits(lens_flare_visibility(f, 1.0f, 0.0f), 0.0f));
}

TEST_CASE("lens flare: cloud_dim scales the dimming") {
    LensFlare3D f;
    f.cloud_dim = 0.5f;
    CHECK(lens_flare_visibility(f, 1.0f, 0.0f) == doctest::Approx(0.5f));
    CHECK(lens_flare_visibility(f, 0.4f, 0.2f) == doctest::Approx(0.8f));
    f.cloud_dim = 0.0f;
    CHECK(same_bits(lens_flare_visibility(f, 1.0f, 1.0f), 1.0f));
}

TEST_CASE("lens flare: inputs clamp and NaN counts as 0") {
    LensFlare3D f;
    CHECK(same_bits(lens_flare_visibility(f, 5.0f, 0.0f), 0.0f));
    CHECK(same_bits(lens_flare_visibility(f, -3.0f, -1.0f), 1.0f));
    CHECK(same_bits(lens_flare_visibility(f, kNaN, kNaN), 1.0f));
    CHECK(lens_flare_visibility(f, kNaN, 0.5f) == doctest::Approx(0.5f));
    CHECK(same_bits(lens_flare_visibility(f, kInf, 0.0f), 0.0f));
    f.cloud_dim = kNaN;  // the default 1
    CHECK(lens_flare_visibility(f, 0.5f, 0.0f) == doctest::Approx(0.5f));
    f.cloud_dim = 7.0f;  // clamps to 1
    CHECK(lens_flare_visibility(f, 0.5f, 0.0f) == doctest::Approx(0.5f));
    f.cloud_dim = -2.0f;  // clamps to 0
    CHECK(same_bits(lens_flare_visibility(f, 1.0f, 1.0f), 1.0f));
}

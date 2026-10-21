// M-WINDDIR 8b: SnowCover::wind_dir and how apply_quality normalizes it.

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "mce/render/quality.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::render;

namespace {

SnowCover with_dir(f32 x, f32 y) {
    SnowCover s;
    s.cover = 0.8f;
    s.wind_dir = math::vec2{x, y};
    return s;
}

}  // namespace

TEST_CASE("snow wind_dir: the default is +x, and apply_quality keeps it") {
    const SnowCover d;
    CHECK(d.wind_dir.x == 1.0f);
    CHECK(d.wind_dir.y == 0.0f);
    const SnowCover out = apply_quality(quality_settings(QualityTier::High), d);
    CHECK(out.wind_dir.x == 1.0f);
    CHECK(out.wind_dir.y == 0.0f);
}

TEST_CASE("snow wind_dir: normalized to unit length") {
    const QualitySettings q = quality_settings(QualityTier::High);
    SnowCover out = apply_quality(q, with_dir(0.0f, 2.0f));
    CHECK(out.wind_dir.x == doctest::Approx(0.0f));
    CHECK(out.wind_dir.y == doctest::Approx(1.0f));
    out = apply_quality(q, with_dir(3.0f, 4.0f));
    CHECK(out.wind_dir.x == doctest::Approx(0.6f));
    CHECK(out.wind_dir.y == doctest::Approx(0.8f));
    out = apply_quality(q, with_dir(-5.0f, 0.0f));
    CHECK(out.wind_dir.x == doctest::Approx(-1.0f));
    CHECK(out.wind_dir.y == doctest::Approx(0.0f));
}

TEST_CASE("snow wind_dir: zero, tiny, NaN and inf fall back to +x") {
    const QualitySettings q = quality_settings(QualityTier::High);
    const f32 nan = std::nanf("");
    const f32 inf = std::numeric_limits<f32>::infinity();
    const f32 cases[][2] = {{0.0f, 0.0f}, {1.0e-7f, -1.0e-7f}, {nan, 1.0f}, {1.0f, nan},
                            {inf, 0.0f},  {0.0f, -inf},        {inf, inf}};
    for (const auto& c : cases) {
        INFO(c[0] << " " << c[1]);
        const SnowCover out = apply_quality(q, with_dir(c[0], c[1]));
        CHECK(out.wind_dir.x == 1.0f);
        CHECK(out.wind_dir.y == 0.0f);
    }
}

TEST_CASE("snow wind_dir: the other fields are unchanged by it") {
    const QualitySettings q = quality_settings(QualityTier::High);
    SnowCover a = with_dir(0.0f, 1.0f);
    a.coverage = 0.4f;
    a.noise_scale = 7.0f;
    a.wind_bias = 0.25f;
    const SnowCover out = apply_quality(q, a);
    CHECK(out.cover == 0.8f);
    CHECK(out.coverage == 0.4f);
    CHECK(out.noise_scale == 7.0f);
    CHECK(out.wind_bias == 0.25f);
    CHECK(out.noise_enabled);
}

TEST_CASE("snow wind_dir: the Low tier still drops only the noise") {
    const SnowCover out = apply_quality(quality_settings(QualityTier::Low), with_dir(3.0f, 4.0f));
    CHECK(out.cover == 0.8f);
    CHECK_FALSE(out.noise_enabled);
    CHECK(out.wind_dir.x == doctest::Approx(0.6f));
    CHECK(out.wind_dir.y == doctest::Approx(0.8f));
}

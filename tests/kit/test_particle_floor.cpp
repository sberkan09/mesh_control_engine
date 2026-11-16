// Particle box floor (M-PARTICLEFLOOR 32a): the emitter centre puts the box floor at the assumed ground
// (camera - eye) and never lower; the spawn range is camera - eye .. camera - eye + 2 * half; NaN, infinite and negative
// inputs fall back to finite values; the helper is deterministic.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <limits>

#include "mce/kit/particle_floor.h"

using namespace mce;
using namespace mce::kit;

namespace {
constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();
constexpr f32 kInf = std::numeric_limits<f32>::infinity();
bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }
}  // namespace

TEST_CASE("particle floor: the box floor sits at the assumed ground") {
    const f32 cam = 1.6f;
    const f32 half = 3.0f;
    const f32 c = particle_floor_center_y(cam, half, 1.6f);
    CHECK(c - half == doctest::Approx(0.0f));  // floor at the ground (camera - eye)
    CHECK(c + half == doctest::Approx(6.0f));
}

TEST_CASE("particle floor: the floor follows the camera height and the eye height") {
    CHECK(particle_floor_center_y(11.6f, 1.2f, 1.6f) - 1.2f == doctest::Approx(10.0f));
    CHECK(particle_floor_center_y(5.0f, 2.0f, 0.0f) - 2.0f == doctest::Approx(5.0f));
    CHECK(particle_floor_center_y(5.0f, 2.0f, 2.5f) - 2.0f == doctest::Approx(2.5f));
}

TEST_CASE("particle floor: the default eye height is 1.6 m") {
    CHECK(kDefaultEyeM == doctest::Approx(1.6f));
    CHECK(particle_floor_center_y(0.0f, 0.0f, kDefaultEyeM) == doctest::Approx(-1.6f));
}

TEST_CASE("particle floor: a larger half height raises the centre one for one") {
    const f32 a = particle_floor_center_y(2.0f, 1.0f, 1.6f);
    const f32 b = particle_floor_center_y(2.0f, 3.0f, 1.6f);
    CHECK(b - a == doctest::Approx(2.0f));
}

TEST_CASE("particle floor: NaN, infinite and negative inputs fall back to finite values") {
    CHECK(std::isfinite(particle_floor_center_y(kNaN, 1.0f, 1.6f)));
    CHECK(std::isfinite(particle_floor_center_y(kInf, 1.0f, 1.6f)));
    CHECK(particle_floor_center_y(kNaN, 1.0f, 1.6f) == doctest::Approx(-0.6f));      // camera 0
    CHECK(particle_floor_center_y(2.0f, kNaN, 1.6f) == doctest::Approx(0.4f));        // half height 0
    CHECK(particle_floor_center_y(2.0f, -3.0f, 1.6f) == doctest::Approx(0.4f));       // negative half height is 0
    CHECK(particle_floor_center_y(2.0f, 1.0f, kNaN) == doctest::Approx(1.4f));        // default eye
    CHECK(particle_floor_center_y(2.0f, 1.0f, -1.0f) == doctest::Approx(1.4f));       // negative eye is the default
    CHECK(std::isfinite(particle_floor_center_y(kInf, kInf, kInf)));
}

TEST_CASE("particle floor: deterministic") {
    for (int i = 0; i < 4; ++i) {
        const f32 a = particle_floor_center_y(1.25f * static_cast<f32>(i), 0.5f * static_cast<f32>(i), 1.6f);
        const f32 b = particle_floor_center_y(1.25f * static_cast<f32>(i), 0.5f * static_cast<f32>(i), 1.6f);
        CHECK(same_bits(a, b));
    }
}

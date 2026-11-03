// LightFlicker3D (M-FLICKER 36a): the scale is a pure function of (authoring, time, salt); it stays in [1 - depth, 1] for every
// style and any time, is exactly 1 for depth 0 or enabled 0, is continuous for the fire style, drops out now and then for the
// failing lamp, repeats with the pulse period, and NaN or out-of-range input stays finite; the component defaults hold.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <limits>

#include "mce/kit/light_flicker.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();
constexpr f64 kInf = std::numeric_limits<f64>::infinity();

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }

LightFlicker3D make(u32 style, f32 depth = 0.5f) {
    LightFlicker3D f;
    f.style = style;
    f.depth = depth;
    return f;
}

}  // namespace

TEST_CASE("light flicker: defaults") {
    const LightFlicker3D f;
    CHECK(f.enabled == 1);
    CHECK(f.style == 0);
    CHECK(f.depth == 0.35f);
    CHECK(f.rate_hz == 8.0f);
    CHECK(f.radius_depth == 0.05f);
    CHECK(f.seed == 1);
    CHECK(f.phase == 0.0f);
    CHECK(f.emissive_follow == 0.0f);
    CHECK(f.pad_[0] == 0);
    CHECK(f.pad_[1] == 0);
    CHECK(f.pad_[2] == 0);
}

TEST_CASE("light flicker: the same inputs give the same bits, another salt another curve") {
    for (u32 style = 0; style < 3; ++style) {
        const LightFlicker3D f = make(style, 1.0f);
        bool differs = false;
        for (int i = 0; i < 400; ++i) {
            const f64 t = 0.173 * i;
            CHECK(same_bits(light_flicker_scale(f, t, 5), light_flicker_scale(f, t, 5)));
            CHECK(same_bits(light_flicker_radius_scale(f, t, 5), light_flicker_radius_scale(f, t, 5)));
            if (!same_bits(light_flicker_scale(f, t, 5), light_flicker_scale(f, t, 6))) differs = true;
        }
        if (style != 2) CHECK(differs);  // the pulse has no hash: lamps differ by phase only
    }
    LightFlicker3D a = make(0);
    LightFlicker3D b = make(0);
    b.seed = 2;
    bool differs = false;
    for (int i = 0; i < 200; ++i) differs = differs || !same_bits(light_flicker_scale(a, 0.1 * i, 0), light_flicker_scale(b, 0.1 * i, 0));
    CHECK(differs);
}

TEST_CASE("light flicker: the scale stays in [1 - depth, 1] for every style and time, large ones included") {
    const f64 bases[] = {0.0, 1.0e3, 1.0e7, 1.0e12};
    const f32 depths[] = {0.1f, 0.35f, 1.0f};
    for (u32 style = 0; style < 3; ++style) {
        for (const f32 depth : depths) {
            LightFlicker3D f = make(style, depth);
            f.radius_depth = 0.4f;
            f.phase = 0.3f;
            for (const f64 base : bases) {
                for (int i = 0; i < 2000; ++i) {
                    const f64 t = base + 0.037 * i;
                    const f32 s = light_flicker_scale(f, t, 9);
                    CHECK(std::isfinite(s));
                    CHECK(s >= 1.0f - depth);
                    CHECK(s <= 1.0f);
                    const f32 r = light_flicker_radius_scale(f, t, 9);
                    CHECK(std::isfinite(r));
                    CHECK(r >= 1.0f - 0.4f);
                    CHECK(r <= 1.0f);
                }
            }
        }
    }
}

TEST_CASE("light flicker: depth 0 or enabled 0 give exactly 1, radius_depth 0 leaves the radius alone") {
    for (u32 style = 0; style < 3; ++style) {
        LightFlicker3D off = make(style, 0.0f);
        LightFlicker3D dis = make(style, 0.8f);
        dis.enabled = 0;
        dis.radius_depth = 0.3f;
        LightFlicker3D flat = make(style, 0.8f);
        flat.radius_depth = 0.0f;
        for (int i = 0; i < 100; ++i) {
            const f64 t = 0.31 * i;
            CHECK(light_flicker_scale(off, t, 3) == 1.0f);
            CHECK(light_flicker_scale(dis, t, 3) == 1.0f);
            CHECK(light_flicker_radius_scale(dis, t, 3) == 1.0f);
            CHECK(light_flicker_radius_scale(flat, t, 3) == 1.0f);
        }
    }
}

TEST_CASE("light flicker: the radius breathes with the intensity") {
    LightFlicker3D f = make(0, 1.0f);
    f.radius_depth = 0.5f;
    for (int i = 0; i < 300; ++i) {
        const f64 t = 0.05 * i;
        // 1 - depth * x and 1 - radius_depth * x with one x: radius_scale == 1 - 0.5 * (1 - scale) (depth is 1)
        CHECK(light_flicker_radius_scale(f, t, 4) == doctest::Approx(1.0f - 0.5f * (1.0f - light_flicker_scale(f, t, 4))).epsilon(1e-5));
    }
}

TEST_CASE("light flicker: NaN, infinite, negative and out-of-range input stays finite and in range") {
    for (u32 style = 0; style < 5; ++style) {  // 3 and 4 are not styles: they count as fire
        LightFlicker3D f = make(style, kNaN);
        f.rate_hz = kNaN;
        f.radius_depth = kNaN;
        f.phase = kNaN;
        const f64 times[] = {kNaN, kInf, -kInf, -1.0, -1.0e9, 0.0, -0.0, 1.0e18};
        for (const f64 t : times) {
            const f32 s = light_flicker_scale(f, t, 1);
            const f32 r = light_flicker_radius_scale(f, t, 1);
            CHECK(std::isfinite(s));
            CHECK(std::isfinite(r));
            CHECK(s >= 1.0f - 0.35f);  // NaN depth is the default 0.35
            CHECK(s <= 1.0f);
            CHECK(r >= 1.0f - 0.05f);
            CHECK(r <= 1.0f);
        }
        LightFlicker3D wild = make(style, 7.0f);  // clamps to 1
        wild.rate_hz = -5.0f;
        wild.radius_depth = 9.0f;
        wild.phase = 42.0f;
        for (int i = 0; i < 100; ++i) {
            const f32 s = light_flicker_scale(wild, -3.7 * i, 0);
            const f32 r = light_flicker_radius_scale(wild, -3.7 * i, 0);
            CHECK(std::isfinite(s));
            CHECK(s >= 0.0f);
            CHECK(s <= 1.0f);
            CHECK(r >= 0.5f);
            CHECK(r <= 1.0f);
        }
    }
    LightFlicker3D neg = make(0, -1.0f);  // clamps to 0: off
    CHECK(light_flicker_scale(neg, 1.0, 0) == 1.0f);
    CHECK(light_flicker_scale(make(0, kNaN), 1.0, 0) < 1.0f + 1e-6f);
}

TEST_CASE("light flicker: the fire style is continuous") {
    LightFlicker3D f = make(0, 1.0f);
    f.rate_hz = 30.0f;  // the fastest: the third octave runs at 183 Hz
    f32 lo = 1.0f;
    f32 hi = 0.0f;
    f32 worst = 0.0f;
    for (const f64 base : {0.0, 1.0e7}) {
        f32 prev = light_flicker_scale(f, base, 2);
        for (int i = 1; i <= 20000; ++i) {
            const f32 s = light_flicker_scale(f, base + 0.001 * i, 2);
            worst = std::max(worst, std::fabs(s - prev));
            lo = std::min(lo, s);
            hi = std::max(hi, s);
            prev = s;
        }
    }
    CHECK(worst < 0.1f);  // the steepest slope of the three octaves adds up to under 92 per second at 30 Hz: 1 ms is under 0.1
    CHECK(hi - lo > 0.3f);  // and it does move
}

TEST_CASE("light flicker: the failing lamp is mostly bright with short dropouts") {
    LightFlicker3D f = make(1, 0.9f);
    int bright = 0;
    int dark = 0;
    int total = 0;
    f32 worst = 0.0f;
    f32 prev = light_flicker_scale(f, 0.0, 7);
    for (int i = 1; i <= 60000; ++i) {  // 60 s at 1 ms
        const f32 s = light_flicker_scale(f, 0.001 * i, 7);
        ++total;
        if (s > 0.99f) ++bright;
        if (s < 0.2f) ++dark;
        worst = std::max(worst, std::fabs(s - prev));
        prev = s;
    }
    CHECK(bright > total / 2);
    CHECK(dark > 0);
    CHECK(dark < total / 3);
    CHECK(worst < 0.1f);  // the edges are smooth: a fifth of a 125 ms cell
}

TEST_CASE("light flicker: the pulse is a sine at a tenth of rate_hz") {
    LightFlicker3D f = make(2, 1.0f);
    f.rate_hz = 10.0f;  // 1 Hz: a period of 1 s
    CHECK(light_flicker_scale(f, 0.0, 0) == doctest::Approx(1.0f).epsilon(1e-5));      // x 0: bright
    CHECK(light_flicker_scale(f, 0.5, 0) == doctest::Approx(0.0f).epsilon(1e-5));      // x 1: the deepest dip
    CHECK(light_flicker_scale(f, 0.25, 0) == doctest::Approx(0.5f).epsilon(0.01));     // the half way point
    for (int i = 0; i < 50; ++i) {
        const f64 t = 0.0137 * i;
        CHECK(light_flicker_scale(f, t, 0) == doctest::Approx(light_flicker_scale(f, t + 3.0, 0)).epsilon(1e-4));
        CHECK(light_flicker_scale(f, t, 0) == doctest::Approx(light_flicker_scale(f, t, 99)).epsilon(1e-6));
        // close to the cosine it stands in for
        const f64 ref = 0.5 - 0.5 * std::cos(6.283185307179586 * t);
        CHECK(light_flicker_scale(f, t, 0) == doctest::Approx(static_cast<f32>(1.0 - ref)).epsilon(0.01));
    }
    f.phase = 0.5f;  // half a cycle later
    CHECK(light_flicker_scale(f, 0.0, 0) == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(light_flicker_scale(f, 0.5, 0) == doctest::Approx(1.0f).epsilon(1e-5));
}

TEST_CASE("light flicker: emissive_follow 0 gives exactly 1, 1 equals the light scale, 0.5 is midway (M-LAMPGLOW 37a)") {
    for (u32 style = 0; style < 3; ++style) {
        LightFlicker3D none = make(style, 0.8f);
        LightFlicker3D full = make(style, 0.8f);
        full.emissive_follow = 1.0f;
        LightFlicker3D half = make(style, 0.8f);
        half.emissive_follow = 0.5f;
        for (int i = 0; i < 100; ++i) {
            const f64 t = 0.37 * i;
            const f32 s = light_flicker_scale(full, t, 5);
            CHECK(light_flicker_emissive_scale(none, t, 5) == 1.0f);
            CHECK(same_bits(light_flicker_emissive_scale(full, t, 5), s));
            CHECK(light_flicker_emissive_scale(half, t, 5) == doctest::Approx(1.0f + 0.5f * (s - 1.0f)).epsilon(1e-6));
        }
    }
}

TEST_CASE("light flicker: the emissive scale is exactly 1 when disabled or depth 0, and always finite in range") {
    for (u32 style = 0; style < 3; ++style) {
        LightFlicker3D dis = make(style, 0.8f);
        dis.emissive_follow = 1.0f;
        dis.enabled = 0;
        LightFlicker3D flat = make(style, 0.0f);
        flat.emissive_follow = 1.0f;
        LightFlicker3D wild = make(style, 0.6f);
        wild.emissive_follow = 7.0f;  // clamps to 1
        LightFlicker3D nan = make(style, 0.6f);
        nan.emissive_follow = kNaN;  // counts as 0
        LightFlicker3D neg = make(style, 0.6f);
        neg.emissive_follow = -2.0f;  // clamps to 0
        for (int i = 0; i < 100; ++i) {
            const f64 t = 0.41 * i;
            CHECK(light_flicker_emissive_scale(dis, t, 3) == 1.0f);
            CHECK(light_flicker_emissive_scale(flat, t, 3) == 1.0f);
            CHECK(light_flicker_emissive_scale(nan, t, 3) == 1.0f);
            CHECK(light_flicker_emissive_scale(neg, t, 3) == 1.0f);
            const f32 w = light_flicker_emissive_scale(wild, t, 3);
            CHECK(std::isfinite(w));
            CHECK(w >= 0.4f - 1e-6f);
            CHECK(w <= 1.0f);
            CHECK(same_bits(w, light_flicker_scale(wild, t, 3)));
        }
        CHECK(std::isfinite(light_flicker_emissive_scale(wild, kInf, 3)));
        CHECK(std::isfinite(light_flicker_emissive_scale(wild, 1.0e15, 3)));
    }
}

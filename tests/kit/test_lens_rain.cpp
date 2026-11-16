// LensRain3D (M-LENSRAIN 42d): the component defaults and the integer kit_lens_rain system. wet_q rises toward the first
// Weather's intensity under rain or storm (about 4 s / wet_rate), dries otherwise (about 40 s / wet_rate), is integer-exact
// on the fixed tick, is left alone without a Weather, clamps to 2^24, survives a save and replays to the same state.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <limits>

#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/lens_rain.h"
#include "mce/kit/weather.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
constexpr u32 kMax = 1u << 24;
constexpr u32 kUp = kMax / 240;    // 4 s at 60 Hz
constexpr u32 kDown = kMax / 2400;  // 40 s at 60 Hz

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }

struct Rig {
    World world;
    u64 t = 0;
    explicit Rig(bool with_weather, bool with_rain, WeatherKind kind = WeatherKind::Rain, f32 wet_rate = 1.0f) {
        world.register_components<SaveKey>();
        install_kit3d(world, nullptr);
        install_weather(world, nullptr);
        world.spawn_with(SaveKey{3}, LocalTransform{});  // an unrelated entity
        if (with_weather) {
            Weather w;
            w.target = kind;
            w.transition_seconds = 1.0f;
            world.spawn_with(SaveKey{1}, w);
        }
        if (with_rain) {
            LensRain3D lr;
            lr.wet_rate = wet_rate;
            world.spawn_with(SaveKey{2}, lr);
        }
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) world.tick(kDt, t++);
    }
    u32 wet() {
        u32 q = 0;
        bool first = true;
        world.query<const LensRain3D&>().each([&](const LensRain3D& c) {
            if (first) q = c.wet_q;
            first = false;
        });
        return q;
    }
    void set_wet(u32 q) {
        world.query<LensRain3D&>().each([&](LensRain3D& c) { c.wet_q = q; });
    }
    void set_target(WeatherKind k) {
        world.query<Weather&>().each([&](Weather& w) { w.target = k; });
    }
    Weather weather() {
        Weather w;
        world.query<const Weather&>().each([&](const Weather& c) { w = c; });
        return w;
    }
};

}  // namespace

TEST_CASE("lens rain: component defaults and zeroed pad") {
    const LensRain3D c;
    CHECK(c.enabled == 1u);
    CHECK(same_bits(c.density, 1.0f));
    CHECK(same_bits(c.size, 1.0f));
    CHECK(same_bits(c.refraction, 1.0f));
    CHECK(same_bits(c.blur, 0.35f));
    CHECK(same_bits(c.trail, 0.5f));
    CHECK(same_bits(c.fall_speed, 0.25f));
    CHECK(c.seed == 1u);
    CHECK(same_bits(c.wet_rate, 1.0f));
    CHECK(c.wet_q == 0u);
    CHECK((c.pad_[0] | c.pad_[1] | c.pad_[2]) == 0u);
}

TEST_CASE("lens rain: wet_q rises toward the Weather intensity, integer-exact") {
    Rig r(true, true);
    u32 q = 0;
    for (u32 i = 0; i < 600; ++i) {
        r.run(1);
        const Weather w = r.weather();
        if (w.current == WeatherKind::Rain && w.fade_den > 0) {
            const u32 target = static_cast<u32>(static_cast<u64>(kMax) * w.fade / w.fade_den);
            if (q < target) q = (q + kUp < target) ? q + kUp : target;
        }
        REQUIRE(r.wet() == q);
        REQUIRE(r.wet() <= kMax);
    }
    CHECK(r.wet() == kMax);  // 10 s of rain, 4 s to wet
}

TEST_CASE("lens rain: it stops at an intermediate intensity and a storm wets too") {
    Rig r(true, true, WeatherKind::Storm);
    r.run(30);  // half way through the 1 s fade in
    const Weather w = r.weather();
    REQUIRE(w.current == WeatherKind::Storm);
    const u32 target = static_cast<u32>(static_cast<u64>(kMax) * w.fade / w.fade_den);
    CHECK(r.wet() == 30 * kUp);  // the 4 s rate is the limit here, the target (half) is not reached yet
    CHECK(r.wet() <= target);
    r.run(15);  // still rising at the 4 s rate
    CHECK(r.wet() == 45 * kUp);
}

TEST_CASE("lens rain: it dries in about 40 s once the rain stops, and snow or overcast do not wet") {
    Rig r(true, true);
    r.run(600);
    REQUIRE(r.wet() == kMax);
    r.set_target(WeatherKind::Clear);
    r.run(61);  // fade out (60 ticks); current is Clear after it
    REQUIRE(r.weather().current == WeatherKind::Clear);
    const u32 before = r.wet();
    r.run(1);
    CHECK(before - r.wet() == kDown);
    r.run(2500);
    CHECK(r.wet() == 0u);
    r.set_target(WeatherKind::Snow);
    r.run(600);
    CHECK(r.wet() == 0u);
    r.set_target(WeatherKind::Overcast);
    r.run(600);
    CHECK(r.wet() == 0u);
}

TEST_CASE("lens rain: wet_rate scales both rates") {
    Rig slow(true, true, WeatherKind::Rain, 0.5f);
    Rig norm(true, true, WeatherKind::Rain, 1.0f);
    Rig fast(true, true, WeatherKind::Rain, 4.0f);
    slow.run(100);
    norm.run(100);
    fast.run(100);
    CHECK(slow.wet() < norm.wet());
    CHECK(norm.wet() < fast.wet());
    // full rain: 4 s at rate 1 is 240 ticks, 8 s at 0.5, 1 s at 4
    Rig a(true, true, WeatherKind::Rain, 4.0f);
    a.run(61 + 10);
    CHECK(a.wet() == kMax);
    Rig b(true, true, WeatherKind::Rain, 0.5f);
    b.run(60 + 400);
    CHECK(b.wet() < kMax);
    b.run(200);
    CHECK(b.wet() == kMax);
    // dry rate: 40 s at 1, 400 s at 0.1
    Rig d(true, true, WeatherKind::Clear, 0.1f);
    d.set_wet(kMax);
    d.run(1);
    CHECK(kMax - d.wet() == kMax / 24000);
}

TEST_CASE("lens rain: wet_rate clamps, NaN counts as 1") {
    Rig lo(true, true, WeatherKind::Clear, 0.0f);
    Rig mid(true, true, WeatherKind::Clear, 0.1f);
    lo.set_wet(kMax);
    mid.set_wet(kMax);
    lo.run(10);
    mid.run(10);
    CHECK(lo.wet() == mid.wet());
    Rig hi(true, true, WeatherKind::Clear, 1000.0f);
    Rig four(true, true, WeatherKind::Clear, 4.0f);
    hi.set_wet(kMax);
    four.set_wet(kMax);
    hi.run(10);
    four.run(10);
    CHECK(hi.wet() == four.wet());
    Rig nan(true, true, WeatherKind::Clear, std::numeric_limits<f32>::quiet_NaN());
    Rig one(true, true, WeatherKind::Clear, 1.0f);
    nan.set_wet(kMax);
    one.set_wet(kMax);
    nan.run(10);
    one.run(10);
    CHECK(nan.wet() == one.wet());
}

TEST_CASE("lens rain: wet_q above 2^24 clamps, and above the intensity it falls back to it") {
    Rig r(true, true);
    r.set_wet(0xFFFFFFFFu);
    r.run(1);
    CHECK(r.wet() <= kMax);
    Rig s(true, true);
    s.run(120);
    s.set_wet(kMax);
    s.set_target(WeatherKind::Overcast);  // fades out: current stays Rain, the intensity falls
    s.run(30);
    const Weather w = s.weather();
    REQUIRE(w.current == WeatherKind::Rain);
    CHECK(s.wet() >= static_cast<u32>(static_cast<u64>(kMax) * w.fade / w.fade_den));
    CHECK(s.wet() < kMax);
}

TEST_CASE("lens rain: without a Weather wet_q is left alone") {
    Rig r(false, true);
    r.set_wet(1234567);
    r.run(300);
    CHECK(r.wet() == 1234567u);
}

TEST_CASE("lens rain: only the first component in ECS order is advanced") {
    Rig r(true, true);
    const Entity second = r.world.spawn_with(SaveKey{4}, LensRain3D{});
    r.run(300);
    CHECK(r.wet() > 0u);
    CHECK(r.world.try_get<LensRain3D>(second)->wet_q == 0u);
}

TEST_CASE("lens rain: the component does not change the rest of the world, and the same ticks replay to the same state") {
    Rig without(true, false);
    Rig with(true, true);
    without.run(900);
    with.run(900);
    const Weather a = without.weather();
    const Weather b = with.weather();
    CHECK(a.current == b.current);
    CHECK(a.fade == b.fade);
    CHECK(a.wet_q == b.wet_q);
    CHECK(same_bits(a.intensity, b.intensity));
    CHECK(same_bits(a.wetness, b.wetness));
    for (u32 i = 0; i < 15; ++i) CHECK(same_bits(a.env_applied[i], b.env_applied[i]));
    Rig again(true, true);
    again.run(900);
    CHECK(again.wet() == with.wet());
    CHECK(again.world.state_hash() == with.world.state_hash());
    CHECK(with.world.state_hash() != without.world.state_hash());  // wet_q is hashed
}

TEST_CASE("lens rain: save and load mid-rain resumes with the same wet_q and state hash") {
    Array<byte> bytes;
    u64 hash_a = 0;
    u32 wet_a = 0;
    {
        Rig a(true, true);
        a.run(200);
        REQUIRE(a.wet() > 0u);
        REQUIRE(a.wet() < kMax);
        save::write_save(a.world, {}, bytes);
        a.run(300);
        hash_a = a.world.state_hash();
        wet_a = a.wet();
    }
    Rig b(true, true);
    b.run(77);
    REQUIRE(save::read_save(b.world, bytes.span()));
    b.t = 200;
    b.run(300);
    CHECK(b.wet() == wet_a);
    CHECK(b.world.state_hash() == hash_a);
}

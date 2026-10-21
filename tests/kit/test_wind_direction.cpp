// WindDirection (M-WINDDIR 8a): kit_wind_direction keeps the heading as an integer angle_q (1/65536 degree) advanced by
// a fixed-point drift with a carried remainder; drift 0 follows the authored degrees; the state is saved.

#include <doctest/doctest.h>

#include <cfloat>
#include <cmath>
#include <limits>

#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/weather.h"
#include "mce/kit/wind_direction.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr u32 kFull = 360u * 65536u;
// At 60 Hz a drift of 3600 deg/min is exactly 1 degree (65536 quanta) per tick, 1800 is 32768.

struct Rig {
    World world;
    f32 dt;
    u64 t = 0;
    explicit Rig(f32 degrees = 0.0f, f32 drift = 0.0f, bool with = true, f32 step = 1.0f / 60.0f) : dt(step) {
        world.register_components<SaveKey>();
        install_kit3d(world, nullptr);
        install_weather(world, nullptr);
        if (with) {
            WindDirection w;
            w.degrees = degrees;
            w.drift_degrees_per_minute = drift;
            world.spawn_with(SaveKey{1}, w);
        }
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) world.tick(dt, t++);
    }
    void set(f32 degrees, f32 drift) {
        world.query<WindDirection&>().each([&](WindDirection& w) {
            w.degrees = degrees;
            w.drift_degrees_per_minute = drift;
        });
    }
    WindDirection wind() {
        WindDirection s;
        bool first = true;
        world.query<const WindDirection&>().each([&](const WindDirection& c) {
            if (first) s = c;
            first = false;
        });
        return s;
    }
};

}  // namespace

TEST_CASE("wind_direction: defaults") {
    const WindDirection w;
    CHECK(w.degrees == 0.0f);
    CHECK(w.drift_degrees_per_minute == 0.0f);
    CHECK(w.angle_q == 0);
    CHECK(w.frac == 0);
    CHECK(w.seeded == 0);
    CHECK(w.pad_ == 0);
    const math::vec2 v = wind_direction_vector(w);
    CHECK(v.x == doctest::Approx(1.0).epsilon(1e-6));
    CHECK(v.y == doctest::Approx(0.0).epsilon(1e-6));
}

TEST_CASE("wind_direction: degrees convert to angle_q and the vector follows (90 is +z)") {
    Rig r(90.0f);
    r.run(1);
    CHECK(r.wind().angle_q == 90u * 65536u);
    const math::vec2 v = wind_direction_vector(r.wind());
    CHECK(std::abs(v.x) < 1e-6f);
    CHECK(std::abs(v.y - 1.0f) < 1e-6f);
    r.set(-90.0f, 0.0f);  // editing takes effect on the next tick, wrapped
    r.run(1);
    CHECK(r.wind().angle_q == 270u * 65536u);
    r.set(720.0f + 45.0f, 0.0f);
    r.run(1);
    CHECK(r.wind().angle_q == 45u * 65536u);
    r.set(360.0f, 0.0f);
    r.run(1);
    CHECK(r.wind().angle_q == 0);
}

TEST_CASE("wind_direction: drift advances exactly, 60 deg/min for 60 s is 60 degrees") {
    Rig r(0.0f, 60.0f);
    r.run(3600);  // 60 s at 1/60
    const i64 d = static_cast<i64>(r.wind().angle_q) - 60 * 65536;
    CHECK(std::abs(d) <= 1);
    Rig e(0.0f, 1800.0f);  // exactly half a degree per tick
    e.run(1);
    CHECK(e.wind().angle_q == 32768);
    CHECK(e.wind().frac == 0);
    e.run(3);
    CHECK(e.wind().angle_q == 2u * 65536u);
}

TEST_CASE("wind_direction: a drift starts from the authored degrees and 0 drift drops back to them") {
    Rig r(90.0f, 3600.0f);
    r.run(4);
    CHECK(r.wind().angle_q == 94u * 65536u);
    CHECK(r.wind().seeded == 1);
    r.set(10.0f, 0.0f);
    r.run(1);
    CHECK(r.wind().angle_q == 10u * 65536u);
    CHECK(r.wind().seeded == 0);
    CHECK(r.wind().frac == 0);
}

TEST_CASE("wind_direction: 360 degrees of drift returns to the start angle exactly") {
    Rig full(30.0f, 3600.0f);
    full.run(360);  // 360 degrees at 1 degree per tick
    CHECK(full.wind().angle_q == 30u * 65536u);
    Rig neg(30.0f, -3600.0f);
    neg.run(360);
    CHECK(neg.wind().angle_q == 30u * 65536u);
    Rig slow(30.0f, 60.0f);  // 1 degree per second, not exact in binary: the remainder is carried
    slow.run(360 * 60);
    CHECK(slow.wind().angle_q == 30u * 65536u);
    Rig slowneg(30.0f, -60.0f);
    slowneg.run(360 * 60);
    CHECK(slowneg.wind().angle_q == 30u * 65536u);
    // A rate that is not exact at 1/60: the remainder is carried, so a whole number of turns stays within one quantum.
    Rig h(0.0f, 360.0f);
    h.run(3600 * 10);  // 10 turns
    const u32 q = h.wind().angle_q;
    CHECK((q <= 1 || q >= kFull - 1));  // within one quantum
}

TEST_CASE("wind_direction: negative drift wraps below zero") {
    Rig r(0.0f, -1800.0f);
    r.run(1);
    CHECK(r.wind().angle_q == kFull - 32768);
    r.run(3);
    CHECK(r.wind().angle_q == kFull - 2u * 65536u);
    CHECK(r.wind().angle_q < kFull);
    Rig s(0.0f, -1.0f);   // a tiny negative step: floor carries the remainder
    s.run(60 * 60 * 60);  // one hour: -60 degrees
    const i64 d = static_cast<i64>(s.wind().angle_q) - static_cast<i64>(kFull - 60 * 65536);
    CHECK(std::abs(d) <= 1);
}

TEST_CASE("wind_direction: NaN, Inf and huge fields are clamped") {
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 inf = std::numeric_limits<f32>::infinity();
    {
        Rig r(nan, nan);
        r.run(5);
        CHECK(r.wind().angle_q == 0);
        CHECK(r.wind().seeded == 0);
    }
    for (f32 deg : {inf, -inf, FLT_MAX, -FLT_MAX, 1e30f}) {
        Rig r(deg, 0.0f);
        r.run(3);
        CHECK(r.wind().angle_q < kFull);
    }
    for (f32 drift : {inf, FLT_MAX, 1e30f}) {  // clamped to 3600 deg/min = 1 degree per tick
        Rig r(0.0f, drift);
        r.run(1);
        CHECK(r.wind().angle_q == 65536u);
    }
    {
        Rig r(0.0f, -inf);
        r.run(1);
        CHECK(r.wind().angle_q == kFull - 65536u);
    }
    {
        Rig r(nan, 60.0f);  // a NaN start is 0
        r.run(10);
        CHECK(r.wind().angle_q < kFull);
        CHECK(r.wind().angle_q > 0);
    }
}

TEST_CASE("wind_direction: save and load mid-drift resumes with the same state hash") {
    Array<byte> bytes;
    u64 hash_a = 0;
    {
        Rig a(15.0f, 77.0f);
        a.run(301);
        REQUIRE(a.wind().angle_q > 0);
        REQUIRE(a.wind().frac != 0);
        save::write_save(a.world, {}, bytes);
        a.run(200);
        hash_a = a.world.state_hash();
    }
    Rig b(15.0f, 77.0f);
    b.run(41);
    REQUIRE(save::read_save(b.world, bytes.span()));
    b.t = 301;
    b.run(200);
    CHECK(b.world.state_hash() == hash_a);
    Rig c(15.0f, 77.0f);
    c.run(501);
    CHECK(c.wind().angle_q == b.wind().angle_q);
    CHECK(c.wind().frac == b.wind().frac);
}

TEST_CASE("wind_direction: a scene without the component keeps its hash") {
    Rig none(0.0f, 0.0f, false);
    Rig also(0.0f, 0.0f, false);
    none.run(100);
    also.run(100);
    CHECK(none.world.state_hash() == also.world.state_hash());
    CHECK(none.wind().angle_q == 0);
}

TEST_CASE("wind_direction: a second WindDirection is ignored") {
    Rig r(10.0f, 60.0f);
    WindDirection other;
    other.degrees = 200.0f;
    other.drift_degrees_per_minute = 600.0f;
    const Entity second = r.world.spawn_with(SaveKey{2}, other);
    r.run(120);
    CHECK(r.wind().angle_q > 10u * 65536u);
    const WindDirection* s = r.world.try_get<WindDirection>(second);
    REQUIRE(s != nullptr);
    CHECK(s->angle_q == 0);
    CHECK(s->frac == 0);
    CHECK(s->seeded == 0);
}

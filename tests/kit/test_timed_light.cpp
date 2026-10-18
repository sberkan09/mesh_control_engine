// Timed lights (M-DAYNIGHT 3): off at noon, ramp 0.5 ten minutes after on_hour (20 min fade), fully on at 19:30,
// on across midnight, off again after off_hour + fade, the emissive follows, no clock => untouched, the base
// intensity does not drift, a stable state hash and one event per crossing.

#include <doctest/doctest.h>

#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/timed_light.h"
#include "mce/kit/world_clock.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

// A 60 s day: one tick is 0.4 game minutes, 150 ticks an hour.
struct Lamp {
    World w;
    u64 t = 0;
    Entity e;
    Entity ce;
    Array<f32> dusks, dawns;
    explicit Lamp(bool with_clock = true, f32 hour = 12.0f) {
        w.register_components<SaveKey>();
        install_timed_light(w, nullptr);
        PointLight3D pl;
        pl.intensity = 4.0f;
        e = w.spawn_with(SaveKey{1}, TimedLight3D{}, pl, MeshRef{});
        if (with_clock) {
            WorldClock c;
            c.hour = hour;
            c.day_length_seconds = 60.0f;
            ce = w.spawn_with(SaveKey{2}, c);
        }
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) {
            w.tick(kDt, t++);
            auto ev = EventAccess(w);
            for (const DuskStarted& d : ev.read<DuskStarted>()) dusks.push_back(d.hour);
            for (const DawnStarted& d : ev.read<DawnStarted>()) dawns.push_back(d.hour);
        }
    }
    PointLight3D& light() { return *w.try_get_mut<PointLight3D>(e); }
    MeshRef& mesh() { return *w.try_get_mut<MeshRef>(e); }
    TimedLight3D& tl() { return *w.try_get_mut<TimedLight3D>(e); }
};

}  // namespace

TEST_CASE("timed_light: off at noon, the light costs nothing") {
    Lamp l(true, 12.0f);
    l.run(10);
    CHECK(l.light().enabled == 0);
    CHECK(l.light().intensity == 0.0f);
    CHECK(l.mesh().emissive_tint.x == 0.0f);
    CHECK(l.tl().light_scale == 4.0f);
}

TEST_CASE("timed_light: ramp 0.5 ten minutes after on_hour, fully on at 19:30") {
    Lamp l(true, 18.5f);  // one tick = 0.4 game minutes; the first tick starts at 18:30 and advances
    l.run(25);            // 18:40, half way through the 20 minute fade
    CHECK(l.light().enabled == 1);
    CHECK(l.light().intensity == doctest::Approx(2.0f).epsilon(1e-4));
    CHECK(l.mesh().emissive_tint.x == doctest::Approx(0.5f).epsilon(1e-4));
    CHECK(l.mesh().emissive_tint.z == l.mesh().emissive_tint.x);
    Lamp m(true, 19.0f);
    m.run(75);  // 19:30
    CHECK(m.light().intensity == 4.0f);
    CHECK(m.light().enabled == 1);
    CHECK(m.mesh().emissive_tint.y == 1.0f);
}

TEST_CASE("timed_light: on across midnight, off again after off_hour + fade") {
    Lamp l(true, 23.0f);
    l.run(300);  // 01:00
    CHECK(l.light().intensity == 4.0f);
    Lamp d(true, 5.0f);
    d.run(75);  // 05:30
    CHECK(d.light().intensity == 4.0f);
    d.run(100);  // 06:10, half way down
    CHECK(d.light().intensity == doctest::Approx(2.0f).epsilon(1e-4));
    d.run(50);  // 06:30, past 06:20
    CHECK(d.light().enabled == 0);
    CHECK(d.light().intensity == 0.0f);
    CHECK(d.mesh().emissive_tint.x == 0.0f);
}

TEST_CASE("timed_light: custom emissive range and a spot light share the ramp") {
    Lamp l(true, 20.0f);
    l.tl().emissive_on = 5.0f;
    l.tl().emissive_off = 1.0f;
    SpotLight3D sp;
    sp.intensity = 8.0f;
    l.w.add(l.e, sp);
    l.run(1);
    CHECK(l.mesh().emissive_tint.x == 5.0f);
    CHECK(l.w.try_get<SpotLight3D>(l.e)->intensity == 8.0f);
    CHECK(l.tl().spot_scale == 8.0f);
    Lamp n(true, 12.0f);
    n.tl().emissive_on = 5.0f;
    n.tl().emissive_off = 1.0f;
    n.run(1);
    CHECK(n.mesh().emissive_tint.x == 1.0f);
}

TEST_CASE("timed_light: no clock leaves the lamp untouched") {
    Lamp l(false);
    l.run(50);
    CHECK(l.light().enabled == 1);
    CHECK(l.light().intensity == 4.0f);
    CHECK(l.mesh().emissive_tint.x == 1.0f);
    CHECK(l.dusks.size() == 0);
    CHECK(l.dawns.size() == 0);
}

TEST_CASE("timed_light: the base intensity does not drift over 1000 ticks") {
    Lamp l(true, 18.0f);
    l.run(1000);  // dusk, the night and part of the next morning ramp; ~ 6.7 h
    CHECK(l.tl().light_scale == 4.0f);
    l.w.try_get_mut<WorldClock>(l.ce)->hour = 22.0f;
    l.run(1);
    CHECK(l.light().intensity == 4.0f);
}

TEST_CASE("timed_light: the same run twice has the same state hash") {
    auto run = [](f32 start) {
        Lamp l(true, start);
        l.run(1000);
        return l.w.state_hash();
    };
    CHECK(run(18.0f) == run(18.0f));
    CHECK(run(18.0f) != run(12.0f));
}

TEST_CASE("timed_light: DuskStarted and DawnStarted fire once per crossing") {
    Lamp l(true, 18.0f);
    l.run(150);  // 18:00 -> 19:00
    REQUIRE(l.dusks.size() == 1);
    CHECK(l.dusks[0] == 18.5f);
    CHECK(l.dawns.size() == 0);
    l.run(3600);  // a whole day later: dawn at 6:00 once and dusk once more
    CHECK(l.dusks.size() == 2);
    REQUIRE(l.dawns.size() == 1);
    CHECK(l.dawns[0] == 6.0f);
    // Setting the clock is a jump, not a crossing.
    l.w.try_get_mut<WorldClock>(l.ce)->hour = 3.0f;
    l.run(5);
    l.w.try_get_mut<WorldClock>(l.ce)->hour = 17.0f;
    l.run(5);
    CHECK(l.dusks.size() == 2);
    CHECK(l.dawns.size() == 1);
}

// World clock (M-DAYNIGHT 1): the hour after N ticks is an exact value, a 60 s day is at 06:00 after 15 s,
// HourPassed and DayPassed fire once per boundary, pause and time_scale stop it, Environment3D.time_of_day
// follows, a save mid-day resumes and the same run twice has the same state hash.

#include <doctest/doctest.h>

#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/world_clock.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;  // the engine's fixed rate

struct Clk {
    World w;
    u64 t = 0;
    Entity e;
    Array<u32> hours;  // each HourPassed, with the tick it came on in `hour_ticks`
    Array<u64> hour_ticks;
    Array<u32> days;
    Array<u64> day_ticks;
    explicit Clk(f32 hour = 8.0f, f32 day_len = 1200.0f, bool with_clock = true) {
        w.register_components<SaveKey>();
        install_world_clock(w, nullptr);
        if (with_clock) {
            WorldClock c;
            c.hour = hour;
            c.day_length_seconds = day_len;
            e = w.spawn_with(SaveKey{1}, c);
        } else {
            e = w.spawn_with(SaveKey{1});
        }
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) {
            w.tick(kDt, t++);
            auto ev = EventAccess(w);
            for (const HourPassed& h : ev.read<HourPassed>()) {
                hours.push_back(h.hour);
                hour_ticks.push_back(t);
            }
            for (const DayPassed& d : ev.read<DayPassed>()) {
                days.push_back(d.day);
                day_ticks.push_back(t);
            }
        }
    }
    WorldClock& clk() { return *w.try_get_mut<WorldClock>(e); }
    f32 tod() { return w.try_resource<Environment3D>()->time_of_day; }
};

}  // namespace

TEST_CASE("world_clock: the hour after N ticks is exact") {
    Clk c;  // 8:00, a 1200 s day = 72000 ticks
    c.run(3000);  // 50 s = one hour
    CHECK(c.clk().hour == 9.0f);
    c.run(1500);  // half an hour more
    CHECK(c.clk().hour == 9.5f);
    c.run(600);   // 10 s = 12 minutes
    CHECK(c.clk().hour == doctest::Approx(9.7f).epsilon(1e-6));
    CHECK(c.clk().day == 0);
}

TEST_CASE("world_clock: a 60 s day is at 06:00 after 15 s") {
    Clk c(0.0f, 60.0f);
    c.run(900);
    CHECK(c.clk().hour == 6.0f);
    c.run(900);
    CHECK(c.clk().hour == 12.0f);
    c.run(1800);  // 24 h: wrapped to midnight
    CHECK(c.clk().hour == 0.0f);
    CHECK(c.clk().day == 1);
}

TEST_CASE("world_clock: HourPassed once per boundary, DayPassed at the wrap") {
    Clk c(22.5f, 60.0f);  // 150 ticks per hour
    c.run(3600);          // one whole day
    REQUIRE(c.hours.size() == 24);
    CHECK(c.hours[0] == 23);
    CHECK(c.hour_ticks[0] == 75);  // tick 1 starts at 22.5 and advances; 75 ticks reach 23:00
    CHECK(c.hours[1] == 0);
    CHECK(c.hour_ticks[1] == 75 + 150);
    for (u32 i = 0; i < c.hours.size(); ++i) CHECK(c.hours[i] == (23 + i) % 24);
    REQUIRE(c.days.size() == 1);
    CHECK(c.days[0] == 1);
    CHECK(c.day_ticks[0] == c.hour_ticks[1]);  // midnight is both
    c.run(3600);
    CHECK(c.days.size() == 2);
    CHECK(c.days[1] == 2);
    CHECK(c.hours.size() == 48);
}

TEST_CASE("world_clock: pause, time_scale 0 and negative scale stop it, a scale of 2 doubles it") {
    Clk c(6.0f, 60.0f);
    c.run(150);
    CHECK(c.clk().hour == 7.0f);
    c.clk().paused = 1;
    c.run(300);
    CHECK(c.clk().hour == 7.0f);
    c.clk().paused = 0;
    c.clk().time_scale = 0.0f;
    c.run(300);
    CHECK(c.clk().hour == 7.0f);
    c.clk().time_scale = -3.0f;
    c.run(300);
    CHECK(c.clk().hour == 7.0f);
    CHECK(c.hours.size() == 1);
    c.clk().time_scale = 2.0f;
    c.run(75);  // 150 ticks' worth
    CHECK(c.clk().hour == 8.0f);
    c.clk().time_scale = 0.5f;
    c.run(300);  // 150 ticks' worth
    CHECK(c.clk().hour == 9.0f);
}

TEST_CASE("world_clock: Environment3D.time_of_day follows, and drive_environment 0 leaves it alone") {
    Clk c(6.0f, 60.0f);
    CHECK(c.tod() == -1.0f);
    c.run(150);
    CHECK(c.tod() == 7.0f);
    c.run(150);
    CHECK(c.tod() == 8.0f);
    c.clk().drive_environment = 0;
    c.run(150);
    CHECK(c.clk().hour == 9.0f);
    CHECK(c.tod() == 8.0f);
}

TEST_CASE("world_clock: writing hour sets the time of day, a changed day length keeps it") {
    Clk c(6.0f, 60.0f);
    c.run(10);
    c.clk().hour = 18.0f;
    c.run(150);
    CHECK(c.clk().hour == 19.0f);
    c.clk().day_length_seconds = 120.0f;  // same hour, half the speed
    c.run(150);
    CHECK(c.clk().hour == 19.5f);
}

TEST_CASE("world_clock: a save mid-day resumes where it was") {
    Array<byte> bytes;
    f32 expected = 0;
    {
        Clk a(8.0f, 60.0f);
        a.run(1234);
        save::write_save(a.w, {}, bytes);
        a.run(766);
        expected = a.clk().hour;
        CHECK(a.clk().day == 0);
    }
    Clk b(0.0f, 1200.0f);  // a different base clock: the load overrides it
    auto st = save::read_save(b.w, bytes.span());
    REQUIRE(st);
    CHECK(b.clk().day_length_seconds == 60.0f);
    b.t = 1234;
    b.run(766);
    CHECK(b.clk().hour == expected);
    CHECK(b.clk().hour == doctest::Approx(8.0f + 24.0f * 2000.0f / 3600.0f).epsilon(1e-6));
}

TEST_CASE("world_clock: the same run twice has the same state hash, and no clock changes nothing") {
    auto run = [](bool with_clock) {
        Clk c(8.0f, 60.0f, with_clock);
        c.run(500);
        if (with_clock) c.clk().time_scale = 3.0f;
        c.run(500);
        return c.w.state_hash();
    };
    CHECK(run(true) == run(true));
    CHECK(run(true) != run(false));
    // A scene without a WorldClock: the environment is untouched, and the hash is the same as before install.
    Clk n(8.0f, 60.0f, false);
    n.run(100);
    CHECK(n.tod() == -1.0f);
    World plain;
    plain.register_components<SaveKey>();
    install_kit3d(plain, nullptr);
    plain.spawn_with(SaveKey{1});
    for (u64 i = 0; i < 100; ++i) plain.tick(kDt, i);
    CHECK(plain.state_hash() == n.w.state_hash());
}

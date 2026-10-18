// Day cycle (M-DAYNIGHT 2): the default preset at 06:00 and 18:00, exact key hits, linear blending, the wrap
// between the last key and the first across midnight, unsorted keys, no write when disabled or without a clock,
// a scene without a DayCycle unchanged, and a stable replay hash with a WorldClock.

#include <doctest/doctest.h>

#include "mce/ecs/save.h"
#include "mce/ecs/world.h"
#include "mce/kit/day_cycle.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/world_clock.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

struct Cyc {
    World w;
    u64 t = 0;
    Entity e;
    Entity ce;
    explicit Cyc(bool with_clock = true, f32 hour = 0.0f, f32 day_len = 60.0f, bool with_cycle = true) {
        w.register_components<SaveKey>();
        install_day_cycle(w, nullptr);
        e = with_cycle ? w.spawn_with(SaveKey{1}, DayCycle{}) : w.spawn_with(SaveKey{1});
        if (with_clock) {
            WorldClock c;
            c.hour = hour;
            c.day_length_seconds = day_len;
            ce = w.spawn_with(SaveKey{2}, c);
        }
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    Environment3D& env() { return *w.try_resource<Environment3D>(); }
    DayCycle& cyc() { return *w.try_get_mut<DayCycle>(e); }
};

}  // namespace

TEST_CASE("day_cycle: the default preset at 06:00 and 18:00 is exactly the dawn and dusk keys") {
    Cyc c(true, 0.0f, 60.0f);  // 60 s day: 06:00 after 900 ticks
    c.run(900);
    REQUIRE(c.env().time_of_day == 6.0f);
    CHECK(c.env().sun_color.x == 1.0f);
    CHECK(c.env().sun_color.y == 0.6f);
    CHECK(c.env().sun_color.z == 0.35f);
    CHECK(c.env().sun_intensity == 1.0f);
    CHECK(c.env().ambient_sky.x == 0.35f);
    CHECK(c.env().ambient_ground.x == 0.15f);
    CHECK(c.env().fog_color.x == 0.75f);
    CHECK(c.env().fog_density == 0.006f);
    CHECK(c.env().exposure == 1.0f);
    c.run(1800);  // 12 h later: 18:00
    REQUIRE(c.env().time_of_day == 18.0f);
    CHECK(c.env().sun_color.x == 1.0f);
    CHECK(c.env().sun_color.y == 0.5f);
    CHECK(c.env().sun_color.z == 0.25f);
    CHECK(c.env().sun_intensity == 1.0f);
    CHECK(c.env().ambient_sky.y == 0.3f);
    CHECK(c.env().fog_color.y == 0.5f);
}

TEST_CASE("day_cycle: noon is the stock environment, 09:00 is half way from dawn") {
    Cyc c(true, 0.0f, 60.0f);
    c.run(1800);  // 12:00
    CHECK(c.env().sun_intensity == 2.0f);
    CHECK(c.env().sun_color.y == 0.96f);
    CHECK(c.env().ambient_sky.z == 0.55f);
    CHECK(c.env().fog_density == 0.002f);
    Cyc d(true, 0.0f, 60.0f);
    d.run(1350);  // 09:00
    REQUIRE(d.env().time_of_day == 9.0f);
    CHECK(d.env().sun_intensity == doctest::Approx(1.5f).epsilon(1e-6));
    CHECK(d.env().sun_color.y == doctest::Approx(0.78f).epsilon(1e-6));
}

TEST_CASE("day_cycle: evaluate hits keys exactly and wraps across midnight") {
    DayCycle d;
    d.key_count = 2;
    d.k0_hour = 6.0f;
    d.k0_sun_i = 1.0f;
    d.k0_exp = 1.0f;
    d.k1_hour = 18.0f;
    d.k1_sun_i = 3.0f;
    d.k1_exp = 2.0f;
    CHECK(evaluate_day_cycle(d, 6.0f).sun_intensity == 1.0f);
    CHECK(evaluate_day_cycle(d, 18.0f).sun_intensity == 3.0f);
    CHECK(evaluate_day_cycle(d, 12.0f).sun_intensity == 2.0f);
    // 18:00 -> 06:00 (next day) is 12 h: midnight is the middle.
    CHECK(evaluate_day_cycle(d, 0.0f).sun_intensity == 2.0f);
    CHECK(evaluate_day_cycle(d, 0.0f).exposure == 1.5f);
    CHECK(evaluate_day_cycle(d, 22.0f).sun_intensity == doctest::Approx(3.0 - 2.0 * 4.0 / 12.0).epsilon(1e-6));
    CHECK(evaluate_day_cycle(d, 3.0f).sun_intensity == doctest::Approx(1.0 + 2.0 * 3.0 / 12.0).epsilon(1e-6));
    CHECK(evaluate_day_cycle(d, 24.0f).sun_intensity == 2.0f);
}

TEST_CASE("day_cycle: keys in any order are sorted, one key is constant") {
    DayCycle d;
    d.key_count = 2;
    d.k0_hour = 18.0f;
    d.k0_sun_i = 3.0f;
    d.k1_hour = 6.0f;
    d.k1_sun_i = 1.0f;
    CHECK(evaluate_day_cycle(d, 6.0f).sun_intensity == 1.0f);
    CHECK(evaluate_day_cycle(d, 12.0f).sun_intensity == 2.0f);
    CHECK(evaluate_day_cycle(d, 0.0f).sun_intensity == 2.0f);
    d.key_count = 1;
    CHECK(evaluate_day_cycle(d, 3.0f).sun_intensity == 3.0f);
    CHECK(evaluate_day_cycle(d, 20.0f).sun_intensity == 3.0f);
}

TEST_CASE("day_cycle: enabled 0 and no clock write nothing") {
    Cyc a(true, 12.0f, 60.0f);
    a.cyc().enabled = 0;
    const f32 base_i = a.env().sun_intensity;
    a.run(100);
    CHECK(a.env().time_of_day > 0.0f);
    CHECK(a.env().sun_intensity == base_i);
    CHECK(a.env().exposure == 1.0f);
    Cyc b(false);  // a cycle but no clock: time_of_day stays -1
    const f32 sky_x = b.env().ambient_sky.x;
    b.run(100);
    CHECK(b.env().time_of_day == -1.0f);
    CHECK(b.env().sun_intensity == base_i);
    CHECK(b.env().ambient_sky.x == sky_x);
    CHECK(b.env().fog_density == 0.0f);
}

TEST_CASE("day_cycle: a scene without a DayCycle is unchanged, with a clock too") {
    Cyc a(true, 6.0f, 60.0f, false);
    a.run(300);
    CHECK(a.env().time_of_day == 8.0f);
    CHECK(a.env().sun_intensity == 2.0f);
    CHECK(a.env().exposure == 1.0f);
    CHECK(a.env().fog_density == 0.0f);
    World plain;
    plain.register_components<SaveKey>();
    install_world_clock(plain, nullptr);
    plain.spawn_with(SaveKey{1});
    WorldClock c;
    c.hour = 6.0f;
    c.day_length_seconds = 60.0f;
    plain.spawn_with(SaveKey{2}, c);
    for (u64 i = 0; i < 300; ++i) plain.tick(kDt, i);
    CHECK(plain.state_hash() == a.w.state_hash());
}

TEST_CASE("day_cycle: the same run twice has the same state hash") {
    auto run = [](f32 scale) {
        Cyc c(true, 5.0f, 60.0f);
        c.run(700);
        c.w.try_get_mut<WorldClock>(c.ce)->time_scale = scale;
        c.run(700);
        return std::pair{c.w.state_hash(), c.env().sun_intensity};
    };
    const auto a = run(3.0f);
    const auto b = run(3.0f);
    CHECK(a.first == b.first);
    CHECK(a.second == b.second);
    CHECK(run(1.0f).first != a.first);
}

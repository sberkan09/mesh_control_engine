// LightningBolt (M-LIGHTNINGBOLT 11a): the strike position is a pure function of seed, counter and camera; it uses its
// own hash stream so strike times and delays of an existing scene do not move; LightningStruck carries the place;
// ThunderDelay follows the real distance when the event has a position and is unchanged otherwise; no component (or
// enabled 0) leaves the event and the world as before; save/load mid-flash resumes; a scene without a camera works.

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <vector>

#include "mce/audio/components.h"
#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/lightning_bolt.h"
#include "mce/kit/precipitation.h"
#include "mce/kit/thunder_delay.h"
#include "mce/kit/weather.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

struct Strike {
    u64 tick;  // the tick whose Pre phase pushed it
    LightningStruck ev;
};
struct Sfx {
    u64 tick;
    f32 gain_db;
};

struct Opts {
    bool with_bolt = true;
    bool camera = true;
    math::vec3 cam_pos{};
    bool thunder = false;
    u32 seed = 5;
};

struct Rig {
    World w;
    u64 t = 0;
    Entity bolt, light, td;
    std::vector<Strike> strikes;
    std::vector<Sfx> played;
    Rig() : Rig(Opts{}) {}
    explicit Rig(const Opts& o) {
        w.register_components<SaveKey>();
        install_precipitation(w, nullptr);
        w.register_event<audio::PlaySfx>(Phase::FixedUpdate_Pre);
        if (o.camera) {
            LocalTransform lt;
            lt.position = o.cam_pos;
            w.spawn_with(lt, Camera3DComponent{});
        }
        Weather wx;
        wx.target = WeatherKind::Storm;
        wx.transition_seconds = 0.1f;
        w.spawn_with(SaveKey{2}, wx);
        Lightning l;
        l.seed = o.seed;
        l.min_gap_seconds = 2.0f;
        l.max_gap_seconds = 4.0f;
        l.flash_ticks = 8;
        light = w.spawn_with(SaveKey{3}, l);
        if (o.with_bolt) bolt = w.spawn_with(SaveKey{4}, LightningBolt{});
        if (o.thunder) {
            ThunderDelay c;
            c.clip.guid = asset::Guid{7, 9};
            td = w.spawn_with(SaveKey{1}, c);
        }
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) {
            w.tick(kDt, t);
            for (const LightningStruck& s : EventAccess(w).read<LightningStruck>()) strikes.push_back({t, s});
            for (const audio::PlaySfx& s : EventAccess(w).read<audio::PlaySfx>()) played.push_back({t, s.gain_db});
            ++t;
        }
    }
    LightningBolt& b() { return *w.try_get_mut<LightningBolt>(bolt); }
    Lightning& l() { return *w.try_get_mut<Lightning>(light); }
};

}  // namespace

TEST_CASE("lightning_bolt: defaults and an old-style event mean no position") {
    const LightningBolt c;
    CHECK(c.min_range_m == 150.0f);
    CHECK(c.max_range_m == 900.0f);
    CHECK(c.ground_y == 0.0f);
    CHECK(c.height_m == 600.0f);
    CHECK(c.width_m == 1.5f);
    CHECK(c.segments_log2 == 5);
    CHECK(c.jitter == 0.25f);
    CHECK(c.fork == 1);
    CHECK(c.enabled == 1);
    CHECK(c.light_intensity == 0.0f);
    CHECK(c.light_radius_m == 400.0f);
    CHECK(c.strike_id == 0);
    CHECK(c.active == 0);
    for (u8 b : c.pad_) CHECK(b == 0);
    const LightningStruck e{77};
    CHECK(e.thunder_delay_ticks == 77);
    CHECK(e.has_pos == 0);
    CHECK(e.distance_m == 0.0f);
    CHECK(e.pos.x == 0.0f);
    CHECK(e.pos.y == 0.0f);
    CHECK(e.pos.z == 0.0f);
}

TEST_CASE("lightning_bolt: the position is a pure function of seed, counter, authoring and camera") {
    const LightningBolt b;
    const math::vec3 o{10.0f, 3.0f, -20.0f};
    const BoltStrike a = bolt_strike_position(b, 5, 2, o);
    const BoltStrike a2 = bolt_strike_position(b, 5, 2, o);
    CHECK(a.x == a2.x);
    CHECK(a.z == a2.z);
    CHECK(a.distance_m == a2.distance_m);
    bool differs_counter = false, differs_seed = false;
    for (u32 c = 0; c < 8; ++c) {
        const BoltStrike s = bolt_strike_position(b, 5, c, o);
        const f64 dx = static_cast<f64>(s.x) - 10.0, dz = static_cast<f64>(s.z) + 20.0;
        const f64 r = std::sqrt(dx * dx + dz * dz);
        CHECK(r >= 149.99);
        CHECK(r <= 900.01);
        CHECK(s.distance_m == doctest::Approx(std::sqrt(r * r + 9.0)).epsilon(1e-5));
        if (s.x != a.x) differs_counter = true;
        if (bolt_strike_position(b, 6, c, o).x != s.x) differs_seed = true;
    }
    CHECK(differs_counter);
    CHECK(differs_seed);
    // The camera only translates the strike on the ground plane.
    const BoltStrike z0 = bolt_strike_position(b, 5, 2, {0, 0, 0});
    const BoltStrike z1 = bolt_strike_position(b, 5, 2, {1000, 0, 500});
    CHECK(z1.x - z0.x == doctest::Approx(1000.0f).epsilon(1e-4));
    CHECK(z1.z - z0.z == doctest::Approx(500.0f).epsilon(1e-4));
    CHECK(z1.distance_m == doctest::Approx(z0.distance_m).epsilon(1e-4));
}

TEST_CASE("lightning_bolt: ranges are clamped, swapped when inverted and NaN safe") {
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    LightningBolt b;
    b.min_range_m = 400.0f;
    b.max_range_m = 200.0f;  // inverted
    for (u32 c = 0; c < 16; ++c) {
        const BoltStrike s = bolt_strike_position(b, 1, c, {});
        CHECK(s.distance_m >= 199.99f);
        CHECK(s.distance_m <= 400.01f);
    }
    b.min_range_m = 0.0f;  // below Range: 10
    b.max_range_m = 1.0e9f;  // above Range: 5000
    for (u32 c = 0; c < 16; ++c) {
        const BoltStrike s = bolt_strike_position(b, 1, c, {});
        CHECK(s.distance_m >= 9.99f);
        CHECK(s.distance_m <= 5000.01f);
    }
    b.min_range_m = nan;
    b.max_range_m = nan;
    b.ground_y = nan;
    for (u32 c = 0; c < 16; ++c) {
        const BoltStrike s = bolt_strike_position(b, 1, c, {});
        CHECK(s.distance_m >= 149.99f);
        CHECK(s.distance_m <= 900.01f);
    }
    b = LightningBolt{};
    b.min_range_m = 300.0f;
    b.max_range_m = 300.0f;
    CHECK(bolt_strike_position(b, 9, 0, {}).distance_m == doctest::Approx(300.0f).epsilon(1e-5));
}

TEST_CASE("lightning_bolt: bolt_delay_ticks is max(1, ceil(d / 343 / dt))") {
    CHECK(bolt_delay_ticks(343.0f, 1.0 / 60.0) == 60);
    CHECK(bolt_delay_ticks(344.0f, 1.0 / 60.0) == 61);
    CHECK(bolt_delay_ticks(3430.0f, 1.0 / 60.0) == 600);
    CHECK(bolt_delay_ticks(0.5f, 1.0 / 60.0) == 1);
    CHECK(bolt_delay_ticks(0.0f, 1.0 / 60.0) == 1);
    CHECK(bolt_delay_ticks(500.0f, 0.0) == 1);
    CHECK(bolt_delay_ticks(std::numeric_limits<f32>::infinity(), 1.0 / 60.0) == (1u << 31));
}

TEST_CASE("lightning_bolt: a strike carries its place, a distance delay and is stored on the component") {
    Rig r({true, true, {100.0f, 4.0f, -50.0f}, false, 5});
    r.run(60 * 40);
    REQUIRE(r.strikes.size() > 5);
    for (const Strike& s : r.strikes) {
        const LightningStruck& e = s.ev;
        CHECK(e.has_pos == 1);
        const f64 dx = static_cast<f64>(e.pos.x) - 100.0, dz = static_cast<f64>(e.pos.z) + 50.0;
        const f64 range = std::sqrt(dx * dx + dz * dz);
        CHECK(range >= 149.9);
        CHECK(range <= 900.1);
        CHECK(e.pos.y == 0.0f);
        CHECK(e.distance_m == doctest::Approx(std::sqrt(range * range + 16.0)).epsilon(1e-4));
        CHECK(e.thunder_delay_ticks == bolt_delay_ticks(e.distance_m, static_cast<f64>(kDt)));
        CHECK(e.thunder_delay_ticks >= 1);
    }
    // Strike k matches the pure function (counter k) and the last one is on the component.
    for (u32 k = 0; k < r.strikes.size(); ++k) {
        const BoltStrike p = bolt_strike_position(LightningBolt{}, 5, k, {100.0f, 4.0f, -50.0f});
        CHECK(r.strikes[k].ev.pos.x == p.x);
        CHECK(r.strikes[k].ev.pos.z == p.z);
        CHECK(r.strikes[k].ev.distance_m == p.distance_m);
    }
    const LightningStruck& last = r.strikes.back().ev;
    CHECK(r.b().strike_x == last.pos.x);
    CHECK(r.b().strike_z == last.pos.z);
    CHECK(r.b().strike_dist_m == last.distance_m);
    CHECK(r.b().strike_id == r.strikes.size() - 1);
    CHECK(r.l().counter == r.strikes.size());
}

TEST_CASE("lightning_bolt: ground_y moves the strike point and the distance") {
    Rig r;
    r.b().ground_y = 40.0f;
    r.run(60 * 10);
    REQUIRE(!r.strikes.empty());
    const LightningStruck& e = r.strikes[0].ev;
    CHECK(e.pos.y == 40.0f);
    const f64 range = std::sqrt(static_cast<f64>(e.pos.x) * e.pos.x + static_cast<f64>(e.pos.z) * e.pos.z);
    CHECK(e.distance_m == doctest::Approx(std::sqrt(range * range + 1600.0)).epsilon(1e-4));
}

TEST_CASE("lightning_bolt: the primary camera is the origin; no camera means the origin (0, 0)") {
    {
        Rig r({true, true, {500.0f, 0.0f, 500.0f}, false, 5});
        r.w.spawn_with(LocalTransform{}, Camera3DComponent{});  // a second primary camera: the first in ECS order wins
        r.run(60 * 10);
        REQUIRE(!r.strikes.empty());
        const BoltStrike p = bolt_strike_position(LightningBolt{}, 5, 0, {500.0f, 0.0f, 500.0f});
        CHECK(r.strikes[0].ev.pos.x == p.x);
    }
    {
        Rig r({true, true, {500.0f, 0.0f, 500.0f}, false, 5});
        Camera3DComponent secondary;
        secondary.primary = 0;
        r.w.spawn_with(LocalTransform{}, secondary);
        r.run(60 * 10);
        REQUIRE(!r.strikes.empty());
        const BoltStrike p = bolt_strike_position(LightningBolt{}, 5, 0, {500.0f, 0.0f, 500.0f});
        CHECK(r.strikes[0].ev.pos.x == p.x);
    }
    Rig n({true, false, {}, false, 5});
    n.run(60 * 10);
    REQUIRE(!n.strikes.empty());
    const BoltStrike p = bolt_strike_position(LightningBolt{}, 5, 0, {});
    CHECK(n.strikes[0].ev.has_pos == 1);
    CHECK(n.strikes[0].ev.pos.x == p.x);
    CHECK(n.strikes[0].ev.pos.z == p.z);
    CHECK(n.strikes[0].ev.distance_m == p.distance_m);
    CHECK(n.b().strike_dist_m == n.strikes.back().ev.distance_m);
}

TEST_CASE("lightning_bolt: strike times and Lightning bytes do not move with a bolt; delay is the real distance") {
    Rig with({true, true, {}, false, 5});
    Rig without({false, true, {}, false, 5});
    for (u32 i = 0; i < 60 * 60; ++i) {
        with.run(1);
        without.run(1);
        const Lightning& a = with.l();
        const Lightning& b = without.l();
        REQUIRE(a.counter == b.counter);
        REQUIRE(a.timer == b.timer);
        REQUIRE(a.remaining == b.remaining);
    }
    REQUIRE(with.strikes.size() == without.strikes.size());
    REQUIRE(with.strikes.size() > 10);
    for (u32 k = 0; k < with.strikes.size(); ++k) {
        CHECK(with.strikes[k].tick == without.strikes[k].tick);
        CHECK(with.strikes[k].ev.has_pos == 1);
        CHECK(without.strikes[k].ev.has_pos == 0);
        CHECK(without.strikes[k].ev.distance_m == 0.0f);
        CHECK(without.strikes[k].ev.pos.x == 0.0f);
        CHECK(without.strikes[k].ev.pos.z == 0.0f);
        // Without a bolt the delay is the hashed 30..300 (as before); with one it is the real distance.
        CHECK(without.strikes[k].ev.thunder_delay_ticks >= 30);
        CHECK(without.strikes[k].ev.thunder_delay_ticks <= 300);
        CHECK(with.strikes[k].ev.thunder_delay_ticks ==
              bolt_delay_ticks(with.strikes[k].ev.distance_m, static_cast<f64>(kDt)));
    }
    // The Weather flash follows the same schedule.
    const Weather* wa = nullptr;
    const Weather* wb = nullptr;
    with.w.query<const Weather>().each([&](Entity, const Weather& c) { wa = &c; });
    without.w.query<const Weather>().each([&](Entity, const Weather& c) { wb = &c; });
    REQUIRE(wa);
    REQUIRE(wb);
    CHECK(wa->flash_mul == wb->flash_mul);
}

TEST_CASE("lightning_bolt: no component, or enabled 0, gives the old event and an untouched world") {
    Rig none({false, true, {}, false, 5});
    Rig off({true, true, {}, false, 5});
    off.b().enabled = 0;
    Rig none2({false, true, {}, false, 5});
    none.run(60 * 30);
    off.run(60 * 30);
    none2.run(60 * 30);
    CHECK(none.w.state_hash() == none2.w.state_hash());  // deterministic without a bolt
    REQUIRE(none.strikes.size() == off.strikes.size());
    REQUIRE(!none.strikes.empty());
    for (u32 k = 0; k < none.strikes.size(); ++k) {
        CHECK(off.strikes[k].tick == none.strikes[k].tick);
        CHECK(off.strikes[k].ev.has_pos == 0);
        CHECK(off.strikes[k].ev.thunder_delay_ticks == none.strikes[k].ev.thunder_delay_ticks);
        CHECK(off.strikes[k].ev.distance_m == 0.0f);
    }
    CHECK(off.b().strike_id == 0);
    CHECK(off.b().strike_dist_m == 0.0f);
    CHECK(off.b().active == 0);
}

TEST_CASE("lightning_bolt: active follows the flash and is 0 outside a storm") {
    Rig r;
    bool saw_active = false;
    for (u32 i = 0; i < 60 * 20; ++i) {
        r.run(1);
        CHECK(r.b().active == r.l().remaining);
        if (r.b().active > 0) saw_active = true;
    }
    CHECK(saw_active);
    Weather* wx = nullptr;
    r.w.query<Weather>().each([&](Entity, Weather& c) { wx = &c; });
    REQUIRE(wx);
    wx->target = WeatherKind::Clear;
    wx->transition_seconds = 0.05f;
    r.run(60);
    CHECK(r.b().active == 0);
}

TEST_CASE("lightning_bolt: ThunderDelay uses distance_m when the event has a position") {
    struct Pusher {
        static void strike(Rig& r, f32 dist, u32 delay) {
            LightningStruck e{delay};
            e.has_pos = 1;
            e.distance_m = dist;
            r.w.event_queue<LightningStruck>()->push(e);
        }
    };
    {  // 3430 m: 600 ticks at 343 m/s, far beyond the old 30..300 clamp
        Rig r({false, true, {}, true, 5});
        r.w.destroy(r.light);
        r.run(3);
        const u64 t0 = r.t;
        Pusher::strike(r, 3430.0f, 30);  // the delay field is ignored
        r.run(700);
        REQUIRE(r.played.size() == 1);
        CHECK(r.played[0].tick == t0 + 600);
        CHECK(r.played[0].gain_db ==
              doctest::Approx(20.0 * std::log10(1.0 / (1.0 + 3430.0 / 100.0))).epsilon(1e-4));
    }
    {  // speed_mps now changes a positioned strike's delay
        Rig r({false, true, {}, true, 5});
        r.w.destroy(r.light);
        r.w.try_get_mut<ThunderDelay>(r.td)->speed_mps = 686.0f;
        r.run(3);
        const u64 t0 = r.t;
        Pusher::strike(r, 3430.0f, 30);
        r.run(400);
        REQUIRE(r.played.size() == 1);
        CHECK(r.played[0].tick == t0 + 300);
    }
    {  // has_pos 0 is the old path: distance_m is ignored
        Rig r({false, true, {}, true, 5});
        r.w.destroy(r.light);
        r.run(3);
        const u64 t0 = r.t;
        LightningStruck e{90};
        e.distance_m = 5000.0f;
        r.w.event_queue<LightningStruck>()->push(e);
        r.run(200);
        REQUIRE(r.played.size() == 1);
        CHECK(r.played[0].tick == t0 + 90);
        CHECK(r.played[0].gain_db ==
              doctest::Approx(20.0 * std::log10(1.0 / (1.0 + (90.0 * kDt * 343.0) / 100.0))).epsilon(1e-4));
    }
    {  // a nearer positioned strike is louder
        Rig r({false, true, {}, true, 5});
        r.w.destroy(r.light);
        r.run(3);
        Pusher::strike(r, 200.0f, 1);
        Pusher::strike(r, 2000.0f, 1);
        r.run(800);
        REQUIRE(r.played.size() == 2);
        CHECK(r.played[0].gain_db > r.played[1].gain_db);
    }
}

TEST_CASE("lightning_bolt: kit_lightning and ThunderDelay together sound a strike after its real distance") {
    Rig r({true, true, {}, true, 5});
    r.run(60 * 30);
    REQUIRE(r.strikes.size() > 3);
    // Every strike except those still in flight has sounded at strike tick + delay (the queue holds 8).
    std::vector<u64> expected;
    for (const Strike& s : r.strikes) {
        if (s.tick + s.ev.thunder_delay_ticks < r.t - 1) expected.push_back(s.tick + s.ev.thunder_delay_ticks);
    }
    REQUIRE(!expected.empty());
    REQUIRE(r.played.size() >= expected.size());
    for (u32 i = 0; i < expected.size(); ++i) CHECK(r.played[i].tick == expected[i]);
}

TEST_CASE("lightning_bolt: save and load in the middle of a flash resumes on the same state") {
    Array<byte> bytes;
    u64 hash_a = 0, hash_end_a = 0;
    LightningBolt bolt_a;
    {
        Rig a;
        for (u32 i = 0; i < 60 * 30 && a.b().active < 4; ++i) a.run(1);
        REQUIRE(a.b().active >= 1);
        REQUIRE(a.l().remaining >= 1);
        save::write_save(a.w, {}, bytes);
        hash_a = a.w.state_hash();
        bolt_a = a.b();
        a.run(60 * 10);
        hash_end_a = a.w.state_hash();
    }
    Rig b;
    b.run(77);  // another state, replaced by the save
    REQUIRE(save::read_save(b.w, bytes.span()));
    CHECK(b.w.state_hash() == hash_a);
    CHECK(b.b().strike_x == bolt_a.strike_x);
    CHECK(b.b().strike_z == bolt_a.strike_z);
    CHECK(b.b().strike_dist_m == bolt_a.strike_dist_m);
    CHECK(b.b().strike_id == bolt_a.strike_id);
    CHECK(b.b().active == bolt_a.active);
    b.run(60 * 10);
    CHECK(b.w.state_hash() == hash_end_a);
}

TEST_CASE("lightning_bolt: a scene with no Lightning or never in a storm is untouched") {
    World w;
    w.register_components<SaveKey>();
    install_precipitation(w, nullptr);
    w.spawn_with(LocalTransform{}, Camera3DComponent{});
    w.spawn_with(SaveKey{2}, Weather{});  // clear
    const Entity b = w.spawn_with(SaveKey{4}, LightningBolt{});
    for (u64 i = 0; i < 600; ++i) w.tick(kDt, i);
    const LightningBolt* c = w.try_get<LightningBolt>(b);
    REQUIRE(c);
    CHECK(c->strike_id == 0);
    CHECK(c->active == 0);
    CHECK(c->strike_dist_m == 0.0f);
    CHECK(c->strike_x == 0.0f);
}

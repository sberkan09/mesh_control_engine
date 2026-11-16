// ThunderDelay (M-RAINFX 7e): defaults, no component => no sound, a strike at distance d sounds exactly
// ceil(d / speed / dt) ticks later, nearer is louder, a clear scene spawns nothing, the queue caps at 8 and drops the
// newcomer, save/load mid-flight resumes on the same state hash, and a NaN/inf/negative speed is clamped.

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
#include "mce/kit/precipitation.h"
#include "mce/kit/thunder_delay.h"
#include "mce/kit/weather.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

struct Sfx {
    u64 tick;  // the tick (0-based) whose Pre phase pushed it
    f32 gain_db;
};

struct Rig {
    World w;
    u64 t = 0;
    Entity td;
    std::vector<Sfx> played;
    explicit Rig(bool with_td = true, bool camera = true, bool clip = true, bool storm = false, bool audio = true) {
        w.register_components<SaveKey>();
        install_precipitation(w, nullptr);
        if (audio) w.register_event<audio::PlaySfx>(Phase::FixedUpdate_Pre);
        if (camera) w.spawn_with(LocalTransform{}, Camera3DComponent{});
        if (with_td) {
            ThunderDelay c;
            if (clip) c.clip.guid = asset::Guid{7, 9};
            td = w.spawn_with(SaveKey{1}, c);
        }
        if (storm) {
            Weather wx;
            wx.target = WeatherKind::Storm;
            wx.transition_seconds = 0.1f;
            w.spawn_with(SaveKey{2}, wx);
            Lightning l;
            l.seed = 5;
            l.min_gap_seconds = 2.0f;
            l.max_gap_seconds = 4.0f;
            w.spawn_with(SaveKey{3}, l);
        }
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) {
            w.tick(kDt, t);
            if (w.event_queue<audio::PlaySfx>())
                for (const audio::PlaySfx& s : EventAccess(w).read<audio::PlaySfx>()) played.push_back({t, s.gain_db});
            ++t;
        }
    }
    void strike(u32 delay_ticks) { w.event_queue<LightningStruck>()->push(LightningStruck{delay_ticks}); }
    // Pushes a strike for the next tick and returns that tick.
    u64 strike_next(u32 delay_ticks) {
        strike(delay_ticks);
        return t;
    }
    ThunderDelay& c() { return *w.try_get_mut<ThunderDelay>(td); }
};

}  // namespace

TEST_CASE("thunder_delay: defaults") {
    const ThunderDelay c;
    CHECK(c.speed_mps == 343.0f);
    CHECK(c.volume == 1.0f);
    CHECK(c.clip.guid == asset::Guid{});
    CHECK(c.pending == 0);
    for (u32 i = 0; i < kThunderQueue; ++i) {
        CHECK(c.due[i] == 0);
        CHECK(c.gain_db[i] == 0.0f);
    }
}

TEST_CASE("thunder_delay: without a ThunderDelay, a clip, a camera or audio a strike makes no sound") {
    for (int variant = 0; variant < 4; ++variant) {
        Rig r(variant != 0, variant != 1, variant != 2, false, variant != 3);
        r.run(1);
        r.strike(60);
        r.run(400);
        CHECK(r.played.empty());
        if (variant != 0) CHECK(r.c().pending == 0);
    }
}

TEST_CASE("thunder_delay: a strike sounds exactly ceil(d / speed / dt) ticks later") {
    struct Case {
        u32 delay;
        f32 speed;
    };
    // d = delay * dt * 343. Default speed: exactly `delay` ticks; faster or slower speeds round up.
    const Case cases[] = {{30, 343.0f},  {61, 343.0f},  {300, 343.0f}, {120, 686.0f},
                          {121, 686.0f}, {100, 100.0f}, {45, 1000.0f}};
    for (const Case& k : cases) {
        Rig r;
        r.c().speed_mps = k.speed;
        r.run(3);
        const u64 strike_tick = r.strike_next(k.delay);
        r.run(k.delay * 4 + 10);
        const f64 d = static_cast<f64>(k.delay) * static_cast<f64>(kDt) * 343.0;
        const f64 exact = d / static_cast<f64>(k.speed) / static_cast<f64>(kDt);
        const u64 n = static_cast<u64>(std::ceil(exact - 1e-6));
        INFO("delay " << k.delay << " speed " << k.speed << " n " << n);
        REQUIRE(r.played.size() == 1);
        CHECK(r.played[0].tick == strike_tick + (n > 0 ? n : 1));
        CHECK(r.c().pending == 0);
    }
    // Pinned numbers: 343 m/s -> the event's own delay; double speed halves it (rounded up).
    Rig a;
    const u64 ta = a.strike_next(90);
    a.run(200);
    REQUIRE(a.played.size() == 1);
    CHECK(a.played[0].tick - ta == 90);
    Rig b;
    b.c().speed_mps = 686.0f;
    const u64 tb = b.strike_next(91);
    b.run(200);
    REQUIRE(b.played.size() == 1);
    CHECK(b.played[0].tick - tb == 46);
}

TEST_CASE("thunder_delay: a nearer strike is louder and volume scales the gain") {
    Rig r;
    r.strike(30);
    r.strike(300);
    r.run(400);
    REQUIRE(r.played.size() == 2);
    CHECK(r.played[0].gain_db > r.played[1].gain_db);
    // 30 ticks: d = 171.5 m, linear 1 / 2.715.
    CHECK(r.played[0].gain_db == doctest::Approx(20.0 * std::log10(1.0 / (1.0 + 171.5 / 100.0))).epsilon(1e-4));
    CHECK(r.played[1].gain_db > -60.0f);

    Rig q;
    q.c().volume = 0.5f;
    q.strike(30);
    q.run(100);
    REQUIRE(q.played.size() == 1);
    CHECK(q.played[0].gain_db == doctest::Approx(r.played[0].gain_db - 6.0206f).epsilon(1e-3));

    Rig z;
    z.c().volume = 0.0f;
    z.strike(30);
    z.run(100);
    REQUIRE(z.played.size() == 1);
    CHECK(z.played[0].gain_db == -60.0f);
}

TEST_CASE("thunder_delay: a clear scene spawns nothing; a storm makes one sound per strike") {
    {
        Rig r(true, true, true, false);
        Weather wx;
        r.w.spawn_with(SaveKey{9}, wx);
        r.w.spawn_with(SaveKey{10}, Lightning{});
        r.run(2000);
        CHECK(r.played.empty());
    }
    Rig r(true, true, true, true);
    u64 strikes = 0;
    for (u32 i = 0; i < 3000; ++i) {
        r.run(1);
        strikes += EventAccess(r.w).read<LightningStruck>().size();
    }
    REQUIRE(strikes > 5);
    CHECK(r.played.size() + r.c().pending == strikes);  // gaps of 2..4 s keep at most 3 in flight: none dropped
}

TEST_CASE("thunder_delay: the queue holds 8 strikes and drops the newcomer") {
    Rig r;
    r.run(1);
    for (u32 i = 0; i < 8; ++i) {
        r.strike(100 + i);
        r.run(1);
    }
    r.strike(30);  // a ninth, nearest of all, finds the queue full
    r.run(2);
    CHECK(r.c().pending == 8);
    r.run(400);
    CHECK(r.played.size() == 8);
    CHECK(r.c().pending == 0);
    for (u32 i = 0; i < kThunderQueue; ++i) {
        CHECK(r.c().due[i] == 0);
        CHECK(r.c().gain_db[i] == 0.0f);
    }
    // Room again afterwards.
    r.strike(30);
    r.run(100);
    CHECK(r.played.size() == 9);
}

TEST_CASE("thunder_delay: save and load mid-flight resumes with the same state hash") {
    Array<byte> bytes;
    u64 hash_a = 0;
    std::vector<Sfx> rest_a;
    {
        Rig a;
        a.run(5);
        a.strike(200);
        a.run(3);
        a.strike(120);
        a.run(10);
        REQUIRE(a.c().pending == 2);
        save::write_save(a.w, {}, bytes);
        hash_a = a.w.state_hash();
        a.run(300);
        rest_a = a.played;
    }
    Rig b;
    b.run(77);  // another state, replaced by the save
    b.strike(40);
    b.run(2);
    REQUIRE(save::read_save(b.w, bytes.span()));
    CHECK(b.w.state_hash() == hash_a);
    CHECK(b.c().pending == 2);
    b.played.clear();
    b.run(300);
    REQUIRE(b.played.size() == 2);
    REQUIRE(rest_a.size() == 2);
    // Same gains, same spacing between the two sounds.
    CHECK(b.played[0].gain_db == rest_a[0].gain_db);
    CHECK(b.played[1].gain_db == rest_a[1].gain_db);
    CHECK(b.played[1].tick - b.played[0].tick == rest_a[1].tick - rest_a[0].tick);
}

TEST_CASE("thunder_delay: NaN, infinite and negative speed and volume are clamped") {
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 inf = std::numeric_limits<f32>::infinity();
    {  // NaN speed = 343: the event's own delay
        Rig r;
        r.c().speed_mps = nan;
        const u64 t0 = r.strike_next(90);
        r.run(300);
        REQUIRE(r.played.size() == 1);
        CHECK(r.played[0].tick - t0 == 90);
    }
    {  // +inf: 1e6 m/s, the next tick
        Rig r;
        r.c().speed_mps = inf;
        const u64 t0 = r.strike_next(90);
        r.run(300);
        REQUIRE(r.played.size() == 1);
        CHECK(r.played[0].tick - t0 == 1);
    }
    {  // negative and zero: 1 m/s, so d = 30 * dt * 343 = 171.5 m is 171.5 s away (10290 ticks): late, not instant
        Rig r;
        r.c().speed_mps = -5.0f;
        r.strike(30);
        r.run(2);
        REQUIRE(r.c().pending == 1);
        CHECK(r.c().due[0] > 10000);
        r.c().speed_mps = 0.0f;
        r.strike(30);
        r.run(2);
        REQUIRE(r.c().pending == 2);
        CHECK(r.c().due[1] > 10000);
        CHECK(r.played.empty());
    }
    {  // NaN volume = 1, inf volume = 4
        Rig r;
        r.c().volume = nan;
        r.strike(30);
        r.run(100);
        Rig s;
        s.strike(30);
        s.run(100);
        REQUIRE(r.played.size() == 1);
        CHECK(r.played[0].gain_db == s.played[0].gain_db);
        Rig u;
        u.c().volume = inf;
        u.strike(30);
        u.run(100);
        REQUIRE(u.played.size() == 1);
        CHECK(u.played[0].gain_db == doctest::Approx(s.played[0].gain_db + 12.0412f).epsilon(1e-3));
    }
}

// RainSplash (M-RAINFX 7f): defaults, no component / clear / no camera => nothing, rain bursts land within radius_m of
// the primary camera at the entity's own height, the count scales with intensity and rate, two identical runs and a
// save mid-storm give the same state hash, and NaN/inf/negative fields are clamped.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/precipitation.h"
#include "mce/kit/rain_splash.h"
#include "mce/kit/weather.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

// ParticleEmitter3D has tail padding a temporary leaves indeterminate; the state hash reads it.
ParticleEmitter3D clean_emitter() {
    alignas(ParticleEmitter3D) unsigned char buf[sizeof(ParticleEmitter3D)];
    std::memset(buf, 0, sizeof buf);
    return *new (buf) ParticleEmitter3D{};
}

struct Burst {
    u64 tick;
    u32 count;
    math::vec3 pos;
};

struct Rig {
    World w;
    u64 t = 0;
    Entity sp, wx;
    math::vec3 cam{10.0f, 5.0f, -20.0f};
    std::vector<Burst> bursts;
    u32 seen_version = 0;
    explicit Rig(WeatherKind kind = WeatherKind::Rain, bool with_splash = true, bool camera = true,
                 RainSplash cfg = {}) {
        w.register_components<SaveKey>();
        install_precipitation(w, nullptr);
        if (camera) {
            LocalTransform ct;
            ct.position = cam;
            w.spawn_with(ct, Camera3DComponent{});
        }
        Weather c;
        c.target = kind;
        c.transition_seconds = 0.05f;
        wx = w.spawn_with(SaveKey{1}, c);
        if (with_splash) {
            LocalTransform lt;
            lt.position = {0.0f, 0.2f, 0.0f};
            sp = w.spawn_with(SaveKey{2}, cfg, clean_emitter(), lt);
        }
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) {
            w.tick(kDt, t);
            if (const ParticleEmitter3D* e = w.try_get<ParticleEmitter3D>(sp); e && e->burst_version != seen_version) {
                seen_version = e->burst_version;
                bursts.push_back({t, e->burst, w.try_get<LocalTransform>(sp)->position});
            }
            ++t;
        }
    }
    RainSplash& c() { return *w.try_get_mut<RainSplash>(sp); }
    ParticleEmitter3D& em() { return *w.try_get_mut<ParticleEmitter3D>(sp); }
    Weather& weather() { return *w.try_get_mut<Weather>(wx); }
};

u64 total(const std::vector<Burst>& b) {
    u64 n = 0;
    for (const Burst& x : b) n += x.count;
    return n;
}

}  // namespace

TEST_CASE("rain_splash: defaults") {
    const RainSplash c;
    CHECK(c.rate == 1.0f);
    CHECK(c.height_m == 0.15f);
    CHECK(c.life_seconds == 0.25f);
    CHECK(c.radius_m == 25.0f);
    CHECK(c.counter == 0);
}

TEST_CASE("rain_splash: without a RainSplash, in clear weather or without a camera nothing spawns") {
    {
        Rig r(WeatherKind::Rain, false);
        r.run(300);
        CHECK(r.bursts.empty());
    }
    for (WeatherKind k : {WeatherKind::Clear, WeatherKind::Overcast, WeatherKind::Snow}) {
        Rig r(k);
        r.run(300);
        CHECK(r.bursts.empty());
        CHECK(r.c().counter == 0);
        CHECK(r.em().emitting == 0);
        CHECK(r.em().rate == 0.0f);
        CHECK(r.em().burst == 0);
    }
    Rig r(WeatherKind::Rain, true, false);
    r.run(300);
    CHECK(r.bursts.empty());
    CHECK(r.em().emitting == 0);
}

TEST_CASE("rain_splash: rain bursts land within radius_m of the primary camera at the entity's height") {
    RainSplash cfg;
    cfg.radius_m = 8.0f;
    Rig r(WeatherKind::Rain, true, true, cfg);
    r.run(200);
    REQUIRE(r.bursts.size() > 100);
    f32 max_d = 0.0f;
    bool spread = false;
    for (const Burst& b : r.bursts) {
        const f32 dx = b.pos.x - r.cam.x, dz = b.pos.z - r.cam.z;
        max_d = std::fmax(max_d, std::sqrt(dx * dx + dz * dz));
        CHECK(b.pos.y == 0.2f);
        if (std::fabs(dx) > 2.0f) spread = true;
    }
    CHECK(max_d <= 8.001f);
    CHECK(max_d > 4.0f);
    CHECK(spread);
    CHECK(r.em().emitting == 1);
    CHECK(r.em().rate == 0.0f);
    CHECK(r.c().counter == r.bursts.size());
}

TEST_CASE("rain_splash: a storm splashes too; the count scales with intensity and rate") {
    auto count_at_full = [](f32 rate) -> u32 {
        RainSplash cfg;
        cfg.rate = rate;
        Rig r(WeatherKind::Rain, true, true, cfg);
        r.run(30);  // the 0.05 s fade is over
        REQUIRE(r.weather().intensity == 1.0f);
        r.bursts.clear();
        r.run(60);
        if (rate == 0.0f) {
            CHECK(r.bursts.empty());
            return 0u;
        }
        REQUIRE(r.bursts.size() == 60);
        return r.bursts.back().count;
    };
    CHECK(count_at_full(1.0f) == 4);
    CHECK(count_at_full(2.5f) == 10);
    CHECK(count_at_full(0.0f) == 0);

    {  // a slow fade-in: the counts follow the intensity up, never above the full-intensity 4
        Rig r;
        r.weather().transition_seconds = 4.0f;
        r.run(240);
        REQUIRE(!r.bursts.empty());
        u32 prev = 0;
        for (const Burst& b : r.bursts) {
            CHECK(b.count >= prev);
            CHECK(b.count <= 4);
            prev = b.count;
        }
        CHECK(r.bursts.front().count < 4);
        CHECK(r.bursts.back().count == 4);
    }
    Rig s(WeatherKind::Storm);
    s.run(60);
    CHECK(total(s.bursts) > 100);

    RainSplash lo, hi;
    lo.rate = 0.25f;
    hi.rate = 1.0f;
    Rig a(WeatherKind::Rain, true, true, lo), b(WeatherKind::Rain, true, true, hi);
    a.run(120);
    b.run(120);
    CHECK(total(a.bursts) < total(b.bursts));
    CHECK(total(a.bursts) > 0);
}

TEST_CASE("rain_splash: the same run twice has the same state hash; a different seed differs") {
    auto run = [](u32 seed) {
        RainSplash cfg;
        cfg.seed = seed;
        Rig r(WeatherKind::Storm, true, true, cfg);
        r.run(500);
        return r.w.state_hash();
    };
    CHECK(run(3) == run(3));
    CHECK(run(3) != run(4));
}

TEST_CASE("rain_splash: a save mid-storm resumes on the same state hash") {
    Array<byte> bytes;
    u64 expected = 0;
    u32 counter = 0;
    {
        Rig a(WeatherKind::Storm);
        a.c().seed = 9;
        a.run(400);
        REQUIRE(a.c().counter > 100);
        save::write_save(a.w, {}, bytes);
        a.run(300);
        expected = a.w.state_hash();
        counter = a.c().counter;
    }
    Rig b(WeatherKind::Clear);
    b.run(11);  // another state, replaced by the save
    REQUIRE(save::read_save(b.w, bytes.span()));
    CHECK(b.c().seed == 9);
    b.t = 400;
    b.run(300);
    CHECK(b.c().counter == counter);
    CHECK(b.w.state_hash() == expected);
}

TEST_CASE("rain_splash: NaN, infinite and negative fields are clamped and not rewritten") {
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 inf = std::numeric_limits<f32>::infinity();
    for (f32 bad : {nan, inf, -inf, -3.0f}) {
        RainSplash cfg;
        cfg.rate = bad;
        cfg.height_m = bad;
        cfg.life_seconds = bad;
        cfg.radius_m = bad;
        Rig r(WeatherKind::Rain, true, true, cfg);
        r.run(120);
        for (const Burst& b : r.bursts) {
            CHECK(std::isfinite(b.pos.x));
            CHECK(std::isfinite(b.pos.z));
            CHECK(b.count <= 256);
            const f32 dx = b.pos.x - r.cam.x, dz = b.pos.z - r.cam.z;
            CHECK(std::sqrt(dx * dx + dz * dz) <= 500.01f);
        }
        const ParticleEmitter3D& e = r.em();
        CHECK(std::isfinite(e.speed_max));
        CHECK(e.lifetime_max >= 0.02f);
        CHECK(e.lifetime_max <= 5.0f);
        CHECK(e.max_particles >= 1);
        CHECK(e.max_particles <= 65536);
        if (std::isnan(bad)) {
            CHECK(!r.bursts.empty());  // NaN means the default
            CHECK(r.bursts.back().count == 4);
        }
        CHECK(std::isnan(r.c().rate) == std::isnan(bad));  // the authored field is untouched
    }
}

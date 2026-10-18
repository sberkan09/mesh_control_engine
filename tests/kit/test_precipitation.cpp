// Precipitation and lightning (M-WEATHER 3): pinned rain and snow emitter numbers, camera follow, quiet scenes
// untouched, seeded lightning (same seed same strikes, no strikes outside a storm, the flash decays to the exact
// pre-flash environment, no drift in 2000 ticks, one event per strike), a stable state hash and an exact resume.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <cstring>
#include <new>

#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/precipitation.h"
#include "mce/kit/weather.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

// ParticleEmitter3D has 4 bytes of tail padding that a temporary leaves indeterminate; the state hash reads them, so
// the emitter is built over zeroed storage.
ParticleEmitter3D clean_emitter() {
    alignas(ParticleEmitter3D) unsigned char buf[sizeof(ParticleEmitter3D)];
    std::memset(buf, 0, sizeof buf);
    return *new (buf) ParticleEmitter3D{};
}

struct Px {
    World w;
    u64 t = 0;
    Entity we, pe, le;
    Array<u64> strikes;
    Array<u32> delays;
    explicit Px(WeatherKind target, bool with_weather = true, bool with_lightning = false, u32 seed = 1, f32 transition = 1.0f) {
        w.register_components<SaveKey>();
        install_precipitation(w, nullptr);
        w.set_resource(Environment3D{});
        if (with_weather) {
            Weather c;
            c.target = target;
            c.transition_seconds = transition;
            we = w.spawn_with(SaveKey{1}, c);
        }
        LocalTransform lt;
        lt.position = {1.0f, 2.0f, 3.0f};
        pe = w.spawn_with(SaveKey{2}, Precipitation3D{}, clean_emitter(), lt);
        if (with_lightning) {
            Lightning l;
            l.seed = seed;
            le = w.spawn_with(SaveKey{3}, l);
        }
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) {
            w.tick(kDt, t++);
            auto ev = EventAccess(w);
            for (const LightningStruck& s : ev.read<LightningStruck>()) {
                strikes.push_back(t);
                delays.push_back(s.thunder_delay_ticks);
            }
        }
    }
    ParticleEmitter3D& em() { return *w.try_get_mut<ParticleEmitter3D>(pe); }
    Environment3D& env() { return *w.try_resource<Environment3D>(); }
    Weather& wx() { return *w.try_get_mut<Weather>(we); }
    Lightning& lt() { return *w.try_get_mut<Lightning>(le); }
};

u32 bits(f32 x) { return std::bit_cast<u32>(x); }

bool same_env(const Environment3D& a, const Environment3D& b) {
    auto v3 = [](const math::vec3& p, const math::vec3& q) {
        return bits(p.x) == bits(q.x) && bits(p.y) == bits(q.y) && bits(p.z) == bits(q.z);
    };
    return bits(a.sun_intensity) == bits(b.sun_intensity) && bits(a.exposure) == bits(b.exposure) &&
           bits(a.fog_density) == bits(b.fog_density) && v3(a.sun_color, b.sun_color) &&
           v3(a.ambient_sky, b.ambient_sky) && v3(a.ambient_ground, b.ambient_ground) && v3(a.fog_color, b.fog_color);
}

}  // namespace

TEST_CASE("precipitation: rain at full intensity sets the pinned emitter fields") {
    Px x(WeatherKind::Rain);
    x.run(60);
    REQUIRE(x.wx().intensity == 1.0f);
    const ParticleEmitter3D& e = x.em();
    CHECK(e.emitting == 1);
    CHECK(e.rate == 3000.0f);
    CHECK(e.gravity.y == -9.0f);
    CHECK(e.gravity.x == doctest::Approx(1.5f));  // 3 x wind 0.5
    CHECK(e.direction.y < -0.9f);
    CHECK(e.direction.x == doctest::Approx(0.25f / std::sqrt(1.0625f)));
    CHECK(e.additive == 0);
    CHECK(e.box.x == 14.0f);
    CHECK(e.box.y == 0.5f);
    CHECK(e.box.z == 14.0f);
    CHECK(e.lifetime_min == doctest::Approx(12.0f / 18.0f));
    CHECK(e.speed_min == doctest::Approx(16.2f));
}

TEST_CASE("precipitation: half intensity is half the rate and a storm is x1.6") {
    Px h(WeatherKind::Rain, true, false, 1, 2.0f);
    h.run(60);
    REQUIRE(h.wx().intensity == 0.5f);
    CHECK(h.em().rate == 1500.0f);
    Px s(WeatherKind::Storm);
    s.run(60);
    CHECK(s.em().rate == doctest::Approx(4800.0f));
}

TEST_CASE("precipitation: snow differs from rain") {
    Px r(WeatherKind::Rain);
    Px s(WeatherKind::Snow);
    r.run(60);
    s.run(60);
    CHECK(s.em().rate == 800.0f);
    CHECK(s.em().emitting == 1);
    CHECK(s.em().gravity.y == doctest::Approx(-0.45f));
    CHECK(s.em().size_start > r.em().size_start);
    CHECK(s.em().color_start.w == doctest::Approx(0.9f));
    CHECK(s.em().lifetime_min == doctest::Approx(8.0f));
}

TEST_CASE("precipitation: clear stops emitting; with no Weather the emitter is untouched") {
    Px x(WeatherKind::Rain);
    x.run(60);
    x.wx().target = WeatherKind::Clear;
    x.run(120);
    CHECK(x.em().emitting == 0);
    CHECK(x.em().rate == 0.0f);
    Px n(WeatherKind::Clear, false);
    const ParticleEmitter3D before = n.em();
    n.run(30);
    CHECK(n.em().rate == before.rate);
    CHECK(n.em().emitting == before.emitting);
    CHECK(n.em().gravity.y == before.gravity.y);
    CHECK(n.em().box.x == before.box.x);
}

TEST_CASE("precipitation: the entity follows the primary camera") {
    Px x(WeatherKind::Rain);
    LocalTransform ct;
    ct.position = {10.0f, 1.5f, -20.0f};
    x.w.spawn_with(Camera3DComponent{}, ct);
    x.run(1);
    const LocalTransform* t = x.w.try_get<LocalTransform>(x.pe);
    CHECK(t->position.x == 10.0f);
    CHECK(t->position.y == 13.5f);
    CHECK(t->position.z == -20.0f);
    Px n(WeatherKind::Rain);  // no camera: the transform stays
    n.run(1);
    CHECK(n.w.try_get<LocalTransform>(n.pe)->position.y == 2.0f);
}

TEST_CASE("precipitation: no write when the values are unchanged") {
    Px x(WeatherKind::Rain);
    x.run(80);
    const ParticleEmitter3D e = x.em();
    x.run(100);
    CHECK(bits(x.em().rate) == bits(e.rate));
    CHECK(bits(x.em().gravity.x) == bits(e.gravity.x));
}

TEST_CASE("lightning: same seed same strikes, a different seed differs") {
    Px a(WeatherKind::Storm, true, true, 7), b(WeatherKind::Storm, true, true, 7), c(WeatherKind::Storm, true, true, 8);
    a.run(6000);
    b.run(6000);
    c.run(6000);
    REQUIRE(a.strikes.size() >= 3);
    REQUIRE(a.strikes.size() == b.strikes.size());
    bool same = true;
    for (usize i = 0; i < a.strikes.size(); ++i) same = same && a.strikes[i] == b.strikes[i] && a.delays[i] == b.delays[i];
    CHECK(same);
    bool differs = a.strikes.size() != c.strikes.size();
    for (usize i = 0; !differs && i < a.strikes.size(); ++i) differs = a.strikes[i] != c.strikes[i];
    CHECK(differs);
    for (u32 d : a.delays) {
        CHECK(d >= 30);
        CHECK(d <= 300);
    }
    CHECK(a.lt().counter == a.strikes.size());  // one event per strike
    // gaps are within 6..20 s (plus the 8 flash ticks and the transition's first tick)
    for (usize i = 1; i < a.strikes.size(); ++i) {
        CHECK(a.strikes[i] - a.strikes[i - 1] >= 360);
        CHECK(a.strikes[i] - a.strikes[i - 1] <= 1200 + 8 + 1);
    }
}

TEST_CASE("lightning: no strikes unless Storm") {
    for (WeatherKind k : {WeatherKind::Clear, WeatherKind::Overcast, WeatherKind::Rain, WeatherKind::Snow}) {
        Px x(k, true, true, 3);
        x.run(3000);
        CHECK(x.strikes.size() == 0);
        CHECK(x.wx().flash_mul == 1.0f);
    }
    Px n(WeatherKind::Storm, false, true, 3);
    n.run(1000);
    CHECK(n.strikes.size() == 0);
}

TEST_CASE("lightning: the flash decays to exactly the pre-flash environment") {
    Px with(WeatherKind::Storm, true, true, 5);
    Px without(WeatherKind::Storm);
    bool boosted = false;
    f32 peak = 0.0f, prev_mul = 1.0f;  // the environment of a tick carries the flash_mul written on the tick before
    for (u32 i = 0; i < 2000; ++i) {
        with.run(1);
        without.run(1);
        peak = std::max(peak, with.env().sun_intensity / without.env().sun_intensity);
        if (prev_mul == 1.0f) CHECK(same_env(with.env(), without.env()));  // bit-identical outside a flash
        else boosted = true;
        prev_mul = with.wx().flash_mul;
    }
    CHECK(boosted);
    CHECK(peak > 3.0f);
    CHECK(peak <= 4.0f + 1e-3f);
    // the flash multiplier is a pure function of `remaining`
    CHECK(with.wx().flash_mul == 1.0f + 3.0f * (static_cast<f32>(with.lt().remaining) / 8.0f));
}

TEST_CASE("lightning: no drift over 2000 ticks of storm and clearing returns the authored environment") {
    Px x(WeatherKind::Storm, true, true, 11);
    const Environment3D authored = x.env();
    x.run(2000);
    REQUIRE(x.strikes.size() >= 2);
    x.wx().target = WeatherKind::Clear;
    x.run(200);
    CHECK(x.lt().remaining == 0);
    CHECK(same_env(x.env(), authored));
}

TEST_CASE("lightning: the same run twice has the same state hash") {
    auto run = [](bool lightning) {
        Px x(WeatherKind::Storm, true, lightning, 4);
        x.run(1500);
        return x.w.state_hash();
    };
    CHECK(run(true) == run(true));
    CHECK(run(true) != run(false));
}

TEST_CASE("lightning: a save mid-storm resumes exactly") {
    Array<byte> bytes;
    Array<u64> expected;
    u32 counter = 0;
    Environment3D env;
    {
        Px a(WeatherKind::Storm, true, true, 9);
        a.run(1100);
        REQUIRE(a.lt().timer + a.lt().remaining > 0);
        save::write_save(a.w, {}, bytes);
        a.strikes.clear();
        a.run(2000);
        expected = a.strikes;
        counter = a.lt().counter;
        env = a.env();
    }
    Px b(WeatherKind::Clear, true, true, 99);
    auto st = save::read_save(b.w, bytes.span());
    REQUIRE(st);
    CHECK(b.lt().seed == 9);
    b.t = 1100;
    b.run(2000);
    REQUIRE(b.strikes.size() == expected.size());
    for (usize i = 0; i < expected.size(); ++i) CHECK(b.strikes[i] == expected[i]);
    CHECK(b.lt().counter == counter);
    CHECK(same_env(b.env(), env));
}

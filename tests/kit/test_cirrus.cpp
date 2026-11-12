// Cirrus (M-CIRRUS 53c): a plain saved component, no system, and the pure helper cirrus_coverage. Kit3DExtract fills
// render::Cirrus only from a scene with a Weather and a Cirrus (coverage from the helper at the first WorldClock's day and
// hour, wind_dir from the first WindDirection, speed = Weather.wind * speed_scale), then apply_quality; any other scene
// keeps the default carrier, bit for bit. Weather, CloudLayer and WindDirection are never written.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <limits>

#include "mce/containers/array.h"
#include "mce/convar.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/cirrus.h"
#include "mce/kit/cloud_layer.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/weather.h"
#include "mce/kit/wind_direction.h"
#include "mce/kit/world_clock.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"
#include "mce/string_id.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();
constexpr f32 kInf = std::numeric_limits<f32>::infinity();

Weather weather_of(WeatherKind current, WeatherKind target, f32 intensity) {
    Weather w;
    w.current = current;
    w.target = target;
    w.intensity = intensity;
    return w;
}
Weather settled(WeatherKind k, f32 intensity) { return weather_of(k, k, intensity); }

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }
bool same_cirrus(const render::Cirrus& a, const render::Cirrus& b) {  // the render struct has no implicit padding
    return same_bits(a.coverage, b.coverage) && same_bits(a.density, b.density) && same_bits(a.scale_m, b.scale_m) &&
           same_bits(a.stretch, b.stretch) && same_bits(a.speed, b.speed) && same_bits(a.wind_dir.x, b.wind_dir.x) &&
           same_bits(a.wind_dir.y, b.wind_dir.y) && same_bits(a.height_m, b.height_m) && a.enabled == b.enabled;
}
bool same_authoring(const Cirrus& a, const Cirrus& b) {
    return same_bits(a.strength, b.strength) && same_bits(a.fair_coverage, b.fair_coverage) &&
           same_bits(a.front_coverage, b.front_coverage) && same_bits(a.density, b.density) &&
           same_bits(a.scale_m, b.scale_m) && same_bits(a.stretch, b.stretch) && same_bits(a.speed_scale, b.speed_scale) &&
           same_bits(a.height_m, b.height_m);
}

struct Rig {
    render::MeshStore store;
    World world;
    Kit3DExtract extract;
    u64 t = 0;
    Entity cirrus_e, weather_e, wind_e, clock_e, cloud_e;
    explicit Rig(bool with_weather = true, bool with_cirrus = true) {
        world.register_components<SaveKey>();
        install_kit3d(world, nullptr);
        install_weather(world, nullptr);
        extract.init(store, nullptr);
        world.spawn_with(LocalTransform{}, Camera3DComponent{});
        if (with_weather) weather_e = world.spawn_with(SaveKey{1}, settled(WeatherKind::Clear, 0.0f));
        if (with_cirrus) cirrus_e = world.spawn_with(SaveKey{2}, Cirrus{});
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) world.tick(kDt, t++);
    }
    Cirrus& cirrus() { return *world.try_get_mut<Cirrus>(cirrus_e); }
    Weather& weather() { return *world.try_get_mut<Weather>(weather_e); }
    Environment3D& env() { return world.resource<Environment3D>(); }
    void add_clock(u32 day, f32 hour) {
        WorldClock c;
        c.day = day;
        c.hour = hour;
        clock_e = world.spawn_with(SaveKey{3}, c);
    }
    void add_wind(f32 degrees) {
        WindDirection d;
        d.degrees = degrees;
        wind_e = world.spawn_with(SaveKey{4}, d);
    }
    render::Cirrus carrier() {
        render::Camera3D cam;
        REQUIRE(extract.camera(world, 1.0f, cam));
        render::Scene3D s(store, mem::heap());
        s.begin(cam, {320, 180});
        extract.extract(world, s, 1.0f);
        return s.cirrus();
    }
};

}  // namespace

TEST_CASE("cirrus: defaults and the day fraction") {
    const Cirrus c;
    CHECK(c.strength == 1.0f);
    CHECK(c.fair_coverage == 0.45f);
    CHECK(c.front_coverage == 0.7f);
    CHECK(c.density == 0.35f);
    CHECK(c.scale_m == 3000.0f);
    CHECK(c.stretch == 6.0f);
    CHECK(c.speed_scale == 2.0f);
    CHECK(c.height_m == 9000.0f);
    CHECK(sizeof(Cirrus) == 8 * sizeof(f32));  // no padding to hash
    CHECK(cirrus_day_fraction(0, 0.0f) == 0.0f);
    CHECK(cirrus_day_fraction(3, 12.0f) == doctest::Approx(3.5f));
    CHECK(cirrus_day_fraction(3, 36.0f) == doctest::Approx(4.0f));  // the hour clamps to a day
    CHECK(cirrus_day_fraction(3, -5.0f) == 3.0f);
    CHECK(cirrus_day_fraction(3, kNaN) == 3.0f);
}

TEST_CASE("cirrus: the coverage stays in 0..1 over weather, clock and authoring") {
    const WeatherKind kinds[] = {WeatherKind::Clear, WeatherKind::Overcast, WeatherKind::Rain, WeatherKind::Storm,
                                 WeatherKind::Snow};
    for (const WeatherKind cur : kinds) {
        for (const WeatherKind tgt : {cur, WeatherKind::Rain}) {
            for (const f32 inten : {0.0f, 0.3f, 1.0f, 7.0f, -3.0f, kNaN}) {
                for (const f32 hi : {0.0f, 0.5f, 1.0f, 5.0f}) {
                    Cirrus c;
                    c.fair_coverage = hi * 0.5f;
                    c.front_coverage = hi;
                    for (f32 day = 0.0f; day < 40.0f; day += 0.37f) {
                        const f32 v = cirrus_coverage(c, weather_of(cur, tgt, inten), day);
                        CHECK(v >= 0.0f);
                        CHECK(v <= 1.0f);
                    }
                }
            }
        }
    }
}

TEST_CASE("cirrus: fair, ahead of a front and overcast") {
    Cirrus c;
    c.fair_coverage = 0.3f;
    c.front_coverage = 0.5f;
    const f32 day = 2.25f;
    const f32 fair = cirrus_coverage(c, settled(WeatherKind::Clear, 0.0f), day);
    const f32 front = cirrus_coverage(c, weather_of(WeatherKind::Clear, WeatherKind::Rain, 0.0f), day);
    CHECK(front - fair == doctest::Approx(0.2f));  // the same day variation on both
    CHECK(front > fair);
    // the front has arrived and the deck thickens: back toward fair, then the deck takes the cirrus off
    const f32 arriving = cirrus_coverage(c, settled(WeatherKind::Overcast, 0.5f), day);
    const f32 overcast = cirrus_coverage(c, settled(WeatherKind::Overcast, 1.0f), day);
    CHECK(overcast < arriving);
    CHECK(arriving < front);
    CHECK(overcast < 0.2f * fair + 1e-4f);  // (fair + variation) x (1 - 0.85)
    CHECK(overcast > 0.0f);
    // snow is a little thinner a deck than rain
    CHECK(cirrus_coverage(c, settled(WeatherKind::Snow, 1.0f), day) > overcast);
    // a pending change while a deck is fading out counts as the front too
    CHECK(cirrus_coverage(c, weather_of(WeatherKind::Rain, WeatherKind::Clear, 0.0f), day) == doctest::Approx(front));
    // the weather's intensity out of range and an unknown kind read as clear
    CHECK(cirrus_coverage(c, settled(WeatherKind::Clear, 1.0f), day) == fair);
    CHECK(cirrus_coverage(c, settled(static_cast<WeatherKind>(9), 1.0f), day) == fair);
}

TEST_CASE("cirrus: strength 0 gives exactly 0 and strength scales") {
    Cirrus c;
    c.strength = 0.0f;
    for (const f32 day : {0.0f, 1.5f, 99.9f, kNaN})
        for (const f32 inten : {0.0f, 1.0f, kNaN})
            CHECK(std::bit_cast<u32>(cirrus_coverage(c, settled(WeatherKind::Clear, inten), day)) == 0u);
    c.strength = 0.5f;
    const Cirrus full;
    const Weather w = weather_of(WeatherKind::Clear, WeatherKind::Storm, 0.0f);
    CHECK(cirrus_coverage(c, w, 4.5f) == doctest::Approx(0.5f * cirrus_coverage(full, w, 4.5f)));
}

TEST_CASE("cirrus: the day variation is a pure function of the clock") {
    const Cirrus c;
    const Weather w = settled(WeatherKind::Clear, 0.0f);
    f32 lo = 2.0f;
    f32 hi = -1.0f;
    for (u32 d = 0; d < 60; ++d) {
        const f32 a = cirrus_coverage(c, w, static_cast<f32>(d) + 0.4f);
        CHECK(std::bit_cast<u32>(a) == std::bit_cast<u32>(cirrus_coverage(c, w, static_cast<f32>(d) + 0.4f)));
        CHECK(std::fabs(a - c.fair_coverage) <= kCirrusDayVariation + 1e-5f);
        lo = std::fmin(lo, a);
        hi = std::fmax(hi, a);
    }
    CHECK(hi - lo > 0.1f);  // it does vary from day to day
    // smooth over midnight: no jump across a day boundary
    for (u32 d = 1; d < 20; ++d) {
        const f32 before = cirrus_coverage(c, w, static_cast<f32>(d) - 1e-3f);
        const f32 after = cirrus_coverage(c, w, static_cast<f32>(d) + 1e-3f);
        CHECK(std::fabs(after - before) < 0.01f);
    }
    // the variation does not depend on the weather's other state
    Weather busy = w;
    busy.seed = 77;
    busy.counter = 5;
    busy.timer = 9;
    CHECK(cirrus_coverage(c, busy, 3.3f) == cirrus_coverage(c, w, 3.3f));
}

TEST_CASE("cirrus: NaN and inverted authoring fall back to the defaults") {
    const Cirrus def;
    const Weather w = weather_of(WeatherKind::Clear, WeatherKind::Rain, 0.0f);
    const f32 want = cirrus_coverage(def, w, 1.7f);
    Cirrus c;
    c.strength = kNaN;
    CHECK(cirrus_coverage(c, w, 1.7f) == want);
    c = Cirrus{};
    c.fair_coverage = kNaN;
    c.front_coverage = kNaN;
    CHECK(cirrus_coverage(c, w, 1.7f) == want);
    c = Cirrus{};
    c.fair_coverage = 0.9f;
    c.front_coverage = 0.2f;  // inverted
    CHECK(cirrus_coverage(c, w, 1.7f) == want);
    c = Cirrus{};
    c.fair_coverage = -1.0f;  // out of range clamps
    c.front_coverage = 9.0f;
    CHECK(cirrus_coverage(c, w, 1.7f) <= 1.0f);
    // a non-finite or negative clock counts as day 0
    const f32 day0 = cirrus_coverage(def, w, 0.0f);
    CHECK(cirrus_coverage(def, w, kNaN) == day0);
    CHECK(cirrus_coverage(def, w, kInf) == day0);
    CHECK(cirrus_coverage(def, w, -4.0f) == day0);
    // a non-finite intensity counts as 0
    CHECK(cirrus_coverage(def, settled(WeatherKind::Rain, kNaN), 1.0f) == cirrus_coverage(def, settled(WeatherKind::Rain, 0.0f), 1.0f));
    CHECK(cirrus_coverage(def, settled(WeatherKind::Rain, kInf), 1.0f) == cirrus_coverage(def, settled(WeatherKind::Rain, 0.0f), 1.0f));
}

TEST_CASE("cirrus extract: no Cirrus, no Weather or an equirect sky texture gives the default carrier") {
    const render::Cirrus def;
    {
        Rig r(true, false);
        CHECK(same_cirrus(r.carrier(), def));
    }
    {
        Rig r(false, true);
        CHECK(same_cirrus(r.carrier(), def));
    }
    {
        Rig r;
        CHECK(r.carrier().coverage > 0.0f);
        r.env().sky_texture.guid = asset::Guid{1, 2};
        CHECK(same_cirrus(r.carrier(), def));
    }
    {
        Rig r;
        r.cirrus().strength = 0.0f;
        CHECK(std::bit_cast<u32>(r.carrier().coverage) == 0u);
    }
}

TEST_CASE("cirrus extract: a Cirrus changes nothing but the cirrus carrier") {
    Rig with;
    Rig without(true, false);
    with.world.spawn_with(SaveKey{5}, CloudLayer{});
    without.world.spawn_with(SaveKey{5}, CloudLayer{});
    render::Camera3D cam;
    REQUIRE(with.extract.camera(with.world, 1.0f, cam));
    render::Scene3D a(with.store, mem::heap());
    render::Scene3D b(without.store, mem::heap());
    a.begin(cam, {320, 180});
    b.begin(cam, {320, 180});
    with.extract.extract(with.world, a, 1.0f);
    without.extract.extract(without.world, b, 1.0f);
    CHECK(same_bits(a.clouds().coverage, b.clouds().coverage));
    CHECK(same_bits(a.clouds().speed, b.clouds().speed));
    CHECK(same_bits(a.clouds().scale_m, b.clouds().scale_m));
    CHECK(same_bits(a.fog().density, b.fog().density));
    CHECK(same_bits(a.night_sky().night_fade, b.night_sky().night_fade));
    CHECK(same_bits(a.aurora().level, b.aurora().level));
    CHECK(a.clouds().coverage > 0.0f);
    CHECK(same_cirrus(b.cirrus(), render::Cirrus{}));
    CHECK(a.cirrus().coverage > 0.0f);
}

TEST_CASE("cirrus extract: the carrier is the helper's coverage, the wind direction and the authoring") {
    Rig r;
    r.add_clock(4, 15.0f);
    r.add_wind(90.0f);
    r.world.try_get_mut<WindDirection>(r.wind_e)->angle_q = 90u * 65536u;  // engine-written; no tick runs here
    r.weather() = weather_of(WeatherKind::Clear, WeatherKind::Rain, 0.0f);
    r.weather().wind = 0.4f;
    r.cirrus().density = 0.5f;
    r.cirrus().scale_m = 5000.0f;
    r.cirrus().stretch = 9.0f;
    r.cirrus().speed_scale = 3.0f;
    r.cirrus().height_m = 12000.0f;
    const Weather w = r.weather();
    const render::Cirrus s = r.carrier();
    CHECK(s.enabled);
    CHECK(s.coverage == cirrus_coverage(r.cirrus(), w, cirrus_day_fraction(4, 15.0f)));
    CHECK(s.coverage > 0.0f);
    CHECK(s.density == 0.5f);
    CHECK(s.scale_m == 5000.0f);
    CHECK(s.stretch == 9.0f);
    CHECK(s.speed == doctest::Approx(1.2f));
    CHECK(s.height_m == 12000.0f);
    const WindDirection& wd = *r.world.try_get<WindDirection>(r.wind_e);
    const math::vec2 want = wind_direction_vector(wd);
    CHECK(s.wind_dir.x == doctest::Approx(want.x).epsilon(1e-5));
    CHECK(s.wind_dir.y == doctest::Approx(want.y).epsilon(1e-5));
    CHECK(std::fabs(s.wind_dir.x) < 1e-3f);  // 90 degrees: along z
    CHECK(std::fabs(std::fabs(s.wind_dir.y) - 1.0f) < 1e-3f);
    CHECK(s.wind_dir.x != 1.0f);

    // the clock moves the coverage: another day, another variation
    Rig other;
    other.add_clock(5, 15.0f);
    other.weather() = w;
    CHECK(other.carrier().coverage != s.coverage);

    // no WindDirection: +x; no clock: day 0, hour 0
    Rig plain;
    const render::Cirrus p = plain.carrier();
    CHECK(p.wind_dir.x == 1.0f);
    CHECK(p.wind_dir.y == 0.0f);
    CHECK(p.coverage == cirrus_coverage(Cirrus{}, plain.weather(), 0.0f));
}

TEST_CASE("cirrus extract: a front lifts the coverage and the deck lowers it") {
    Rig r;
    const f32 fair = r.carrier().coverage;
    r.weather() = weather_of(WeatherKind::Clear, WeatherKind::Storm, 0.0f);
    const f32 front = r.carrier().coverage;
    r.weather() = settled(WeatherKind::Overcast, 1.0f);
    const f32 deck = r.carrier().coverage;
    CHECK(front > fair);
    CHECK(deck < fair);
}

TEST_CASE("cirrus extract: the Low tier and r.cirrus 0 zero the coverage, the clamps apply") {
    Rig r;
    r.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    CHECK(r.carrier().coverage == 0.0f);
    r.extract.set_quality(render::quality_settings(render::QualityTier::High));
    CHECK(r.carrier().coverage > 0.0f);

    ConVar* cv = ConVar::find(StringId("r.cirrus"));
    REQUIRE(cv != nullptr);
    cv->set_float(0.0f);
    const f32 off = r.carrier().coverage;
    cv->reset();
    CHECK(off == 0.0f);
    CHECK(r.carrier().coverage > 0.0f);

    r.cirrus().scale_m = kNaN;  // authoring the renderer repairs (apply_quality)
    r.cirrus().stretch = 99.0f;
    r.cirrus().height_m = 1.0f;
    const render::Cirrus s = r.carrier();
    CHECK(s.scale_m == 3000.0f);
    CHECK(s.stretch == 16.0f);
    CHECK(s.height_m == 1000.0f);
}

TEST_CASE("cirrus extract: Weather, CloudLayer, WindDirection and Cirrus are never written") {
    Rig r;
    r.add_wind(33.0f);
    r.world.spawn_with(SaveKey{6}, CloudLayer{});
    r.weather() = weather_of(WeatherKind::Clear, WeatherKind::Rain, 0.0f);
    const u64 before = r.world.state_hash();  // hashes every saved column: Weather, CloudLayer, WindDirection, Cirrus
    const Cirrus c0 = r.cirrus();
    for (int i = 0; i < 3; ++i) CHECK(r.carrier().coverage > 0.0f);
    CHECK(r.world.state_hash() == before);
    CHECK(same_authoring(r.cirrus(), c0));
}

TEST_CASE("cirrus: a second Cirrus is ignored and the extract is deterministic") {
    Rig r;
    Cirrus other;
    other.strength = 0.1f;
    r.world.spawn_with(SaveKey{7}, other);
    const render::Cirrus a = r.carrier();
    CHECK(a.coverage == cirrus_coverage(Cirrus{}, r.weather(), 0.0f));
    CHECK(same_cirrus(a, r.carrier()));
}

TEST_CASE("cirrus: save and load round-trips the fields and the state hash") {
    Array<byte> bytes;
    u64 hash_a = 0;
    u64 hash_s = 0;
    Cirrus l;
    l.strength = 0.7f;
    l.fair_coverage = 0.2f;
    l.front_coverage = 0.8f;
    l.density = 0.45f;
    l.scale_m = 4000.0f;
    l.stretch = 8.0f;
    l.speed_scale = 3.0f;
    l.height_m = 11000.0f;
    {
        Rig a;
        a.cirrus() = l;
        a.run(100);
        save::write_save(a.world, {}, bytes);
        hash_s = a.world.state_hash();
        a.run(50);
        hash_a = a.world.state_hash();
    }
    Rig b;
    b.run(77);
    REQUIRE(save::read_save(b.world, bytes.span()));
    CHECK(b.world.state_hash() == hash_s);
    b.t = 100;
    b.run(50);
    CHECK(b.world.state_hash() == hash_a);
    CHECK(same_authoring(b.cirrus(), l));
}

TEST_CASE("cirrus: a scene without the component hashes as before") {
    Rig a(true, false);
    Rig b(true, false);
    a.run(30);
    b.run(30);
    CHECK(a.world.state_hash() == b.world.state_hash());
}

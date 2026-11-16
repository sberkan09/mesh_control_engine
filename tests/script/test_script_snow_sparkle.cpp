// M-SNOWSPARKLE step 31c: the snow sparkle functions of the `weather` Luau table over the first kit::SnowSparkle. Getters,
// clamping, snow_sparkle_active against the kit helper over the cover, the sun, the cloud and the rain, named errors, nothing
// written on error, and a missing SnowSparkle raises (the setter never adds one).

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <string>

#include "mce/ecs/transform.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/snow_cover.h"
#include "mce/kit/snow_sparkle.h"
#include "mce/kit/weather.h"
#include "mce/render/atmosphere.h"
#include "mce/script/compiler.h"

using namespace mce;
using script::ScriptHost;
using script::ScriptState;

namespace {

struct SparkleRig {
    World w;
    ScriptHost host;
    u64 tick = 0;
    Entity sp_e, probe, weather_e, cover_e;

    explicit SparkleRig(const char* source, bool with_sparkle = true, bool install = true) {
        w.register_components<LocalTransform>();
        if (install) kit::install_weather(w, nullptr);
        INFO(source);
        Result<Array<u8>> bc = script::compile_unchecked("a", source);
        REQUIRE(bc);
        REQUIRE(host.add_bytecode("a", bc.value().span()));
        host.install(w);
        probe = w.spawn_with(LocalTransform{});
        if (with_sparkle) sp_e = w.spawn_with(kit::SnowSparkle{});
    }
    kit::SnowSparkle& sp() { return *w.try_get_mut<kit::SnowSparkle>(sp_e); }
    void step(u32 n = 1) {
        for (u32 i = 0; i < n; ++i) w.tick(1.0 / 60, ++tick);
    }
    const LocalTransform& p() { return *w.try_get<LocalTransform>(probe); }
    const ScriptHost& h() const { return host; }
};

std::string err(const ScriptHost& h) { return h.scripts()[0].error.c_str(); }

void expect_error(const char* body, const char* needle, bool with_sparkle = true, bool install = true) {
    const std::string src = std::string("function update()\n    if mce.tick() == 1 then\n        ") + body + "\n    end\nend\n";
    SparkleRig r(src.c_str(), with_sparkle, install);
    r.step(2);
    INFO(body);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    INFO(err(r.h()));
    CHECK(err(r.h()).find(needle) != std::string::npos);
}

u32 bits(f32 v) { return std::bit_cast<u32>(v); }

const char* const kProbeSource = R"(
function update()
    mce.each("LocalTransform", function(e)
        mce.set(e, "LocalTransform", "position", vector.create(weather.snow_sparkle_active(), 0, 0))
    end)
end
)";

// The inputs of the extract's snow sparkle block, recomputed here from the world state.
f32 expected_amount(const kit::SnowSparkle& sp, const kit::SnowCover* cover, f32 tod, const kit::Weather* wx) {
    const f32 c = cover ? std::clamp(cover->cover * cover->strength, 0.0f, 1.0f) : 0.0f;
    f32 sun = 0.0f;
    if (tod >= 0.0f) sun = std::clamp(-render::sun_direction_at(tod).y, 0.0f, 1.0f);
    const bool raining = wx && (wx->current == kit::WeatherKind::Rain || wx->current == kit::WeatherKind::Storm);
    const f32 rain = raining ? wx->intensity : 0.0f;
    f32 cloud = 0.0f;
    if (wx) {
        const f32 kind = wx->current == kit::WeatherKind::Clear ? 0.0f : wx->current == kit::WeatherKind::Snow ? 0.9f : 1.0f;
        cloud = kind * std::clamp(wx->intensity, 0.0f, 1.0f);
    }
    return kit::snow_sparkle_amount(sp, c, sun, cloud, rain);
}

}  // namespace

TEST_CASE("snow sparkle api: the getters read the first SnowSparkle, the setter writes and clamps to the Range") {
    SparkleRig r(R"(
function update()
    local t = mce.tick()
    local function near(a, b) return math.abs(a - b) < 1e-6 end
    if t == 1 then
        assert(weather.snow_sparkle_enabled() == 1 and weather.snow_sparkle_manual() == 0 and weather.snow_sparkle_strength() == 1)
        assert(near(weather.snow_sparkle_cover_low(), 0.2) and near(weather.snow_sparkle_cover_full(), 0.8))
        assert(near(weather.snow_sparkle_sun_low(), 0.05) and near(weather.snow_sparkle_sun_high(), 0.6))
        assert(weather.snow_sparkle_rate() == 24 and weather.snow_sparkle_box_xz_m() == 14)
        assert(near(weather.snow_sparkle_height_m(), 1.2) and near(weather.snow_sparkle_size_m(), 0.03))
        assert(near(weather.snow_sparkle_lifetime_s(), 0.6))
        assert(weather.snow_sparkle_max_particles() == 96 and weather.snow_sparkle_seed() == 1)
        local r, g, b = weather.snow_sparkle_color()
        assert(r == 1 and g == 1 and b == 1)
        weather.set_snow_sparkle{cover_low = 0.1, cover_full = 0.9, sun_low = 0.25, sun_high = 0.75, manual = 0.5, rate = 100,
                                 box_xz_m = 100, height_m = 10, size_m = 0.5, lifetime_s = 5, max_particles = 512, seed = 77,
                                 strength = 0.75, color = {1, 0.5, 0.25}}
    end
    if t == 2 then
        assert(near(weather.snow_sparkle_cover_low(), 0.1) and near(weather.snow_sparkle_cover_full(), 0.9))
        assert(weather.snow_sparkle_sun_low() == 0.25 and weather.snow_sparkle_sun_high() == 0.75 and weather.snow_sparkle_manual() == 0.5)
        assert(weather.snow_sparkle_rate() == 100 and weather.snow_sparkle_box_xz_m() == 100 and weather.snow_sparkle_height_m() == 10)
        assert(weather.snow_sparkle_size_m() == 0.5 and weather.snow_sparkle_lifetime_s() == 5)
        assert(weather.snow_sparkle_max_particles() == 512 and weather.snow_sparkle_seed() == 77 and weather.snow_sparkle_strength() == 0.75)
        local r, g, b = weather.snow_sparkle_color()
        assert(r == 1 and g == 0.5 and b == 0.25)
        weather.set_snow_sparkle{enabled = 0}
    end
    if t == 3 then
        assert(weather.snow_sparkle_enabled() == 0 and weather.snow_sparkle_rate() == 100)  -- only the given keys changed
        weather.set_snow_sparkle{enabled = 7, cover_low = 1e9, cover_full = 1e9, sun_low = 1e9, sun_high = 1e9, manual = 1e9,
                                 rate = 1e9, box_xz_m = 1e9, height_m = 1e9, size_m = 1e9, lifetime_s = 1e9, max_particles = 1e9,
                                 seed = 1e12, strength = 1e9, color = {1e9, -1, 0.5}}
    end
    if t == 4 then
        assert(weather.snow_sparkle_enabled() == 1 and weather.snow_sparkle_cover_low() == 1 and weather.snow_sparkle_cover_full() == 1)
        assert(weather.snow_sparkle_sun_low() == 1 and weather.snow_sparkle_sun_high() == 1 and weather.snow_sparkle_manual() == 1)
        assert(weather.snow_sparkle_rate() == 200 and weather.snow_sparkle_box_xz_m() == 100 and weather.snow_sparkle_height_m() == 10)
        assert(weather.snow_sparkle_size_m() == 0.5 and weather.snow_sparkle_lifetime_s() == 5)
        assert(weather.snow_sparkle_max_particles() == 512 and weather.snow_sparkle_seed() == 4294967295 and weather.snow_sparkle_strength() == 1)
        local r, g, b = weather.snow_sparkle_color()
        assert(r == 1 and g == 0 and b == 0.5)
        weather.set_snow_sparkle{enabled = 0, cover_low = -1, cover_full = -1, sun_low = -1, sun_high = -1, manual = -1, rate = -1,
                                 box_xz_m = -1, height_m = -1, size_m = -1, lifetime_s = -1, max_particles = -1, seed = -1,
                                 strength = -1}
        weather.set_snow_sparkle{}  -- nothing given, nothing changes
    end
    if t == 5 then
        assert(weather.snow_sparkle_enabled() == 0 and weather.snow_sparkle_cover_low() == 0 and weather.snow_sparkle_cover_full() == 0)
        assert(weather.snow_sparkle_sun_low() == 0 and weather.snow_sparkle_sun_high() == 0 and weather.snow_sparkle_manual() == 0)
        assert(weather.snow_sparkle_rate() == 0 and weather.snow_sparkle_box_xz_m() == 1 and near(weather.snow_sparkle_height_m(), 0.2))
        assert(near(weather.snow_sparkle_size_m(), 0.005) and near(weather.snow_sparkle_lifetime_s(), 0.1))
        assert(weather.snow_sparkle_max_particles() == 8 and weather.snow_sparkle_seed() == 0 and weather.snow_sparkle_strength() == 0)
    end
end
)");
    r.step(6);
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.sp().strength == 0.0f);
    CHECK(r.sp().enabled == 0);
    CHECK(r.sp().pad_[0] == 0);
    CHECK(r.sp().pad_[1] == 0);
    CHECK(r.sp().pad_[2] == 0);
}

TEST_CASE("snow sparkle api: snow_sparkle_active is the kit helper over the cover, the sun, the cloud and the rain") {
    SparkleRig r(kProbeSource);
    kit::SnowCover cover;
    r.cover_e = r.w.spawn_with(cover);
    kit::Environment3D& e = r.w.set_resource(kit::Environment3D{});
    r.weather_e = r.w.spawn_with(kit::Weather{});
    u32 distinct = 0, zero = 0, nonzero = 0;
    f32 last = -1.0f;
    for (f32 cv : {0.0f, 0.3f, 0.6f, 1.0f}) {
        r.w.try_get_mut<kit::SnowCover>(r.cover_e)->cover = cv;
        for (f32 manual : {0.0f, 0.3f}) {
            r.sp().manual = manual;
            for (f32 tod : {-1.0f, 0.0f, 6.2f, 7.0f, 8.0f, 9.0f, 12.0f, 16.0f, 17.5f, 18.5f}) {
                e.time_of_day = tod;
                for (kit::WeatherKind kind : {kit::WeatherKind::Clear, kit::WeatherKind::Rain, kit::WeatherKind::Storm, kit::WeatherKind::Snow}) {
                    for (f32 inten : {0.2f, 0.6f}) {
                        kit::Weather& wx = *r.w.try_get_mut<kit::Weather>(r.weather_e);
                        wx.current = wx.target = kind;
                        wx.intensity = inten;
                        r.step(2);
                        REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
                        const kit::Weather& after = *r.w.try_get<kit::Weather>(r.weather_e);
                        const f32 expected = expected_amount(r.sp(), r.w.try_get<kit::SnowCover>(r.cover_e), tod, &after);
                        INFO("cover " << cv << " manual " << manual << " tod " << tod << " kind " << int(kind) << " intensity " << inten);
                        CHECK(bits(r.p().position.x) == bits(expected));
                        CHECK(r.p().position.x >= 0.0f);
                        CHECK(r.p().position.x <= 1.0f);
                        if (r.p().position.x > 0.0f) ++nonzero; else ++zero;
                        if (r.p().position.x != last) ++distinct;
                        last = r.p().position.x;
                    }
                }
            }
        }
    }
    CHECK(distinct > 8);
    CHECK(nonzero > 20);
    CHECK(zero > 20);
    // enabled 0 and strength 0 are exactly 0
    r.sp() = kit::SnowSparkle{};
    r.sp().manual = 0.5f;
    r.sp().enabled = 0;
    r.step(1);
    CHECK(r.p().position.x == 0.0f);
    r.sp().enabled = 1;
    r.sp().strength = 0.0f;
    r.step(1);
    CHECK(r.p().position.x == 0.0f);
}

TEST_CASE("snow sparkle api: with no SnowCover or Weather only the manual floor shows, and a clear sky is no cloud") {
    SparkleRig q(kProbeSource);
    kit::Environment3D& env = q.w.set_resource(kit::Environment3D{});
    env.time_of_day = 8.0f;
    q.step(2);
    REQUIRE(q.h().scripts()[0].state == ScriptState::Running);
    CHECK(q.p().position.x == 0.0f);  // no cover, manual 0
    q.sp().manual = 0.625f;
    q.step(1);
    CHECK(bits(q.p().position.x) == bits(expected_amount(q.sp(), nullptr, 8.0f, nullptr)));
    CHECK(q.p().position.x == 0.625f);  // no cloud, no rain: the floor passes whole
    // an overcast Weather thins it (the Weather system moves the intensity, so compare with the kit helper); a clear one does not
    const Entity wx_e = q.w.spawn_with(kit::Weather{});
    kit::Weather& wx = *q.w.try_get_mut<kit::Weather>(wx_e);
    wx.current = wx.target = kit::WeatherKind::Overcast;
    wx.intensity = 1.0f;
    q.step(1);
    const kit::Weather& after = *q.w.try_get<kit::Weather>(wx_e);
    CHECK(bits(q.p().position.x) == bits(expected_amount(q.sp(), nullptr, 8.0f, &after)));
    CHECK(q.p().position.x < 0.625f);
    q.w.try_get_mut<kit::Weather>(wx_e)->current = kit::WeatherKind::Clear;
    q.w.try_get_mut<kit::Weather>(wx_e)->target = kit::WeatherKind::Clear;
    q.step(1);
    CHECK(q.p().position.x == 0.625f);
}

TEST_CASE("snow sparkle api: a world without a SnowSparkle raises a named error and the setter never adds one") {
    for (const char* name : {"snow_sparkle_enabled", "snow_sparkle_cover_low", "snow_sparkle_cover_full", "snow_sparkle_sun_low",
                             "snow_sparkle_sun_high", "snow_sparkle_manual", "snow_sparkle_rate", "snow_sparkle_box_xz_m",
                             "snow_sparkle_height_m", "snow_sparkle_size_m", "snow_sparkle_lifetime_s", "snow_sparkle_color",
                             "snow_sparkle_max_particles", "snow_sparkle_seed", "snow_sparkle_strength", "snow_sparkle_active"})
        expect_error((std::string("weather.") + name + "()").c_str(),
                     (std::string("weather.") + name + ": the scene has no SnowSparkle entity").c_str(), false);
    expect_error("weather.set_snow_sparkle{}", "weather.set_snow_sparkle: the scene has no SnowSparkle entity", false);
    expect_error("weather.set_snow_sparkle{strength = 0}", "weather.set_snow_sparkle: the scene has no SnowSparkle entity", false);
    expect_error("weather.snow_sparkle_active()", "weather.snow_sparkle_active: this world has no weather", false, false);
    expect_error("weather.set_snow_sparkle{}", "weather.set_snow_sparkle: this world has no weather", false, false);
    SparkleRig r("function update()\n    if mce.tick() == 1 then weather.set_snow_sparkle{enabled = 0} end\nend\n", false);
    r.step(2);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    usize n = 0;
    r.w.query<kit::SnowSparkle>().each([&](Entity, kit::SnowSparkle&) { ++n; });
    CHECK(n == 0);
}

TEST_CASE("snow sparkle api: bad arguments raise an error that names the call and write nothing") {
    expect_error("weather.set_snow_sparkle{bogus = 1}", "weather.set_snow_sparkle: unknown key 'bogus'");
    expect_error("weather.set_snow_sparkle{speed = 1}", "weather.set_snow_sparkle: unknown key 'speed'");
    expect_error("weather.set_snow_sparkle{rate = 'fast'}", "weather.set_snow_sparkle: 'rate' is not a number");
    expect_error("weather.set_snow_sparkle{strength = 0/0}", "weather.set_snow_sparkle: 'strength' is not a number");
    expect_error("weather.set_snow_sparkle{enabled = true}", "weather.set_snow_sparkle: 'enabled' is not a number");
    expect_error("weather.set_snow_sparkle{sun_low = 'x'}", "weather.set_snow_sparkle: 'sun_low' is not a number");
    expect_error("weather.set_snow_sparkle{seed = 'x'}", "weather.set_snow_sparkle: 'seed' is not a number");
    expect_error("weather.set_snow_sparkle{color = 1}", "weather.set_snow_sparkle: 'color' is not a table of three numbers");
    expect_error("weather.set_snow_sparkle{color = {1, 2}}", "weather.set_snow_sparkle: 'color' is not a table of three numbers");
    expect_error("weather.set_snow_sparkle{color = {1, 'x', 3}}", "weather.set_snow_sparkle: 'color' is not a table of three numbers");
    expect_error("weather.set_snow_sparkle{[1] = 0.5}", "weather.set_snow_sparkle: the keys are names");
    expect_error("weather.set_snow_sparkle(0.5)", "table expected");
    expect_error("weather.set_snow_sparkle()", "table expected");
    const kit::SnowSparkle def;
    for (const char* body : {"weather.set_snow_sparkle{manual = 0.9, strength = 'x'}", "weather.set_snow_sparkle{strength = 'x', manual = 0.9}",
                             "weather.set_snow_sparkle{manual = 0.9, nope = 1}", "weather.set_snow_sparkle{enabled = 0, rate = 'x'}",
                             "weather.set_snow_sparkle{color = {0, 0, 0}, height_m = 'x'}",
                             "weather.set_snow_sparkle{color = {1, 2}, manual = 0.9, enabled = 0}"}) {
        const std::string src = std::string("function update()\n    if mce.tick() == 1 then ") + body + " end\nend\n";
        SparkleRig r(src.c_str());
        r.step(2);
        INFO(body);
        REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
        CHECK(r.sp().manual == def.manual);
        CHECK(r.sp().strength == def.strength);
        CHECK(r.sp().rate == def.rate);
        CHECK(r.sp().enabled == def.enabled);
        CHECK(r.sp().height_m == def.height_m);
        CHECK(r.sp().color.x == def.color.x);
        CHECK(r.sp().color.z == def.color.z);
    }
}

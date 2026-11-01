// M-DUSTWIND step 25c: the dust functions of the `weather` Luau table over the first kit::Dust. Getters, clamping,
// dust_blowing against the kit helper over wind, gusts and rain, named errors, nothing written on error, and a missing
// Dust raises (the setter never adds one).

#include <doctest/doctest.h>

#include <bit>
#include <string>

#include "mce/ecs/transform.h"
#include "mce/kit/dust.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/weather.h"
#include "mce/kit/wind_gust.h"
#include "mce/script/compiler.h"

using namespace mce;
using script::ScriptHost;
using script::ScriptState;

namespace {

struct DustRig {
    World w;
    ScriptHost host;
    u64 tick = 0;
    Entity du_e, probe, weather_e, gust_e;

    explicit DustRig(const char* source, bool with_dust = true, bool install = true) {
        w.register_components<LocalTransform>();
        if (install) kit::install_weather(w, nullptr);
        INFO(source);
        Result<Array<u8>> bc = script::compile_unchecked("a", source);
        REQUIRE(bc);
        REQUIRE(host.add_bytecode("a", bc.value().span()));
        host.install(w);
        probe = w.spawn_with(LocalTransform{});
        if (with_dust) du_e = w.spawn_with(kit::Dust{});
    }
    kit::Dust& du() { return *w.try_get_mut<kit::Dust>(du_e); }
    void step(u32 n = 1) {
        for (u32 i = 0; i < n; ++i) w.tick(1.0 / 60, ++tick);
    }
    const LocalTransform& p() { return *w.try_get<LocalTransform>(probe); }
    const ScriptHost& h() const { return host; }
};

std::string err(const ScriptHost& h) { return h.scripts()[0].error.c_str(); }

void expect_error(const char* body, const char* needle, bool with_dust = true, bool install = true) {
    const std::string src = std::string("function update()\n    if mce.tick() == 1 then\n        ") + body + "\n    end\nend\n";
    DustRig r(src.c_str(), with_dust, install);
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
        mce.set(e, "LocalTransform", "position", vector.create(weather.dust_blowing(), 0, 0))
    end)
end
)";

}  // namespace

TEST_CASE("dust api: the getters read the first Dust, the setter writes and clamps to the Range") {
    DustRig r(R"(
function update()
    local t = mce.tick()
    local function near(a, b) return math.abs(a - b) < 1e-6 end
    if t == 1 then
        assert(weather.dust_enabled() == 1 and weather.dust_manual() == 0 and weather.dust_strength() == 1)
        assert(near(weather.dust_wind_start(), 0.3) and near(weather.dust_wind_full(), 0.8))
        assert(weather.dust_rate() == 40 and weather.dust_box_xz_m() == 14 and weather.dust_height_m() == 3)
        assert(near(weather.dust_size_m(), 0.05) and near(weather.dust_lifetime_s(), 2.5) and weather.dust_speed() == 6)
        assert(near(weather.dust_wind_follow(), 0.8))
        assert(weather.dust_max_particles() == 1024 and weather.dust_seed() == 1)
        local r, g, b = weather.dust_color()
        assert(near(r, 0.78) and near(g, 0.66) and near(b, 0.46))
        weather.set_dust{wind_start = 0.25, wind_full = 0.5, manual = 0.5, rate = 100, box_xz_m = 30, height_m = 12,
                         size_m = 0.5, lifetime_s = 10, speed = 20, wind_follow = 1.5, max_particles = 2048, seed = 77,
                         strength = 0.75, color = {1, 0.5, 0.25}}
    end
    if t == 2 then
        assert(weather.dust_wind_start() == 0.25 and weather.dust_wind_full() == 0.5 and weather.dust_manual() == 0.5)
        assert(weather.dust_rate() == 100 and weather.dust_box_xz_m() == 30 and weather.dust_height_m() == 12)
        assert(weather.dust_size_m() == 0.5 and weather.dust_lifetime_s() == 10 and weather.dust_speed() == 20 and weather.dust_wind_follow() == 1.5)
        assert(weather.dust_max_particles() == 2048 and weather.dust_seed() == 77 and weather.dust_strength() == 0.75)
        local r, g, b = weather.dust_color()
        assert(r == 1 and g == 0.5 and b == 0.25)
        weather.set_dust{enabled = 0}
    end
    if t == 3 then
        assert(weather.dust_enabled() == 0 and weather.dust_rate() == 100)  -- only the given keys changed
        weather.set_dust{enabled = 7, wind_start = 1e9, wind_full = 1e9, manual = 1e9, rate = 1e9, box_xz_m = 1e9,
                         height_m = 1e9, size_m = 1e9, lifetime_s = 1e9, speed = 1e9, wind_follow = 1e9, max_particles = 1e9,
                         seed = 1e12, strength = 1e9, color = {1e9, -1, 0.5}}
    end
    if t == 4 then
        assert(weather.dust_enabled() == 1 and weather.dust_wind_start() == 1 and weather.dust_wind_full() == 1)
        assert(weather.dust_manual() == 1 and weather.dust_rate() == 400 and weather.dust_box_xz_m() == 100)
        assert(weather.dust_height_m() == 20 and weather.dust_size_m() == 0.5 and weather.dust_lifetime_s() == 10)
        assert(weather.dust_speed() == 30 and weather.dust_wind_follow() == 2)
        assert(weather.dust_max_particles() == 4096 and weather.dust_seed() == 4294967295 and weather.dust_strength() == 1)
        local r, g, b = weather.dust_color()
        assert(r == 1 and g == 0 and b == 0.5)
        weather.set_dust{enabled = 0, wind_start = -1, wind_full = -1, manual = -1, rate = -1, box_xz_m = -1, height_m = -1,
                         size_m = -1, lifetime_s = -1, speed = -1, wind_follow = -1, max_particles = -1, seed = -1, strength = -1}
        weather.set_dust{}  -- nothing given, nothing changes
    end
    if t == 5 then
        assert(weather.dust_enabled() == 0 and weather.dust_wind_start() == 0 and weather.dust_wind_full() == 0)
        assert(weather.dust_manual() == 0 and weather.dust_rate() == 0 and weather.dust_box_xz_m() == 1 and weather.dust_height_m() == 1)
        assert(near(weather.dust_size_m(), 0.01) and weather.dust_lifetime_s() == 0.5 and weather.dust_speed() == 0 and weather.dust_wind_follow() == 0)
        assert(weather.dust_max_particles() == 16 and weather.dust_seed() == 0 and weather.dust_strength() == 0)
    end
end
)");
    r.step(6);
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.du().strength == 0.0f);
    CHECK(r.du().enabled == 0);
    CHECK(r.du().pad_[0] == 0);
    CHECK(r.du().pad_[1] == 0);
    CHECK(r.du().pad_[2] == 0);
}

TEST_CASE("dust api: dust_blowing is the kit helper over wind, gusts and rain") {
    DustRig r(kProbeSource);
    r.weather_e = r.w.spawn_with(kit::Weather{});
    kit::WindGust g;
    g.strength = 1.0f;
    g.min_gap_s = 0.05f;
    g.max_gap_s = 0.2f;
    r.gust_e = r.w.spawn_with(g);
    u32 distinct = 0, zero = 0, nonzero = 0;
    bool saw_one = false, gust_changed = false;
    f32 last = -1.0f;
    for (bool gusts : {true, false}) {
        r.w.try_get_mut<kit::WindGust>(r.gust_e)->enabled = gusts ? 1 : 0;
        for (f32 manual : {0.0f, 0.3f}) {
            r.du().manual = manual;
            for (kit::WeatherKind kind : {kit::WeatherKind::Clear, kit::WeatherKind::Rain, kit::WeatherKind::Storm, kit::WeatherKind::Snow}) {
                for (f32 intensity : {0.0f, 0.5f, 1.0f}) {
                    for (u32 i = 0; i < 40; ++i) {
                        kit::Weather& wx = *r.w.try_get_mut<kit::Weather>(r.weather_e);
                        wx.current = wx.target = kind;
                        wx.intensity = intensity;
                        r.step(1);
                        REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
                        // the weather and gust systems ran before the script this tick, so their post-tick state is what it read
                        const kit::Weather& after = *r.w.try_get<kit::Weather>(r.weather_e);
                        const kit::WindGust& gs = *r.w.try_get<kit::WindGust>(r.gust_e);
                        const bool raining = after.current == kit::WeatherKind::Rain || after.current == kit::WeatherKind::Storm;
                        const f32 wind = kit::effective_wind(after, &gs, kit::kGustSway);
                        gust_changed = gust_changed || wind != after.wind;
                        const f32 expected = kit::dust_amount(r.du(), wind, raining ? after.intensity : 0.0f);
                        INFO("kind " << int(kind) << " intensity " << intensity << " manual " << manual << " gusts " << gusts << " i " << i);
                        CHECK(bits(r.p().position.x) == bits(expected));
                        CHECK(r.p().position.x >= 0.0f);
                        CHECK(r.p().position.x <= 1.0f);
                        saw_one = saw_one || r.p().position.x == 1.0f;
                        if (r.p().position.x > 0.0f) ++nonzero; else ++zero;
                        if (r.p().position.x != last) ++distinct;
                        last = r.p().position.x;
                    }
                }
            }
        }
    }
    CHECK(distinct > 5);
    CHECK(nonzero > 50);
    CHECK(zero > 50);
    CHECK(gust_changed);
    (void)saw_one;
    // enabled 0 and strength 0 are exactly 0
    r.w.try_get_mut<kit::Weather>(r.weather_e)->current = kit::WeatherKind::Clear;
    r.w.try_get_mut<kit::Weather>(r.weather_e)->target = kit::WeatherKind::Clear;
    r.w.try_get_mut<kit::Weather>(r.weather_e)->intensity = 0.0f;
    r.du() = kit::Dust{};
    r.du().manual = 0.3f;
    r.du().enabled = 0;
    r.step(1);
    CHECK(r.p().position.x == 0.0f);
    r.du().enabled = 1;
    r.du().strength = 0.0f;
    r.step(1);
    CHECK(r.p().position.x == 0.0f);
    r.du().strength = 1.0f;
    r.step(1);
    CHECK(r.p().position.x >= 0.3f);
}

TEST_CASE("dust api: with no Weather the wind is calm so only the manual floor shows, and a gust needs a Weather") {
    // a world with the weather installed but no Weather entity
    DustRig q(kProbeSource);
    q.step(2);
    REQUIRE(q.h().scripts()[0].state == ScriptState::Running);
    CHECK(q.p().position.x == 0.0f);
    q.du().manual = 0.625f;
    q.step(1);
    CHECK(q.p().position.x == 0.625f);
    q.du().wind_start = 0.0f;  // wind 0 is still calm
    q.du().wind_full = 0.5f;
    q.step(1);
    CHECK(q.p().position.x == 0.625f);
    // a WindGust without a Weather changes nothing
    q.gust_e = q.w.spawn_with(kit::WindGust{});
    q.step(30);
    CHECK(q.p().position.x == 0.625f);
    // a Weather brings wind and rain: the amount is then the kit helper's
    q.weather_e = q.w.spawn_with(kit::Weather{});
    kit::Weather& wx = *q.w.try_get_mut<kit::Weather>(q.weather_e);
    wx.current = wx.target = kit::WeatherKind::Storm;
    wx.intensity = 1.0f;
    q.step(1);
    const kit::Weather& after = *q.w.try_get<kit::Weather>(q.weather_e);
    const kit::WindGust& gs = *q.w.try_get<kit::WindGust>(q.gust_e);
    CHECK(bits(q.p().position.x) == bits(kit::dust_amount(q.du(), kit::effective_wind(after, &gs, kit::kGustSway), after.intensity)));
    CHECK(q.p().position.x < 0.625f);
}

TEST_CASE("dust api: a world without a Dust raises a named error and the setter never adds one") {
    for (const char* name : {"dust_enabled", "dust_wind_start", "dust_wind_full", "dust_manual", "dust_rate", "dust_box_xz_m", "dust_height_m",
                             "dust_size_m", "dust_lifetime_s", "dust_speed", "dust_wind_follow", "dust_color", "dust_max_particles",
                             "dust_seed", "dust_strength"})
        expect_error((std::string("weather.") + name + "()").c_str(),
                     (std::string("weather.") + name + ": the scene has no Dust entity").c_str(), false);
    expect_error("weather.dust_blowing()", "weather.dust_blowing: the scene has no Dust entity", false);
    expect_error("weather.set_dust{}", "weather.set_dust: the scene has no Dust entity", false);
    expect_error("weather.set_dust{strength = 0}", "weather.set_dust: the scene has no Dust entity", false);
    expect_error("weather.dust_blowing()", "weather.dust_blowing: this world has no weather", false, false);
    expect_error("weather.set_dust{}", "weather.set_dust: this world has no weather", false, false);
    DustRig r("function update()\n    if mce.tick() == 1 then weather.set_dust{enabled = 0} end\nend\n", false);
    r.step(2);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    usize n = 0;
    r.w.query<kit::Dust>().each([&](Entity, kit::Dust&) { ++n; });
    CHECK(n == 0);
}

TEST_CASE("dust api: bad arguments raise an error that names the call and write nothing") {
    expect_error("weather.set_dust{bogus = 1}", "weather.set_dust: unknown key 'bogus'");
    expect_error("weather.set_dust{leaf_color = 1}", "weather.set_dust: unknown key 'leaf_color'");
    expect_error("weather.set_dust{rate = 'fast'}", "weather.set_dust: 'rate' is not a number");
    expect_error("weather.set_dust{strength = 0/0}", "weather.set_dust: 'strength' is not a number");
    expect_error("weather.set_dust{enabled = true}", "weather.set_dust: 'enabled' is not a number");
    expect_error("weather.set_dust{speed = 'x'}", "weather.set_dust: 'speed' is not a number");
    expect_error("weather.set_dust{seed = 'x'}", "weather.set_dust: 'seed' is not a number");
    expect_error("weather.set_dust{color = 1}", "weather.set_dust: 'color' is not a table of three numbers");
    expect_error("weather.set_dust{color = {1, 2}}", "weather.set_dust: 'color' is not a table of three numbers");
    expect_error("weather.set_dust{color = {1, 'x', 3}}", "weather.set_dust: 'color' is not a table of three numbers");
    expect_error("weather.set_dust{[1] = 0.5}", "weather.set_dust: the keys are names");
    expect_error("weather.set_dust(0.5)", "table expected");
    expect_error("weather.set_dust()", "table expected");
    const kit::Dust def;
    for (const char* body : {"weather.set_dust{manual = 0.9, strength = 'x'}", "weather.set_dust{strength = 'x', manual = 0.9}",
                             "weather.set_dust{manual = 0.9, nope = 1}", "weather.set_dust{enabled = 0, rate = 'x'}",
                             "weather.set_dust{color = {0, 0, 0}, height_m = 'x'}",
                             "weather.set_dust{color = {1, 2}, manual = 0.9, enabled = 0}"}) {
        const std::string src = std::string("function update()\n    if mce.tick() == 1 then ") + body + " end\nend\n";
        DustRig r(src.c_str());
        r.step(2);
        INFO(body);
        REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
        CHECK(r.du().manual == def.manual);
        CHECK(r.du().strength == def.strength);
        CHECK(r.du().rate == def.rate);
        CHECK(r.du().enabled == def.enabled);
        CHECK(r.du().speed == def.speed);
        CHECK(r.du().height_m == def.height_m);
        CHECK(r.du().color.x == def.color.x);
        CHECK(r.du().color.z == def.color.z);
    }
}

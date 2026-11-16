// M-LEAFFALL step 21c: the leaf-fall functions of the `weather` Luau table over the first kit::LeafFall. Getters, clamping,
// leaf_falling against the kit helper over days and hours, named errors, nothing written on error, and a missing LeafFall
// raises (the setter never adds one).

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <string>

#include "mce/ecs/transform.h"
#include "mce/kit/leaf_fall.h"
#include "mce/kit/season.h"
#include "mce/kit/weather.h"
#include "mce/kit/world_clock.h"
#include "mce/script/compiler.h"

using namespace mce;
using script::ScriptHost;
using script::ScriptState;

namespace {

struct LeafRig {
    World w;
    ScriptHost host;
    u64 tick = 0;
    Entity leaf_e, probe, season_e, clock_e;

    explicit LeafRig(const char* source, bool with_leaf = true, bool install = true) {
        w.register_components<LocalTransform>();
        if (install) kit::install_weather(w, nullptr);
        INFO(source);
        Result<Array<u8>> bc = script::compile_unchecked("a", source);
        REQUIRE(bc);
        REQUIRE(host.add_bytecode("a", bc.value().span()));
        host.install(w);
        probe = w.spawn_with(LocalTransform{});
        if (with_leaf) leaf_e = w.spawn_with(kit::LeafFall{});
    }
    kit::LeafFall& leaf() { return *w.try_get_mut<kit::LeafFall>(leaf_e); }
    void step(u32 n = 1) {
        for (u32 i = 0; i < n; ++i) w.tick(1.0 / 60, ++tick);
    }
    const LocalTransform& p() { return *w.try_get<LocalTransform>(probe); }
    const ScriptHost& h() const { return host; }
};

std::string err(const ScriptHost& h) { return h.scripts()[0].error.c_str(); }

void expect_error(const char* body, const char* needle, bool with_leaf = true, bool install = true) {
    const std::string src = std::string("function update()\n    if mce.tick() == 1 then\n        ") + body + "\n    end\nend\n";
    LeafRig r(src.c_str(), with_leaf, install);
    r.step(2);
    INFO(body);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    INFO(err(r.h()));
    CHECK(err(r.h()).find(needle) != std::string::npos);
}

u32 bits(f32 v) { return std::bit_cast<u32>(v); }

}  // namespace

TEST_CASE("leaf fall api: the getters read the first LeafFall, the setter writes and clamps to the Range") {
    LeafRig r(R"(
function update()
    local t = mce.tick()
    local function near(a, b) return math.abs(a - b) < 1e-6 end
    if t == 1 then
        assert(weather.leaf_fall_enabled() == 1 and weather.leaf_fall_manual() == 0 and weather.leaf_fall_strength() == 1)
        assert(near(weather.leaf_fall_autumn_start(), 0.5) and near(weather.leaf_fall_autumn_full(), 0.6))
        assert(near(weather.leaf_fall_autumn_end_full(), 0.65) and near(weather.leaf_fall_autumn_end(), 0.75))
        assert(weather.leaf_fall_rate() == 24 and weather.leaf_fall_box_xz_m() == 14 and weather.leaf_fall_height_m() == 7)
        assert(near(weather.leaf_fall_size_m(), 0.14) and weather.leaf_fall_lifetime_s() == 7 and near(weather.leaf_fall_fall_speed(), 0.9))
        assert(weather.leaf_fall_wind_follow() == 1 and weather.leaf_fall_tint_follow() == 1)
        assert(weather.leaf_fall_max_particles() == 1024 and weather.leaf_fall_seed() == 1)
        local r, g, b = weather.leaf_fall_leaf_color()
        assert(near(r, 0.95) and near(g, 0.5) and near(b, 0.15))
        weather.set_leaf_fall{autumn_start = 0.25, autumn_full = 0.5, autumn_end_full = 0.75, autumn_end = 0.875, manual = 0.5,
                              rate = 100, box_xz_m = 30, height_m = 12, size_m = 0.5, lifetime_s = 10, fall_speed = 2,
                              wind_follow = 0.5, tint_follow = 0.25, max_particles = 2048, seed = 77, strength = 0.75,
                              leaf_color = {1, 0.5, 0.25}}
    end
    if t == 2 then
        assert(weather.leaf_fall_autumn_start() == 0.25 and weather.leaf_fall_autumn_full() == 0.5)
        assert(weather.leaf_fall_autumn_end_full() == 0.75 and weather.leaf_fall_autumn_end() == 0.875 and weather.leaf_fall_manual() == 0.5)
        assert(weather.leaf_fall_rate() == 100 and weather.leaf_fall_box_xz_m() == 30 and weather.leaf_fall_height_m() == 12)
        assert(weather.leaf_fall_size_m() == 0.5 and weather.leaf_fall_lifetime_s() == 10 and weather.leaf_fall_fall_speed() == 2)
        assert(weather.leaf_fall_wind_follow() == 0.5 and weather.leaf_fall_tint_follow() == 0.25)
        assert(weather.leaf_fall_max_particles() == 2048 and weather.leaf_fall_seed() == 77 and weather.leaf_fall_strength() == 0.75)
        local r, g, b = weather.leaf_fall_leaf_color()
        assert(r == 1 and g == 0.5 and b == 0.25)
        weather.set_leaf_fall{enabled = 0}
    end
    if t == 3 then
        assert(weather.leaf_fall_enabled() == 0 and weather.leaf_fall_rate() == 100)  -- only the given keys changed
        weather.set_leaf_fall{enabled = 7, autumn_start = 1e9, autumn_full = 1e9, autumn_end_full = 1e9, autumn_end = 1e9,
                              manual = 1e9, rate = 1e9, box_xz_m = 1e9, height_m = 1e9, size_m = 1e9, lifetime_s = 1e9,
                              fall_speed = 1e9, wind_follow = 1e9, tint_follow = 1e9, max_particles = 1e9, seed = 1e12,
                              strength = 1e9, leaf_color = {1e9, -1, 0.5}}
    end
    if t == 4 then
        assert(weather.leaf_fall_enabled() == 1 and weather.leaf_fall_autumn_start() == 1 and weather.leaf_fall_autumn_full() == 1)
        assert(weather.leaf_fall_autumn_end_full() == 1 and weather.leaf_fall_autumn_end() == 1 and weather.leaf_fall_manual() == 1)
        assert(weather.leaf_fall_rate() == 500 and weather.leaf_fall_box_xz_m() == 100 and weather.leaf_fall_height_m() == 50)
        assert(weather.leaf_fall_size_m() == 1 and weather.leaf_fall_lifetime_s() == 30 and weather.leaf_fall_fall_speed() == 5)
        assert(weather.leaf_fall_wind_follow() == 2 and weather.leaf_fall_tint_follow() == 1)
        assert(weather.leaf_fall_max_particles() == 8192 and weather.leaf_fall_seed() == 4294967295 and weather.leaf_fall_strength() == 1)
        local r, g, b = weather.leaf_fall_leaf_color()
        assert(r == 1 and g == 0 and b == 0.5)
        weather.set_leaf_fall{enabled = 0, autumn_start = -1, autumn_full = -1, autumn_end_full = -1, autumn_end = -1,
                              manual = -1, rate = -1, box_xz_m = -1, height_m = -1, size_m = -1, lifetime_s = -1,
                              fall_speed = -1, wind_follow = -1, tint_follow = -1, max_particles = -1, seed = -1, strength = -1}
        weather.set_leaf_fall{}  -- nothing given, nothing changes
    end
    if t == 5 then
        assert(weather.leaf_fall_enabled() == 0 and weather.leaf_fall_autumn_start() == 0 and weather.leaf_fall_autumn_full() == 0)
        assert(weather.leaf_fall_autumn_end_full() == 0 and weather.leaf_fall_autumn_end() == 0 and weather.leaf_fall_manual() == 0)
        assert(weather.leaf_fall_rate() == 0 and weather.leaf_fall_box_xz_m() == 1 and weather.leaf_fall_height_m() == 1)
        assert(near(weather.leaf_fall_size_m(), 0.02) and weather.leaf_fall_lifetime_s() == 1 and weather.leaf_fall_fall_speed() == 0)
        assert(weather.leaf_fall_wind_follow() == 0 and weather.leaf_fall_tint_follow() == 0)
        assert(weather.leaf_fall_max_particles() == 64 and weather.leaf_fall_seed() == 0 and weather.leaf_fall_strength() == 0)
    end
end
)");
    r.step(6);
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.leaf().strength == 0.0f);
    CHECK(r.leaf().enabled == 0);
    CHECK(r.leaf().pad_[0] == 0);
    CHECK(r.leaf().pad_[1] == 0);
    CHECK(r.leaf().pad_[2] == 0);
}

TEST_CASE("leaf fall api: leaf_falling is the kit helper over days and hours") {
    // Every tick the script writes the amount to position.x.
    LeafRig r(R"(
function update()
    mce.each("LocalTransform", function(e)
        mce.set(e, "LocalTransform", "position", vector.create(weather.leaf_falling(), 0, 0))
    end)
end
)");
    kit::Season season;
    r.season_e = r.w.spawn_with(season);
    kit::WorldClock c;
    c.paused = 1;
    c.time_scale = 0.0f;
    c.drive_environment = 0;
    r.clock_e = r.w.spawn_with(c);
    u32 distinct = 0;
    bool saw_zero = false, saw_one = false;
    f32 last = -1.0f;
    for (f32 year_days : {12.0f, 1.0f, 365.0f}) {
        for (f32 manual : {0.0f, 0.3f}) {
            r.w.try_get_mut<kit::Season>(r.season_e)->year_days = year_days;
            r.leaf().manual = manual;
            for (u32 day : {0u, 1u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 400u, 100000u}) {
                for (f32 hour : {0.0f, 5.0f, 12.0f, 23.5f}) {
                    kit::WorldClock& clk = *r.w.try_get_mut<kit::WorldClock>(r.clock_e);
                    clk.day = day;
                    clk.hour = hour;
                    r.step(1);
                    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
                    const kit::WorldClock& after = *r.w.try_get<kit::WorldClock>(r.clock_e);
                    const kit::Season& s = *r.w.try_get<kit::Season>(r.season_e);
                    const f32 expected = kit::leaf_fall_amount(r.leaf(), &s, kit::season_phase(s, after.day, after.hour));
                    INFO("year_days " << year_days << " manual " << manual << " day " << day << " hour " << hour);
                    CHECK(bits(r.p().position.x) == bits(expected));
                    CHECK(r.p().position.x >= manual);
                    CHECK(r.p().position.x <= 1.0f);
                    saw_zero = saw_zero || r.p().position.x == 0.0f;
                    saw_one = saw_one || r.p().position.x == 1.0f;
                    if (r.p().position.x != last) ++distinct;
                    last = r.p().position.x;
                }
            }
        }
    }
    CHECK(distinct > 10);
    CHECK(saw_zero);
    CHECK(saw_one);
    // enabled 0 and strength 0 are exactly 0, the Season disabled is the manual floor
    r.leaf().manual = 0.3f;
    r.leaf().enabled = 0;
    r.step(1);
    CHECK(r.p().position.x == 0.0f);
    r.leaf().enabled = 1;
    r.leaf().strength = 0.0f;
    r.step(1);
    CHECK(r.p().position.x == 0.0f);
    r.leaf().strength = 1.0f;
    r.w.try_get_mut<kit::Season>(r.season_e)->enabled = 0;
    r.step(1);
    CHECK(bits(r.p().position.x) == bits(0.3f));
}

TEST_CASE("leaf fall api: with no clock the phase is the fixed start_phase, with no Season the amount is the manual floor") {
    LeafRig r(R"(
function update()
    mce.each("LocalTransform", function(e)
        mce.set(e, "LocalTransform", "position", vector.create(weather.leaf_falling(), 0, 0))
    end)
end
)");
    // no Season, no clock: manual only, never nil
    r.step(2);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.p().position.x == 0.0f);
    r.leaf().manual = 0.625f;
    r.step(1);
    CHECK(r.p().position.x == 0.625f);
    // a Season and no clock
    r.season_e = r.w.spawn_with(kit::Season{});
    r.leaf().manual = 0.0f;
    for (f32 start : {0.0f, 0.3f, 0.625f, 0.8f, 0.95f, 0.55f}) {
        r.w.try_get_mut<kit::Season>(r.season_e)->start_phase = start;
        r.step(2);
        REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
        const kit::Season& s = *r.w.try_get<kit::Season>(r.season_e);
        INFO("start_phase " << start);
        CHECK(bits(r.p().position.x) == bits(kit::leaf_fall_amount(r.leaf(), &s, kit::season_phase(s, 0, 0.0f))));
    }
    CHECK(r.p().position.x > 0.0f);  // 0.55 is inside the rising edge of autumn
}

TEST_CASE("leaf fall api: a world without a LeafFall raises a named error and the setter never adds one") {
    for (const char* name : {"leaf_fall_enabled", "leaf_fall_autumn_start", "leaf_fall_autumn_full", "leaf_fall_autumn_end_full",
                             "leaf_fall_autumn_end", "leaf_fall_manual", "leaf_fall_rate", "leaf_fall_box_xz_m",
                             "leaf_fall_height_m", "leaf_fall_size_m", "leaf_fall_lifetime_s", "leaf_fall_fall_speed",
                             "leaf_fall_wind_follow", "leaf_fall_leaf_color", "leaf_fall_tint_follow", "leaf_fall_max_particles",
                             "leaf_fall_seed", "leaf_fall_strength"})
        expect_error((std::string("weather.") + name + "()").c_str(),
                     (std::string("weather.") + name + ": the scene has no LeafFall entity").c_str(), false);
    expect_error("weather.leaf_falling()", "weather.leaf_falling: the scene has no LeafFall entity", false);
    expect_error("weather.set_leaf_fall{}", "weather.set_leaf_fall: the scene has no LeafFall entity", false);
    expect_error("weather.set_leaf_fall{strength = 0}", "weather.set_leaf_fall: the scene has no LeafFall entity", false);
    expect_error("weather.leaf_falling()", "weather.leaf_falling: this world has no weather", false, false);
    expect_error("weather.set_leaf_fall{}", "weather.set_leaf_fall: this world has no weather", false, false);
    LeafRig r("function update()\n    if mce.tick() == 1 then weather.set_leaf_fall{enabled = 0} end\nend\n", false);
    r.step(2);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    usize n = 0;
    r.w.query<kit::LeafFall>().each([&](Entity, kit::LeafFall&) { ++n; });
    CHECK(n == 0);
}

TEST_CASE("leaf fall api: bad arguments raise an error that names the call and write nothing") {
    expect_error("weather.set_leaf_fall{bogus = 1}", "weather.set_leaf_fall: unknown key 'bogus'");
    expect_error("weather.set_leaf_fall{color = 1}", "weather.set_leaf_fall: unknown key 'color'");
    expect_error("weather.set_leaf_fall{rate = 'fast'}", "weather.set_leaf_fall: 'rate' is not a number");
    expect_error("weather.set_leaf_fall{strength = 0/0}", "weather.set_leaf_fall: 'strength' is not a number");
    expect_error("weather.set_leaf_fall{enabled = true}", "weather.set_leaf_fall: 'enabled' is not a number");
    expect_error("weather.set_leaf_fall{seed = 'x'}", "weather.set_leaf_fall: 'seed' is not a number");
    expect_error("weather.set_leaf_fall{leaf_color = 1}", "weather.set_leaf_fall: 'leaf_color' is not a table of three numbers");
    expect_error("weather.set_leaf_fall{leaf_color = {1, 2}}", "weather.set_leaf_fall: 'leaf_color' is not a table of three numbers");
    expect_error("weather.set_leaf_fall{leaf_color = {1, 'x', 3}}", "weather.set_leaf_fall: 'leaf_color' is not a table of three numbers");
    expect_error("weather.set_leaf_fall{[1] = 0.5}", "weather.set_leaf_fall: the keys are names");
    expect_error("weather.set_leaf_fall(0.5)", "table expected");
    expect_error("weather.set_leaf_fall()", "table expected");
    const kit::LeafFall def;
    for (const char* body : {"weather.set_leaf_fall{manual = 0.9, strength = 'x'}", "weather.set_leaf_fall{strength = 'x', manual = 0.9}",
                             "weather.set_leaf_fall{manual = 0.9, nope = 1}", "weather.set_leaf_fall{enabled = 0, rate = 'x'}",
                             "weather.set_leaf_fall{leaf_color = {0, 0, 0}, height_m = 'x'}",
                             "weather.set_leaf_fall{leaf_color = {1, 2}, manual = 0.9, enabled = 0}"}) {
        const std::string src = std::string("function update()\n    if mce.tick() == 1 then ") + body + " end\nend\n";
        LeafRig r(src.c_str());
        r.step(2);
        INFO(body);
        REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
        CHECK(r.leaf().manual == def.manual);
        CHECK(r.leaf().strength == def.strength);
        CHECK(r.leaf().rate == def.rate);
        CHECK(r.leaf().enabled == def.enabled);
        CHECK(r.leaf().height_m == def.height_m);
        CHECK(r.leaf().leaf_color.x == def.leaf_color.x);
        CHECK(r.leaf().leaf_color.z == def.leaf_color.z);
    }
}

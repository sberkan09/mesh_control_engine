// M-WATER step 15d: the `water` Luau table over the first kit::Water. Getters, clamping, height_at / normal_at against
// the kit helpers over positions and times, submerged at the boundary, named errors and nothing written on error.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <string>

#include "mce/ecs/transform.h"
#include "mce/kit/water.h"
#include "mce/kit/weather.h"
#include "mce/kit/wind_direction.h"
#include "mce/script/compiler.h"

using namespace mce;
using script::ScriptHost;
using script::ScriptState;

namespace {

struct WaterRig {
    World w;
    ScriptHost host;
    u64 tick = 0;
    Entity water_e, wind_e, probe;

    explicit WaterRig(const char* source, bool with_water = true, bool install = true, bool with_wind = false) {
        w.register_components<LocalTransform>();
        if (install) kit::install_weather(w, nullptr);
        INFO(source);
        Result<Array<u8>> bc = script::compile_unchecked("w", source);
        REQUIRE(bc);
        REQUIRE(host.add_bytecode("w", bc.value().span()));
        host.install(w);
        probe = w.spawn_with(LocalTransform{});
        if (with_water) water_e = w.spawn_with(kit::Water{});
        if (with_wind) wind_e = w.spawn_with(kit::WindDirection{});
    }
    kit::Water& water() { return *w.try_get_mut<kit::Water>(water_e); }
    void step(u32 n = 1) {
        for (u32 i = 0; i < n; ++i) w.tick(1.0 / 60, ++tick);
    }
    const LocalTransform& p() { return *w.try_get<LocalTransform>(probe); }
    const ScriptHost& h() const { return host; }
};

std::string err(const ScriptHost& h) { return h.scripts()[0].error.c_str(); }

void expect_error(const char* body, const char* needle, bool with_water = true, bool install = true) {
    const std::string src = std::string("function update()\n    if mce.tick() == 1 then\n        ") + body + "\n    end\nend\n";
    WaterRig r(src.c_str(), with_water, install);
    r.step(2);
    INFO(body);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    INFO(err(r.h()));
    CHECK(err(r.h()).find(needle) != std::string::npos);
}

u32 bits(f32 v) { return std::bit_cast<u32>(v); }

// The scene time the binding uses: tick * dt (the f32 fixed step) in f64.
f64 scene_time(u64 tick) { return static_cast<f64>(tick) * static_cast<f64>(static_cast<f32>(1.0 / 60)); }

}  // namespace

TEST_CASE("water api: the getters read the first Water, the setter writes and clamps to the Range") {
    WaterRig r(R"(
function update()
    local t = mce.tick()
    if t == 1 then
        assert(water.level() == 0 and water.half_extent() == 500 and water.wave_count() == 4)
        assert(water.amplitude() > 0.399 and water.amplitude() < 0.401 and water.wavelength() == 24)
        assert(water.steepness() > 0.599 and water.steepness() < 0.601 and water.wind_dir() == 0 and water.wind_follow() == 1)
        assert(water.speed_scale() == 1 and water.depth_scale() == 8 and water.reflectivity() == 1)
        assert(water.roughness() > 0.079 and water.roughness() < 0.081 and water.foam_amount() == 0.5)
        assert(water.foam_width() == 1.5 and water.enabled() == 1)
        local r, g, b = water.shallow_color()
        assert(r > 0.099 and r < 0.101 and g > 0.379 and g < 0.381 and b > 0.399 and b < 0.401)
        r, g, b = water.deep_color()
        assert(r > 0.009 and r < 0.011 and g > 0.079 and g < 0.081 and b > 0.149 and b < 0.151)
        water.set{level = 3.5, wave_count = 6.9, amplitude = 1, wind_dir = 90, shallow_color = {0.5, 0.25, 1}, deep_color = {0, 0.5, 0.75}}
    end
    if t == 2 then
        assert(water.level() == 3.5 and water.wave_count() == 6 and water.amplitude() == 1 and water.wind_dir() == 90)
        local r, g, b = water.shallow_color()
        assert(r == 0.5 and g == 0.25 and b == 1)
        r, g, b = water.deep_color()
        assert(r == 0 and g == 0.5 and b == 0.75)
        assert(water.half_extent() == 500 and water.enabled() == 1)  -- only the given keys changed
        water.set{level = 1e9, half_extent = 1e9, wave_count = 1e9, amplitude = 1e9, wavelength = 1e9, steepness = 1e9,
                  wind_dir = 1e9, wind_follow = 1e9, speed_scale = 1e9, shallow_color = {9, 9, 9}, deep_color = {9, 9, 9},
                  depth_scale = 1e9, reflectivity = 1e9, roughness = 1e9, foam_amount = 1e9, foam_width = 1e9, enabled = 0}
    end
    if t == 3 then
        assert(water.level() == 1000 and water.half_extent() == 20000 and water.wave_count() == 8)
        assert(water.amplitude() == 10 and water.wavelength() == 500 and water.steepness() == 1)
        assert(water.wind_dir() == 360 and water.wind_follow() == 1 and water.speed_scale() == 4)
        local r, g, b = water.shallow_color()
        assert(r == 1 and g == 1 and b == 1)
        r, g, b = water.deep_color()
        assert(r == 1 and g == 1 and b == 1)
        assert(water.depth_scale() == 200 and water.reflectivity() == 1 and water.roughness() == 1)
        assert(water.foam_amount() == 1 and water.foam_width() == 20 and water.enabled() == 0)
        water.set{level = -1e9, half_extent = -1, wave_count = 0, amplitude = -1, wavelength = 0, steepness = -1,
                  wind_dir = -5, wind_follow = -1, speed_scale = -1, shallow_color = {-1, -1, -1}, deep_color = {-1, -1, -1},
                  depth_scale = 0, reflectivity = -1, roughness = -1, foam_amount = -1, foam_width = 0, enabled = 7}
        water.set{}  -- nothing given, nothing changes
    end
    if t == 4 then
        assert(water.level() == -1000 and water.half_extent() == 1 and water.wave_count() == 1)
        assert(water.amplitude() == 0 and water.wavelength() == 1 and water.steepness() == 0)
        assert(water.wind_dir() == 0 and water.wind_follow() == 0 and water.speed_scale() == 0)
        local r, g, b = water.shallow_color()
        assert(r == 0 and g == 0 and b == 0)
        assert(water.depth_scale() > 0.099 and water.depth_scale() < 0.101 and water.reflectivity() == 0)
        assert(water.roughness() == 0 and water.foam_amount() == 0)
        assert(water.foam_width() > 0.049 and water.foam_width() < 0.051 and water.enabled() == 1)
    end
end
)");
    r.step(5);
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.water().wave_count == 1);
    CHECK(r.water().enabled == 1);
}

TEST_CASE("water api: height_at and normal_at are the kit helpers over positions and times") {
    // Every tick the script writes the height to the probe's position.x and the normal to its scale, at a position that
    // moves with the tick.
    WaterRig r(R"(
function update()
    local t = mce.tick()
    local x, z = t * 0.7 - 5.0, t * 0.3 + 2.0
    local nx, ny, nz = water.normal_at(x, z)
    mce.each("LocalTransform", function(e)
        mce.set(e, "LocalTransform", "position", vector.create(water.height_at(x, z), 0, 0))
        mce.set(e, "LocalTransform", "scale", vector.create(nx, ny, nz))
    end)
end
)");
    r.water().level_y = 2.0f;
    r.water().wind_dir_deg = 30.0f;
    r.water().wind_follow = 0.0f;
    r.water().amplitude = 1.5f;
    for (u32 i = 1; i <= 400; ++i) {
        r.step(1);
        REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
        const f64 x = static_cast<f64>(i) * 0.7 - 5.0, z = static_cast<f64>(i) * 0.3 + 2.0;
        const math::vec2 p{static_cast<f32>(x), static_cast<f32>(z)};
        const f64 t = scene_time(i);
        CHECK(bits(r.p().position.x) == bits(kit::water_height_at(r.water(), p, t)));
        const math::vec3 n = kit::water_normal_at(r.water(), p, t);
        CHECK(bits(r.p().scale.x) == bits(n.x));
        CHECK(bits(r.p().scale.y) == bits(n.y));
        CHECK(bits(r.p().scale.z) == bits(n.z));
    }
    // A later time than one period still matches (the time wraps in f64).
    r.step(4000);
    const math::vec2 p{static_cast<f32>(4400 * 0.7 - 5.0), static_cast<f32>(4400 * 0.3 + 2.0)};
    CHECK(bits(r.p().position.x) == bits(kit::water_height_at(r.water(), p, scene_time(4400))));
    // Heights stay within the level +- the amplitudes, and the normal is a unit vector pointing up.
    CHECK(r.p().position.x <= 2.0f + 1.5f * 2.0f);
    CHECK(r.p().position.x >= 2.0f - 1.5f * 2.0f);
    const f32 len = r.p().scale.x * r.p().scale.x + r.p().scale.y * r.p().scale.y + r.p().scale.z * r.p().scale.z;
    CHECK(len == doctest::Approx(1.0f).epsilon(1e-4));
    CHECK(r.p().scale.y > 0.0f);
}

TEST_CASE("water api: wind_follow takes the WindDirection heading, wind_follow 0 the authored direction") {
    WaterRig r(R"(
function update()
    local nx, ny, nz = water.normal_at(10.0, 20.0)
    mce.each("LocalTransform", function(e)
        mce.set(e, "LocalTransform", "position", vector.create(water.height_at(10.0, 20.0), 0, 0))
        mce.set(e, "LocalTransform", "scale", vector.create(nx, ny, nz))
    end)
end
)",
               true, true, true);
    r.water().wind_dir_deg = 10.0f;
    r.w.try_get_mut<kit::WindDirection>(r.wind_e)->degrees = 120.0f;
    r.step(30);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    const math::vec2 p{10.0f, 20.0f};
    CHECK(bits(r.p().position.x) == bits(kit::water_height_at(r.water(), p, scene_time(30), 120.0f)));
    CHECK(r.p().position.x != kit::water_height_at(r.water(), p, scene_time(30), 10.0f));
    r.water().wind_follow = 0.0f;
    r.step(1);
    CHECK(bits(r.p().position.x) == bits(kit::water_height_at(r.water(), p, scene_time(31), 10.0f)));
}

TEST_CASE("water api: submerged is true below the wave height, false at it and above, and a flat sea is its level") {
    WaterRig r(R"(
function update()
    local t = mce.tick()
    local x, z = t * 1.3 - 20.0, t * 0.9
    local h = water.height_at(x, z)
    assert(water.submerged(x, h - 0.01, z))
    assert(not water.submerged(x, h, z))
    assert(not water.submerged(x, h + 0.01, z))
    assert(water.submerged(x, -1e6, z) and not water.submerged(x, 1e6, z))
    if t == 50 then water.set{amplitude = 0, level = 4} end
    if t == 51 then
        assert(water.height_at(x, z) == 4)
        local nx, ny, nz = water.normal_at(x, z)
        assert(nx == 0 and ny == 1 and nz == 0)
        assert(water.submerged(x, 3.99, z) and not water.submerged(x, 4, z))
    end
end
)");
    r.step(60);
    INFO(err(r.h()));
    CHECK(r.h().scripts()[0].state == ScriptState::Running);
}

TEST_CASE("water api: a world without a Water raises a named error and the setter never adds one") {
    for (const char* name : {"level", "half_extent", "wave_count", "amplitude", "wavelength", "steepness", "wind_dir",
                             "wind_follow", "speed_scale", "shallow_color", "deep_color", "depth_scale", "reflectivity",
                             "roughness", "foam_amount", "foam_width", "enabled"})
        expect_error((std::string("water.") + name + "()").c_str(),
                     (std::string("water.") + name + ": the scene has no Water entity").c_str(), false);
    expect_error("water.height_at(0, 0)", "water.height_at: the scene has no Water entity", false);
    expect_error("water.normal_at(0, 0)", "water.normal_at: the scene has no Water entity", false);
    expect_error("water.submerged(0, 0, 0)", "water.submerged: the scene has no Water entity", false);
    expect_error("water.set{}", "water.set: the scene has no Water entity", false);
    expect_error("water.level()", "water.level: this world has no weather", false, false);
    expect_error("water.set{}", "water.set: this world has no weather", false, false);
    WaterRig r("function update()\n    if mce.tick() == 1 then water.set{enabled = 0} end\nend\n", false);
    r.step(2);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    usize n = 0;
    r.w.query<kit::Water>().each([&](Entity, kit::Water&) { ++n; });
    CHECK(n == 0);
}

TEST_CASE("water api: bad arguments raise an error that names the call and write nothing") {
    expect_error("water.set{bogus = 1}", "water.set: unknown key 'bogus'");
    expect_error("water.set{level_y = 1}", "water.set: unknown key 'level_y'");
    expect_error("water.set{level = 'high'}", "water.set: 'level' is not a number");
    expect_error("water.set{amplitude = 0/0}", "water.set: 'amplitude' is not a number");
    expect_error("water.set{enabled = true}", "water.set: 'enabled' is not a number");
    expect_error("water.set{shallow_color = 1}", "water.set: 'shallow_color' is not a table of three numbers");
    expect_error("water.set{deep_color = {1, 2}}", "water.set: 'deep_color' is not a table of three numbers");
    expect_error("water.set{deep_color = {1, 'x', 3}}", "water.set: 'deep_color' is not a table of three numbers");
    expect_error("water.set{[1] = 0.5}", "water.set: the keys are names");
    expect_error("water.set(0.5)", "table expected");
    expect_error("water.set()", "table expected");
    expect_error("water.height_at('a', 0)", "number expected");
    expect_error("water.height_at(0)", "number expected");
    expect_error("water.height_at(1/0, 0)", "water.height_at: x is not a finite number");
    expect_error("water.normal_at(0, 0/0)", "water.normal_at: z is not a finite number");
    expect_error("water.submerged(0, 1/0, 0)", "water.submerged: y is not a finite number");
    const kit::Water def;
    for (const char* body : {"water.set{level = 9, amplitude = 'x'}", "water.set{amplitude = 'x', level = 9}",
                             "water.set{level = 9, nope = 1}", "water.set{level = 9, deep_color = {1, 1}}",
                             "water.set{deep_color = {1, 1}, level = 9}", "water.set{level = 9, shallow_color = {1, 1, 1}, deep_color = 'x'}",
                             "water.set{deep_color = 'x', level = 9, shallow_color = {1, 1, 1}}"}) {
        const std::string src = std::string("function update()\n    if mce.tick() == 1 then ") + body + " end\nend\n";
        WaterRig r(src.c_str());
        r.step(2);
        INFO(body);
        REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
        CHECK(r.water().level_y == def.level_y);
        CHECK(r.water().amplitude == def.amplitude);
        CHECK(r.water().shallow_color.x == def.shallow_color.x);
        CHECK(r.water().deep_color.y == def.deep_color.y);
    }
}

// M-WATER2 step 49g: the current in the `water` Luau table over the first kit::WaterFlow. Getters, clamping, named
// errors and nothing written on error, set_flow never adds a WaterFlow, flow_at is the kit's velocity, and height_at /
// normal_at / submerged follow the flowing wave set (bit for bit against kit::water_height_at / water_normal_at with
// the WaterFlow) over 400 ticks and past the 64 s period.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <string>

#include "mce/ecs/transform.h"
#include "mce/kit/water.h"
#include "mce/kit/water_flow.h"
#include "mce/kit/weather.h"
#include "mce/script/compiler.h"

using namespace mce;
using script::ScriptHost;
using script::ScriptState;

namespace {

struct FlowRig {
    World w;
    ScriptHost host;
    u64 tick = 0;
    Entity water_e, flow_e, probe;

    explicit FlowRig(const char* source, bool with_water = true, bool with_flow = true, bool install = true) {
        w.register_components<LocalTransform>();
        if (install) kit::install_weather(w, nullptr);
        INFO(source);
        Result<Array<u8>> bc = script::compile_unchecked("f", source);
        REQUIRE(bc);
        REQUIRE(host.add_bytecode("f", bc.value().span()));
        host.install(w);
        probe = w.spawn_with(LocalTransform{});
        if (with_water) water_e = w.spawn_with(kit::Water{});
        if (with_flow) flow_e = w.spawn_with(kit::WaterFlow{});
    }
    kit::Water& water() { return *w.try_get_mut<kit::Water>(water_e); }
    kit::WaterFlow& flow() { return *w.try_get_mut<kit::WaterFlow>(flow_e); }
    void step(u32 n = 1) {
        for (u32 i = 0; i < n; ++i) w.tick(1.0 / 60, ++tick);
    }
    const LocalTransform& p() { return *w.try_get<LocalTransform>(probe); }
    const ScriptHost& h() const { return host; }
};

std::string err(const ScriptHost& h) { return h.scripts()[0].error.c_str(); }

void expect_error(const char* body, const char* needle, bool with_water = true, bool with_flow = true,
                  bool install = true) {
    const std::string src =
        std::string("function update()\n    if mce.tick() == 1 then\n        ") + body + "\n    end\nend\n";
    FlowRig r(src.c_str(), with_water, with_flow, install);
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

TEST_CASE("water flow api: the getters read the first WaterFlow, set_flow writes and clamps") {
    FlowRig r(R"(
function update()
    local t = mce.tick()
    if t == 1 then
        assert(water.flow_enabled() == 1 and water.flow_dir() == 0 and water.flow_speed() == 0)
        water.set_flow{dir = 135, speed = 1.25}
    end
    if t == 2 then
        assert(water.flow_dir() == 135 and water.flow_speed() == 1.25 and water.flow_enabled() == 1)
        water.set_flow{dir = 1e9, speed = 1e9, enabled = 0}
    end
    if t == 3 then
        assert(water.flow_dir() == 360 and water.flow_speed() == 5 and water.flow_enabled() == 0)
        water.set_flow{dir = -5, speed = -1, enabled = 7}
        water.set_flow{}  -- nothing given, nothing changes
    end
    if t == 4 then
        assert(water.flow_dir() == 0 and water.flow_speed() == 0 and water.flow_enabled() == 1)
        water.set_flow{speed = 2}
    end
end
)");
    r.step(5);
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.flow().speed == 2.0f);
    CHECK(r.flow().dir_deg == 0.0f);
    CHECK(r.flow().enabled == 1);
    for (u8 b : r.flow().pad_) CHECK(b == 0);
}

TEST_CASE("water flow api: flow_at is the kit's velocity, and 0, 0 with a Water but no WaterFlow") {
    // The script writes flow_at to the probe's position x and z every tick.
    const char* src = R"(
function update()
    local vx, vz = water.flow_at(12.5, -40.0)
    mce.each("LocalTransform", function(e)
        mce.set(e, "LocalTransform", "position", vector.create(vx, 0, vz))
    end)
end
)";
    FlowRig r(src);
    r.flow().dir_deg = 30.0f;
    r.flow().speed = 1.2f;
    r.step(1);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    const math::vec2 v = kit::water_flow_velocity(&r.flow());
    CHECK(bits(r.p().position.x) == bits(v.x));
    CHECK(bits(r.p().position.z) == bits(v.y));
    CHECK(r.p().position.x == doctest::Approx(1.2 * std::cos(30.0 * 3.14159265358979 / 180.0)).epsilon(1e-5));
    CHECK(r.p().position.z == doctest::Approx(1.2 * std::sin(30.0 * 3.14159265358979 / 180.0)).epsilon(1e-5));
    r.flow().enabled = 0;
    r.step(1);
    CHECK(r.p().position.x == 0.0f);
    CHECK(r.p().position.z == 0.0f);

    FlowRig still(src, true, false);
    still.step(1);
    INFO(err(still.h()));
    REQUIRE(still.h().scripts()[0].state == ScriptState::Running);
    CHECK(still.p().position.x == 0.0f);
    CHECK(still.p().position.z == 0.0f);
}

TEST_CASE("water flow api: height_at and normal_at are the kit's flowing helpers over 400 ticks and past a period") {
    FlowRig r(R"(
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
    r.water().wind_dir_deg = 30.0f;
    r.water().wind_follow = 0.0f;
    r.water().amplitude = 0.3f;
    r.water().wavelength = 3.0f;
    r.flow().dir_deg = 200.0f;
    r.flow().speed = 2.5f;
    usize differs = 0;
    for (u32 i = 1; i <= 400; ++i) {
        r.step(1);
        REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
        const f64 x = static_cast<f64>(i) * 0.7 - 5.0, z = static_cast<f64>(i) * 0.3 + 2.0;
        const math::vec2 p{static_cast<f32>(x), static_cast<f32>(z)};
        const f64 t = scene_time(i);
        CHECK(bits(r.p().position.x) == bits(kit::water_height_at(r.water(), &r.flow(), p, t)));
        const math::vec3 n = kit::water_normal_at(r.water(), &r.flow(), p, t);
        CHECK(bits(r.p().scale.x) == bits(n.x));
        CHECK(bits(r.p().scale.y) == bits(n.y));
        CHECK(bits(r.p().scale.z) == bits(n.z));
        if (bits(r.p().position.x) != bits(kit::water_height_at(r.water(), p, t))) ++differs;
    }
    CHECK(differs > 300);  // the current moves the surface: the still heights are not what the script reads
    // Past one period the flowing surface still matches (the time wraps in f64, every frequency a whole multiple).
    r.step(4000);
    const math::vec2 p{static_cast<f32>(4400 * 0.7 - 5.0), static_cast<f32>(4400 * 0.3 + 2.0)};
    CHECK(bits(r.p().position.x) == bits(kit::water_height_at(r.water(), &r.flow(), p, scene_time(4400))));
    // A disabled current gives the still set's bits.
    r.flow().enabled = 0;
    r.step(1);
    const math::vec2 q{static_cast<f32>(4401 * 0.7 - 5.0), static_cast<f32>(4401 * 0.3 + 2.0)};
    CHECK(bits(r.p().position.x) == bits(kit::water_height_at(r.water(), q, scene_time(4401))));
}

TEST_CASE("water flow api: submerged follows the flowing height") {
    FlowRig r(R"(
function update()
    local t = mce.tick()
    local x, z = t * 1.3 - 20.0, t * 0.9
    local h = water.height_at(x, z)
    assert(water.submerged(x, h - 0.001, z))
    assert(not water.submerged(x, h, z))
    assert(not water.submerged(x, h + 0.001, z))
    mce.each("LocalTransform", function(e)
        mce.set(e, "LocalTransform", "position", vector.create(h, 0, 0))
    end)
end
)");
    r.water().amplitude = 0.5f;
    r.water().wavelength = 4.0f;
    r.flow().speed = 3.0f;
    r.flow().dir_deg = 90.0f;
    r.step(120);
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    const math::vec2 p{static_cast<f32>(120 * 1.3 - 20.0), static_cast<f32>(120 * 0.9)};
    CHECK(bits(r.p().position.x) == bits(kit::water_height_at(r.water(), &r.flow(), p, scene_time(120))));
}

TEST_CASE("water flow api: a scene without a WaterFlow raises a named error and set_flow never adds one") {
    for (const char* name : {"flow_enabled", "flow_dir", "flow_speed"})
        expect_error((std::string("water.") + name + "()").c_str(),
                     (std::string("water.") + name + ": the scene has no WaterFlow entity").c_str(), true, false);
    expect_error("water.set_flow{}", "water.set_flow: the scene has no WaterFlow entity", true, false);
    expect_error("water.set_flow{speed = 1}", "water.set_flow: the scene has no WaterFlow entity", false, false);
    expect_error("water.flow_at(0, 0)", "water.flow_at: the scene has no Water entity", false, true);
    expect_error("water.flow_dir()", "water.flow_dir: this world has no weather", false, false, false);
    expect_error("water.set_flow{}", "water.set_flow: this world has no weather", false, false, false);
    FlowRig r("function update()\n    if mce.tick() == 1 then water.set_flow{speed = 1} end\nend\n", true, false);
    r.step(2);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    usize n = 0;
    r.w.query<kit::WaterFlow>().each([&](Entity, kit::WaterFlow&) { ++n; });
    CHECK(n == 0);
}

TEST_CASE("water flow api: bad arguments raise an error that names the call and write nothing") {
    expect_error("water.set_flow{bogus = 1}", "water.set_flow: unknown key 'bogus'");
    expect_error("water.set_flow{dir_deg = 1}", "water.set_flow: unknown key 'dir_deg'");
    expect_error("water.set_flow{speed = 'fast'}", "water.set_flow: 'speed' is not a number");
    expect_error("water.set_flow{dir = 0/0}", "water.set_flow: 'dir' is not a number");
    expect_error("water.set_flow{enabled = true}", "water.set_flow: 'enabled' is not a number");
    expect_error("water.set_flow{[1] = 0.5}", "water.set_flow: the keys are names");
    expect_error("water.set_flow(0.5)", "table expected");
    expect_error("water.set_flow()", "table expected");
    expect_error("water.flow_at('a', 0)", "number expected");
    expect_error("water.flow_at(0)", "number expected");
    expect_error("water.flow_at(1/0, 0)", "water.flow_at: x is not a finite number");
    expect_error("water.flow_at(0, 0/0)", "water.flow_at: z is not a finite number");
    const kit::WaterFlow def;
    for (const char* body :
         {"water.set_flow{speed = 3, dir = 'x'}", "water.set_flow{dir = 'x', speed = 3}",
          "water.set_flow{speed = 3, nope = 1}", "water.set_flow{enabled = 0, speed = 3, dir = 0/0}"}) {
        const std::string src = std::string("function update()\n    if mce.tick() == 1 then ") + body + " end\nend\n";
        FlowRig r(src.c_str());
        r.step(2);
        INFO(body);
        REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
        CHECK(r.flow().speed == def.speed);
        CHECK(r.flow().dir_deg == def.dir_deg);
        CHECK(r.flow().enabled == def.enabled);
    }
}

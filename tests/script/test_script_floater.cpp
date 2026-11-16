// M-FLOAT step 55c: the `floater` Luau table over one entity's kit::Floater. Getters, set with clamping, named errors
// and nothing written on error, set never adds a Floater, and a scripted tilt change reaches kit_floater.

#include <doctest/doctest.h>

#include <string>

#include "mce/ecs/transform.h"
#include "mce/kit/floater.h"
#include "mce/kit/water.h"
#include "mce/kit/weather.h"
#include "mce/script/compiler.h"

using namespace mce;
using script::ScriptHost;
using script::ScriptState;

namespace {

struct FRig {
    World w;
    ScriptHost host;
    u64 tick = 0;
    Entity boat, rock;

    explicit FRig(const char* source) {
        w.register_components<LocalTransform>();
        kit::install_weather(w, nullptr);
        INFO(source);
        Result<Array<u8>> bc = script::compile_unchecked("f", source);
        REQUIRE(bc);
        REQUIRE(host.add_bytecode("f", bc.value().span()));
        host.install(w);
        kit::Water water;
        water.amplitude = 1.0f;
        w.spawn_with(water);
        boat = w.spawn_with(LocalTransform{}, kit::Floater{});
        rock = w.spawn_with(LocalTransform{});
    }
    kit::Floater& floater() { return *w.try_get_mut<kit::Floater>(boat); }
    void step(u32 n = 1) {
        for (u32 i = 0; i < n; ++i) w.tick(1.0 / 60, ++tick);
    }
};

std::string err(const ScriptHost& h) { return h.scripts()[0].error.c_str(); }

void expect_error(const char* body, const char* needle) {
    const std::string src = std::string("function update()\n    if mce.tick() == 1 then\n        ") + body +
                            "\n    end\nend\n";
    FRig r(src.c_str());
    r.step(2);
    INFO(body);
    REQUIRE(r.host.scripts()[0].state == ScriptState::Failed);
    INFO(err(r.host));
    CHECK(err(r.host).find(needle) != std::string::npos);
}

}  // namespace

TEST_CASE("floater api: getters read the fields, set writes and clamps") {
    FRig r(R"(
function update()
    local t = mce.tick()
    mce.each("Floater", function(e)
        if t == 1 then
            assert(floater.enabled(e) == 1 and floater.offset(e) == 0 and floater.tilt(e) == 1 and floater.follow(e) == 1)
            floater.set(e, { offset = 0.5, tilt = 0.25, follow = 0.5 })
        end
        if t == 2 then
            assert(floater.offset(e) == 0.5 and floater.tilt(e) == 0.25 and floater.follow(e) == 0.5)
            floater.set(e, { offset = 900, tilt = -3, follow = 7, enabled = 0 })
        end
        if t == 3 then
            assert(floater.offset(e) == 50 and floater.tilt(e) == 0 and floater.follow(e) == 1 and floater.enabled(e) == 0)
        end
    end)
end
)");
    r.step(4);
    CHECK(r.host.scripts()[0].state != ScriptState::Failed);
    CHECK(r.floater().offset_y == 50.0f);
    CHECK(r.floater().enabled == 0);
}

TEST_CASE("floater api: named errors, nothing written on error, set never adds a Floater") {
    expect_error("mce.each('Floater', function(e) floater.set(e, { nope = 1 }) end)", "floater.set: unknown key 'nope'");
    expect_error("mce.each('Floater', function(e) floater.set(e, { tilt = 'x' }) end)", "floater.set");
    expect_error("mce.each('Floater', function(e) floater.set(e, { tilt = 0.5, offset = 0 / 0 }) end)", "floater.set");
    // An entity without a Floater: the Floater entity count in the scene is 1, so use a plain LocalTransform holder.
    expect_error("mce.each('LocalTransform', function(e) floater.set(e, { tilt = 0.5 }) end)",
                 "the entity has no Floater");
    expect_error("mce.each('LocalTransform', function(e) floater.offset(e) end)", "the entity has no Floater");
    FRig r(R"(
function update()
    if mce.tick() == 1 then
        mce.each("Floater", function(e) pcall(floater.set, e, { tilt = 0.5, offset = 0 / 0 }) end)
    end
end
)");
    r.step(2);
    CHECK(r.floater().tilt == 1.0f);  // the bad table wrote nothing
}

TEST_CASE("floater api: a scripted tilt reaches kit_floater") {
    FRig r(R"(
function update()
    if mce.tick() == 2 then
        mce.each("Floater", function(e) floater.set(e, { tilt = 0 }) end)
    end
end
)");
    r.step(60);
    const LocalTransform& lt = *r.w.try_get<LocalTransform>(r.boat);
    // tilt 0: the up axis is straight up, so the rotation is a pure twist about Y (x and z parts are 0).
    CHECK(lt.rotation.x == 0.0f);
    CHECK(lt.rotation.z == 0.0f);
}

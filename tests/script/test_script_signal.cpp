// M-SIGNALHEADS step 56c: the `signal` Luau table over one entity's kit::SignalHead. Getters, set, named errors, nothing
// written on error, set never adds a SignalHead, and state answers "none" without a road network.

#include <doctest/doctest.h>

#include <string>

#include "mce/ecs/transform.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/roads.h"
#include "mce/kit/signal_head.h"
#include "mce/script/compiler.h"

using namespace mce;
using script::ScriptHost;
using script::ScriptState;

namespace {

struct SRig {
    World w;
    ScriptHost host;
    u64 tick = 0;
    Entity lamp, rock;

    explicit SRig(const char* source) {
        w.register_components<LocalTransform>();
        kit::install_kit3d(w, nullptr);
        kit::install_roads(w, nullptr);
        INFO(source);
        Result<Array<u8>> bc = script::compile_unchecked("s", source);
        REQUIRE(bc);
        REQUIRE(host.add_bytecode("s", bc.value().span()));
        host.install(w);
        kit::SignalHead h;
        h.lamp = kit::RoadLight::Amber;
        h.approach = {1.0f, 0.0f, 0.0f};
        lamp = w.spawn_with(LocalTransform{}, kit::MeshRef{}, h);
        rock = w.spawn_with(LocalTransform{});
    }
    kit::SignalHead& head() { return *w.try_get_mut<kit::SignalHead>(lamp); }
    void step(u32 n = 1) {
        for (u32 i = 0; i < n; ++i) w.tick(1.0 / 60, ++tick);
    }
};

std::string err(const ScriptHost& h) { return h.scripts()[0].error.c_str(); }

void expect_error(const char* body, const char* needle) {
    const std::string src = std::string("function update()\n    if mce.tick() == 1 then\n        ") + body +
                            "\n    end\nend\n";
    SRig r(src.c_str());
    r.step(2);
    INFO(body);
    REQUIRE(r.host.scripts()[0].state == ScriptState::Failed);
    INFO(err(r.host));
    CHECK(err(r.host).find(needle) != std::string::npos);
}

}  // namespace

TEST_CASE("signal api: getters read the fields, set writes them, state is none without a network") {
    SRig r(R"(
function update()
    local t = mce.tick()
    mce.each("SignalHead", function(e)
        if t == 1 then
            assert(signal.enabled(e) == 1 and signal.lamp(e) == "amber")
            local x, z = signal.approach(e)
            assert(x == 1 and z == 0)
            assert(signal.state(e) == "none")
            signal.set(e, { approach_x = 0, approach_z = -1, enabled = 0 })
        end
        if t == 2 then
            local x, z = signal.approach(e)
            assert(x == 0 and z == -1 and signal.enabled(e) == 0)
        end
    end)
end
)");
    r.step(3);
    CHECK(r.host.scripts()[0].state != ScriptState::Failed);
    CHECK(r.head().enabled == 0);
    CHECK(r.head().approach.z == -1.0f);
}

TEST_CASE("signal api: named errors, nothing written on error, set never adds a SignalHead") {
    expect_error("mce.each('SignalHead', function(e) signal.set(e, { nope = 1 }) end)", "signal.set: unknown key 'nope'");
    expect_error("mce.each('SignalHead', function(e) signal.set(e, { enabled = 'x' }) end)", "signal.set");
    expect_error("mce.each('SignalHead', function(e) signal.set(e, { approach_x = 1 / 0 }) end)", "signal.set");
    expect_error("mce.each('LocalTransform', function(e) signal.set(e, { enabled = 0 }) end)", "the entity has no SignalHead");
    expect_error("mce.each('LocalTransform', function(e) signal.lamp(e) end)", "the entity has no SignalHead");
    SRig r(R"(
function update()
    if mce.tick() == 1 then
        mce.each("SignalHead", function(e) pcall(signal.set, e, { enabled = 0, approach_x = 1 / 0 }) end)
    end
end
)");
    r.step(2);
    CHECK(r.head().enabled == 1);  // the bad table wrote nothing
    CHECK(r.head().approach.x == 1.0f);
}

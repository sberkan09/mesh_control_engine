// M-DAYNIGHT 4: samples/11_daynight. The sample's own assets are cooked and played with the stock systems and the
// scene scripts, as mce_player does. The clock starts at 05:00 on a 60 s day (3600 ticks, 150 ticks per hour), so
// 06:00 is tick 150, noon tick 1050, 18:00 tick 1950, 19:30 tick 2175 and midnight tick 2850. Pinned: the sun in the
// Environment3D resource at 06:00 and 18:00, the lamps off at noon and at their authored intensity at 19:30, the
// window emissives, the day counter, the same run twice has the same state hash, and a Luau script calling
// clock.set(12) moves the sun. (The player-level run, with the sample's own script and input, is
// samples/11_daynight/daynight_test.cmake.)

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "asset/cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"
#include "mce/ecs/transform.h"
#include "mce/kit/day_cycle.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/stock.h"
#include "mce/kit/timed_light.h"
#include "mce/kit/world_clock.h"
#include "mce/scene/scene.h"
#include "mce/script/compiler.h"
#include "mce/script/scene_scripts.h"

using namespace mce;
using namespace cook_test;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
constexpr const char* kScene = "11d11000-0000-4000-8000-000000000001";
// Entities in the scene's order.
constexpr u32 kSky = 1, kClock = 2, kWindowEarly = 5 /* on at 18:30 */, kWindowLate = 9 /* on at 21:00 */, kLamp1 = 32, kLamp2 = 34;
constexpr f32 kLampBase = 9.0f;

const std::string& project() {
    static const std::string root = [] {
        const std::string r = fresh_dir("daynight_demo_project");
        std::filesystem::copy(std::string(MCE_SOURCE_DIR) + "/samples/11_daynight/assets", r + "/assets",
                              std::filesystem::copy_options::recursive);
        cook::CookService svc;
        const cook::CookReport rep = svc.cook(StringView(r.c_str()), cook::CookOptions{});
        std::string diags;
        for (const cook::Diagnostic& d : rep.diags.all()) {
            cook::String line;
            cook::format_diagnostic(d, line);
            diags += std::string(line.view().data(), line.view().size()) + "\n";
        }
        INFO(diags);
        REQUIRE(rep.ok());
        return r;
    }();
    return root;
}

struct Demo {
    asset::AssetSystem assets;
    World world;
    script::ScriptHost host;  // a test script on top of the sample's own
    Array<Entity> e;
    u64 ticks = 0;

    explicit Demo(const char* test_script = nullptr) {
        const std::string cooked = project() + "/cooked";
        const StringView roots[] = {StringView(cooked.c_str())};
        asset::MountOptions own;
        own.allow_code = true;
        REQUIRE(assets.mount(Span<const StringView>(roots, 1), own));
        kit::install_stock(world, &assets);
        script::install_scene_scripts(world, &assets);
        REQUIRE(scene::load(world, assets, asset::parse_guid(kScene).value(), &e));
        REQUIRE(e.size() == 36);
        if (test_script) {
            REQUIRE(script::add_source(host, "t", test_script));
            host.install(world);
        }
    }
    void run_to(u64 tick) {
        while (ticks < tick) world.tick(kDt, ++ticks);
    }
    const kit::WorldClock& clock() { return *world.try_get<kit::WorldClock>(e[kClock]); }
    const kit::Environment3D& env() { return *world.try_resource<kit::Environment3D>(); }
    const kit::PointLight3D& lamp(u32 i) { return *world.try_get<kit::PointLight3D>(e[i]); }
    f32 emissive(u32 i) { return world.try_get<kit::MeshRef>(e[i])->emissive_tint.x; }
};

}  // namespace

TEST_CASE("daynight demo: the sample's assets cook") { CHECK(!project().empty()); }

TEST_CASE("daynight demo: the scene loads with the clock at 05:00 and no atmosphere") {
    Demo d;
    CHECK(d.clock().hour == 5.0f);
    CHECK(d.clock().day_length_seconds == 60.0f);
    CHECK(d.world.try_get<kit::DayCycle>(d.e[kClock]) != nullptr);
    CHECK(d.world.try_get<kit::TimedLight3D>(d.e[kLamp1]) != nullptr);
    CHECK(d.world.try_get<kit::TimedLight3D>(d.e[kWindowEarly]) != nullptr);
    CHECK(d.world.try_get<kit::Environment3D>(d.e[kSky])->atmosphere == 0);
    d.run_to(1);
    CHECK(d.env().atmosphere == 0);
}

TEST_CASE("daynight demo: the hour after N ticks and the sun at 06:00 and 18:00") {
    Demo d;
    d.run_to(150);  // one game hour: 5:00 -> 6:00
    CHECK(d.clock().hour == doctest::Approx(6.0f).epsilon(1e-6));
    CHECK(d.env().time_of_day == doctest::Approx(6.0f).epsilon(1e-6));
    CHECK(d.env().sun_color.x == doctest::Approx(1.0f).epsilon(1e-5));
    CHECK(d.env().sun_color.y == doctest::Approx(0.6f).epsilon(1e-5));
    CHECK(d.env().sun_color.z == doctest::Approx(0.35f).epsilon(1e-5));
    CHECK(d.env().sun_intensity == doctest::Approx(1.0f).epsilon(1e-5));

    d.run_to(1050);
    CHECK(d.clock().hour == doctest::Approx(12.0f).epsilon(1e-6));
    CHECK(d.env().sun_intensity == doctest::Approx(2.0f).epsilon(1e-5));
    CHECK(d.env().sun_color.y == doctest::Approx(0.96f).epsilon(1e-5));

    d.run_to(1950);
    CHECK(d.clock().hour == doctest::Approx(18.0f).epsilon(1e-6));
    CHECK(d.env().sun_color.x == doctest::Approx(1.0f).epsilon(1e-5));
    CHECK(d.env().sun_color.y == doctest::Approx(0.5f).epsilon(1e-5));
    CHECK(d.env().sun_color.z == doctest::Approx(0.25f).epsilon(1e-5));
    CHECK(d.env().sun_intensity == doctest::Approx(1.0f).epsilon(1e-5));
}

TEST_CASE("daynight demo: the lamps are off at noon and at their authored intensity at 19:30, the windows follow") {
    Demo d;
    d.run_to(1050);  // noon
    for (u32 i : {kLamp1, kLamp2}) {
        CHECK(d.lamp(i).intensity == 0.0f);
        CHECK(d.lamp(i).enabled == 0);
        CHECK(d.emissive(i) == 0.0f);
    }
    CHECK(d.emissive(kWindowEarly) == 0.0f);

    d.run_to(2175);  // 19:30, past the 20 minute fade of 18:30
    CHECK(d.clock().hour == doctest::Approx(19.5f).epsilon(1e-6));
    for (u32 i : {kLamp1, kLamp2}) {
        CHECK(d.lamp(i).intensity == doctest::Approx(kLampBase).epsilon(1e-5));
        CHECK(d.lamp(i).enabled == 1);
        CHECK(d.emissive(i) == doctest::Approx(1.0f).epsilon(1e-5));
    }
    CHECK(d.emissive(kWindowEarly) == doctest::Approx(1.0f).epsilon(1e-5));  // on at 18:30
    CHECK(d.emissive(kWindowLate) == 0.0f);                                   // on at 21:00
    d.run_to(3150);                                                           // 02:00 of the next day: all on
    CHECK(d.emissive(kWindowLate) == doctest::Approx(1.0f).epsilon(1e-5));
}

TEST_CASE("daynight demo: midnight counts the day") {
    Demo d;
    d.run_to(2849);
    CHECK(d.clock().day == 0);
    d.run_to(2850);  // 19 hours after 05:00
    CHECK(d.clock().day == 1);
    CHECK(d.clock().hour < 0.01f);
}

TEST_CASE("daynight demo: the same run twice has the same state hash") {
    auto run = [] {
        Demo d;
        d.run_to(2175);
        return d.world.state_hash();
    };
    const u64 a = run();
    const u64 b = run();
    CHECK(a == b);
}

TEST_CASE("daynight demo: a Luau script calling clock.set(12) moves the sun and the lamps follow") {
    Demo d(R"(
function update()
    if mce.tick() == 100 then
        assert(clock.hour() > 5.6 and clock.hour() < 5.7)
        clock.set(12)
        assert(clock.hour() == 12)
    end
end
)");
    d.run_to(100);
    CHECK(d.clock().hour == 12.0f);  // written at tick 100, re-seated by the clock on tick 101
    d.run_to(101);
    CHECK(d.clock().hour == doctest::Approx(12.0f + 1.0f / 150.0f).epsilon(1e-6));
    CHECK(d.env().time_of_day == doctest::Approx(12.0f + 1.0f / 150.0f).epsilon(1e-6));
    CHECK(d.env().sun_intensity == doctest::Approx(2.0f).epsilon(1e-3));
    CHECK(d.host.scripts()[0].state == script::ScriptState::Running);
    d.run_to(110);
    CHECK(d.lamp(kLamp1).enabled == 0);
}

// The 500-agent budget (M-AI gate): 500 brains on one guard tree (observer aborts, a utility choice
// over curves, a token pool on the target, stock tasks) with Perceivers watching a crowd of
// stimuli, ticked through install_ai's systems. A tick stays within 2 ms in optimized builds, every
// brain runs, and two runs hash the same every tick.

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <string>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"
#include "mce/ecs/transform.h"
#include "mce/kit/ai.h"
#include "mce/kit/perception.h"

using namespace mce;
using namespace cook_test;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
constexpr const char* kCrowd = "a5000000-0000-4000-8000-000000000001";
constexpr u32 kAgents = 500;

const std::string& project() {
    static const std::string root = [] {
        const std::string r = fresh_dir("ai_budget_project");
        write_file(r + "/assets/crowd.mcbt", R"({ "format": "mce-bt", "version": 1, "guid": "a5000000-0000-4000-8000-000000000001",
            "name": "crowd",
            "blackboard": { "target": "entity", "sees": "bool", "last_seen": "vec3", "heard_at": "vec3",
                            "health": {"type": "float", "default": 100}, "alert": {"type": "bool", "default": false},
                            "turns": {"type": "int", "default": 0} },
            "root": {"parallel": [
              {"repeat": {"sequence": [{"wait": 0.25}, {"set": "health", "add": -1}]}, "name": "bleed"},
              {"selector": [
                {"guard": {"utility": [
                  {"score": {"token": {"sequence": [{"set": "turns", "add": 1}, {"task": "look_at", "key": "target"}, {"wait": 0.2}]},
                             "pool": "attack", "key": "target"},
                   "by": [{"key": "health", "from": 0, "to": 100}]},
                  {"score": {"sequence": [{"set": "alert", "value": true}, {"wait": 0.3}]},
                   "by": [{"key": "health", "from": 0, "to": 100, "invert": true, "curve": "power", "exponent": 2}]}
                ]}, "key": "target", "abort": "lower", "name": "fight"},
                {"guard": {"sequence": [{"task": "look_at", "key": "heard_at"}, {"wait": 0.5}, {"set": "heard_at", "clear": true}]},
                 "key": "heard_at", "abort": "lower", "name": "investigate"},
                {"repeat": {"sequence": [{"check": "alert"}, {"wait": 0.1}]}, "name": "idle"}
              ]}
            ]} })");
        cook::CookService svc;
        const cook::CookReport rep = svc.cook(StringView(r.c_str()), cook::CookOptions{});
        REQUIRE(rep.ok());
        return r;
    }();
    return root;
}

LocalTransform at(f32 x, f32 z) { return LocalTransform{{x, 0, z}, math::quat::identity(), {1, 1, 1}}; }

struct Crowd {
    asset::AssetSystem assets;
    World world;
    u64 ticks = 0;

    Crowd() {
        const std::string cooked = project() + "/cooked";
        const StringView roots[] = {StringView(cooked.c_str())};
        REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
        world.register_components<LocalTransform>();
        kit::install_ai(world, &assets);
        // 500 guards on a 25 x 20 grid, every fourth one checking per tick; a line of 20 intruders
        // through the middle, each holding an attack pool of two tokens.
        kit::Perceiver eyes;
        eyes.sight_range = 6.0f;
        eyes.sight_angle = 120.0f;
        eyes.hearing = 1.0f;
        eyes.interval = 4;
        eyes.team = 1;
        kit::AiBrain brain;
        brain.tree.guid = asset::parse_guid(kCrowd).value();
        for (u32 row = 0; row < 20; ++row)
            for (u32 col = 0; col < 25; ++col) {
                const f32 yaw = static_cast<f32>((row * 25 + col) % 8) * 0.785f;
                LocalTransform t = at(static_cast<f32>(col) * 2.0f, static_cast<f32>(row) * 2.0f);
                t.rotation = math::quat_from_axis_angle({0, 1, 0}, yaw);
                world.spawn_with(t, eyes, brain, kit::Blackboard{});
            }
        kit::Stimulus shown;
        shown.team = 2;
        kit::AiTokenPool pool;
        pool.name.assign("attack");
        pool.capacity = 2;
        for (u32 i = 0; i < 20; ++i) world.spawn_with(at(static_cast<f32>(i) * 2.5f + 1.0f, 19.0f), shown, pool);
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) world.tick(kDt, ticks++);
    }
};

}  // namespace

TEST_CASE("ai budget: 500 brains with perception tick within budget") {
    Crowd c;
    c.run(10);  // warm-up: trees load, observers settle
    const u32 ticks = 120;
    f64 worst = 0.0;
    const auto t0 = std::chrono::steady_clock::now();
    for (u32 i = 0; i < ticks; ++i) {
        const auto s = std::chrono::steady_clock::now();
        c.run(1);
        worst = std::fmax(worst, std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - s).count());
    }
    const f64 ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count() / ticks;

    u32 brains = 0, seeing = 0, fighting = 0, calm = 0;
    c.world.query<const kit::AiBrain, const kit::Perceiver>().each([&](Entity e, const kit::AiBrain& b, const kit::Perceiver& p) {
        ++brains;
        seeing += p.sees;
        f32 turns = 0, health = 0;
        REQUIRE(kit::bb_number(c.world, e, "turns", turns));
        REQUIRE(kit::bb_number(c.world, e, "health", health));
        fighting += turns > 0.0f;
        calm += b.result == kit::BtStatus::Idle && health == 100.0f;
    });
    MESSAGE("500 brains + perception: " << ms << " ms per tick (worst " << worst << " ms); " << seeing << " seeing, "
                                        << fighting << " took a turn");
    CHECK(brains == kAgents);
    CHECK(seeing > 0);
    CHECK(seeing < kAgents);
    CHECK(fighting > 0);
    CHECK(calm == 0);  // every brain ran: the bleed lowered every health
    const kit::PerceptionStats& st = c.world.resource<kit::PerceptionStats>();
    CHECK(st.checks == kAgents / 4);
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__)
    CHECK(ms < 2.0);  // optimized builds only: Debug/ASan numbers are not the budget
#endif
}

TEST_CASE("ai budget: the 500-brain crowd hashes the same every tick on two runs") {
    Crowd a, b;
    for (u32 i = 0; i < 90; ++i) {
        a.run(1);
        b.run(1);
        REQUIRE(a.world.state_hash() == b.world.state_hash());
    }
}

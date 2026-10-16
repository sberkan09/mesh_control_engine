// M-AI 4: the utility node (scores from response curves, the best child runs, hysteresis keeps
// it), token nodes over AiTokenPool (a cap on how many brains run a branch, tokens back on finish
// and halt, dead holders dropped), find_point (rings of points kept on the grid or the navmesh,
// visible or hidden from a key, near or far), the cook's errors, and two runs hashing the same.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"
#include "mce/ecs/transform.h"
#include "mce/kit/ai.h"
#include "mce/kit/components.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/nav3d.h"
#include "mce/kit/pathfinding.h"
#include "mce/kit/perception.h"

using namespace mce;
using namespace cook_test;
using kit::AiBrain;
using kit::BtStatus;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

std::string diag_text(const cook::CookReport& r) {
    std::string s;
    for (const cook::Diagnostic& d : r.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        s += std::string(line.view().data(), line.view().size()) + "\n";
    }
    return s;
}

struct Tree {
    const char* name;
    const char* blackboard;
    const char* root;
};

// "spin" never finishes and "ok" succeeds; vector[0] numbers the node in the counters.
const Tree kTrees[] = {
    {"choose", R"({"health": "float"})",
     R"({"utility": [
          {"score": {"task": "spin", "vector": [0, 0, 0]}, "by": [{"key": "health", "from": 0, "to": 100}]},
          {"score": {"task": "spin", "vector": [1, 0, 0]}, "by": [{"key": "health", "from": 0, "to": 100, "invert": true}]}
        ], "hysteresis": 0.2})"},
    {"curves", R"({"x": {"type": "float", "default": 50}, "e": "entity"})",
     R"({"utility": [
          {"score": {"task": "ok"}, "by": [{"key": "x", "to": 100, "curve": "power", "exponent": 2}]},
          {"score": {"task": "ok"}, "by": [{"key": "x", "to": 100, "curve": "logistic", "exponent": 10, "weight": 2}]},
          {"score": {"task": "ok"}, "base": 0.1, "by": [{"key": "x", "to": 100, "curve": "step", "exponent": 0.6}]},
          {"score": {"task": "ok"}, "by": [{"key": "e"}]}
        ]})"},
    {"nothing", R"({"x": "float"})", R"({"utility": [{"score": {"task": "ok"}, "by": [{"key": "x"}]}]})"},
    {"finish", nullptr, R"({"utility": [{"score": {"task": "ok", "vector": [2, 0, 0]}, "base": 1}]})"},
    {"attack", R"({"target": "entity"})", R"({"token": {"task": "spin", "vector": [0, 0, 0]}, "pool": "attack", "key": "target"})"},
    {"squad", nullptr, R"({"token": {"task": "spin", "vector": [1, 0, 0]}, "pool": "flank"})"},
    {"spot", R"({"spot": "vec3"})", R"({"task": "find_point", "store": "spot", "radius": 3, "rings": 1, "count": 4})"},
    {"hide", R"({"spot": "vec3", "enemy": {"type": "vec3", "default": [0, 0, -8]}})",
     R"({"task": "find_point", "store": "spot", "from": "enemy", "need": "hidden", "radius": 4, "rings": 2, "count": 8})"},
    {"flee", R"({"spot": "vec3", "enemy": {"type": "vec3", "default": [4, 0, 0]}})",
     R"({"task": "find_point", "store": "spot", "from": "enemy", "prefer": "far", "radius": 3, "rings": 1, "count": 4})"},
};

std::string guid_of(usize i) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "a4000000-0000-4000-8000-%012zx", i + 1);
    return buf;
}

const std::string& project() {
    static const std::string root = [] {
        const std::string r = fresh_dir("ai_utility_project");
        for (usize i = 0; i < std::size(kTrees); ++i) {
            std::string doc = std::string(R"({ "format": "mce-bt", "version": 1, "guid": ")") + guid_of(i) + R"(", "name": ")" +
                              kTrees[i].name + "\", ";
            if (kTrees[i].blackboard) doc += std::string("\"blackboard\": ") + kTrees[i].blackboard + ", ";
            doc += std::string("\"root\": ") + kTrees[i].root + " }";
            write_file(r + "/assets/" + kTrees[i].name + ".mcbt", doc);
        }
        cook::CookService svc;
        const cook::CookReport rep = svc.cook(StringView(r.c_str()), cook::CookOptions{});
        INFO(diag_text(rep));
        REQUIRE(rep.ok());
        return r;
    }();
    return root;
}

struct Counter {
    u32 enter = 0, calls = 0, halt = 0;
};
Counter g_count[4];

BtStatus count(kit::BtTaskCall& c, BtStatus r) {
    Counter& n = g_count[static_cast<u32>(c.vector().x) & 3u];
    n.enter += c.entering;
    n.halt += c.halting;
    n.calls += !c.halting;
    return r;
}
BtStatus task_spin(kit::BtTaskCall& c) { return count(c, BtStatus::Running); }
BtStatus task_ok(kit::BtTaskCall& c) { return count(c, BtStatus::Success); }

LocalTransform at(f32 x, f32 y, f32 z) { return LocalTransform{{x, y, z}, math::quat::identity(), {1, 1, 1}}; }

struct Stage {
    asset::AssetSystem assets;
    World world;
    u64 ticks = 0;

    explicit Stage(bool physics3d = false) {
        for (Counter& c : g_count) c = Counter{};
        const std::string cooked = project() + "/cooked";
        const StringView roots[] = {StringView(cooked.c_str())};
        REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
        world.register_components<LocalTransform>();
        if (physics3d) kit::install_kit3d(world, nullptr);
        kit::install_ai(world, &assets);
        auto& reg = world.resource<kit::BtRegistry>();
        reg.register_task("spin", &task_spin);
        reg.register_task("ok", &task_ok);
    }
    template <class... Cs>
    Entity brain(const char* name, const Cs&... cs) {
        for (usize i = 0; i < std::size(kTrees); ++i)
            if (std::string(kTrees[i].name) == name) {
                AiBrain b;
                b.tree.guid = asset::parse_guid(StringView(guid_of(i).c_str())).value();
                return world.spawn_with(b, kit::Blackboard{}, cs...);
            }
        FAIL("no tree ", name);
        return Entity::null;
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) world.tick(kDt, ticks++);
    }
    const AiBrain& of(Entity e) { return *world.try_get<AiBrain>(e); }
    std::vector<f32> scores(Entity e) {
        Array<f32> s;
        REQUIRE(kit::bt_utility_scores(world, e, 0, s));
        return std::vector<f32>(s.begin(), s.end());
    }
    math::vec3 spot(Entity e) {
        math::vec3 v;
        REQUIRE(kit::bb_vector(world, e, "spot", v));
        return v;
    }
};

kit::AiTokenPool pool(const char* name, u32 capacity) {
    kit::AiTokenPool p;
    p.name.assign(name);
    p.capacity = capacity;
    return p;
}

constexpr BtStatus R = BtStatus::Running;

}  // namespace

TEST_CASE("ai utility: the best score runs, and hysteresis keeps the running choice") {
    Stage s;
    const Entity e = s.brain("choose");
    struct Step {
        f32 health;
        u32 running;  // the child that runs after the tick
    };
    // 0.8 / 0.2, then 0.45 / 0.55 (not 0.2 better: stays), 0.3 / 0.7 (switches), 0.45 / 0.55 (stays).
    const Step steps[] = {{80, 0}, {45, 0}, {30, 1}, {45, 1}, {90, 0}};
    for (const Step& st : steps) {
        REQUIRE(kit::bb_set_number(s.world, e, "health", st.health));
        s.run(1);
        INFO("health ", st.health);
        CHECK(kit::bt_node_status(s.of(e), st.running == 0 ? 2u : 4u) == R);
        CHECK(kit::bt_node_status(s.of(e), st.running == 0 ? 4u : 2u) == BtStatus::Idle);
    }
    CHECK(g_count[0].enter == 2);
    CHECK(g_count[0].halt == 1);
    CHECK(g_count[1].enter == 1);
    CHECK(g_count[1].halt == 1);
    const std::vector<f32> sc = s.scores(e);
    REQUIRE(sc.size() == 2);
    CHECK(sc[0] == doctest::Approx(0.9f));
    CHECK(sc[1] == doctest::Approx(0.1f));
}

TEST_CASE("ai utility: curves, unset keys, nothing worth doing, a finished child") {
    Stage s;
    const Entity e = s.brain("curves");
    s.run(1);
    std::vector<f32> sc = s.scores(e);
    REQUIRE(sc.size() == 4);
    CHECK(sc[0] == doctest::Approx(0.25f));  // 0.5 squared
    CHECK(sc[1] == doctest::Approx(1.0f));   // the logistic is 0.5 at the middle, weight 2
    CHECK(sc[2] == doctest::Approx(0.1f));   // under the 0.6 step: the base alone
    CHECK(sc[3] == 0.0f);                    // an unset entity key
    REQUIRE(kit::bb_set_number(s.world, e, "x", 80));
    REQUIRE(kit::bb_set_entity(s.world, e, "e", e));
    sc = s.scores(e);
    CHECK(sc[0] == doctest::Approx(0.64f));
    CHECK(sc[1] == doctest::Approx(2.0f / (1.0f + std::exp(-3.0f))));
    CHECK(sc[2] == doctest::Approx(1.1f));
    CHECK(sc[3] == 1.0f);

    const Entity none = s.brain("nothing");
    const Entity done = s.brain("finish");
    s.run(1);
    CHECK(s.of(none).result == BtStatus::Failure);
    CHECK(s.of(done).result == BtStatus::Success);
    CHECK(g_count[2].calls == 1);
}

TEST_CASE("ai tokens: a pool caps the brains running a branch; tokens come back") {
    Stage s;
    const Entity target = s.world.spawn_with(pool("attack", 2));
    Entity brains[3];
    for (Entity& b : brains) {
        b = s.brain("attack");
        REQUIRE(kit::bb_set_entity(s.world, b, "target", target));
    }
    s.run(1);
    CHECK(s.world.try_get<kit::AiTokenPool>(target)->held == 2);
    CHECK(kit::bt_node_status(s.of(brains[0]), 0) == R);
    CHECK(kit::bt_node_status(s.of(brains[1]), 0) == R);
    CHECK(s.of(brains[2]).result == BtStatus::Failure);
    CHECK(g_count[0].enter == 2);
    // A restart halts the first brain: its token goes back, and it takes one again as it starts over.
    s.world.try_get_mut<AiBrain>(brains[0])->restart = true;
    s.run(1);
    CHECK(g_count[0].halt == 1);
    CHECK(s.world.try_get<kit::AiTokenPool>(target)->held == 2);
    CHECK(kit::bt_node_status(s.of(brains[0]), 0) == R);
    CHECK(s.of(brains[2]).result == BtStatus::Failure);
    // A destroyed holder is dropped the next time a brain asks, and the third brain takes its token.
    s.world.destroy(brains[1]);
    s.run(1);
    const kit::AiTokenPool& p = *s.world.try_get<kit::AiTokenPool>(target);
    CHECK(p.held == 2);
    bool third = false;
    for (u32 i = 0; i < p.held; ++i) third |= p.holders[i] == brains[2];
    CHECK(third);
    CHECK(kit::bt_node_status(s.of(brains[2]), 0) == R);

    // A pool by name (no key): the first with the name.
    Stage t;
    t.world.spawn_with(pool("other", 5));
    const Entity flank = t.world.spawn_with(pool("flank", 1));
    const Entity a = t.brain("squad"), b = t.brain("squad");
    t.run(1);
    CHECK(t.world.try_get<kit::AiTokenPool>(flank)->held == 1);
    CHECK(t.world.try_get<kit::AiTokenPool>(flank)->holders[0] == a);
    CHECK(t.of(b).result == BtStatus::Failure);
}

TEST_CASE("ai find_point: rings of points on the grid, hidden from a key, far from it") {
    {
        Stage s;
        kit::NavGrid g(20, 20, 1.0f, {-10.0f, -10.0f});
        kit::install_path_agents(s.world, static_cast<kit::NavGrid&&>(g));
        const Entity e = s.brain("spot", at(0, 0, 0), kit::PathAgent{});
        s.run(1);
        CHECK(s.of(e).result == BtStatus::Success);
        math::vec3 p = s.spot(e);
        CHECK(math::length(p) == doctest::Approx(3.0f));
        CHECK(p.x > 0.0f);  // the first of four equally near points: 45 degrees
        CHECK(p.y > 0.0f);
        // That cell blocked, the next one (135 degrees) is chosen.
        kit::NavGrid& grid = s.world.resource<kit::NavGrid>();
        grid.set_cost(grid.cell_at({p.x, p.y}), kit::NavGrid::kBlocked);
        s.run(1);
        p = s.spot(e);
        CHECK(p.x < 0.0f);
        CHECK(p.y > 0.0f);
    }
    {
        Stage s(true);
        kit::BoxCollider3D wall;
        wall.half = {6.0f, 2.0f, 0.2f};
        s.world.spawn_with(at(0, 1, -2), wall);
        const Entity e = s.brain("hide", at(0, 0, 0));
        s.run(2);
        CHECK(s.of(e).result == BtStatus::Success);
        const math::vec3 p = s.spot(e);
        // The enemy at z = -8 is on the far side of the wall at z = -2, so the nearest hidden point is on the
        // first ring, on this side of the wall.
        CHECK(math::length(p) == doctest::Approx(2.0f));
        CHECK(p.z > -2.0f);
    }
    {
        Stage s;
        const Entity e = s.brain("flee", at(0, 0, 0));
        s.run(1);
        const math::vec3 p = s.spot(e);
        CHECK(p.x < 0.0f);  // away from the enemy at +X
    }
    {
        Stage s;
        kit::install_nav3d(s.world, nullptr);
        kit::BoxCollider3D floor;
        floor.half = {1.0f, 0.5f, 1.0f};
        s.world.spawn_with(at(0, -0.5f, 0), floor);
        kit::NavMeshSurface3D surf;
        surf.cell_size = 0.2f;
        s.world.spawn_with(at(0, 0, 0), surf);
        const Entity e = s.brain("spot", at(0, 0, 0));
        s.run(2);
        CHECK(s.of(e).result == BtStatus::Failure);  // every point is off the 2 x 2 m mesh
    }
}

TEST_CASE("ai utility: two runs hash the same") {
    auto setup = [](Stage& s) {
        const Entity target = s.world.spawn_with(pool("attack", 1));
        for (u32 i = 0; i < 3; ++i) {
            const Entity b = s.brain("attack");
            REQUIRE(kit::bb_set_entity(s.world, b, "target", target));
        }
        return s.brain("choose");
    };
    Stage a, b;
    const Entity ca = setup(a), cb = setup(b);
    for (u32 i = 0; i < 120; ++i) {
        const f32 health = static_cast<f32>((i * 37) % 100);
        REQUIRE(kit::bb_set_number(a.world, ca, "health", health));
        REQUIRE(kit::bb_set_number(b.world, cb, "health", health));
        a.run(1);
        b.run(1);
        REQUIRE(a.world.state_hash() == b.world.state_hash());
    }
}

TEST_CASE("ai utility: cook errors") {
    struct Case {
        const char* blackboard;
        const char* root;
        const char* expect;
    };
    const Case cases[] = {
        {"{}", R"({"utility": [{"wait": 1}]})", "a utility node's children are score nodes"},
        {R"({"x": "float"})", R"({"utility": [{"score": {"wait": 1}, "by": [{"key": "x", "curve": "wavy"}]}]})", "'curve' is"},
        {R"({"x": "float"})", R"({"score": {"wait": 1}, "by": [{"key": "x", "slope": 2}]})", "'slope' is not a key of a consideration"},
        {"{}", R"({"score": {"wait": 1}, "by": [{"curve": "linear"}]})", "a consideration needs 'key'"},
        {"{}", R"({"utility": [{"score": {"wait": 1}}], "hysteresis": -1})", "'hysteresis' must be a number >= 0"},
        {"{}", R"({"token": {"wait": 1}})", "a token node needs 'pool'"},
        {R"({"n": "int"})", R"({"token": {"wait": 1}, "pool": "a", "key": "n"})", "a token node's 'key' is an entity key"},
        {"{}", R"({"task": "find_point"})", "the find_point task needs 'store'"},
        {R"({"s": "vec3"})", R"({"task": "find_point", "store": "s", "need": "hidden"})", "'need' needs 'from'"},
        {R"({"s": "vec3"})", R"({"task": "find_point", "store": "s", "rings": 9})", "'rings' must be an integer from 1 to 8"},
    };
    u32 i = 0;
    for (const Case& c : cases) {
        const std::string root = fresh_dir("ai_utility_errors_" + std::to_string(i++));
        write_file(root + "/assets/bad.mcbt", std::string(R"({ "format": "mce-bt", "version": 1, "guid": "a4000000-0000-4000-8000-0000000000f0",
    "blackboard": )") + c.blackboard + R"(, "root": )" + c.root + " }");
        cook::CookService svc;
        const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
        CHECK_FALSE(r.ok());
        const std::string text = diag_text(r);
        INFO(text);
        CHECK(text.find(c.expect) != std::string::npos);
        CHECK(text.find("bad.mcbt:") != std::string::npos);
    }
}

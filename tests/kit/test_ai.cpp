// M-AI 1: behavior trees — the .mcbt cook and its errors, a truth table for every node kind run
// against the tick (results, which children run, halts), the blackboard by name and its defaults,
// a guard brain from a cooked scene, and determinism: two runs hash the same every tick, and a save
// mid-tree resumes to the same state as a run that never stopped.

#include <doctest/doctest.h>

#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/kit/ai.h"
#include "mce/scene/scene.h"

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
    const char* blackboard;  // JSON object or nullptr
    const char* root;
};

// Scripted tasks: "ok" and "no" finish at once; "run" runs for `value` ticks then succeeds, "flop"
// fails after `value` ticks, and "spin" never finishes. vector[0] numbers the node in the counters.
const Tree kTrees[] = {
    {"seq_fail", nullptr, R"({"sequence": [{"task": "ok", "vector": [0, 0, 0]}, {"task": "no", "vector": [1, 0, 0]}, {"task": "ok", "vector": [2, 0, 0]}]})"},
    {"sel", nullptr, R"({"selector": [{"task": "no", "vector": [0, 0, 0]}, {"task": "ok", "vector": [1, 0, 0]}, {"task": "ok", "vector": [2, 0, 0]}]})"},
    {"seq_run", nullptr, R"({"sequence": [{"task": "run", "value": 3, "vector": [0, 0, 0]}, {"task": "ok", "vector": [1, 0, 0]}]})"},
    {"par_all", nullptr, R"({"parallel": [{"task": "run", "value": 2, "vector": [0, 0, 0]}, {"task": "run", "value": 4, "vector": [1, 0, 0]}]})"},
    {"par_all_fail", nullptr, R"({"parallel": [{"task": "spin", "vector": [0, 0, 0]}, {"task": "flop", "value": 2, "vector": [1, 0, 0]}]})"},
    {"par_one", nullptr, R"({"parallel": [{"task": "run", "value": 2, "vector": [0, 0, 0]}, {"task": "spin", "vector": [1, 0, 0]}], "policy": "one"})"},
    {"invert", nullptr, R"({"invert": {"task": "ok"}})"},
    {"succeed", nullptr, R"({"succeed": {"task": "no"}})"},
    {"fail", nullptr, R"({"fail": {"task": "ok"}})"},
    {"repeat", nullptr, R"({"repeat": {"task": "ok", "vector": [0, 0, 0]}, "count": 3})"},
    {"retry", nullptr, R"({"retry": {"task": "no", "vector": [0, 0, 0]}, "count": 2})"},
    {"cooldown", nullptr, R"({"cooldown": {"task": "ok", "vector": [0, 0, 0]}, "seconds": 0.5})"},
    {"timeout", nullptr, R"({"timeout": {"task": "spin", "vector": [0, 0, 0]}, "seconds": 0.25})"},
    {"wait", nullptr, R"({"wait": 0.5})"},
    {"gate", R"({"alert": {"type": "bool", "default": false}})", R"({"guard": {"task": "ok"}, "key": "alert", "value": true})"},
    {"checks", R"({"count": {"type": "int", "default": 5}, "home": {"type": "vec3", "default": [1, 2, 3]}, "target": "entity"})",
     R"({"sequence": [{"check": "count", "op": ">", "value": 4}, {"check": "count", "op": "<=", "value": 5},
        {"check": "count", "op": "!=", "value": 3}, {"check": "count", "value": 5}, {"check": "home"},
        {"check": "target", "op": "not_set"}]})"},
    {"sets", R"({"n": {"type": "int", "default": 1}, "m": "int", "f": "float", "v": "vec3", "me": "entity", "gone": {"type": "float", "default": 2}})",
     R"({"sequence": [{"set": "n", "add": 2}, {"set": "m", "from": "n"}, {"set": "f", "value": 0.5}, {"set": "v", "value": [4, 5, 6]},
        {"set": "me", "value": "self"}, {"set": "gone", "clear": true}, {"emit": "done", "name": "tell"}]})"},
};

std::string guid_of(usize i) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "a1000000-0000-4000-8000-%012zx", i + 1);
    return buf;
}

constexpr const char* kGuard = "a1000000-0000-4000-8000-000000000100";
constexpr const char* kScene = "a1000000-0000-4000-8000-000000000101";

// The guard: patrols (waits, then steps), and when alerted raises the alarm once and calms down.
constexpr const char* kGuardTree = R"({ "format": "mce-bt", "version": 1, "guid": "a1000000-0000-4000-8000-000000000100",
    "name": "guard",
    "blackboard": { "alert": {"type": "bool", "default": false}, "alarms": {"type": "int", "default": 0},
                    "home": {"type": "vec3", "default": [1, 2, 3]}, "me": "entity" },
    "root": {"selector": [
        {"sequence": [{"check": "alert", "value": true}, {"emit": "alarm"}, {"set": "alarms", "add": 1},
                      {"set": "alert", "value": false}], "name": "alarm"},
        {"sequence": [{"wait": 0.5}, {"emit": "step"}, {"set": "me", "value": "self"}], "name": "patrol"}
    ]} })";

const std::string& project() {
    static const std::string root = [] {
        const std::string r = fresh_dir("ai_project");
        for (usize i = 0; i < std::size(kTrees); ++i) {
            std::string doc = std::string(R"({ "format": "mce-bt", "version": 1, "guid": ")") + guid_of(i) + R"(", "name": ")" +
                              kTrees[i].name + "\", ";
            if (kTrees[i].blackboard) doc += std::string("\"blackboard\": ") + kTrees[i].blackboard + ", ";
            doc += std::string("\"root\": ") + kTrees[i].root + " }";
            write_file(r + "/assets/" + kTrees[i].name + ".mcbt", doc);
        }
        write_file(r + "/assets/guard.mcbt", kGuardTree);
        write_file(r + "/assets/post.mcscene", R"({ "format": "mce-scene", "version": 1,
            "guid": "a1000000-0000-4000-8000-000000000101", "name": "post", "entities": [
            { "name": "Guard", "components": { "SaveKey": { "key": 1 }, "LocalTransform": {},
                "AiBrain": { "tree": "guid:a1000000-0000-4000-8000-000000000100" } } } ] })");
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
Counter g_count[8];

Counter& counter(const kit::BtTaskCall& c) { return g_count[static_cast<u32>(c.vector().x) & 7u]; }

BtStatus count(kit::BtTaskCall& c, BtStatus r) {
    Counter& n = counter(c);
    n.enter += c.entering;
    n.halt += c.halting;
    n.calls += !c.halting;
    return r;
}
BtStatus task_ok(kit::BtTaskCall& c) { return count(c, BtStatus::Success); }
BtStatus task_no(kit::BtTaskCall& c) { return count(c, BtStatus::Failure); }
BtStatus task_spin(kit::BtTaskCall& c) { return count(c, BtStatus::Running); }
BtStatus ticks_then(kit::BtTaskCall& c, BtStatus end) {
    if (!c.halting) c.memory += 1.0f;
    return count(c, c.memory >= c.value() ? end : BtStatus::Running);
}
BtStatus task_run(kit::BtTaskCall& c) { return ticks_then(c, BtStatus::Success); }
BtStatus task_flop(kit::BtTaskCall& c) { return ticks_then(c, BtStatus::Failure); }

struct Stage {
    asset::AssetSystem assets;
    World world;
    u64 ticks = 0;

    Stage() {
        for (Counter& c : g_count) c = Counter{};
        const std::string cooked = project() + "/cooked";
        const StringView roots[] = {StringView(cooked.c_str())};
        REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
        world.register_components<LocalTransform, SaveKey>();
        kit::install_ai(world, &assets);
        auto& reg = world.resource<kit::BtRegistry>();
        reg.register_task("ok", &task_ok);
        reg.register_task("no", &task_no);
        reg.register_task("spin", &task_spin);
        reg.register_task("run", &task_run);
        reg.register_task("flop", &task_flop);
    }
    Entity brain(const char* tree) {
        for (usize i = 0; i < std::size(kTrees); ++i)
            if (std::string(kTrees[i].name) == tree) {
                AiBrain b;
                b.tree.guid = asset::parse_guid(StringView(guid_of(i).c_str())).value();
                return world.spawn_with(b, kit::Blackboard{});
            }
        FAIL("no tree ", tree);
        return Entity::null;
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) world.tick(kDt, ticks++);
    }
    const AiBrain& of(Entity e) { return *world.try_get<AiBrain>(e); }
};

// Runs `tree` tick by tick and returns the root's result on each (Running while it runs).
std::vector<BtStatus> results(Stage& s, Entity e, u32 n) {
    std::vector<BtStatus> out;
    for (u32 i = 0; i < n; ++i) {
        const u32 before = s.of(e).runs;
        s.run(1);
        out.push_back(s.of(e).runs != before ? s.of(e).result : BtStatus::Running);
    }
    return out;
}

constexpr BtStatus R = BtStatus::Running, S = BtStatus::Success, F = BtStatus::Failure;

}  // namespace

TEST_CASE("ai: .mcbt cooks to CHNK_BT") {
    Stage s;
    const asset::AssetKey key = s.assets.find(asset::parse_guid(kGuard).value());
    REQUIRE(key != asset::kInvalidAssetKey);
    auto h = s.assets.load_sync<asset::BehaviorTree>(key);
    REQUIRE(h);
    auto view = asset::BtView::read(s.assets.resolve(h.value()));
    REQUIRE(view);
    const asset::BtView& v = view.value();
    CHECK(v.name() == StringView("guard"));
    REQUIRE(v.keys.size() == 4);
    CHECK(v.string(v.keys[0].name) == StringView("alert"));
    CHECK(v.keys[0].type == asset::BtKeyType::Bool);
    CHECK(v.keys[1].slot == 1);  // bool and int share the numbers
    CHECK(v.keys[2].type == asset::BtKeyType::Vec3);
    CHECK(v.keys[2].slot == 0);
    CHECK(v.keys[2].value[2] == 3.0f);
    CHECK(v.keys[3].has_default == 0);
    // selector, alarm (4 leaves), patrol (3 leaves): pre-order with subtree ends.
    REQUIRE(v.nodes.size() == 10);
    CHECK(v.nodes[0].kind == asset::BtNodeKind::Selector);
    CHECK(v.nodes[0].child_count == 2);
    CHECK(v.nodes[0].end == 10);
    CHECK(v.string(v.nodes[1].name) == StringView("alarm"));
    CHECK(v.nodes[1].end == 6);
    CHECK(v.nodes[6].kind == asset::BtNodeKind::Sequence);
    CHECK(v.nodes[7].kind == asset::BtNodeKind::Wait);
    CHECK(v.nodes[7].slot == 0);
    CHECK(v.header->slot_count == 1);
    CHECK(v.nodes[4].op == static_cast<u8>(asset::BtSetOp::Add));
    s.assets.release(asset::AnyHandle(h.value()));
}

TEST_CASE("ai: composites, decorators and leaves against the tick") {
    SUBCASE("a sequence stops at the first failure, a selector at the first success") {
        Stage s;
        const Entity a = s.brain("seq_fail"), b = s.brain("sel");
        s.run(1);
        CHECK(s.of(a).result == F);
        CHECK(s.of(b).result == S);
        CHECK(s.of(a).runs == 1);
        CHECK(g_count[2].calls == 0);  // neither third child ran
        CHECK(g_count[0].calls == 2);
        CHECK(g_count[1].calls == 2);
        CHECK(kit::bt_node_status(s.of(a), 2) == F);
        CHECK(kit::bt_node_status(s.of(a), 3) == BtStatus::Idle);
    }
    SUBCASE("a running child is resumed, not restarted") {
        Stage s;
        const Entity e = s.brain("seq_run");
        CHECK(results(s, e, 4) == std::vector<BtStatus>{R, R, S, R});
        CHECK(g_count[0].enter == 2);  // once per run of the tree
        CHECK(g_count[1].calls == 1);
    }
    SUBCASE("parallel: all, all with a failure, one") {
        Stage s;
        const Entity e = s.brain("par_all");
        CHECK(results(s, e, 4) == std::vector<BtStatus>{R, R, R, S});
        CHECK(g_count[0].calls == 2);  // the finished child is not ticked again
        Stage t;
        const Entity f = t.brain("par_all_fail");
        CHECK(results(t, f, 2) == std::vector<BtStatus>{R, F});
        CHECK(g_count[0].halt == 1);  // the spinning task heard it stop
        Stage u;
        const Entity g = u.brain("par_one");
        CHECK(results(u, g, 2) == std::vector<BtStatus>{R, S});
        CHECK(g_count[1].halt == 1);
    }
    SUBCASE("invert, succeed, fail") {
        Stage s;
        const Entity a = s.brain("invert"), b = s.brain("succeed"), c = s.brain("fail");
        s.run(1);
        CHECK(s.of(a).result == F);
        CHECK(s.of(b).result == S);
        CHECK(s.of(c).result == F);
    }
    SUBCASE("repeat and retry start the child again on the next tick") {
        Stage s;
        const Entity a = s.brain("repeat");
        CHECK(results(s, a, 4) == std::vector<BtStatus>{R, R, S, R});
        CHECK(g_count[0].enter == 4);
        Stage t;
        const Entity b = t.brain("retry");
        CHECK(results(t, b, 3) == std::vector<BtStatus>{R, F, R});
    }
    SUBCASE("cooldown: the child runs again 0.5 s after it finished") {
        Stage s;
        const Entity e = s.brain("cooldown");
        std::vector<BtStatus> r = results(s, e, 31);
        CHECK(r[0] == S);
        for (usize i = 1; i < 30; ++i) CHECK(r[i] == F);
        CHECK(r[30] == S);
        CHECK(g_count[0].calls == 2);
    }
    SUBCASE("timeout fails a child that runs too long, on its 15th tick at 0.25 s") {
        Stage s;
        const Entity e = s.brain("timeout");
        std::vector<BtStatus> r = results(s, e, 15);
        for (usize i = 0; i < 14; ++i) CHECK(r[i] == R);
        CHECK(r[14] == F);
        CHECK(g_count[0].halt == 1);
        CHECK(g_count[0].calls == 15);
    }
    SUBCASE("wait 0.5 s completes on its 30th tick") {
        Stage s;
        const Entity e = s.brain("wait");
        std::vector<BtStatus> r = results(s, e, 30);
        for (usize i = 0; i < 29; ++i) CHECK(r[i] == R);
        CHECK(r[29] == S);
    }
    SUBCASE("guard: the child runs only when the condition holds") {
        Stage s;
        const Entity e = s.brain("gate");
        s.run(1);
        CHECK(s.of(e).result == F);
        REQUIRE(kit::bb_set_number(s.world, e, "alert", 1.0f));
        s.run(1);
        CHECK(s.of(e).result == S);
    }
    SUBCASE("checks compare numbers and test what is set") {
        Stage s;
        const Entity e = s.brain("checks");
        s.run(1);
        CHECK(s.of(e).result == S);
        REQUIRE(kit::bb_set_entity(s.world, e, "target", e));
        s.run(1);
        CHECK(s.of(e).result == F);  // target is set now
        REQUIRE(kit::bb_clear(s.world, e, "target"));
        REQUIRE(kit::bb_set_number(s.world, e, "count", 3.0f));
        s.run(1);
        CHECK(s.of(e).result == F);
    }
    SUBCASE("set: add, copy, assign, self, clear; emit") {
        Stage s;
        const Entity e = s.brain("sets");
        s.run(1);
        CHECK(s.of(e).result == S);
        f32 n = 0;
        REQUIRE(kit::bb_number(s.world, e, "n", n));
        CHECK(n == 3.0f);
        REQUIRE(kit::bb_number(s.world, e, "m", n));
        CHECK(n == 3.0f);
        REQUIRE(kit::bb_number(s.world, e, "f", n));
        CHECK(n == 0.5f);
        math::vec3 v;
        REQUIRE(kit::bb_vector(s.world, e, "v", v));
        CHECK(v.y == 5.0f);
        Entity me;
        REQUIRE(kit::bb_entity(s.world, e, "me", me));
        CHECK(me == e);
        CHECK_FALSE(kit::bb_number(s.world, e, "gone", n));
        // Wrong types and unknown keys are refused.
        CHECK_FALSE(kit::bb_set_number(s.world, e, "v", 1.0f));
        CHECK_FALSE(kit::bb_set_vector(s.world, e, "n", {}));
        CHECK_FALSE(kit::bb_set_number(s.world, e, "nope", 1.0f));
        s.run(1);
        u32 done = 0;
        for (const kit::BtSignal& sig : s.world.event_queue<kit::BtSignal>()->read()) done += sig.id == StringId("done") && sig.entity == e;
        CHECK(done == 1);
    }
}

TEST_CASE("ai: a task with no function fails; restart and a paused brain") {
    Stage s;
    s.world.resource<kit::BtRegistry>().register_task("spin", nullptr);
    const Entity e = s.brain("timeout");
    s.run(1);
    CHECK(s.of(e).result == F);
    CHECK_FALSE(static_cast<bool>(s.world.resource<kit::BtRegistry>().validate_tasks()));
    s.world.resource<kit::BtRegistry>().register_task("spin", &task_spin);
    s.run(5);
    CHECK(s.of(e).runs == 1);
    CHECK(kit::bt_node_status(s.of(e), 1) == R);
    s.world.try_get_mut<AiBrain>(e)->active = false;
    s.run(30);
    CHECK(s.of(e).runs == 1);  // paused: the timeout never fires
    CHECK(g_count[0].halt == 0);
    s.world.try_get_mut<AiBrain>(e)->active = true;
    s.world.try_get_mut<AiBrain>(e)->restart = true;
    s.run(1);
    CHECK(g_count[0].halt == 1);  // the restart halted the running task
    CHECK(s.of(e).runs == 0);
    CHECK(s.of(e).time == doctest::Approx(kDt));
}

namespace {

struct Post {
    Stage s;
    Array<Entity> e;
    Post() { REQUIRE(scene::load(s.world, s.assets, asset::parse_guid(kScene).value(), &e)); }
    Entity guard() const { return e[0]; }
    u32 signals(StringView id) {
        u32 n = 0;
        for (const kit::BtSignal& sig : s.world.event_queue<kit::BtSignal>()->read()) n += sig.id == StringId(id);
        return n;
    }
};

}  // namespace

TEST_CASE("ai: a guard brain from a scene, with its blackboard added and defaults written") {
    Post p;
    p.s.run(1);
    CHECK_FALSE(p.s.of(p.guard()).started);  // the Blackboard arrives at the sync point
    REQUIRE(p.s.world.try_get<kit::Blackboard>(p.guard()));
    math::vec3 home;
    REQUIRE(kit::bb_vector(p.s.world, p.guard(), "home", home));
    CHECK(home.x == 1.0f);
    u32 steps = 0, alarms = 0;
    for (u32 i = 0; i < 31; ++i) {
        p.s.run(1);
        steps += p.signals("step");
    }
    CHECK(steps == 1);  // the 30th tick of the wait, read on the next
    Entity me;
    REQUIRE(kit::bb_entity(p.s.world, p.guard(), "me", me));
    CHECK(me == p.guard());
    // An alert mid-patrol waits for the patrol to finish: the selector resumes its running child.
    p.s.run(10);
    REQUIRE(kit::bb_set_number(p.s.world, p.guard(), "alert", 1.0f));
    for (u32 i = 0; i < 25; ++i) {
        p.s.run(1);
        alarms += p.signals("alarm");
    }
    CHECK(alarms == 1);
    f32 n = 0;
    REQUIRE(kit::bb_number(p.s.world, p.guard(), "alarms", n));
    CHECK(n == 1.0f);
    REQUIRE(kit::bb_number(p.s.world, p.guard(), "alert", n));
    CHECK(n == 0.0f);
}

TEST_CASE("ai: two runs hash the same every tick, and a save resumes to the same state") {
    auto script = [](Post& p, u64 tick) {
        if (tick == 40 || tick == 95) REQUIRE(kit::bb_set_number(p.s.world, p.guard(), "alert", 1.0f));
    };
    Post a, b;
    for (u32 i = 0; i < 150; ++i) {
        script(a, a.s.ticks);
        script(b, b.s.ticks);
        a.s.run(1);
        b.s.run(1);
        REQUIRE(a.s.world.state_hash() == b.s.world.state_hash());
    }
    for (u32 cut : {2u, 20u, 41u, 77u}) {
        Post c;
        while (c.s.ticks < cut) {
            script(c, c.s.ticks);
            c.s.run(1);
        }
        Array<byte> bytes;
        save::write_save(c.s.world, save::SaveInfo{}, bytes);
        Post d;
        d.s.ticks = c.s.ticks;
        REQUIRE(save::read_save(d.s.world, bytes.span()));
        while (d.s.ticks < 150) {
            script(d, d.s.ticks);
            d.s.run(1);
        }
        const AiBrain& x = a.s.of(a.guard());
        const AiBrain& y = d.s.of(d.guard());
        CHECK(y.runs == x.runs);
        CHECK(y.time == doctest::Approx(x.time));
        CHECK(std::memcmp(y.nodes, x.nodes, sizeof(x.nodes)) == 0);
        f32 n = 0;
        REQUIRE(kit::bb_number(d.s.world, d.guard(), "alarms", n));
        CHECK(n == 2.0f);
    }
}

TEST_CASE("ai: cook errors name the node and the file position") {
    struct Case {
        const char* blackboard;
        const char* root;
        const char* expect;
    };
    const Case cases[] = {
        {"{}", R"({"dance": []})", "unknown node"},
        {"{}", R"({"sequence": []})", "array of 1 to"},
        {"{}", R"({"sequence": [{"wait": 1}], "selector": []})", "exactly one kind key"},
        {"{}", R"({"check": "alert"})", "not a key of the blackboard"},
        {R"({"v": "vec3"})", R"({"check": "v", "op": ">", "value": 1})", "only bool, int and float keys compare"},
        {R"({"e": "entity"})", R"({"set": "e", "value": 3})", "\"self\""},
        {R"({"n": "int"})", R"({"set": "n", "value": 1.5})", "an int key takes an integer"},
        {R"({"n": "int"})", R"({"set": "n", "value": 1, "add": 2})", "exactly one of"},
        {R"({"n": "wobble"})", R"({"wait": 1})", "bool, int, float, vec3 or entity"},
        {"{}", R"({"timeout": {"wait": 1}})", "needs 'seconds'"},
        {"{}", R"({"wait": 1, "speed": 2})", "'speed' is not a key of a wait node"},
        {"{}", R"({"invert": {"wait": 1}, "count": 2})", "not a key of a invert node"},
        {R"({"n": "int"})", R"({"check": "n", "op": "~", "value": 1})", "'op' is"},
    };
    u32 i = 0;
    for (const Case& c : cases) {
        const std::string root = fresh_dir("ai_errors_" + std::to_string(i++));
        write_file(root + "/assets/bad.mcbt", std::string(R"({ "format": "mce-bt", "version": 1, "guid": "a1000000-0000-4000-8000-0000000000f0",
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

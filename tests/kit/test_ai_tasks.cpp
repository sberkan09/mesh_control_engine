// M-AI 2: guard observers and the stock tasks. The abort truth table (self, lower, both, none)
// against scripted tasks; move_to on a navmesh (a point, a chased entity, a halt) and on a grid,
// look_at in 3D and 2D, animate on a sprite and an Animator, play_sound, spawn, raise and
// wait_event, log; the cook's errors for the new arguments and the reader's check of a lower abort;
// and a chase that hashes the same every tick on two runs.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/audio/components.h"
#include "mce/cook/cook.h"
#include "mce/ecs/transform.h"
#include "mce/kit/ai.h"
#include "mce/kit/components.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/nav3d.h"
#include "mce/kit/pathfinding.h"
#include "mce/kit/sequence.h"

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

constexpr const char* kSfx = "a2000000-0000-4000-8000-000000000101";

// "spin" never finishes; vector[0] numbers the node in the counters.
const Tree kTrees[] = {
    {"abort_none", R"({"go": "bool"})", R"({"sequence": [{"guard": {"task": "spin", "vector": [0, 0, 0]}, "key": "go", "value": true}]})"},
    {"abort_self", R"({"go": "bool"})",
     R"({"sequence": [{"guard": {"task": "spin", "vector": [0, 0, 0]}, "key": "go", "value": true, "abort": "self"}]})"},
    {"abort_lower", R"({"alert": "bool"})",
     R"({"selector": [{"guard": {"task": "spin", "vector": [0, 0, 0]}, "key": "alert", "value": true, "abort": "lower"},
                      {"task": "spin", "vector": [1, 0, 0]}]})"},
    {"abort_both", R"({"alert": "bool"})",
     R"({"selector": [{"guard": {"task": "spin", "vector": [0, 0, 0]}, "key": "alert", "value": true, "abort": "both"},
                      {"task": "spin", "vector": [1, 0, 0]}]})"},
    {"walk", R"({"goal": {"type": "vec3", "default": [8, 0, 0]}, "done": "bool"})",
     R"({"sequence": [{"task": "move_to", "key": "goal"}, {"set": "done", "value": true}]})"},
    {"chase", R"({"thief": "entity", "caught": "bool"})",
     R"({"sequence": [{"task": "move_to", "key": "thief", "radius": 1.5}, {"set": "caught", "value": true}]})"},
    {"give_up", R"({"goal": {"type": "vec3", "default": [15, 0, 15]}})", R"({"timeout": {"task": "move_to", "key": "goal"}, "seconds": 0.2})"},
    {"look", R"({"at": "vec3"})", R"({"task": "look_at", "key": "at"})"},
    {"anim", R"({"done": "bool"})", R"({"sequence": [{"task": "animate", "clip": "attack", "finish": true}, {"set": "done", "value": true}]})"},
    {"anim_now", nullptr, R"({"task": "animate", "clip": "idle"})"},
    {"sound", nullptr, R"({"task": "play_sound", "sound": "guid:a2000000-0000-4000-8000-000000000101", "gain_db": -6})"},
    {"spawner", R"({"spot": {"type": "vec3", "default": [3, 0, 0]}})",
     R"({"sequence": [{"task": "spawn", "prefab": "guid:a2000000-0000-4000-8000-000000000100"},
                      {"task": "spawn", "prefab": "guid:a2000000-0000-4000-8000-000000000100", "key": "spot", "offset": [0, 1, 0]}]})"},
    {"bell", nullptr, R"({"sequence": [{"wait": 0.1}, {"task": "raise", "event": "bell"}, {"wait": 10}]})"},
    {"listener", R"({"heard": "bool"})",
     R"({"sequence": [{"task": "wait_event", "event": "bell"}, {"set": "heard", "value": true}, {"task": "log", "text": "heard the bell"}]})"},
};

std::string guid_of(usize i) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "a2000000-0000-4000-8000-%012zx", i + 1);
    return buf;
}

const std::string& project() {
    static const std::string root = [] {
        const std::string r = fresh_dir("ai_tasks_project");
        for (usize i = 0; i < std::size(kTrees); ++i) {
            std::string doc = std::string(R"({ "format": "mce-bt", "version": 1, "guid": ")") + guid_of(i) + R"(", "name": ")" +
                              kTrees[i].name + "\", ";
            if (kTrees[i].blackboard) doc += std::string("\"blackboard\": ") + kTrees[i].blackboard + ", ";
            doc += std::string("\"root\": ") + kTrees[i].root + " }";
            write_file(r + "/assets/" + kTrees[i].name + ".mcbt", doc);
        }
        write_file(r + "/assets/boom.wav", make_wav(22050, 1, 2205, 440.0));
        write_file(r + "/assets/boom.wav.mceasset", sidecar(kSfx, "wav"));
        write_file(r + "/assets/spark.mcprefab", R"({ "format": "mce-prefab", "version": 1,
            "guid": "a2000000-0000-4000-8000-000000000100", "name": "spark", "entities": [
            { "name": "Spark", "components": { "LocalTransform": { "position": [0, 0, 0] } } } ] })");
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

BtStatus task_spin(kit::BtTaskCall& c) {
    Counter& n = g_count[static_cast<u32>(c.vector().x) & 3u];
    n.enter += c.entering;
    n.halt += c.halting;
    n.calls += !c.halting;
    return BtStatus::Running;
}

LocalTransform at(f32 x, f32 y, f32 z) { return LocalTransform{{x, y, z}, math::quat::identity(), {1, 1, 1}}; }

struct Stage {
    asset::AssetSystem assets;
    World world;
    u64 ticks = 0;

    Stage() {
        for (Counter& c : g_count) c = Counter{};
        const std::string cooked = project() + "/cooked";
        const StringView roots[] = {StringView(cooked.c_str())};
        REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
        world.register_components<LocalTransform>();
        kit::install_ai(world, &assets);
        world.resource<kit::BtRegistry>().register_task("spin", &task_spin);
    }
    asset::Guid tree(const char* name) {
        for (usize i = 0; i < std::size(kTrees); ++i)
            if (std::string(kTrees[i].name) == name) return asset::parse_guid(StringView(guid_of(i).c_str())).value();
        FAIL("no tree ", name);
        return {};
    }
    template <class... Cs>
    Entity brain(const char* name, const Cs&... cs) {
        AiBrain b;
        b.tree.guid = tree(name);
        return world.spawn_with(b, kit::Blackboard{}, cs...);
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) world.tick(kDt, ticks++);
    }
    const AiBrain& of(Entity e) { return *world.try_get<AiBrain>(e); }
    bool flag(Entity e, const char* key) {
        f32 v = 0;
        return kit::bb_number(world, e, key, v) && v != 0.0f;
    }
    math::vec3 pos(Entity e) { return world.try_get<LocalTransform>(e)->position; }
};

// A 40 x 40 navmesh floor (top y = 0), baked at the first tick.
void navmesh(Stage& s) {
    kit::install_nav3d(s.world, nullptr);
    kit::BoxCollider3D floor;
    floor.half = {20.0f, 0.5f, 20.0f};
    s.world.spawn_with(at(0, -0.5f, 0), floor);
    kit::NavMeshSurface3D surf;
    surf.cell_size = 0.25f;
    surf.tile_size = 32;
    s.world.spawn_with(at(0, 0, 0), surf);
}

constexpr BtStatus R = BtStatus::Running, F = BtStatus::Failure;

}  // namespace

TEST_CASE("ai: guard observers abort their own branch, lower-priority branches, or both") {
    SUBCASE("none: a guard checks only on entry") {
        Stage s;
        const Entity e = s.brain("abort_none");
        REQUIRE(kit::bb_set_number(s.world, e, "go", 1));
        s.run(2);
        REQUIRE(kit::bb_set_number(s.world, e, "go", 0));
        s.run(5);
        CHECK(kit::bt_node_status(s.of(e), 0) == R);
        CHECK(g_count[0].halt == 0);
        CHECK(g_count[0].calls == 7);
    }
    SUBCASE("self: the running child stops the tick the condition fails") {
        Stage s;
        const Entity e = s.brain("abort_self");
        REQUIRE(kit::bb_set_number(s.world, e, "go", 1));
        s.run(2);
        REQUIRE(kit::bb_set_number(s.world, e, "go", 0));
        s.run(1);
        CHECK(s.of(e).result == F);
        CHECK(s.of(e).runs == 1);
        CHECK(g_count[0].halt == 1);
        CHECK(g_count[0].calls == 2);  // not ticked once the condition failed
        s.run(3);
        CHECK(g_count[0].enter == 1);  // the guard fails on entry from now on
    }
    SUBCASE("lower: a higher-priority guard that starts to hold takes over the selector") {
        Stage s;
        const Entity e = s.brain("abort_lower");
        s.run(3);
        CHECK(g_count[1].enter == 1);
        CHECK(g_count[1].calls == 3);
        REQUIRE(kit::bb_set_number(s.world, e, "alert", 1));
        s.run(1);
        CHECK(g_count[1].halt == 1);  // the patrol heard it stop
        CHECK(g_count[0].enter == 1);
        CHECK(kit::bt_node_status(s.of(e), 1) == R);
        CHECK(kit::bt_node_status(s.of(e), 3) == BtStatus::Idle);
        // Lower only: the guard's own branch carries on when the condition fails again.
        REQUIRE(kit::bb_set_number(s.world, e, "alert", 0));
        s.run(3);
        CHECK(g_count[0].halt == 0);
        CHECK(g_count[0].calls == 4);
        CHECK(g_count[1].enter == 1);
    }
    SUBCASE("both: it takes over, and gives the branch back when the condition fails") {
        Stage s;
        const Entity e = s.brain("abort_both");
        s.run(2);
        REQUIRE(kit::bb_set_number(s.world, e, "alert", 1));
        s.run(2);
        CHECK(g_count[1].halt == 1);
        CHECK(g_count[0].calls == 2);
        REQUIRE(kit::bb_set_number(s.world, e, "alert", 0));
        s.run(1);
        CHECK(g_count[0].halt == 1);
        CHECK(g_count[1].enter == 2);  // the selector moved on to the patrol in the same tick
        CHECK(kit::bt_node_status(s.of(e), 3) == R);
        CHECK(s.of(e).runs == 0);
    }
}

TEST_CASE("ai: move_to walks a NavAgent3D to a point and chases an entity; a halt stops it") {
    SUBCASE("a point") {
        Stage s;
        navmesh(s);
        kit::NavAgent3D a;
        a.speed = 6.0f;
        const Entity e = s.brain("walk", at(-8, 0, 0), a);
        s.run(2);
        CHECK(s.world.try_get<kit::NavAgent3D>(e)->move_to == 1);
        CHECK(s.world.try_get<kit::NavAgent3D>(e)->state == kit::NavAgentState::Moving);
        u32 ticks = 0;
        while (!s.flag(e, "done") && ticks++ < 600) s.run(1);
        CHECK(s.flag(e, "done"));
        CHECK(s.pos(e).x == doctest::Approx(8.0f).epsilon(0.05));
        CHECK(s.world.try_get<kit::NavAgent3D>(e)->move_to == 0);
    }
    SUBCASE("a chased entity, within a radius") {
        Stage s;
        navmesh(s);
        const Entity thief = s.world.spawn_with(at(6, 0, 6));
        const Entity e = s.brain("chase", at(-6, 0, -6), kit::NavAgent3D{});
        REQUIRE(kit::bb_set_entity(s.world, e, "thief", thief));
        s.run(2);
        CHECK(s.world.try_get<kit::NavAgent3D>(e)->follow == thief);
        u32 ticks = 0;
        while (!s.flag(e, "caught") && ticks++ < 900) {
            s.world.try_get_mut<LocalTransform>(thief)->position.x -= 0.01f;  // walking away slowly
            s.run(1);
        }
        CHECK(s.flag(e, "caught"));
        CHECK(math::length(s.pos(e) - s.pos(thief)) <= 1.6f);
        CHECK(s.world.try_get<kit::NavAgent3D>(e)->follow.is_null());
    }
    SUBCASE("a timeout halts the walk") {
        Stage s;
        navmesh(s);
        const Entity e = s.brain("give_up", at(-15, 0, -15), kit::NavAgent3D{});
        s.run(5);
        CHECK(s.world.try_get<kit::NavAgent3D>(e)->move_to == 1);
        s.run(7);  // the 12th tick: 0.2 s (the tree starts over, and walks again, on the next)
        CHECK(s.of(e).result == F);
        CHECK(s.world.try_get<kit::NavAgent3D>(e)->move_to == 0);
    }
    SUBCASE("no agent, or an unset key, fails") {
        Stage s;
        const Entity a = s.brain("walk", at(0, 0, 0));
        const Entity b = s.brain("chase", at(0, 0, 0), kit::NavAgent3D{});
        s.run(2);
        CHECK(s.of(a).result == F);
        CHECK(s.of(b).result == F);
    }
}

TEST_CASE("ai: move_to walks a PathAgent over the grid") {
    Stage s;
    kit::NavGrid g(12, 6);
    for (i32 y = 0; y < 5; ++y) g.set_cost({6, y}, kit::NavGrid::kBlocked);
    kit::install_path_agents(s.world, static_cast<kit::NavGrid&&>(g));
    kit::PathAgent p;
    p.speed = 6.0f;
    p.active = false;
    const Entity e = s.brain("walk", at(1.5f, 0.5f, 0), p);
    REQUIRE(kit::bb_set_vector(s.world, e, "goal", {10.5f, 0.5f, 0}));
    s.run(2);
    CHECK(s.world.try_get<kit::PathAgent>(e)->active);
    u32 ticks = 0;
    f32 max_y = 0;
    while (!s.flag(e, "done") && ticks++ < 600) {
        s.run(1);
        max_y = std::fmax(max_y, s.pos(e).y);
    }
    CHECK(s.flag(e, "done"));
    CHECK(max_y > 5.0f);  // around the wall
    CHECK(s.pos(e).x == doctest::Approx(10.5f).epsilon(0.02));
}

TEST_CASE("ai: look_at, animate, play_sound, spawn, raise, wait_event and log") {
    SUBCASE("look_at turns about +Y in 3D and about +Z in 2D") {
        Stage s;
        s.world.register_components<kit::Sprite>();
        const Entity a = s.brain("look", at(0, 0, 0));
        const Entity b = s.brain("look", at(0, 0, 0), kit::Sprite{});
        REQUIRE(kit::bb_set_vector(s.world, a, "at", {5, 0, 0}));
        REQUIRE(kit::bb_set_vector(s.world, b, "at", {0, 5, 0}));
        s.run(2);
        CHECK(s.of(a).result == BtStatus::Success);
        const math::vec3 fwd = math::rotate(s.world.try_get<LocalTransform>(a)->rotation, {0, 0, -1});
        CHECK(fwd.x == doctest::Approx(1.0f));
        CHECK(angle_z(s.world.try_get<LocalTransform>(b)->rotation) == doctest::Approx(1.5707963f));
    }
    SUBCASE("animate sets the clip and waits for a Once clip to finish") {
        Stage s;
        s.world.register_components<kit::AnimatedSprite, kit::Animator>();
        const Entity e = s.brain("anim", kit::AnimatedSprite{});
        const Entity m = s.brain("anim_now", kit::Animator{});
        const Entity none = s.brain("anim_now");
        s.run(2);
        CHECK(s.world.try_get<kit::AnimatedSprite>(e)->clip.view() == StringView("attack"));
        CHECK(kit::bt_node_status(s.of(e), 1) == R);
        CHECK(s.world.try_get<kit::Animator>(m)->clip.view() == StringView("idle"));
        CHECK(s.world.try_get<kit::Animator>(m)->restart);
        CHECK(s.of(m).result == BtStatus::Success);
        CHECK(s.of(none).result == F);
        kit::AnimatedSprite& sp = *s.world.try_get_mut<kit::AnimatedSprite>(e);
        sp.playing_clip.assign("attack");
        sp.finished = true;
        s.run(1);
        CHECK(s.flag(e, "done"));
    }
    SUBCASE("play_sound pushes a PlaySfx") {
        Stage s;
        s.world.register_event<audio::PlaySfx>(Phase::FixedUpdate_Pre);
        s.brain("sound");
        s.run(2);
        const Span<const audio::PlaySfx> played = s.world.event_queue<audio::PlaySfx>()->read();
        REQUIRE(played.size() == 1);
        CHECK(played[0].clip == asset::parse_guid(kSfx).value());
        CHECK(played[0].gain_db == -6.0f);
    }
    SUBCASE("spawn instantiates the prefab at the entity and at a key plus the offset") {
        Stage s;
        const Entity e = s.brain("spawner", at(-2, 0, 0));
        s.run(1);  // one run of the tree (it starts over, and spawns again, on every tick)
        std::vector<math::vec3> sparks;
        s.world.query<const LocalTransform>().each([&](Entity x, const LocalTransform& t) {
            if (x != e) sparks.push_back(t.position);
        });
        REQUIRE(sparks.size() == 2);
        CHECK((sparks[0].x == -2.0f || sparks[1].x == -2.0f));
        CHECK((sparks[0].y == 1.0f || sparks[1].y == 1.0f));
    }
    SUBCASE("raise reaches wait_event and the sequences' signals") {
        Stage s;
        kit::install_sequences(s.world, &s.assets);
        s.brain("bell");
        const Entity l = s.brain("listener");
        u32 seq = 0, ticks = 0;
        while (!s.flag(l, "heard") && ticks++ < 30) {
            s.run(1);
            for (const kit::SeqSignal& sig : s.world.event_queue<kit::SeqSignal>()->read()) seq += sig.id == StringId("bell");
        }
        CHECK(s.flag(l, "heard"));
        CHECK(seq == 1);
        s.run(1);
        CHECK(s.of(l).result == BtStatus::Success);  // the log task succeeded too
    }
}

TEST_CASE("ai: a chase hashes the same every tick on two runs") {
    auto setup = [](Stage& s) {
        navmesh(s);
        const Entity thief = s.world.spawn_with(at(6, 0, 6));
        const Entity e = s.brain("chase", at(-6, 0, -6), kit::NavAgent3D{});
        REQUIRE(kit::bb_set_entity(s.world, e, "thief", thief));
        return thief;
    };
    Stage a, b;
    const Entity ta = setup(a), tb = setup(b);
    for (u32 i = 0; i < 240; ++i) {
        a.world.try_get_mut<LocalTransform>(ta)->position.z -= 0.02f;
        b.world.try_get_mut<LocalTransform>(tb)->position.z -= 0.02f;
        a.run(1);
        b.run(1);
        REQUIRE(a.world.state_hash() == b.world.state_hash());
    }
}

TEST_CASE("ai: cook errors for aborts and stock task arguments; the reader checks a lower abort") {
    struct Case {
        const char* blackboard;
        const char* root;
        const char* expect;
    };
    const Case cases[] = {
        {R"({"a": "bool"})", R"({"sequence": [{"guard": {"wait": 1}, "key": "a", "abort": "lower"}]})", "only as a selector's child"},
        {R"({"a": "bool"})", R"({"guard": {"wait": 1}, "key": "a", "abort": "sideways"})", "'abort' is"},
        {"{}", R"({"task": "move_to"})", "the move_to task needs 'key'"},
        {R"({"f": "float"})", R"({"task": "look_at", "key": "f"})", "a vec3 or entity key"},
        {"{}", R"({"task": "animate"})", "the animate task needs 'clip'"},
        {"{}", R"({"task": "play_sound", "sound": "boom"})", "expected \"guid:<uuid>\""},
        {R"({"g": "vec3"})", R"({"task": "move_to", "key": "g", "speed": 2})", "'speed' is not an argument of the move_to task"},
        {"{}", R"({"task": "raise", "event": ""})", "'event' must be a non-empty string"},
        {R"({"g": "vec3"})", R"({"task": "move_to", "key": "g", "radius": -1})", "'radius' must be a number >= 0"},
    };
    u32 i = 0;
    for (const Case& c : cases) {
        const std::string root = fresh_dir("ai_task_errors_" + std::to_string(i++));
        write_file(root + "/assets/bad.mcbt", std::string(R"({ "format": "mce-bt", "version": 1, "guid": "a2000000-0000-4000-8000-0000000000f0",
    "blackboard": )") + c.blackboard + R"(, "root": )" + c.root + " }");
        cook::CookService svc;
        const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
        CHECK_FALSE(r.ok());
        const std::string text = diag_text(r);
        INFO(text);
        CHECK(text.find(c.expect) != std::string::npos);
        CHECK(text.find("bad.mcbt:") != std::string::npos);
    }

    // A payload written by hand: a lower abort under a sequence does not read, under a selector it does.
    asset::BtHeader h{};
    asset::BtKey k{};
    k.id = StringId("a").value;
    k.name = 0;
    k.type = asset::BtKeyType::Bool;
    asset::BtNode n[3]{};
    for (asset::BtNode& x : n) {
        x.slot = asset::kBtNoSlot;
        x.key = x.key2 = asset::kBtNoKey;
        x.name = x.text = x.arg = asset::kBtNoString;
    }
    n[0].kind = asset::BtNodeKind::Sequence;
    n[0].child_count = 1;
    n[0].end = 3;
    n[1].kind = asset::BtNodeKind::Guard;
    n[1].child_count = 1;
    n[1].end = 3;
    n[1].key = 0;
    n[1].abort = static_cast<u8>(asset::BtAbort::Lower);
    n[2].kind = asset::BtNodeKind::Check;
    n[2].key = 0;
    n[2].end = 3;
    const char strings[] = "a";
    Array<byte> payload;
    asset::write_behavior_tree(h, Span<const asset::BtKey>(&k, 1), Span<const asset::BtNode>(n, 3), Span<const char>(strings, 2), payload);
    CHECK_FALSE(static_cast<bool>(asset::BtView::read(payload.span())));
    n[0].kind = asset::BtNodeKind::Selector;
    asset::write_behavior_tree(h, Span<const asset::BtKey>(&k, 1), Span<const asset::BtNode>(n, 3), Span<const char>(strings, 2), payload);
    CHECK(static_cast<bool>(asset::BtView::read(payload.span())));
    n[0].abort = static_cast<u8>(asset::BtAbort::Self);  // only guards abort
    asset::write_behavior_tree(h, Span<const asset::BtKey>(&k, 1), Span<const asset::BtNode>(n, 3), Span<const char>(strings, 2), payload);
    CHECK_FALSE(static_cast<bool>(asset::BtView::read(payload.span())));
}

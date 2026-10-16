// M-SCRIPT 2: the `mce` API past one field: field handles, structural changes through Commands,
// reflected events, the kit's signals, input, scene requests and the physics queries.

#include <doctest/doctest.h>

#include <string>

#include "asset/cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"
#include "mce/ecs/transform.h"
#include "mce/input/actions.h"
#include "mce/kit/sequence.h"
#include "mce/physics/module.h"
#include "mce/refl/macros.h"
#include "mce/scene/scene.h"
#include "mce/script/compiler.h"

using namespace mce;
using mce::math::vec3;
using script::ScriptHost;
using script::ScriptState;

namespace scapi {

struct Probe {
    vec3 value;
    i32 n = 0;
    bool flag = false;
    u8 pad[3]{};
};
MCE_COMPONENT(Probe, value, n, flag)

struct Tagged {
    i32 level = 7;
};
MCE_COMPONENT(Tagged, level)

struct Ping {
    Entity from;
    i32 value = 0;
};
MCE_EVENT(Ping, from, value)

}  // namespace scapi

using scapi::Ping;
using scapi::Probe;
using scapi::Tagged;

namespace {

// A World with the test components, one script installed in FixedUpdate_Post.
struct Rig {
    World w;
    ScriptHost host;
    u64 tick = 0;

    // `checked` false: bytecode without the compile-time name checks, to reach the run-time errors.
    explicit Rig(const char* source, bool checked = true) {
        w.register_components<Probe, Tagged, LocalTransform>();
        INFO(source);
        if (checked) {
            REQUIRE(script::add_source(host, "api", source));
        } else {
            Result<Array<u8>> bc = script::compile_unchecked("api", source);
            REQUIRE(bc);
            REQUIRE(host.add_bytecode("api", bc.value().span()));
        }
        host.install(w);
    }
    void step(u32 n = 1) {
        for (u32 i = 0; i < n; ++i) w.tick(1.0 / 60, ++tick);
    }
    const ScriptHost& h() const { return host; }
    u32 count_probes() { return static_cast<u32>(w.query<Probe>().count()); }
    Entity first_probe() {
        Entity first = Entity::null;
        w.query<Probe>().each([&](Entity e, Probe&) {
            if (first.is_null()) first = e;
        });
        return first;
    }
};

std::string error_of(const ScriptHost& h) { return h.scripts()[0].error.c_str(); }

}  // namespace

TEST_CASE("script api: field handles resolve once and read and write like names") {
    Rig r(R"(
local N = mce.field("Probe", "n")
local V = mce.field("Probe", "value")
function update()
    mce.each("Probe", function(e)
        mce.set(e, N, mce.get(e, N) + 1)
        mce.set(e, V, mce.get(e, "Probe", "value") + vector.create(0, 2, 0))
    end)
end
)");
    const Entity e = r.w.spawn_with(Probe{});
    r.step(3);
    CHECK(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.w.try_get<Probe>(e)->n == 3);
    CHECK(r.w.try_get<Probe>(e)->value.y == doctest::Approx(6.0f));
}

TEST_CASE("script api: spawn, add, remove and destroy go through Commands") {
    Rig r(R"(
function update()
    local t = mce.tick()
    if t == 1 then
        local e = mce.spawn({ Probe = { value = vector.create(1, 2, 3), n = 4 }, LocalTransform = true })
        assert(mce.alive(e))
        assert(not mce.has(e, "Probe"))  -- appears at the sync point
    elseif t == 2 then
        mce.each("Probe", function(e)
            assert(mce.has(e, "Probe") and mce.has(e, "LocalTransform"))
            mce.add(e, "Tagged", { level = 9 })
            mce.add(e, "Probe", { flag = true })  -- an existing component: the rest is kept
        end)
    elseif t == 3 then
        mce.each("Probe", function(e) mce.remove(e, "Tagged") end)
    elseif t == 4 then
        mce.each("Probe", function(e) mce.destroy(e) end)
    end
end
)");
    r.step();
    REQUIRE(r.count_probes() == 1);
    const Entity e = r.first_probe();
    CHECK(r.w.try_get<Probe>(e)->value.z == doctest::Approx(3.0f));
    CHECK(r.w.try_get<Probe>(e)->n == 4);
    CHECK(r.w.try_get<LocalTransform>(e) != nullptr);
    r.step();
    CHECK(r.w.try_get<Tagged>(e)->level == 9);
    CHECK(r.w.try_get<Probe>(e)->flag);
    CHECK(r.w.try_get<Probe>(e)->n == 4);
    r.step();
    CHECK(r.w.try_get<Tagged>(e) == nullptr);
    r.step();
    CHECK_FALSE(r.w.alive(e));
    CHECK(r.h().scripts()[0].state == ScriptState::Running);
}

TEST_CASE("script api: spawning is deterministic") {
    auto run = [] {
        Rig r(R"(
function update()
    if mce.tick() % 2 == 1 then
        mce.spawn({ Tagged = { level = mce.tick() }, Probe = { n = 1 } })
    end
end
)");
        r.step(10);
        return r.w.state_hash();
    };
    CHECK(run() == run());
}

TEST_CASE("script api: reflected events are read and emitted by name") {
    Rig r(R"(
function update()
    local sum = 0
    for _, p in mce.events("Ping") do sum += p.value end
    mce.each("Probe", function(e)
        mce.set(e, "Probe", "n", sum)
        mce.emit("Ping", { from = e, value = 5 })
    end)
end
)");
    r.w.register_event<Ping>(Phase::FixedUpdate_Post);
    const Entity e = r.w.spawn_with(Probe{});
    r.step();
    CHECK(r.w.try_get<Probe>(e)->n == 0);  // events are readable from the next tick
    r.step();
    CHECK(r.w.try_get<Probe>(e)->n == 5);
    CHECK(r.h().scripts()[0].state == ScriptState::Running);
}

TEST_CASE("script api: an unregistered event reads empty, emitting one is an error") {
    Rig r(R"(
function update()
    assert(#mce.events("Ping") == 0)
    mce.emit("Ping", { value = 1 })
end
)");
    r.step();
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    CHECK(error_of(r.h()).find("event 'Ping' is not registered") != std::string::npos);
}

TEST_CASE("script api: unknown names are errors at run time too") {
    Rig r(R"(
function update() mce.spawn({ Nope = {} }) end
)", false);
    r.step();
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    CHECK(error_of(r.h()).find("unknown component 'Nope'") != std::string::npos);

    Rig r2(R"(
function update() mce.events("Probe") end
)", false);
    r2.step();
    CHECK(error_of(r2.h()).find("unknown event 'Probe'") != std::string::npos);

    Rig r3(R"(
function update() mce.spawn({ Probe = { nope = 1 } }) end
)", false);
    r3.step();
    CHECK(error_of(r3.h()).find("'Probe' has no field 'nope'") != std::string::npos);
}

TEST_CASE("script api: named signals reach sequences and come back") {
    Rig r(R"(
function update()
    mce.each("Probe", function(e)
        mce.set(e, "Probe", "n", #mce.signals("go"))
        if mce.tick() == 1 then mce.signal("go", e) end
    end)
end
)");
    r.w.register_event<kit::SeqSignal>(Phase::FixedUpdate_Post);
    const Entity e = r.w.spawn_with(Probe{});
    r.step();
    CHECK(r.w.try_get<Probe>(e)->n == 0);
    r.step();
    CHECK(r.w.try_get<Probe>(e)->n == 1);
    r.step();
    CHECK(r.w.try_get<Probe>(e)->n == 0);
}

TEST_CASE("script api: input actions by name") {
    Rig r(R"(
function update()
    mce.each("Probe", function(e)
        mce.set(e, "Probe", "flag", mce.pressed("Jump") and not mce.released("Jump") and not mce.held("Nope"))
        mce.set(e, "Probe", "value", mce.axis2("Move") + vector.create(0, 0, mce.axis("Jump")))
    end)
end
)");
    input::InputState in;
    in.action_count = 2;
    in.action_names[0] = StringId("Jump");
    in.actions[0].pressed = true;
    in.actions[0].held = true;
    in.actions[0].x = 1.0f;
    in.action_names[1] = StringId("Move");
    in.actions[1].x = 0.5f;
    in.actions[1].y = -1.0f;
    r.w.set_resource(in);
    const Entity e = r.w.spawn_with(Probe{});
    r.step();
    CHECK(r.w.try_get<Probe>(e)->flag);
    CHECK(r.w.try_get<Probe>(e)->value.x == doctest::Approx(0.5f));
    CHECK(r.w.try_get<Probe>(e)->value.y == doctest::Approx(-1.0f));
    CHECK(r.w.try_get<Probe>(e)->value.z == doctest::Approx(1.0f));
}

TEST_CASE("script api: input reads zeros in a world without input") {
    Rig r(R"(
function update() assert(not mce.pressed("Jump") and mce.axis("Move") == 0) end
)");
    r.step();
    CHECK(r.h().scripts()[0].state == ScriptState::Running);
}

TEST_CASE("script api: scene changes and quitting are requests to the host") {
    Rig r(R"(
function update()
    mce.load_scene("scenes/level2.mcscene")
    mce.quit()
end
)");
    r.step();
    const scene::SceneChange* c = r.w.try_resource<scene::SceneChange>();
    REQUIRE(c != nullptr);
    CHECK(c->next.view() == StringView("scenes/level2.mcscene"));
    CHECK(c->quit);
}

TEST_CASE("script api: 2D physics queries and bodies") {
    Rig r(R"(
function update()
    if mce.tick() < 2 then return end
    mce.each("Probe", function(e)
        local hit = mce.raycast2d(vector.create(0, 0, 0), vector.create(1, 0, 0), 20)
        if hit then
            mce.set(e, "Probe", "value", vector.create(hit.distance, hit.normal.x, #mce.overlap2d(vector.create(4, -1, 0), vector.create(6, 1, 0))))
            mce.set(e, "Probe", "flag", mce.has(hit.entity, "Collider2D"))
        end
        assert(mce.raycast2d(vector.create(0, 0, 0), vector.create(-1, 0, 0), 20) == nil)
    end)
end
)");
    physics::install_physics2d(r.w);
    physics::RigidBody2D rb;
    physics::Collider2D col;
    col.shape = physics::ColliderShape::box({1.0f, 1.0f});
    const Entity wall = r.w.spawn_with(LocalTransform{{5, 0, 0}, math::quat::identity(), {1, 1, 1}}, rb, col);
    (void)wall;
    const Entity e = r.w.spawn_with(Probe{});
    r.step(2);
    CHECK(r.h().scripts()[0].state == ScriptState::Running);
    INFO(error_of(r.h()));
    CHECK(r.w.try_get<Probe>(e)->value.x == doctest::Approx(4.0f));
    CHECK(r.w.try_get<Probe>(e)->value.y == doctest::Approx(-1.0f));
    CHECK(r.w.try_get<Probe>(e)->value.z == doctest::Approx(1.0f));
    CHECK(r.w.try_get<Probe>(e)->flag);
}

TEST_CASE("script api: teleport2d moves a body, and the physics step keeps it there") {
    Rig r(R"(
function update()
    if mce.tick() == 2 then   -- the body exists once the first physics step has synced it
        mce.each("Probe", function(e) mce.teleport2d(e, vector.create(7, 3, 0)) end)
    end
end
)");
    physics::install_physics2d(r.w);
    physics::RigidBody2D rb;
    rb.type = physics::BodyType::Kinematic;
    physics::Collider2D col;
    col.shape = physics::ColliderShape::box({0.5f, 0.5f});
    const Entity e = r.w.spawn_with(LocalTransform{{1, 1, 0}, math::quat::identity(), {1, 1, 1}}, rb, col, Probe{});
    r.step(6);
    INFO(error_of(r.h()));
    CHECK(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.w.try_get<LocalTransform>(e)->position.x == doctest::Approx(7.0f));
    CHECK(r.w.try_get<LocalTransform>(e)->position.y == doctest::Approx(3.0f));
}

TEST_CASE("script api: teleport2d on an entity without a 2D body is a clear error") {
    Rig r(R"(
function update() mce.each("Probe", function(e) mce.teleport2d(e, vector.create(1, 1, 0)) end) end
)");
    physics::install_physics2d(r.w);
    r.w.spawn_with(Probe{});
    r.step(3);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    CHECK(error_of(r.h()).find("no 2D body") != std::string::npos);
}

TEST_CASE("script api: physics calls fail clearly in a world without physics") {
    Rig r(R"(
function update() mce.raycast2d(vector.create(0, 0, 0), vector.create(1, 0, 0), 1) end
)");
    r.step();
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    CHECK(error_of(r.h()).find("no 2D physics") != std::string::npos);
}

TEST_CASE("script api: instantiate places a cooked prefab") {
    using namespace cook_test;
    const std::string root = fresh_dir("script_api_prefab");
    write_file(root + "/assets/spark.mcprefab", R"({ "format": "mce-prefab", "version": 1,
        "guid": "5c000000-0000-4000-8000-000000000100", "name": "spark", "entities": [
        { "name": "Spark", "components": { "LocalTransform": { "position": [0, 0, 0] }, "Tagged": { "level": 3 } } } ] })");
    cook::CookService svc;
    const cook::CookReport rep = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    REQUIRE(rep.ok());
    asset::AssetSystem assets;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    REQUIRE(assets.mount(Span<const StringView>(roots, 1)));

    Rig r(R"(
function update()
    if mce.tick() == 1 then
        local e = mce.instantiate("guid:5c000000-0000-4000-8000-000000000100", vector.create(4, 5, 6))
        assert(e ~= nil)
        assert(mce.instantiate("guid:5c000000-0000-4000-8000-0000000001ff") == nil)  -- not mounted
    end
end
)");
    r.host.set_assets(&assets);
    r.step();
    CHECK(r.h().scripts()[0].state == ScriptState::Running);
    INFO(error_of(r.h()));
    u32 n = 0;
    r.w.query<LocalTransform, Tagged>().each([&](LocalTransform& t, Tagged& tag) {
        CHECK(t.position.x == doctest::Approx(4.0f));
        CHECK(t.position.z == doctest::Approx(6.0f));
        CHECK(tag.level == 3);
        ++n;
    });
    CHECK(n == 1);
}

TEST_CASE("script api: engine events (a 2D contact) read by name") {
    Rig r(R"(
function update()
    for _, c in mce.events("ContactBegin2D") do
        mce.each("Probe", function(e)
            mce.set(e, "Probe", "n", mce.get(e, "Probe", "n") + 1)
            mce.set(e, "Probe", "flag", c.a ~= nil and c.b ~= nil and c.normal.y ~= 0)
        end)
    end
end
)");
    physics::install_physics2d(r.w);
    physics::RigidBody2D ground_rb;
    physics::Collider2D box;
    box.shape = physics::ColliderShape::box({0.5f, 0.5f});
    r.w.spawn_with(LocalTransform{{0, 0, 0}, math::quat::identity(), {1, 1, 1}}, ground_rb, box);
    physics::RigidBody2D falling = ground_rb;
    falling.type = physics::BodyType::Dynamic;
    r.w.spawn_with(LocalTransform{{0, 1.2f, 0}, math::quat::identity(), {1, 1, 1}}, falling, box);
    const Entity e = r.w.spawn_with(Probe{});
    r.step(60);
    CHECK(r.h().scripts()[0].state == ScriptState::Running);
    INFO(error_of(r.h()));
    CHECK(r.w.try_get<Probe>(e)->n == 1);
    CHECK(r.w.try_get<Probe>(e)->flag);
}

TEST_CASE("script api: one get or set call reaches several fields") {
    Rig r(R"(
local N = mce.field("Probe", "n")
local V = mce.field("Probe", "value")
local L = mce.field("Tagged", "level")
assert(type(N) == "userdata" and N == mce.field("Probe", "n"))
function update()
    mce.each("Probe", function(e)
        local n, v, level = mce.get(e, N, V, L)
        mce.set(e, N, n + level, V, v + vector.create(1, 0, 0), "Tagged", "level", level + 1)
    end)
end
)");
    const Entity e = r.w.spawn_with(Probe{}, Tagged{});
    r.step(2);
    CHECK(r.h().scripts()[0].state == ScriptState::Running);
    INFO(error_of(r.h()));
    CHECK(r.w.try_get<Probe>(e)->n == 15);  // 7 + 8
    CHECK(r.w.try_get<Probe>(e)->value.x == doctest::Approx(2.0f));
    CHECK(r.w.try_get<Tagged>(e)->level == 9);
}

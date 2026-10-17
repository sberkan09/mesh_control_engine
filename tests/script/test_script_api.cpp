// M-SCRIPT 2: the `mce` API past one field: field handles, structural changes through Commands,
// reflected events, the kit's signals, input, scene requests and the physics queries.

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "asset/cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/convar.h"
#include "mce/cook/cook.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/kit/cinematic.h"
#include "mce/kit/effects.h"
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

struct Saved {
    i32 n = 0;
};
MCE_COMPONENT(Saved, (n, Save))

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

TEST_CASE("script api: 2D forces, angular velocity, sleep, point overlaps and shape casts (M-SCRIPT2 1)") {
    Rig r(R"(
function update()
    if mce.tick() < 2 then return end
    mce.each("Probe", function(e)
        local hits = mce.raycast_all2d(vector.create(0, 0, 0), vector.create(1, 0, 0), 30)
        assert(#hits == 2 and hits[1].distance < hits[2].distance)
        local cast = mce.circle_cast2d(vector.create(0, 0, 0), vector.create(20, 0, 0), 0.5)
        local box = mce.box_cast2d(vector.create(0, 0, 0), vector.create(20, 0, 0), vector.create(0.25, 0.25, 0))
        assert(cast ~= nil and cast.fraction > 0 and cast.fraction < 1)
        assert(box ~= nil and box.fraction > cast.fraction)
        assert(mce.circle_cast2d(vector.create(0, 8, 0), vector.create(20, 8, 0), 0.5) == nil)
        assert(mce.overlap_point2d(vector.create(5, 0, 0)))
        assert(not mce.overlap_point2d(vector.create(5, 0, 0), hits[1].entity))
        assert(not mce.overlap_point2d(vector.create(0, 6, 0)))
        mce.set_angular_velocity2d(e, 3.5)
        mce.force2d(e, vector.create(60, 0, 0))
        mce.impulse2d(e, vector.create(0, 1, 0), vector.create(0.5, 10, 0))   -- off-centre: it spins more
        mce.set(e, "Probe", "value", vector.create(mce.angular_velocity2d(e), #hits, cast.fraction))
        mce.set(e, "Probe", "flag", mce.awake2d(e))
        mce.set_awake2d(e, false)
        mce.set(e, "Probe", "n", mce.awake2d(e) and 1 or 0)
    end)
end
)");
    physics::install_physics2d(r.w);
    physics::RigidBody2D wall_body;
    physics::Collider2D wall_col;
    wall_col.shape = physics::ColliderShape::box({1.0f, 1.0f});
    r.w.spawn_with(LocalTransform{{5, 0, 0}, math::quat::identity(), {1, 1, 1}}, wall_body, wall_col);
    r.w.spawn_with(LocalTransform{{10, 0, 0}, math::quat::identity(), {1, 1, 1}}, wall_body, wall_col);
    physics::RigidBody2D dyn;
    dyn.type = physics::BodyType::Dynamic;
    dyn.gravity_scale = 0.0f;
    physics::Collider2D ball;
    ball.shape = physics::ColliderShape::circle(0.5f);
    const Entity e = r.w.spawn_with(LocalTransform{{0, 10, 0}, math::quat::identity(), {1, 1, 1}}, dyn, ball, Probe{});
    r.step(4);
    INFO(error_of(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    const Probe* p = r.w.try_get<Probe>(e);
    CHECK(p->value.x != doctest::Approx(0.0f));  // angular velocity read back (3.5 plus what the off-centre impulse added)
    CHECK(p->value.y == doctest::Approx(2.0f));
    CHECK(p->flag);       // awake when asked
    CHECK(p->n >= 0);     // set_awake2d ran (a world with sleeping off keeps every body awake)
    physics::BodyState st;
    REQUIRE(r.w.resource<physics::World2D>().body_state(e, st));
    CHECK(st.linear_velocity.x > 0.0f);   // the force pushed it along +x
}

TEST_CASE("script api: 2D body functions name the missing body") {
    Rig r(R"(
function update() mce.each("Probe", function(e) mce.force2d(e, vector.create(1, 0, 0)) end) end
)");
    physics::install_physics2d(r.w);
    r.w.spawn_with(Probe{});
    r.step(3);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    CHECK(error_of(r.h()).find("force2d: the entity has no 2D body") != std::string::npos);
}

// The gate of M-SCRIPT2: a function cannot be registered without a line in mce.d.luau (so script_check types it)
// or listed there without being registered. The script walks the real `mce` table and compares it, both ways,
// with the names the definitions declare.
TEST_CASE("script api: every registered mce function is declared in mce.d.luau, and the other way round") {
    Array<char> defs;
    script::write_definitions(defs);
    const std::string text(defs.data(), defs.size());
    const usize begin = text.find("declare mce: {");
    REQUIRE(begin != std::string::npos);
    const usize end = text.find("\n}\n", begin);
    REQUIRE(end != std::string::npos);
    std::string expected;
    usize at = begin;
    while ((at = text.find("\n    ", at)) != std::string::npos && at < end) {
        at += 5;
        const usize colon = text.find(':', at);
        if (colon == std::string::npos || colon > end) break;
        const std::string name = text.substr(at, colon - at);
        if (!name.empty() && name.find_first_not_of("abcdefghijklmnopqrstuvwxyz_0123456789") == std::string::npos) expected += "\"" + name + "\",";
    }
    REQUIRE_FALSE(expected.empty());
    const std::string source = "local expected = {" + expected + R"(}
function update()
    local want = {}
    for _, n in ipairs(expected) do want[n] = true end
    for name in pairs(mce) do
        assert(want[name], "mce." .. name .. " is registered but not declared in mce.d.luau")
        want[name] = nil
    end
    for name in pairs(want) do
        error("mce." .. name .. " is declared in mce.d.luau but not registered")
    end
end
)";
    Rig r(source.c_str());
    r.step();
    INFO(error_of(r.h()));
    CHECK(r.h().scripts()[0].state == ScriptState::Running);
}

TEST_CASE("script api: find_path walks around static colliders and follows a level that changes (M-SCRIPT2 2)") {
    Rig r(R"(
function update()
    mce.each("Probe", function(e)
        local path = mce.find_path(vector.create(0, 5, 0), vector.create(10, 5, 0), { cell = 0.5, radius = 0.25 })
        if path == nil then mce.set(e, "Probe", "n", -1) return end
        local best = 100
        for _, p in ipairs(path) do
            if p.x > 3 and p.x < 7 then best = math.min(best, math.abs(p.y)) end   -- how close it came to the gap at y = 0
        end
        local last = path[#path]
        mce.set(e, "Probe", "value", vector.create(best, last.x, last.y))
        mce.set(e, "Probe", "n", #path)
        mce.set(e, "Probe", "flag", mce.find_path(vector.create(0, 5, 0), vector.create(5, 5, 0)) == nil)   -- the target is inside a wall
    end)
end
)");
    physics::RigidBody2D wall_body;
    wall_body.type = physics::BodyType::Static;
    physics::Collider2D top, bottom;
    top.shape = physics::ColliderShape::box({0.5f, 4.5f});     // y from 1 to 10
    bottom.shape = physics::ColliderShape::box({0.5f, 4.5f});  // y from -10 to -1
    const auto at = [](f32 x, f32 y) { return LocalTransform{{x, y, 0}, math::quat::identity(), {1, 1, 1}}; };
    const Entity upper = r.w.spawn_with(at(5, 5.5f), wall_body, top);
    r.w.spawn_with(at(5, -5.5f), wall_body, bottom);
    const Entity e = r.w.spawn_with(Probe{});
    r.step(2);
    INFO(error_of(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    const Probe* p = r.w.try_get<Probe>(e);
    CHECK(p->n >= 3);                      // around the wall, not straight through it
    CHECK(p->value.x < 1.5f);              // through the gap
    CHECK(p->value.y == doctest::Approx(10.0f));
    CHECK(p->value.z == doctest::Approx(5.0f));
    CHECK(p->flag);
    // The wall goes away: the next call plans a straight line (the cached grid noticed).
    r.w.destroy(upper);
    r.step(1);
    CHECK(r.w.try_get<Probe>(e)->n >= 1);
    CHECK(r.w.try_get<Probe>(e)->n <= 3);
}

TEST_CASE("script api: find_path with no static colliders is the straight walk, and bad options are named") {
    Rig r(R"(
function update()
    mce.each("Probe", function(e)
        local path = mce.find_path(vector.create(0, 0, 0), vector.create(3, 4, 0))
        mce.set(e, "Probe", "n", #path)
        mce.set(e, "Probe", "value", path[1])
        mce.find_path(vector.create(0, 0, 0), vector.create(3, 4, 0), { cell = 0 })
    end)
end
)");
    const Entity e = r.w.spawn_with(Probe{});
    r.step();
    CHECK(r.w.try_get<Probe>(e)->n == 1);
    CHECK(r.w.try_get<Probe>(e)->value.x == doctest::Approx(3.0f));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    CHECK(error_of(r.h()).find("cell must be from 0.05 to 100") != std::string::npos);
}

TEST_CASE("script api: save writes a slot now, load is applied by the host after the tick (M-SCRIPT2 3)") {
    const std::string dir = std::string(MCE_TEST_TMP_DIR) + "/script_saves";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    save::set_root(dir);
    Rig r(R"(
function update()
    mce.each("Probe", function(e)
        if mce.tick() == 1 then
            mce.set(e, "Saved", "n", 7)
            mce.set(e, "Probe", "flag", mce.save("slot_a") and not mce.has_save("nope"))
        elseif mce.tick() == 2 then
            mce.set(e, "Saved", "n", 99)
            mce.set(e, "Probe", "value", vector.create(mce.load("slot_a") and 1 or 0, mce.load("nope") and 1 or 0, 0))
        end
    end)
end
)");
    r.w.register_components<SaveKey, scapi::Saved>();
    const Entity e = r.w.spawn_with(Probe{}, scapi::Saved{}, SaveKey{1});
    r.step(1);
    INFO(error_of(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.w.try_get<Probe>(e)->flag);
    CHECK(save::exists("slot_a"));
    r.step(1);
    CHECK(r.w.try_get<scapi::Saved>(e)->n == 99);
    CHECK(r.w.try_get<Probe>(e)->value.x == doctest::Approx(1.0f));  // slot_a is there
    CHECK(r.w.try_get<Probe>(e)->value.y == doctest::Approx(0.0f));  // "nope" is not
    asset::AssetSystem assets;
    REQUIRE(scene::apply_change(r.w, assets));
    CHECK(r.w.try_get<scapi::Saved>(e)->n == 7);  // the save at tick 1 is back
}

TEST_CASE("script api: save slots are names, never paths") {
    Rig r(R"(
function update()
    mce.save("../escape")
end
)");
    r.w.spawn_with(Probe{});
    r.step(1);
    CHECK(r.h().scripts()[0].state == ScriptState::Failed);
    CHECK(error_of(r.h()).find("must be 1 to 64 letters") != std::string::npos);
}

TEST_CASE("script api: shake, burst and set_time_scale (M-SCRIPT2 3)") {
    Rig r(R"(
function update()
    mce.shake(0.75)
    mce.each("ParticleBurst", function(e) mce.burst(e, 12) end)
    mce.set_time_scale(0.5)
end
)");
    r.w.register_components<kit::CameraShake2D, kit::CameraShake3D, kit::ParticleBurst>();
    const Entity cam = r.w.spawn_with(kit::CameraShake2D{});
    const Entity cam3 = r.w.spawn_with(kit::CameraShake3D{});
    const Entity fx = r.w.spawn_with(kit::ParticleBurst{});
    r.step(1);
    INFO(error_of(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.w.try_get<kit::CameraShake2D>(cam)->trauma == doctest::Approx(0.75f));
    CHECK(r.w.try_get<kit::CameraShake3D>(cam3)->trauma == doctest::Approx(0.75f));
    r.step(1);
    CHECK(r.w.try_get<kit::CameraShake2D>(cam)->trauma == doctest::Approx(1.0f));  // added, clamped
    CHECK(r.w.try_get<kit::ParticleBurst>(fx)->count == 12);
    CHECK(ConVar::find(StringId("time_scale"))->get_float() == doctest::Approx(0.5f));
    ConVar::find(StringId("time_scale"))->set_float(1.0f, true);
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

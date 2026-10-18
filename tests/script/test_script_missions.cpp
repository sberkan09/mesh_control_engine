// M-MISSION 3a: the `mission` and `marker` Luau globals over the kit's Mission, Objective and MapMarker components.
// Exact numbers: a script starts a mission, signals it and sees it complete at a pinned tick; markers are added and
// removed; bad arguments raise a Luau error that names the call and disables only that script.

#include <doctest/doctest.h>

#include <string>

#include "mce/ecs/transform.h"
#include "mce/kit/map_markers.h"
#include "mce/kit/missions.h"
#include "mce/script/compiler.h"

using namespace mce;
using script::ScriptHost;
using script::ScriptState;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

// A World with the kit's missions and map markers and one script in FixedUpdate_Post. Tick numbers are the
// script's mce.tick() (1 for the first w.tick).
struct MRig {
    World w;
    ScriptHost host;
    u64 tick = 0;
    Entity mission, first, second;
    Array<u64> completed_at, started_at, failed_at;
    Array<u32> failed_reason;

    explicit MRig(const char* source, bool install_kit = true, bool checked = true) {
        w.register_components<LocalTransform>();
        if (install_kit) kit::install_map_markers(w, nullptr);
        INFO(source);
        if (checked) {
            REQUIRE(script::add_source(host, "m", source));
        } else {
            Result<Array<u8>> bc = script::compile_unchecked("m", source);
            REQUIRE(bc);
            REQUIRE(host.add_bytecode("m", bc.value().span()));
        }
        host.install(w);
    }
    // signal "go" (count 1) -> timer 0.5 s -> end
    void build_chain() {
        kit::Objective timer;
        timer.kind = kit::ObjectiveKind::Timer;
        timer.duration = 0.5f;
        second = w.spawn_with(timer);
        kit::Objective sig;
        sig.kind = kit::ObjectiveKind::Signal;
        sig.signal = "go";
        sig.next = second;
        first = w.spawn_with(sig);
        kit::Mission m;
        m.id = "m1";
        m.objective = first;
        mission = w.spawn_with(m, LocalTransform{});
    }
    void step(u32 n = 1) {
        for (u32 i = 0; i < n; ++i) {
            w.tick(1.0 / 60, ++tick);
            if (!w.event_queue<kit::MissionStarted>()) continue;
            EventAccess ev(w);
            for (const kit::MissionStarted& e : ev.read<kit::MissionStarted>()) {
                (void)e;
                started_at.push_back(tick);
            }
            for (const kit::MissionCompleted& e : ev.read<kit::MissionCompleted>()) {
                (void)e;
                completed_at.push_back(tick);
            }
            for (const kit::MissionFailed& e : ev.read<kit::MissionFailed>()) {
                failed_at.push_back(tick);
                failed_reason.push_back(e.reason);
            }
        }
    }
    const kit::Mission& m() { return *w.try_get<kit::Mission>(mission); }
    const ScriptHost& h() const { return host; }
};

std::string err(const ScriptHost& h) { return h.scripts()[0].error.c_str(); }

// A script that calls `body` once with `e` = a mission entity on tick 1 and must fail with `needle`.
void expect_error(const char* body, const char* needle, bool unchecked = true) {
    const std::string src = std::string("function update()\n    mce.each('Mission', function(e)\n        ") + body + "\n    end)\nend\n";
    MRig r(src.c_str(), true, !unchecked);
    r.build_chain();
    r.step(2);
    INFO(body);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
    INFO(err(r.h()));
    CHECK(err(r.h()).find(needle) != std::string::npos);
}

}  // namespace

TEST_CASE("mission api: a script starts a mission, signals it, and it completes at a pinned tick") {
    // Tick 1: the script starts it. Tick 2 (Pre): the mission goes active, the signal objective with it. Tick 5: the
    // script signals. The script runs after the kit's system (Post), and an event pushed there is readable from the
    // Pre phase two ticks on, so the signal objective is done on tick 7 and the 0.5 s timer goes active. The timer
    // counts 30 ticks (8..37) and the mission completes on tick 37 with 35 of its own ticks (3..37).
    MRig r(R"(
function update()
    local t = mce.tick()
    mce.each("Mission", function(e)
        if t == 1 then
            assert(mission.start(e) == true)
            assert(mission.start(e) == true) -- still idle until the next tick: asking twice is harmless
        elseif t == 5 then
            mission.signal("go")
        end
        local state, objective, elapsed, done = mission.status(e)
        if t == 1 then assert(state == "idle" and objective == nil and done == 0) end
        if t == 3 then assert(state == "active" and mce.has(objective, "Objective") and done == 0) end
        if t == 10 then assert(state == "active" and done == 1) end
        if t == 37 then
            assert(state == "complete" and objective == nil and done == 2)
            assert(mission.start(e) == false and mission.fail(e) == false)
        end
    end)
end
)");
    r.build_chain();
    r.step(40);
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    REQUIRE(r.started_at.size() == 1);
    CHECK(r.started_at[0] == 2);
    REQUIRE(r.completed_at.size() == 1);
    CHECK(r.completed_at[0] == 37);
    CHECK(r.failed_at.empty());
    CHECK(r.m().state == kit::MissionState::Complete);
    CHECK(r.m().done == 2);
    CHECK(r.m().ticks == 35);
    CHECK(r.m().elapsed == doctest::Approx(35 * kDt));
}

TEST_CASE("mission api: status reports the elapsed seconds of a running mission") {
    MRig r(R"(
function update()
    mce.each("Mission", function(e)
        local state, objective, elapsed = mission.status(e)
        if mce.tick() == 11 then
            mce.set(e, "LocalTransform", "position", vector.create(elapsed, state == "active" and 1 or 0, 0))
        end
    end)
end
)");
    r.build_chain();
    r.w.try_get_mut<kit::Mission>(r.mission)->start_on_load = 1;
    r.step(12);
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    CHECK(r.m().state == kit::MissionState::Active);
    CHECK(r.m().ticks == 11);  // active on tick 1, counting from tick 2
    // The script ran after the kit system on tick 11: 10 ticks counted.
    const LocalTransform* t = r.w.try_get<LocalTransform>(r.mission);
    CHECK(t->position.x == doctest::Approx(10 * kDt));
    CHECK(t->position.y == 1.0f);
}

TEST_CASE("mission api: fail fails an active mission once, with reason 3") {
    MRig r(R"(
function update()
    local t = mce.tick()
    mce.each("Mission", function(e)
        if t == 1 then
            mission.start(e)
            assert(mission.fail(e) == false)   -- idle: nothing to fail
        elseif t == 5 then
            assert(mission.fail(e) == true)
            assert(mission.fail(e) == false)   -- already failed
            assert(mission.status(e) == "failed")
        end
    end)
end
)");
    r.build_chain();
    r.step(8);
    INFO(err(r.h()));
    REQUIRE(r.h().scripts()[0].state == ScriptState::Running);
    REQUIRE(r.failed_at.size() == 1);
    CHECK(r.failed_at[0] == 6);  // pushed on tick 5 (Post), readable on tick 6
    CHECK(r.failed_reason[0] == 3);
    CHECK(r.m().state == kit::MissionState::Failed);
    CHECK(r.completed_at.empty());
}

TEST_CASE("mission api: signals count by name and amount") {
    MRig r(R"(
function update()
    if mce.tick() == 4 then mission.signal("other") mission.signal("go", 2) end
    if mce.tick() == 6 then mission.signal("go") end
end
)");
    r.build_chain();
    r.w.try_get_mut<kit::Mission>(r.mission)->start_on_load = 1;
    r.w.try_get_mut<kit::Objective>(r.first)->count = 3;
    r.step(6);  // sent on tick 4 (Post), counted on tick 6; the "other" signal counts for nothing
    CHECK(r.w.try_get<kit::Objective>(r.first)->progress == 2);
    CHECK(r.w.try_get<kit::Objective>(r.first)->state == kit::ObjectiveState::Active);
    r.step(2);  // sent on tick 6, counted on tick 8
    CHECK(r.w.try_get<kit::Objective>(r.first)->progress == 3);
    CHECK(r.w.try_get<kit::Objective>(r.first)->state == kit::ObjectiveState::Done);
    CHECK(r.m().done == 1);
}

TEST_CASE("marker api: add writes a MapMarker, a second add changes only the named fields, remove takes it off") {
    MRig r(R"(
function update()
    local t = mce.tick()
    mce.each("LocalTransform", function(e)
        if mce.has(e, "Mission") then return end
        if t == 1 then
            marker.add(e, { icon = 3, color = { 1, 0.5, 0.25 }, layer = 2, label = "Depot", always_visible = true })
        elseif t == 3 then
            marker.add(e, { colour = vector.create(0, 1, 0), layer = 5 })
        elseif t == 5 then
            marker.remove(e)
            marker.remove(e)
        end
    end)
end
)");
    r.build_chain();
    const Entity e = r.w.spawn_with(LocalTransform{{4, 0, -6}, math::quat::identity(), {1, 1, 1}});
    r.step(1);
    const kit::MapMarker* mk = r.w.try_get<kit::MapMarker>(e);
    REQUIRE(mk != nullptr);
    CHECK(mk->icon == 3);
    CHECK(mk->layer == 2);
    CHECK(mk->label.view() == StringView("Depot"));
    CHECK(mk->always_visible == 1);
    CHECK(mk->color.r == 1.0f);
    CHECK(mk->color.g == 0.5f);
    CHECK(mk->color.b == 0.25f);
    CHECK(mk->color.a == 1.0f);
    Array<kit::MapMarkerView> views;
    kit::collect_map_markers(r.w, views);
    REQUIRE(views.size() == 1);
    CHECK(views[0].pos.x == 4.0f);
    CHECK(views[0].pos.y == -6.0f);
    r.step(2);  // tick 3: the second add
    mk = r.w.try_get<kit::MapMarker>(e);
    REQUIRE(mk != nullptr);
    CHECK(mk->icon == 3);
    CHECK(mk->layer == 5);
    CHECK(mk->label.view() == StringView("Depot"));
    CHECK(mk->color.r == 0.0f);
    CHECK(mk->color.g == 1.0f);
    r.step(2);  // tick 5: removed
    CHECK(r.w.try_get<kit::MapMarker>(e) == nullptr);
    INFO(err(r.h()));
    CHECK(r.h().scripts()[0].state == ScriptState::Running);
}

TEST_CASE("marker api: follow points a marker at a mission, and add without a table is a default marker") {
    MRig r(R"(
function update()
    if mce.tick() == 1 then
        mce.each("Mission", function(m)
            local e = mce.spawn({ LocalTransform = {} })
            marker.add(e, { follow = m })
            marker.add(mce.spawn({ LocalTransform = {} }))
        end)
    end
end
)");
    r.build_chain();
    r.step(2);
    u32 n = 0;
    bool followed = false;
    r.w.query<kit::MapMarker>().each([&](kit::MapMarker& m) {
        ++n;
        if (m.follow_objective == r.mission) followed = true;
    });
    CHECK(n == 2);
    CHECK(followed);
    CHECK(r.h().scripts()[0].state == ScriptState::Running);
}

TEST_CASE("mission api: bad arguments are Luau errors that name the call") {
    expect_error("mission.start(nil)", "argument");
    expect_error("mission.start('x')", "invalid argument");
    expect_error("mission.start(-1)", "not an entity");
    expect_error("mission.start(mce.spawn({ LocalTransform = {} }))", "mission.start: the entity has no Mission");
    expect_error("mission.status(mce.spawn({ LocalTransform = {} }))", "mission.status: the entity has no Mission");
    expect_error("mission.fail(mce.spawn({ LocalTransform = {} }))", "mission.fail: the entity has no Mission");
    expect_error("local d = mce.spawn({ LocalTransform = {} }) mce.destroy(d) mission.status(d)", "mission.status: the entity has no Mission");
    expect_error("mission.signal('')", "mission.signal: the name is empty");
    expect_error("mission.signal('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa')", "mission.signal: the name is longer than 31");
    expect_error("mission.signal('go', 0)", "mission.signal: the amount 0");
    expect_error("mission.signal('go', 1.5)", "mission.signal: the amount 1.5");
    expect_error("mission.signal({})", "invalid argument #1 to 'signal'");
    expect_error("marker.add(e, 5)", "marker.add: the second argument is a table");
    expect_error("marker.add(e, { layer = 40 })", "marker.add: 'layer' 40");
    expect_error("marker.add(e, { layer = 1.5 })", "marker.add: 'layer' 1.5");
    expect_error("marker.add(e, { icon = 'x' })", "marker.add: 'icon' must be a number");
    expect_error("marker.add(e, { color = { 1, 2 } })", "marker.add: 'color' takes {r, g, b}");
    expect_error("marker.add(e, { color = { 1, 2, 'x' } })", "marker.add: 'color' takes numbers");
    expect_error("marker.add(e, { color = 'red' })", "marker.add: 'color' takes");
    expect_error("marker.add(e, { label = 5 })", "marker.add: 'label' must be a string");
    expect_error("marker.add(e, { label = 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa' })", "marker.add: 'label' is longer than 31");
    expect_error("marker.add(e, { always_visible = 1 })", "marker.add: 'always_visible' must be a boolean");
    expect_error("marker.add(e, { colur = { 1, 1, 1 } })", "marker.add: unknown field 'colur'");
    expect_error("marker.add(e, { follow = mce.spawn({ LocalTransform = {} }) })", "marker.add: 'follow' must be an entity with an Objective or a Mission");
    expect_error("marker.add(nil)", "argument");
    expect_error("marker.remove('x')", "invalid argument");
}

TEST_CASE("mission api: a world without the kit's missions raises named errors, not a crash") {
    for (const char* body : {"mission.signal('go')", "marker.add(mce.spawn({ LocalTransform = {} }), {})"}) {
        const std::string src = std::string("function update() ") + body + " end";
        MRig r(src.c_str(), false, false);
        r.step(2);
        INFO(body);
        REQUIRE(r.h().scripts()[0].state == ScriptState::Failed);
        CHECK((err(r.h()).find("this world has no") != std::string::npos));
    }
}

TEST_CASE("mission api: the type checker knows mission and marker") {
    Array<char> defs;
    script::write_definitions(defs);
    const std::string text(defs.data(), defs.size());
    CHECK(text.find("declare mission: {") != std::string::npos);
    CHECK(text.find("declare marker: {") != std::string::npos);
    ScriptHost host;
    CHECK(script::add_source(host, "ok", "function update() mce.each('Mission', function(e) local s, o, t, d = mission.status(e) mission.start(e) mission.signal('go', 2) marker.add(e, { icon = 1, color = {1, 0, 0}, label = 'x' }) marker.remove(e) end) end"));
    ScriptHost bad;
    CHECK_FALSE(script::add_source(bad, "bad", "function update() mission.start('x') end"));
}

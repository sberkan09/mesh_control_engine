// Missions and objectives (M-MISSION 1): a reach -> kill -> signal chain completes at known ticks, the time
// limit and fail_on_death fail it, a save mid-mission resumes, and the same run twice has the same hash.

#include <doctest/doctest.h>

#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/missions.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

LocalTransform at(f32 x, f32 y, f32 z) { return LocalTransform{{x, y, z}, math::quat::identity(), {1, 1, 1}}; }

struct Log {
    u64 started = 0, completed = 0, failed = 0, failed_reason = 99;
    Array<u64> done;  // the tick each ObjectiveDone was seen
};

// A player at the origin, a zone at x = 10, an enemy at x = 20, and the chain reach -> kill -> signal.
struct Scene {
    World w;
    u64 t = 0;
    Log log;
    Entity player, zone, enemy, mission, reach, kill, signal;
    explicit Scene(f32 time_limit = 0.0f, bool fail_on_player_death = false) {
        w.register_components<LocalTransform, SaveKey>();
        install_missions(w, nullptr);
        player = w.spawn_with(SaveKey{1}, at(0, 0, 0), Health3D{});
        zone = w.spawn_with(SaveKey{2}, at(10, 0, 0));
        Health3D weak;
        weak.max = weak.current = 10.0f;
        enemy = w.spawn_with(SaveKey{3}, at(20, 0, 0), weak);
        Objective o3;
        o3.kind = ObjectiveKind::Signal;
        o3.signal = "delivered";
        o3.count = 2;
        signal = w.spawn_with(SaveKey{4}, o3);
        Objective o2;
        o2.kind = ObjectiveKind::Kill;
        o2.target = enemy;
        o2.next = signal;
        kill = w.spawn_with(SaveKey{5}, o2);
        Objective o1;
        o1.kind = ObjectiveKind::ReachZone;
        o1.target = zone;
        o1.radius = 2.0f;
        o1.next = kill;
        reach = w.spawn_with(SaveKey{6}, o1);
        Mission m;
        m.id = "intro";
        m.objective = reach;
        m.actor = player;
        m.time_limit = time_limit;
        if (fail_on_player_death) m.fail_on_death = player;
        m.start_on_load = 1;
        mission = w.spawn_with(SaveKey{7}, m);
    }
    void tally() {
        auto ev = EventAccess(w);
        log.started += ev.read<MissionStarted>().size();
        log.completed += ev.read<MissionCompleted>().size();
        for (const MissionFailed& f : ev.read<MissionFailed>()) {
            ++log.failed;
            log.failed_reason = f.reason;
        }
        for (const ObjectiveDone& d : ev.read<ObjectiveDone>()) {
            (void)d;
            log.done.push_back(t - 1);  // t was incremented by run()
        }
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) {
            w.tick(kDt, t++);
            tally();
        }
    }
    void send(const char* name, u32 amount = 1) {
        MissionSignal s;
        s.name = name;
        s.amount = amount;
        EventAccess(w).push(s);
    }
    void move_player(f32 x) { w.try_get_mut<LocalTransform>(player)->position.x = x; }
    Mission& mis() { return *w.try_get_mut<Mission>(mission); }
    Objective& obj(Entity e) { return *w.try_get_mut<Objective>(e); }
};

}  // namespace

TEST_CASE("missions: reach -> kill -> signal completes at known ticks") {
    Scene s;
    s.run(1);
    CHECK(s.log.started == 1);
    CHECK(s.mis().state == MissionState::Active);
    CHECK(s.mis().current == s.reach);
    CHECK(s.obj(s.reach).state == ObjectiveState::Active);
    CHECK(s.obj(s.kill).state == ObjectiveState::Pending);

    s.run(30);
    CHECK(s.obj(s.reach).state == ObjectiveState::Active);  // too far
    s.move_player(8.5f);                                    // 1.5 m from the zone
    s.run(1);
    CHECK(s.obj(s.reach).state == ObjectiveState::Done);
    CHECK(s.mis().current == s.kill);
    CHECK(s.mis().done == 1);
    REQUIRE(s.log.done.size() == 1);

    s.run(10);
    CHECK(s.obj(s.kill).state == ObjectiveState::Active);
    DamageInfo hit;
    hit.amount = 100.0f;
    hit.instigator = s.player;
    REQUIRE(apply_damage(s.w, s.enemy, hit));
    s.run(2);
    CHECK(s.obj(s.kill).state == ObjectiveState::Done);
    CHECK(s.mis().current == s.signal);

    s.run(5);
    s.send("wrong");
    s.send("delivered");
    s.run(2);
    CHECK(s.obj(s.signal).progress == 1);
    CHECK(s.mis().state == MissionState::Active);
    s.send("delivered");
    s.run(2);
    CHECK(s.mis().state == MissionState::Complete);
    CHECK(s.mis().done == 3);
    CHECK(s.log.completed == 1);
    CHECK(s.log.failed == 0);
    CHECK(s.log.done.size() == 3);
}

TEST_CASE("missions: the time limit fails an active mission") {
    Scene s(1.0f);
    s.run(60);  // the first tick starts it, then 59 ticks of the limit's 60
    CHECK(s.mis().state == MissionState::Active);
    CHECK(s.mis().ticks == 59);
    s.run(1);
    CHECK(s.mis().state == MissionState::Failed);
    CHECK(s.log.failed == 1);
    CHECK(s.log.failed_reason == 0);
    s.move_player(10.0f);  // a failed mission no longer listens
    s.run(5);
    CHECK(s.obj(s.reach).state == ObjectiveState::Active);
    CHECK(s.log.done.size() == 0);
}

TEST_CASE("missions: fail_on_death fails when the entity dies") {
    Scene s(0.0f, true);
    s.run(10);
    CHECK(s.mis().state == MissionState::Active);
    DamageInfo hit;
    hit.amount = 1000.0f;
    REQUIRE(apply_damage(s.w, s.player, hit));
    s.run(2);
    CHECK(s.mis().state == MissionState::Failed);
    CHECK(s.log.failed == 1);
    CHECK(s.log.failed_reason == 1);
}

TEST_CASE("missions: a mission waits for start, and a timer objective counts ticks") {
    World w;
    install_missions(w, nullptr);
    Objective t;
    t.kind = ObjectiveKind::Timer;
    t.duration = 0.5f;
    const Entity o = w.spawn_with(t);
    Mission m;
    m.objective = o;
    const Entity me = w.spawn_with(m);
    for (u64 i = 0; i < 5; ++i) w.tick(kDt, i);
    CHECK(w.try_get<Mission>(me)->state == MissionState::Idle);
    w.try_get_mut<Mission>(me)->start = 1;
    u64 i = 5;
    w.tick(kDt, i++);  // starts
    CHECK(w.try_get<Mission>(me)->state == MissionState::Active);
    for (u32 k = 0; k < 29; ++k) w.tick(kDt, i++);
    CHECK(w.try_get<Mission>(me)->state == MissionState::Active);
    w.tick(kDt, i++);  // the 30th tick since it went active
    CHECK(w.try_get<Mission>(me)->state == MissionState::Complete);
}

TEST_CASE("missions: a save mid-mission resumes where it was") {
    Array<byte> bytes;
    {
        Scene a;
        a.run(31);
        a.move_player(9.0f);
        a.run(3);  // reach done; the kill objective is active
        REQUIRE(a.mis().current == a.kill);
        save::write_save(a.w, {}, bytes);
        DamageInfo hit;
        hit.amount = 100.0f;
        REQUIRE(apply_damage(a.w, a.enemy, hit));
        a.run(3);
        a.send("delivered", 2);
        a.run(3);
        CHECK(a.mis().state == MissionState::Complete);
    }
    Scene b;  // the base world: a fresh, idle-by-default copy
    b.w.try_get_mut<Mission>(b.mission)->start_on_load = 0;
    auto st = save::read_save(b.w, bytes.span());
    REQUIRE(st);
    CHECK(b.mis().state == MissionState::Active);
    CHECK(b.mis().current == b.kill);
    CHECK(b.mis().done == 1);
    CHECK(b.obj(b.reach).state == ObjectiveState::Done);
    CHECK(b.mis().ticks == 33);  // 34 ticks run, the first one started it
    b.t = 34;
    b.run(0);
    DamageInfo hit;
    hit.amount = 100.0f;
    REQUIRE(apply_damage(b.w, b.enemy, hit));
    b.run(3);
    b.send("delivered", 2);
    b.run(3);
    CHECK(b.mis().state == MissionState::Complete);
    CHECK(b.mis().done == 3);
}

TEST_CASE("missions: the same run twice has the same state hash") {
    auto run = [] {
        Scene s(40.0f);
        s.run(31);
        s.move_player(9.0f);
        s.run(5);
        DamageInfo hit;
        hit.amount = 100.0f;
        apply_damage(s.w, s.enemy, hit);
        s.run(4);
        s.send("delivered", 2);
        s.run(4);
        CHECK(s.mis().state == MissionState::Complete);
        return s.w.state_hash();
    };
    CHECK(run() == run());
}

namespace {
// A mission with one Kill objective on a world with the player and three victims of different teams.
struct KillRig {
    World w;
    u64 t = 0;
    Entity player, other, cop1, cop2, cop3, kill, mission;
    explicit KillRig(u8 team, bool by_actor, u32 count, Entity target = Entity{}) {
        w.register_components<LocalTransform, SaveKey>();
        install_missions(w, nullptr);
        player = w.spawn_with(SaveKey{1}, at(0, 0, 0), Health3D{});
        auto victim = [&](u32 key, u8 tm) {
            Health3D h;
            h.max = h.current = 10.0f;
            h.team = tm;
            return w.spawn_with(SaveKey{key}, at(5, 0, 0), h);
        };
        other = victim(2, 3);
        cop1 = victim(3, 2);
        cop2 = victim(4, 2);
        cop3 = victim(5, 2);
        Objective o;
        o.kind = ObjectiveKind::Kill;
        o.team = team;
        o.by_actor = by_actor ? 1 : 0;
        o.count = count;
        o.target = target;
        kill = w.spawn_with(SaveKey{6}, o);
        Mission m;
        m.id = "kill";
        m.objective = kill;
        m.actor = player;
        m.start_on_load = 1;
        mission = w.spawn_with(SaveKey{7}, m);
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    void shoot(Entity victim, Entity by) {
        DamageInfo hit;
        hit.amount = 100.0f;
        hit.instigator = by;
        REQUIRE(apply_damage(w, victim, hit));
        run(2);
    }
    u32 progress() { return w.try_get<Objective>(kill)->progress; }
    MissionState state() { return w.try_get<Mission>(mission)->state; }
};
}  // namespace

TEST_CASE("missions: a kill objective can count one team and only the actor's kills (M-MISSIONKILL)") {
    KillRig r(2, true, 2);
    r.run(2);
    r.shoot(r.other, r.player);  // wrong team
    CHECK(r.progress() == 0);
    r.shoot(r.cop1, Entity{});   // right team, not the actor's
    CHECK(r.progress() == 0);
    r.shoot(r.cop2, r.player);
    CHECK(r.progress() == 1);
    CHECK(r.state() == MissionState::Active);
    r.shoot(r.cop3, r.player);
    CHECK(r.state() == MissionState::Complete);
}

TEST_CASE("missions: a kill objective whose target is already dead is done (M-MISSIONKILL)") {
    KillRig d(0, false, 1, Entity{});
    d.w.try_get_mut<Objective>(d.kill)->target = d.other;
    DamageInfo hit;
    hit.amount = 100.0f;
    REQUIRE(apply_damage(d.w, d.other, hit));
    d.run(4);
    CHECK(d.state() == MissionState::Complete);
}

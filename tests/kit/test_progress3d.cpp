// Progression and goals (M-PROGRESS): counters from events, a goal pays once, deterministic order.

#include <doctest/doctest.h>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/missions.h"
#include "mce/kit/melee3d.h"
#include "mce/kit/progress3d.h"
#include "mce/kit/seats3d.h"
#include "mce/kit/wanted3d.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

LocalTransform at(f32 x, f32 y, f32 z) { return LocalTransform{{x, y, z}, math::quat::identity(), {1, 1, 1}}; }

struct Scene {
    World w;
    u64 t = 0;
    Entity player;
    Scene() {
        w.register_components<LocalTransform>();
        install_progress3d(w);
        player = w.spawn_with(at(0, 0, 0), Progress3D{}, Wallet3D{100});
    }
    Entity goal(const char* name, GoalStat stat, u32 target, u32 reward) {
        Goal3D g;
        g.name = name;
        g.stat = stat;
        g.target = target;
        g.reward = reward;
        g.owner = player;
        return w.spawn_with(at(0, 0, 0), g);
    }
    Entity victim() {
        Health3D h;
        h.max = h.current = 10.0f;
        return w.spawn_with(at(5, 0, 0), h);
    }
    void kill(Entity v, Entity killer) {
        DamageInfo d;
        d.amount = 1000.0f;
        d.instigator = killer;
        apply_damage(w, v, d);
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    Progress3D& prog() { return *w.try_get_mut<Progress3D>(player); }
    i32 cash() { return w.try_get<Wallet3D>(player)->cash; }
};

}  // namespace

TEST_CASE("progress3d: kills count only the player's kills of other entities, a goal pays once") {
    Scene s;
    const Entity g = s.goal("First blood", GoalStat::Kills, 2, 50);
    s.kill(s.victim(), s.player);
    s.run(2);
    CHECK(s.prog().kills == 1);
    CHECK(s.w.try_get<Goal3D>(g)->done == 0);
    s.kill(s.victim(), Entity{});  // somebody else's kill
    s.kill(s.victim(), s.player);
    s.run(2);
    CHECK(s.prog().kills == 2);
    CHECK(s.w.try_get<Goal3D>(g)->done == 1);
    CHECK(s.cash() == 150);
    CHECK(s.prog().goals_done == 1);
    s.kill(s.victim(), s.player);
    s.run(5);
    CHECK(s.prog().kills == 3);
    CHECK(s.cash() == 150);  // paid once
}

TEST_CASE("progress3d: melee hits add up, a ride counts, a mission counts for its actor only") {
    Scene s;
    const Entity g1 = s.goal("Fists", GoalStat::MeleeHits, 3, 10);
    const Entity g2 = s.goal("Rider", GoalStat::Rides, 1, 20);
    const Entity g3 = s.goal("Job", GoalStat::Missions, 1, 30);
    EventAccess(s.w).push(MeleeSwung3D{s.player, 2, 0});
    EventAccess(s.w).push(MeleeSwung3D{Entity{}, 5, 0});  // someone else's swing
    s.run(2);
    CHECK(s.prog().melee_hits == 2);
    CHECK(s.w.try_get<Goal3D>(g1)->done == 0);
    EventAccess(s.w).push(MeleeSwung3D{s.player, 1, 0});
    EventAccess(s.w).push(VehicleEntered3D{s.player, Entity{}});
    s.run(2);
    CHECK(s.prog().melee_hits == 3);
    CHECK(s.prog().rides == 1);
    CHECK(s.w.try_get<Goal3D>(g1)->done == 1);
    CHECK(s.w.try_get<Goal3D>(g2)->done == 1);
    const Entity other = s.w.spawn_with(at(0, 0, 0));
    Mission theirs;
    theirs.actor = other;
    const Entity mt = s.w.spawn_with(theirs);
    Mission mine;
    mine.actor = s.player;
    const Entity mm = s.w.spawn_with(mine);
    EventAccess(s.w).push(MissionCompleted{mt});
    s.run(2);
    CHECK(s.prog().missions == 0);
    EventAccess(s.w).push(MissionCompleted{mm});
    s.run(2);
    CHECK(s.prog().missions == 1);
    CHECK(s.w.try_get<Goal3D>(g3)->done == 1);
    CHECK(s.cash() == 160);
    CHECK(s.prog().goals_done == 3);
}

TEST_CASE("progress3d: a goal without a living owner stays, the same run twice has the same state hash") {
    u64 hashes[2] = {};
    for (u32 k = 0; k < 2; ++k) {
        Scene s;
        Goal3D orphan;
        orphan.target = 1;
        orphan.reward = 500;
        const Entity og = s.w.spawn_with(at(0, 0, 0), orphan);  // owner is null
        s.goal("A", GoalStat::Kills, 1, 5);
        s.goal("B", GoalStat::Kills, 1, 7);
        s.kill(s.victim(), s.player);
        s.run(10);
        CHECK(s.w.try_get<Goal3D>(og)->done == 0);
        CHECK(s.cash() == 112);
        hashes[k] = s.w.state_hash();
    }
    CHECK(hashes[0] == hashes[1]);
}

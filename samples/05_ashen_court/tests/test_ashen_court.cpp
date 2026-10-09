// Ashen Court gameplay (M7), driven through the real systems with scripted input: the enemy FSMs
// (husk strike, wisp orb, brute charge into a wall), sword combat, heart pickups, the swept camera
// boom, death -> restart, and the attract bot clearing the court deterministically.
#include <doctest/doctest.h>

#include <cmath>

#include "game/court.h"
#include "mce/input/ecs.h"

using namespace mce;
using namespace court;
using math::kPi;

namespace {

struct Game {
    World w;
    u64 tick = 0;

    Game() {
        input::install(w);
        REQUIRE(court::install(w));
    }
    void step(input::InputState in = {}) {
        in.tick = tick;
        input::publish(w, in);
        w.tick(1.0 / 60.0, tick++);
    }
    void idle(u32 n) {
        for (u32 i = 0; i < n; ++i) step();
    }
    CourtState& gs() { return w.resource<CourtState>(); }
    Actor& actor(Entity e) { return *w.try_get_mut<Actor>(e); }
    Health& health(Entity e) { return *w.try_get_mut<Health>(e); }
    // Moves a character so its capsule stands at `feet` (keeps the standing height above the floor).
    void place(Entity e, vec3 feet) {
        Actor& a = actor(e);
        const f32 above = a.pos.y;  // spawned standing on the floor (y = 0)
        a.pos = a.prev_pos = {feet.x, feet.y + above, feet.z};
        a.vy = 0.0f;
        w.resource<Physics>().world.teleport(e, a.pos, math::quat::identity());
    }
    template <class Brain>
    Entity first() {
        Entity out;
        w.query<const Brain&>().each([&](Entity e, const Brain&) {
            if (out.is_null()) out = e;
        });
        return out;
    }
    // Leaves only `keep` among the enemies (and the player), so one behavior can be watched alone.
    void isolate(Entity keep) {
        Array<Entity> doomed;
        w.query<const Senses&>().each([&](Entity e, const Senses&) {
            if (e != keep) doomed.push_back(e);
        });
        for (Entity e : doomed) {
            w.resource<Physics>().world.destroy_body(e);
            w.destroy(e);
        }
        gs().enemies = gs().enemies_total = 1;
    }
};

input::InputState fire() {
    input::InputState in;
    in.fire_pressed = true;
    return in;
}

}  // namespace

TEST_CASE("ashen court: install builds the court and spawns the cast") {
    Game g;
    CHECK(g.gs().mode == Mode::Playing);
    CHECK(g.gs().enemies == 9);
    u32 husks = 0, wisps = 0, brutes = 0, hearts = 0;
    g.w.query<const HuskBrain&>().each([&](const HuskBrain&) { ++husks; });
    g.w.query<const WispBrain&>().each([&](const WispBrain&) { ++wisps; });
    g.w.query<const BruteBrain&>().each([&](const BruteBrain&) { ++brutes; });
    g.w.query<const Heart&>().each([&](const Heart&) { ++hearts; });
    CHECK(husks == 4);
    CHECK(wisps == 3);
    CHECK(brutes == 2);
    CHECK(hearts == 4);
    g.idle(30);
    const Actor& p = g.actor(g.gs().player);
    CHECK(p.grounded == 1);  // landed on the floor slab
    CHECK(p.pos.y == doctest::Approx(0.865).epsilon(0.02));
}

TEST_CASE("ashen court: a husk notices the player, closes in and strikes (fsm.h table)") {
    Game g;
    const Entity husk = g.first<HuskBrain>();
    g.isolate(husk);
    const vec3 at = g.actor(husk).pos;
    g.place(g.gs().player, {at.x, 0.0f, at.z + 5.0f});
    bool chased = false, wound_up = false;
    for (u32 i = 0; i < 180 && g.gs().damage_taken == 0; ++i) {
        g.step();
        const HuskState st = g.w.try_get<HuskBrain>(husk)->state;
        chased = chased || st == HuskState::Chase;
        wound_up = wound_up || st == HuskState::WindUp;
    }
    CHECK(chased);
    CHECK(wound_up);
    CHECK(g.gs().damage_taken == 12);
    CHECK(g.health(g.gs().player).hp == 88);
    CHECK(g.w.try_get<HuskBrain>(husk)->state == HuskState::Recover);  // the blow lands on WindUp's exit
}

TEST_CASE("ashen court: two sword hits kill a husk; the first staggers it") {
    Game g;
    const Entity husk = g.first<HuskBrain>();
    g.isolate(husk);
    const vec3 at = g.actor(husk).pos;
    g.place(g.gs().player, {at.x, 0.0f, at.z + 1.4f});  // the player faces -Z, toward the husk
    g.step(fire());
    for (u32 i = 0; i < kSwingHitTick; ++i) g.step();
    CHECK(g.health(husk).hp == 60 - 34);
    CHECK(g.w.try_get<HuskBrain>(husk)->state == HuskState::Stagger);
    g.idle(kSwingTicks);
    const vec3 now = g.actor(husk).pos;
    g.place(g.gs().player, {now.x, 0.0f, now.z + 1.4f});
    g.actor(g.gs().player).yaw = kPi;
    g.step(fire());
    g.idle(kSwingHitTick + 2);
    CHECK(g.gs().kills == 1);
    CHECK(g.gs().enemies == 0);
    CHECK_FALSE(g.w.alive(husk));
    CHECK(g.gs().mode == Mode::Victory);  // the last enemy fell
    CHECK(g.w.try_get<PlayerCtl>(g.gs().player)->hits == 2);
}

TEST_CASE("ashen court: a wisp keeps its distance and its orb hits") {
    Game g;
    const Entity wisp = g.first<WispBrain>();
    g.isolate(wisp);
    const vec3 at = g.actor(wisp).pos;
    g.place(g.gs().player, {at.x, 0.0f, at.z + 8.0f});
    bool cast = false, orb = false;
    for (u32 i = 0; i < 400 && g.gs().damage_taken == 0; ++i) {
        g.step();
        cast = cast || g.w.try_get<WispBrain>(wisp)->state == WispState::Cast;
        g.w.query<const Orb&>().each([&](const Orb&) { orb = true; });
    }
    CHECK(cast);
    CHECK(orb);
    CHECK(g.gs().damage_taken == 10);
    const f32 d = length(g.actor(wisp).pos - g.actor(g.gs().player).pos);
    CHECK(d > 4.0f);  // it hovers off, it does not close to melee
}

TEST_CASE("ashen court: a brute charge hits once, slams the wall and leaves it stunned for double damage") {
    Game g;
    const Entity brute = g.first<BruteBrain>();
    g.isolate(brute);
    g.place(brute, {-14.0f, 0.0f, 6.0f});
    g.place(g.gs().player, {-20.5f, 0.0f, 6.0f});  // back to the west wall (inner face at -21.5)
    bool charged = false, stunned = false;
    for (u32 i = 0; i < 300 && !stunned; ++i) {
        g.step();
        const BruteState st = g.w.try_get<BruteBrain>(brute)->state;
        charged = charged || st == BruteState::Charge;
        stunned = st == BruteState::Stunned;
    }
    REQUIRE(stunned);
    CHECK(charged);
    CHECK(g.gs().damage_taken == 25);  // one hit per charge, however long the overlap
    // Stunned: the sword lands for double.
    Actor& p = g.actor(g.gs().player);
    const vec3 b = g.actor(brute).pos;
    g.place(g.gs().player, {b.x + 1.4f, 0.0f, b.z});
    p.yaw = -0.5f * kPi;  // face -X
    g.step(fire());
    g.idle(kSwingHitTick);
    CHECK(g.health(brute).hp == 150 - 2 * 34);
}

TEST_CASE("ashen court: an ember heart heals a hurt player and is used up; a healthy one leaves it") {
    Game g;
    g.isolate(g.first<HuskBrain>());
    g.place(g.first<HuskBrain>(), {18.0f, 0.0f, 18.0f});  // out of the way, behind the colonnade
    g.place(g.gs().player, {-18.0f, 0.0f, 18.0f});        // on the first heart
    g.idle(3);
    u32 hearts = 0;
    g.w.query<const Heart&>().each([&](const Heart&) { ++hearts; });
    CHECK(hearts == 4);  // full health: not taken
    g.health(g.gs().player).hp = 40;
    g.idle(2);
    hearts = 0;
    g.w.query<const Heart&>().each([&](const Heart&) { ++hearts; });
    CHECK(hearts == 3);
    CHECK(g.health(g.gs().player).hp == 75);
}

TEST_CASE("ashen court: the camera boom is pulled in front of a wall, never through it") {
    Game g;
    g.isolate(g.first<HuskBrain>());
    g.place(g.first<HuskBrain>(), {18.0f, 0.0f, -18.0f});
    g.place(g.gs().player, {0.0f, 0.0f, 19.5f});
    g.gs().cam_yaw = 0.0f;  // the boom points +Z, into the south wall (inner face at 21.5)
    g.idle(2);
    CHECK(g.gs().boom < kBoomLength - 1.0f);
    CHECK(g.gs().camera_hits > 0);
    const vec3 pivot = g.actor(g.gs().player).pos + kHeadOffset;
    const f32 cp = std::cos(g.gs().cam_pitch);
    const f32 eye_z = pivot.z + std::cos(g.gs().cam_yaw) * cp * g.gs().boom;
    CHECK(eye_z + kBoomRadius <= 21.5f + 1e-3f);
    g.gs().cam_yaw = kPi;  // turned toward the open court: full length again
    g.idle(1);
    CHECK(g.gs().boom == doctest::Approx(kBoomLength));
}

TEST_CASE("ashen court: death waits for confirm, then the court restarts with a fresh cast") {
    Game g;
    const u32 serial = g.gs().serial;
    const Entity husk = g.first<HuskBrain>();
    g.isolate(husk);
    g.health(g.gs().player).hp = 5;
    const vec3 at = g.actor(husk).pos;
    g.place(g.gs().player, {at.x, 0.0f, at.z + 1.2f});
    for (u32 i = 0; i < 240 && g.gs().mode == Mode::Playing; ++i) g.step();
    REQUIRE(g.gs().mode == Mode::Dead);
    CHECK(g.gs().deaths == 1);
    g.idle(30);
    CHECK(g.gs().mode == Mode::Dead);  // no auto-restart
    input::InputState confirm;
    confirm.confirm_pressed = true;
    g.step(confirm);
    g.idle(2);
    CHECK(g.gs().mode == Mode::Playing);
    CHECK(g.gs().serial == serial + 1);
    CHECK(g.gs().enemies == 9);
    CHECK(g.gs().kills == 0);
    CHECK(g.health(g.gs().player).hp == 100);
    u32 actors = 0;
    g.w.query<const Actor&>().each([&](const Actor&) { ++actors; });
    CHECK(actors == 10);  // the old cast is gone
}

TEST_CASE("ashen court: the bot clears the court, the same way every run") {
    u64 hashes[2] = {};
    for (u64& out : hashes) {
        Game g;
        for (u32 i = 0; i < 3600 && g.gs().mode == Mode::Playing; ++i) g.step(court::autopilot(g.w, g.tick));
        CHECK(g.gs().mode == Mode::Victory);
        CHECK(g.gs().kills == 9);
        CHECK(g.gs().deaths == 0);
        out = hash_game(g.w, 0);
    }
    CHECK(hashes[0] == hashes[1]);
}

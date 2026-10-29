// Police roadblocks that come and go with the wanted level (M-OPENWORLD-POLICE 3): Roadblock3D spawns its cars and officers
// (through the game's hook here) when an offender reaches min_level at a distance, takes them down after `hold` seconds, and
// replays to the same state.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/roadblock.h"
#include "mce/kit/wanted3d.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

LocalTransform at(f32 x, f32 y, f32 z) { return LocalTransform{{x, y, z}, math::quat::identity(), {1, 1, 1}}; }

u32 g_kinds[2];

u32 g_failed_calls = 0;

Entity fail_piece(Frame&, u32, math::vec3, math::quat) {
    ++g_failed_calls;
    return Entity{};
}

Entity make_piece(Frame& f, u32 kind, math::vec3 position, math::quat rotation) {
    ++g_kinds[kind];
    return f.commands().spawn(LocalTransform{position, rotation, {1, 1, 1}});
}

struct Scene {
    World w;
    u64 t = 0;
    Entity block, offender;
    explicit Scene(f32 heat, f32 offender_x, Roadblock3D rb = {}, RoadblockSpawnHook spawn = RoadblockSpawnHook{&make_piece}) {
        g_failed_calls = 0;
        g_kinds[0] = g_kinds[1] = 0;
        w.register_components<LocalTransform>();
        install_wanted3d(w);
        install_roadblocks(w, nullptr);
        w.set_resource(spawn);
        // The marker faces so that its local X runs along world Z (across a road along X) and its +Z points west.
        LocalTransform lt = at(0, 1, 0);
        lt.rotation = math::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, -1.5707963f);
        block = w.spawn_with(lt, rb);
        Wanted3D wd;
        wd.heat = heat;
        wd.crimes = 1;
        wd.cooldown_delay = 1.0e9f;  // the level stays what the test sets
        offender = w.spawn_with(at(offender_x, 1, 0), wd, Health3D{});
        run(2);
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    u32 pieces() {
        u32 n = 0;
        w.query<const RoadblockPiece3D>().each([&](Entity, const RoadblockPiece3D&) { ++n; });
        return n;
    }
    void set_heat(f32 h) { w.try_get_mut<Wanted3D>(offender)->heat = h; }
};

}  // namespace

TEST_CASE("roadblock: it stands up at its wanted level and from far enough away") {
    {
        Scene s(70.0f, 150.0f);  // level 4, 150 m away
        s.run(2);
        CHECK(s.pieces() == 4);
        CHECK(g_kinds[0] == 2);
        CHECK(g_kinds[1] == 2);
        CHECK(s.w.try_get<Roadblock3D>(s.block)->up == 1);
        // Two cars across the road (world Z), the officers 3 m west of the line.
        f32 car_z[2] = {};
        u32 n = 0;
        s.w.query<const RoadblockPiece3D, const LocalTransform>().each([&](Entity, const RoadblockPiece3D& p, const LocalTransform& lt) {
            CHECK(p.owner == s.block);
            if (std::abs(lt.position.x) < 0.01f && n < 2) car_z[n++] = lt.position.z;
            else CHECK(lt.position.x == doctest::Approx(-3.0f).epsilon(0.01));
        });
        REQUIRE(n == 2);
        CHECK(std::abs(car_z[0] - car_z[1]) == doctest::Approx(3.6f).epsilon(0.01));
    }
    {
        Scene s(70.0f, 30.0f);  // closer than spawn_min: nothing appears under its nose
        s.run(5);
        CHECK(s.pieces() == 0);
    }
    {
        Scene s(30.0f, 150.0f);  // level 2 is below min_level 3
        s.run(5);
        CHECK(s.pieces() == 0);
    }
    {
        Scene s(70.0f, 500.0f);  // beyond spawn_max
        s.run(5);
        CHECK(s.pieces() == 0);
    }
}

TEST_CASE("roadblock: it is taken down after hold seconds below its level, but never in sight") {
    Roadblock3D rb;
    rb.hold = 2.0f;
    Scene s(70.0f, 150.0f, rb);
    s.run(2);
    REQUIRE(s.pieces() == 4);
    s.set_heat(0.0f);
    s.run(60);  // 1 s: still up
    CHECK(s.pieces() == 4);
    s.run(90);  // past hold
    s.run(2);
    CHECK(s.pieces() == 0);
    CHECK(s.w.try_get<Roadblock3D>(s.block)->up == 0);
    // Up again at the level, then the offender walks close: the wanted level drops but the block stays until it is out of sight.
    s.set_heat(70.0f);
    s.run(3);
    REQUIRE(s.pieces() == 4);
    s.w.try_get_mut<LocalTransform>(s.offender)->position = {20.0f, 1.0f, 0.0f};
    s.set_heat(0.0f);
    s.run(300);
    CHECK(s.pieces() == 4);
    s.w.try_get_mut<LocalTransform>(s.offender)->position = {200.0f, 1.0f, 0.0f};
    s.run(5);
    CHECK(s.pieces() == 0);
}

TEST_CASE("roadblock: the same run twice ends in the same state") {
    const auto play = [] {
        Scene s(70.0f, 150.0f);
        s.run(200);
        s.set_heat(0.0f);
        s.run(1000);
        return s.w.state_hash();
    };
    CHECK(play() == play());
}

TEST_CASE("roadblock: a piece's ground must be clear, and a failed spawn is not retried every tick") {
    {
        World w;
        g_kinds[0] = g_kinds[1] = 0;
        w.register_components<LocalTransform>();
        install_wanted3d(w);
        install_roadblocks(w, nullptr);
        w.set_resource(RoadblockSpawnHook{&make_piece});
        LocalTransform lt = at(0, 1, 0);
        lt.rotation = math::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, -1.5707963f);
        w.spawn_with(lt, Roadblock3D{});
        Wanted3D wd;
        wd.heat = 70.0f;
        wd.crimes = 1;
        wd.cooldown_delay = 1.0e9f;
        w.spawn_with(at(150, 1, 0), wd, Health3D{});
        const Entity walker = w.spawn_with(at(-3.0f, 1.0f, 1.8f), Health3D{});  // on an officer's spot
        u64 t = 0;
        for (u32 i = 0; i < 30; ++i) w.tick(kDt, t++);
        u32 pieces = 0;
        w.query<const RoadblockPiece3D>().each([&](Entity, const RoadblockPiece3D&) { ++pieces; });
        CHECK(pieces == 0);
        CHECK(g_kinds[0] + g_kinds[1] == 0);
        w.try_get_mut<LocalTransform>(walker)->position = {-3.0f, 1.0f, 40.0f};
        for (u32 i = 0; i < 5; ++i) w.tick(kDt, t++);
        w.query<const RoadblockPiece3D>().each([&](Entity, const RoadblockPiece3D&) { ++pieces; });
        CHECK(pieces == 4);
    }
    {  // nothing can be made: the hook is asked once per piece, then the marker waits 5 s
        Scene s(70.0f, 150.0f, {}, RoadblockSpawnHook{&fail_piece});
        s.run(60);
        CHECK(g_failed_calls == 4);
        CHECK(s.pieces() == 0);
        s.run(300);  // past the wait: one more round
        CHECK(g_failed_calls == 8);
    }
}

TEST_CASE("roadblock: a loaded game puts the block up again from the wanted level") {
    // Runtime-spawned pieces are not saved. Save a world with the block up, load it into a fresh world that has the same marker and
    // offender and no pieces: the marker does not believe it is up, and the block stands again.
    const auto build = [](Scene& s, f32 heat) {
        s.w.register_components<SaveKey>();
        s.w.add(s.block, SaveKey{1});
        s.w.add(s.offender, SaveKey{2});
        s.set_heat(heat);
    };
    Array<byte> bytes;
    {
        Scene a(70.0f, 150.0f);
        build(a, 70.0f);
        a.run(3);
        REQUIRE(a.pieces() == 4);
        save::write_save(a.w, {}, bytes);
    }
    Scene b(0.0f, 150.0f);
    build(b, 0.0f);
    b.run(3);
    REQUIRE(b.pieces() == 0);
    REQUIRE(save::read_save(b.w, bytes.span()));
    b.run(5);
    CHECK(b.pieces() == 4);
    CHECK(b.w.try_get<Roadblock3D>(b.block)->up == 1);
}

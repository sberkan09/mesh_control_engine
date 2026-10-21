#include <doctest/doctest.h>

#include "mce/ecs/world.h"

using namespace mce;

namespace {
struct Pos {
    f32 x, y;
};
struct Vel {
    f32 x, y;
};
struct Life {
    u32 ticks;
};
struct Rng {
    u64 state;
    u32 next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<u32>(state >> 33);
    }
};

void integrate(Frame& f) {
    const f32 dt = f.dt;
    f.query<Pos&, const Vel&>().each([dt](Pos& p, const Vel& v) {
        p.x += v.x * dt;
        p.y += v.y * dt;
    });
}
void age(Frame& f) {
    f.query<Life&>().each([&](Entity e, Life& l) {
        if (++l.ticks > 90) f.commands().destroy(e);
    });
}
void spawner(Frame& f) {
    Rng& rng = f.resource<Rng>();
    for (int i = 0; i < 8; ++i) {
        const f32 vx = static_cast<f32>(rng.next() % 200) / 10.0f - 10.0f;
        const f32 vy = static_cast<f32>(rng.next() % 200) / 10.0f - 10.0f;
        if (rng.next() % 3) f.commands().spawn(Pos{0, 0}, Vel{vx, vy}, Life{0});
        else f.commands().spawn(Pos{0, 0}, Vel{vx, vy});
    }
}

u64 run_sim(u64 seed, u32 ticks, bool profile = false) {
    World w;
    w.set_profiling(profile);
    w.set_resource(Rng{seed});
    w.add_system(Phase::FixedUpdate_Pre, "spawner", &spawner);
    w.add_system(Phase::FixedUpdate_Physics, "integrate", &integrate);
    w.add_system(Phase::FixedUpdate_Post, "age", &age);
    for (u32 t = 1; t <= ticks; ++t) w.tick(1.0 / 60, t);
    return w.state_hash();
}
}  // namespace

TEST_CASE("state_hash: identical inputs give identical hashes (FR-RUN-040)") {
    const u64 a = run_sim(7, 600);
    const u64 b = run_sim(7, 600);
    const u64 c = run_sim(8, 600);
    CHECK(a == b);
    CHECK(a != c);
}

TEST_CASE("state_hash reflects component data") {
    World w;
    Entity e = w.spawn_with(Pos{1, 1});
    const u64 h1 = w.state_hash();
    w.set(e, Pos{1, 2});
    CHECK(w.state_hash() != h1);
    w.set(e, Pos{1, 1});
    CHECK(w.state_hash() == h1);
}

TEST_CASE("system profiling: per-system timings, off by default, never in the hash") {
    CHECK(run_sim(7, 120, true) == run_sim(7, 120, false));

    World w;
    w.set_resource(Rng{3});
    w.add_system(Phase::FixedUpdate_Pre, "spawner", &spawner);
    w.add_system(Phase::FixedUpdate_Physics, "integrate", &integrate);
    w.add_system(Phase::FixedUpdate_Post, "age", &age);
    Array<SystemTiming> t;
    w.tick(1.0 / 60, 1);
    w.system_timings(t);
    REQUIRE(t.size() == 3);
    CHECK(t[0].runs == 0);  // profiling off: nothing measured
    CHECK_FALSE(w.profiling());

    w.set_profiling(true);
    for (u32 i = 2; i <= 11; ++i) w.tick(1.0 / 60, i);
    w.system_timings(t);
    REQUIRE(t.size() == 3);
    CHECK(StringView(t[0].name) == StringView("spawner"));
    CHECK(t[0].phase == Phase::FixedUpdate_Pre);
    CHECK(StringView(t[1].name) == StringView("integrate"));
    CHECK(t[1].phase == Phase::FixedUpdate_Physics);
    CHECK(StringView(t[2].name) == StringView("age"));
    for (const SystemTiming& s : t) {
        CHECK(s.runs == 10);
        CHECK(s.total_ms >= 0.0);
        CHECK(s.last_ms <= s.total_ms);
        CHECK(s.mean_ms() == doctest::Approx(s.total_ms / 10.0));
    }
    w.reset_system_timings();
    w.system_timings(t);
    CHECK(t[1].runs == 0);
    w.set_profiling(false);
    w.tick(1.0 / 60, 12);
    w.system_timings(t);
    CHECK(t[1].runs == 0);
}

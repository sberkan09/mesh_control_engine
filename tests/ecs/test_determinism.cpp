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

u64 run_sim(u64 seed, u32 ticks) {
    World w;
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

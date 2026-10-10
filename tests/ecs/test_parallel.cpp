// Auto-parallel scheduling experiment (World::set_parallel + SystemAccess): batching rules and
// the FR-RUN-040 property — the parallel schedule hashes exactly like the serial one.
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include "mce/ecs/world.h"

using namespace mce;

namespace {
struct Pos {
    f32 x, y;
};
struct Vel {
    f32 x, y;
};
struct Spin {
    f32 angle, rate;
};
struct Heat {
    f32 value;
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
struct Watch {
    u64 changed_rows = 0;
    u64 pings = 0;
};
struct Ping {
    u32 n;
};

void spawner(Frame& f) {
    Rng& rng = f.resource<Rng>();
    for (int i = 0; i < 16; ++i) {
        const f32 vx = static_cast<f32>(rng.next() % 200) / 10.0f - 10.0f;
        const f32 vy = static_cast<f32>(rng.next() % 200) / 10.0f - 10.0f;
        if (rng.next() % 3) f.commands().spawn(Pos{0, 0}, Vel{vx, vy}, Spin{0, vx}, Heat{0}, Life{0});
        else f.commands().spawn(Pos{0, 0}, Vel{vx, vy}, Spin{0, vy});
    }
}
void integrate(Frame& f) {
    const f32 dt = f.dt;
    f.query<Pos&, const Vel&>().each([dt](Pos& p, const Vel& v) {
        p.x += v.x * dt;
        p.y += v.y * dt;
    });
}
void spin(Frame& f) {
    const f32 dt = f.dt;
    f.query<Spin&>().each([dt](Spin& s) { s.angle = std::fmod(s.angle + s.rate * dt, 6.2831853f); });
}
void heat(Frame& f) {
    f.query<Heat&, const Pos&>().each([](Heat& h, const Pos& p) { h.value = h.value * 0.9f + std::sqrt(p.x * p.x + p.y * p.y); });
}
void age(Frame& f) {
    f.query<Life&>().each([&](Entity e, Life& l) {
        if (++l.ticks > 90) f.commands().destroy(e);
    });
}
void watcher(Frame& f) {
    Watch& w = f.resource<Watch>();
    w.changed_rows += f.query<const Spin&>().changed<Spin>().count();
}
void pinger(Frame& f) { f.events().push(Ping{static_cast<u32>(f.tick)}); }
void listener(Frame& f) {
    Watch& w = f.resource<Watch>();
    for (const Ping& p : f.events().read<Ping>()) w.pings = w.pings * 31 + p.n;
}

void build(World& w, u64 seed) {
    w.set_resource(Rng{seed});
    w.set_resource(Watch{});
    w.register_event<Ping>(Phase::FixedUpdate_Pre);
    w.add_system(Phase::FixedUpdate_Pre, "spawner", &spawner);
    w.add_system(Phase::FixedUpdate_Pre, "integrate", &integrate);
    w.add_system(Phase::FixedUpdate_Pre, "spin", &spin);
    w.add_system(Phase::FixedUpdate_Pre, "heat", &heat);
    w.add_system(Phase::FixedUpdate_Pre, "pinger", &pinger);
    w.add_system(Phase::FixedUpdate_Post, "age", &age);
    w.add_system(Phase::FixedUpdate_Post, "watcher", &watcher);
    w.add_system(Phase::FixedUpdate_Post, "listener", &listener);
    w.set_system_access(Phase::FixedUpdate_Pre, "spawner", SystemAccess().resources<Rng>().commands());
    w.set_system_access(Phase::FixedUpdate_Pre, "integrate", SystemAccess().writes<Pos>().reads<Vel>());
    w.set_system_access(Phase::FixedUpdate_Pre, "spin", SystemAccess().writes<Spin>());
    w.set_system_access(Phase::FixedUpdate_Pre, "heat", SystemAccess().writes<Heat>().reads<Pos>());
    w.set_system_access(Phase::FixedUpdate_Pre, "pinger", SystemAccess().events<Ping>());
    w.set_system_access(Phase::FixedUpdate_Post, "age", SystemAccess().writes<Life>().commands());
    w.set_system_access(Phase::FixedUpdate_Post, "watcher", SystemAccess().reads<Spin>().resources<Watch>());
    w.set_system_access(Phase::FixedUpdate_Post, "listener", SystemAccess().events<Ping>().resources<Watch>());
}

u64 run_sim(u64 seed, u32 ticks, u32 workers) {
    World w;
    build(w, seed);
    w.set_parallel(workers);
    for (u32 t = 1; t <= ticks; ++t) w.tick(1.0 / 60, t);
    const Watch& watch = w.resource<Watch>();
    return hash_combine(hash_combine(w.state_hash(), watch.changed_rows), hash_combine(watch.pings, w.change_tick()));
}

Array<u16> batches(World& w, Phase p) {
    Array<u16> out;
    w.parallel_batches(p, out);
    return out;
}
}  // namespace

TEST_CASE("parallel schedule: contiguous batches of non-conflicting declared systems") {
    World w;
    build(w, 1);
    // spawner (records Commands: alone) | integrate spin | heat pinger: heat reads the Pos
    // integrate writes.
    Array<u16> pre = batches(w, Phase::FixedUpdate_Pre);
    REQUIRE(pre.size() == 3);
    CHECK(pre[0] == 0);
    CHECK(pre[1] == 1);
    CHECK(pre[2] == 3);
    // age (Commands) | watcher | listener: listener shares the Watch resource with watcher.
    Array<u16> post = batches(w, Phase::FixedUpdate_Post);
    CHECK(post.size() == 3);
}

TEST_CASE("parallel schedule: undeclared systems and `after` targets split batches") {
    World w;
    w.add_system(Phase::FixedUpdate_Pre, "a", &integrate);
    w.add_system(Phase::FixedUpdate_Pre, "b", &spin, "a");
    w.add_system(Phase::FixedUpdate_Pre, "c", &heat);
    w.add_system(Phase::FixedUpdate_Pre, "d", &age);
    w.set_system_access(Phase::FixedUpdate_Pre, "a", SystemAccess().writes<Pos>().reads<Vel>());
    w.set_system_access(Phase::FixedUpdate_Pre, "b", SystemAccess().writes<Spin>());
    w.set_system_access(Phase::FixedUpdate_Pre, "c", SystemAccess().writes<Heat>());
    // d undeclared: always alone.
    Array<u16> pre = batches(w, Phase::FixedUpdate_Pre);
    REQUIRE(pre.size() == 3);
    CHECK(pre[0] == 0);
    CHECK(pre[1] == 1);  // b names a as `after`
    CHECK(pre[2] == 3);  // c joins b; d runs alone
}

TEST_CASE("parallel schedule: same state hash and change ticks as serial (FR-RUN-040)") {
    const u64 serial = run_sim(42, 600, 0);
    CHECK(run_sim(42, 600, 1) == serial);
    CHECK(run_sim(42, 600, 3) == serial);
    CHECK(run_sim(42, 600, 3) == serial);
    CHECK(run_sim(43, 600, 3) != serial);
}

// ----------------------------------------------------------------------- measurement
// Not a gate: run with `test_ecs -tc="parallel bench*" --no-skip`.

namespace {
template <int K>
struct Work {
    f32 a, b;
};
template <int K>
void heavy(Frame& f) {
    f.query<Work<K>&>().each([](Work<K>& w) {
        for (int i = 0; i < 8; ++i) w.a = std::sin(w.a + w.b) * 0.5f + std::cos(w.b) * 0.25f;
    });
}
template <int K>
void light(Frame& f) {
    f.query<Work<K>&>().each([](Work<K>& w) { w.a += w.b; });
}

// pace_us > 0 sleeps between ticks (a 60 Hz game's idle gap): measures the worker wake-up cost.
f64 bench(u32 entities_per_system, bool heavy_work, u32 workers, u32 ticks, u64* hash, u32 pace_us = 0) {
    World w;
    for (u32 i = 0; i < entities_per_system; ++i) {
        const f32 v = static_cast<f32>(i) * 0.001f;
        w.spawn_with(Work<0>{v, 0.1f}, Work<1>{v, 0.2f}, Work<2>{v, 0.3f}, Work<3>{v, 0.4f});
    }
    w.add_system(Phase::FixedUpdate_Pre, "w0", heavy_work ? &heavy<0> : &light<0>);
    w.add_system(Phase::FixedUpdate_Pre, "w1", heavy_work ? &heavy<1> : &light<1>);
    w.add_system(Phase::FixedUpdate_Pre, "w2", heavy_work ? &heavy<2> : &light<2>);
    w.add_system(Phase::FixedUpdate_Pre, "w3", heavy_work ? &heavy<3> : &light<3>);
    w.set_system_access(Phase::FixedUpdate_Pre, "w0", SystemAccess().writes<Work<0>>());
    w.set_system_access(Phase::FixedUpdate_Pre, "w1", SystemAccess().writes<Work<1>>());
    w.set_system_access(Phase::FixedUpdate_Pre, "w2", SystemAccess().writes<Work<2>>());
    w.set_system_access(Phase::FixedUpdate_Pre, "w3", SystemAccess().writes<Work<3>>());
    w.set_parallel(workers);
    w.tick(1.0 / 60, 0);  // warm-up
    f64 total = 0;
    for (u32 t = 1; t <= ticks; ++t) {
        if (pace_us) std::this_thread::sleep_for(std::chrono::microseconds(pace_us));
        const auto t0 = std::chrono::steady_clock::now();
        w.tick(1.0 / 60, t);
        total += std::chrono::duration<f64, std::micro>(std::chrono::steady_clock::now() - t0).count();
    }
    const f64 us = total / ticks;
    *hash = w.state_hash();
    return us;
}
}  // namespace

TEST_CASE("parallel bench: 4 independent systems, serial vs 3 workers" * doctest::skip()) {
    struct Case {
        const char* name;
        u32 entities;
        bool heavy;
        u32 ticks;
        u32 pace_us;
    };
    const Case cases[] = {
        {"tiny (game-sized: 64 rows, 1 add)", 64, false, 20000, 0},
        {"tiny, 60 Hz paced (16 ms gaps)", 64, false, 120, 16000},
        {"light (10k rows, 1 add)", 10000, false, 5000, 0},
        {"light (100k rows, 1 add)", 100000, false, 500, 0},
        {"heavy (1k rows, 16 trig)", 1000, true, 5000, 0},
        {"heavy (1k rows), 60 Hz paced", 1000, true, 120, 16000},
        {"heavy (20k rows, 16 trig)", 20000, true, 200, 0},
        {"heavy (100k rows, 16 trig)", 100000, true, 50, 0},
    };
    for (const Case& c : cases) {
        u64 hs = 0, hp = 0;
        const f64 serial = bench(c.entities, c.heavy, 0, c.ticks, &hs, c.pace_us);
        const f64 par = bench(c.entities, c.heavy, 3, c.ticks, &hp, c.pace_us);
        std::printf("%-36s serial %10.2f us/tick | parallel(3) %10.2f us/tick | x%.2f | hash %s\n", c.name, serial, par,
                    serial / par, hs == hp ? "equal" : "DIFFERENT");
        CHECK(hs == hp);
    }
}

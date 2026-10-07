// Sample 02_boids (ROADMAP M1): 100k boids through the ECS, headless, driven by the one app loop.
//
// Systems per fixed tick:
//   FixedUpdate_Pre      grid_build   bin velocities into a uniform grid (resource)
//   FixedUpdate_Pre      steer        align with the cell's average velocity
//   FixedUpdate_Physics  integrate    position += velocity * dt  (the NFR-T-01 benchmark loop)
//   FixedUpdate_Post     wrap         keep boids inside the world box
//
//   sample_02_boids -ticks 600 -boids 100000

#include <chrono>
#include <cstdio>

#include "mce/core.h"
#include "mce/ecs/world.h"

using namespace mce;

namespace {

MCE_CVAR(cv_ticks, "ticks", 600, "Ticks to simulate", CVarFlags::None);
MCE_CVAR(cv_boids, "boids", 100000, "Boid count", CVarFlags::None);

constexpr f32 kWorld = 1000.0f;
constexpr i32 kCells = 32;
constexpr f32 kCellSize = kWorld / kCells;

struct Position {
    f32 x, y;
};
struct Velocity {
    f32 x, y;
};

struct Grid {
    Array<f32> sum_x, sum_y;
    Array<u32> count;
};

struct Timings {
    f64 integrate_ms_total = 0;
    f64 tick_ms_total = 0;
    u64 ticks = 0;
};

i32 cell_of(f32 x, f32 y) {
    const i32 cx = math::clamp(static_cast<i32>(x / kCellSize), 0, kCells - 1);
    const i32 cy = math::clamp(static_cast<i32>(y / kCellSize), 0, kCells - 1);
    return cy * kCells + cx;
}

f64 now_ms() {
    using namespace std::chrono;
    return duration<f64, std::milli>(steady_clock::now().time_since_epoch()).count();
}

void grid_build(Frame& f) {
    Grid& g = f.resource<Grid>();
    for (usize i = 0; i < g.count.size(); ++i) {
        g.sum_x[i] = g.sum_y[i] = 0;
        g.count[i] = 0;
    }
    f.query<const Position&, const Velocity&>().each([&g](const Position& p, const Velocity& v) {
        const auto c = static_cast<usize>(cell_of(p.x, p.y));
        g.sum_x[c] += v.x;
        g.sum_y[c] += v.y;
        ++g.count[c];
    });
}

void steer(Frame& f) {
    const Grid& g = f.resource<Grid>();
    f.query<const Position&, Velocity&>().each([&g](const Position& p, Velocity& v) {
        const auto c = static_cast<usize>(cell_of(p.x, p.y));
        const f32 inv = 1.0f / static_cast<f32>(g.count[c]);
        v.x += (g.sum_x[c] * inv - v.x) * 0.05f;
        v.y += (g.sum_y[c] * inv - v.y) * 0.05f;
    });
}

void integrate(Frame& f) {
    const f64 t0 = now_ms();
    const f32 dt = f.dt;
    f.query<Position&, const Velocity&>().each([dt](Position& p, const Velocity& v) {
        p.x += v.x * dt;
        p.y += v.y * dt;
    });
    f.resource<Timings>().integrate_ms_total += now_ms() - t0;
}

void wrap(Frame& f) {
    f.query<Position&>().each([](Position& p) {
        if (p.x < 0) p.x += kWorld;
        if (p.x >= kWorld) p.x -= kWorld;
        if (p.y < 0) p.y += kWorld;
        if (p.y >= kWorld) p.y -= kWorld;
    });
}

class BoidsApp final : public app::Application {
public:
    World world;

    ResultV on_init(app::Context&) override {
        Grid g;
        g.sum_x.resize(kCells * kCells, 0.0f);
        g.sum_y.resize(kCells * kCells, 0.0f);
        g.count.resize(kCells * kCells, 0u);
        world.set_resource(std::move(g));
        world.set_resource(Timings{});

        world.add_system(Phase::FixedUpdate_Pre, "grid_build", &grid_build);
        world.add_system(Phase::FixedUpdate_Pre, "steer", &steer, "grid_build");
        world.add_system(Phase::FixedUpdate_Physics, "integrate", &integrate);
        world.add_system(Phase::FixedUpdate_Post, "wrap", &wrap);

        u64 rng = 0x2545F4914F6CDD1Dull;
        auto next = [&rng] {
            rng ^= rng << 13;
            rng ^= rng >> 7;
            rng ^= rng << 17;
            return static_cast<f32>(rng % 100000) / 100000.0f;
        };
        const i32 n = cv_boids.get_int();
        for (i32 i = 0; i < n; ++i) {
            world.spawn_with(Position{next() * kWorld, next() * kWorld},
                             Velocity{next() * 40.0f - 20.0f, next() * 40.0f - 20.0f});
        }
        MCE_LOG_INFO("02_boids: spawned {} boids", n);
        return ok();
    }

    void on_tick(const app::TickContext& t) override {
        const f64 t0 = now_ms();
        world.tick(t.dt, t.tick_index);
        Timings& tm = world.resource<Timings>();
        tm.tick_ms_total += now_ms() - t0;
        ++tm.ticks;
    }
};

}  // namespace

int main(int argc, char** argv) {
    log::init({.app_name = "02_boids", .level = log::Level::Info});
    console::parse_command_line(argc, argv);

    BoidsApp sample;
    app::AppDesc desc;
    desc.name = "02_boids";
    desc.headless = true;
    desc.max_ticks = static_cast<u64>(cv_ticks.get_int());
    const int rc = app::run(sample, desc);

    const Timings& tm = sample.world.resource<Timings>();
    const WorldMetrics m = sample.world.metrics();
    const f64 ticks = tm.ticks ? static_cast<f64>(tm.ticks) : 1.0;
    std::printf("02_boids: %u boids, %llu ticks | integrate %.3f ms/tick | full tick %.3f ms | tables %u | hash %016llx\n",
                m.entities, static_cast<unsigned long long>(tm.ticks), tm.integrate_ms_total / ticks,
                tm.tick_ms_total / ticks, m.tables, static_cast<unsigned long long>(sample.world.state_hash()));
    log::shutdown();
    return rc;
}

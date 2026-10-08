// Headless 3D physics scene for the determinism harness (FR-RUN-040 extended to 3D poses,
// ADR-0003 §8; physics.md §6 test 13). Not a ladder sample: 04_cubes carries M5b's visible demo.
//
// A walled courtyard with stairs, a 20 degree ramp, a triangle-mesh mound and a kinematic lift; a
// spawner raining boxes, spheres, capsules and convex shards; a capsule character on a scripted
// patrol that jumps, climbs and rides the lift; and a third-person camera boom swept against the
// level every tick. Prints the World3D state hash (plus the camera track) after N ticks.
//
//   sample_physics3d_headless -ticks 1200

#include <chrono>
#include <cmath>
#include <cstdio>

#include "mce/core.h"
#include "mce/physics3d/world3d.h"

using namespace mce;
using namespace mce::physics;

namespace {

MCE_CVAR(cv_ticks, "ticks", 1200, "Ticks to simulate", CVarFlags::None);

constexpr f32 kDt = 1.0f / 60.0f;

struct Rng {
    u64 s = 0x9E3779B97F4A7C15ull;
    f32 next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<f32>((s >> 33) % 100000) / 100000.0f;
    }
};

Entity ent(u32 i) { return Entity{i, 1}; }

void box(World3D& w, u32 id, BodyType3D type, vec3 pos, vec3 half, quat rot = {}) {
    BodyDesc3D d;
    d.type = type;
    d.shape = Shape3D::box(half);
    d.position = pos;
    d.rotation = rot;
    MCE_VERIFY(w.create_body(ent(id), d));
}

f64 now_ms() {
    using namespace std::chrono;
    return duration<f64, std::milli>(steady_clock::now().time_since_epoch()).count();
}

constexpr u32 kPlayer = 1;
constexpr u32 kLift = 2;
constexpr u32 kFirstProp = 1000;
constexpr u32 kMaxProps = 120;

class Physics3DApp final : public app::Application {
public:
    World3D world;
    Rng rng;
    f32 vy = 0.0f;
    u32 next_prop = kFirstProp;
    u64 camera_hash = kFnv64Offset;
    u32 camera_hits = 0;
    f64 tick_ms_total = 0;
    u64 ticks = 0;
    MoveResult3D last;

    ResultV on_init(app::Context&) override {
        box(world, 100, BodyType3D::Static, {0, -0.5f, 0}, {20, 0.5f, 20});  // floor, top y = 0
        box(world, 101, BodyType3D::Static, {0, 2, -20.5f}, {20, 2, 0.5f});  // four walls
        box(world, 102, BodyType3D::Static, {0, 2, 20.5f}, {20, 2, 0.5f});
        box(world, 103, BodyType3D::Static, {-20.5f, 2, 0}, {0.5f, 2, 20});
        box(world, 104, BodyType3D::Static, {20.5f, 2, 0}, {0.5f, 2, 20});
        for (u32 i = 0; i < 6; ++i)  // stairs: 0.2 m risers, 0.5 m treads, rising along +x
            box(world, 110 + i, BodyType3D::Static, {6.0f + 0.5f * static_cast<f32>(i), 0.1f + 0.1f * static_cast<f32>(i), -6},
                {0.25f, 0.1f + 0.1f * static_cast<f32>(i), 2});
        box(world, 120, BodyType3D::Static, {-8, 1.0f, 6}, {2, 0.1f, 3.2f},
            math::quat_from_axis_angle({1, 0, 0}, -20.0f * 3.14159265f / 180.0f));  // ramp
        // Triangle-mesh mound: a 9 x 9 height grid.
        static vec3 verts[81];
        static u32 idx[8 * 8 * 6];
        for (u32 z = 0; z < 9; ++z)
            for (u32 x = 0; x < 9; ++x) {
                const f32 fx = static_cast<f32>(x) - 4.0f, fz = static_cast<f32>(z) - 4.0f;
                verts[z * 9 + x] = {8.0f + fx, 1.2f * std::exp(-(fx * fx + fz * fz) / 6.0f) - 0.05f, 8.0f + fz};
            }
        u32 n = 0;
        for (u32 z = 0; z < 8; ++z)
            for (u32 x = 0; x < 8; ++x) {
                const u32 a = z * 9 + x, b = a + 1, c = a + 9, d = c + 1;
                const u32 quad[] = {a, c, b, b, c, d};
                for (u32 q : quad) idx[n++] = q;
            }
        BodyDesc3D mound;
        mound.shape = Shape3D::mesh(verts, idx);
        MCE_VERIFY(world.create_body(ent(130), mound));
        box(world, kLift, BodyType3D::Kinematic, {-6, 0.2f, -8}, {1.2f, 0.1f, 1.2f});
        MCE_VERIFY(world.create_character(ent(kPlayer), {0, 0.95f, 0}));
        return ok();
    }

    void spawn() {
        if (next_prop >= kFirstProp + kMaxProps) return;
        BodyDesc3D d;
        d.type = BodyType3D::Dynamic;
        d.restitution = rng.next() * 0.3f;
        d.friction = 0.3f + rng.next() * 0.5f;
        const f32 k = rng.next();
        if (k < 0.3f) {
            d.shape = Shape3D::sphere(0.15f + rng.next() * 0.25f);
        } else if (k < 0.6f) {
            d.shape = Shape3D::box({0.15f + rng.next() * 0.3f, 0.15f + rng.next() * 0.3f, 0.15f + rng.next() * 0.3f});
        } else if (k < 0.85f) {
            d.shape = Shape3D::capsule(0.1f + rng.next() * 0.3f, 0.1f + rng.next() * 0.2f);
        } else {
            static const vec3 shard[] = {{0, 0, 0}, {0.5f, 0.1f, 0}, {0.2f, 0.5f, 0.1f}, {0.1f, 0.1f, 0.45f}, {0.4f, 0.3f, 0.3f}};
            d.shape = Shape3D::convex(shard);
        }
        d.position = {rng.next() * 30.0f - 15.0f, 8.0f + rng.next() * 4.0f, rng.next() * 30.0f - 15.0f};
        d.rotation = math::quat_from_axis_angle(math::normalize(vec3{rng.next(), 1.0f, rng.next()}), rng.next() * 6.0f);
        MCE_VERIFY(world.create_body(ent(next_prop++), d));
    }

    void on_tick(const app::TickContext& t) override {
        const f64 t0 = now_ms();
        const u64 tick = t.tick_index;
        if (tick % 8 == 0) spawn();
        // Lift: up and down 2 m over 4 s.
        world.set_linear_velocity(ent(kLift), {0, 1.5f * std::sin(static_cast<f32>(tick) * kDt * 1.5708f), 0});

        // Patrol: a slowly turning heading, a jump every 1.5 s; gameplay integrates gravity.
        const f32 heading = static_cast<f32>(tick) * kDt * 0.2f;
        const vec3 walk{3.0f * std::sin(heading), 0, 3.0f * std::cos(heading)};
        const bool grounded = (last.flags & ControllerFlags3D::Ground) != 0;
        if (grounded) vy = (tick % 90 == 0) ? 5.0f : 0.0f;
        vy -= 9.81f * kDt;
        const vec3 v = walk + last.ground_velocity;
        last = world.move_character(ent(kPlayer), {v.x * kDt, vy * kDt, v.z * kDt});
        if (last.hit_ceiling && vy > 0.0f) vy = 0.0f;

        world.step(static_cast<f32>(t.dt));

        // Third-person boom: 6 m behind and 1 m above the head, swept against the level.
        const vec3 pivot = last.position + vec3{0, 0.7f, 0};
        const vec3 want = pivot - math::normalize(walk) * 6.0f + vec3{0, 1.0f, 0};
        vec3 cam = want;
        if (auto hit = world.camera_sweep(pivot, want, 0.2f)) {
            cam = hit->position;
            ++camera_hits;
        }
        Hasher64 h(camera_hash);
        h.f32v(cam.x);
        h.f32v(cam.y);
        h.f32v(cam.z);
        camera_hash = h.value();

        tick_ms_total += now_ms() - t0;
        ++ticks;
    }
};

}  // namespace

int main(int argc, char** argv) {
    log::init({.app_name = "physics3d_headless", .level = log::Level::Info});
    console::parse_command_line(argc, argv);

    Physics3DApp sample;
    app::AppDesc desc;
    desc.name = "physics3d_headless";
    desc.headless = true;
    desc.max_ticks = static_cast<u64>(cv_ticks.get_int());
    const int rc = app::run(sample, desc);

    const World3D::Stats st = sample.world.stats();
    const u64 hash = hash_combine(sample.world.hash_state(kFnv64Offset), sample.camera_hash);
    const f64 ticks = sample.ticks ? static_cast<f64>(sample.ticks) : 1.0;
    std::printf("physics3d_headless: %llu ticks | %u bodies (%u awake), %u characters | camera hits %u | "
                "player %.2f %.2f %.2f | tick %.3f ms | hash %016llx\n",
                static_cast<unsigned long long>(sample.ticks), st.bodies, st.active_bodies, st.characters,
                sample.camera_hits, static_cast<f64>(sample.last.position.x), static_cast<f64>(sample.last.position.y),
                static_cast<f64>(sample.last.position.z), sample.tick_ms_total / ticks,
                static_cast<unsigned long long>(hash));
    log::shutdown();
    return rc;
}

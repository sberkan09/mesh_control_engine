#include "room3d.h"

#include "mce/input/actions.h"
#include "mce/input/ecs.h"
#include "mce/kit/kit3d.h"

#include <algorithm>
#include <cmath>

namespace room3d {

using namespace mce;
using namespace mce::math;
using kit::Animator;
using kit::Physics3D;
using physics::ControllerParams3D;
using physics::MoveResult3D;

namespace {

constexpr f32 kGravity = 18.0f;
constexpr f32 kKillY = -20.0f;

ControllerParams3D hero_params() {
    ControllerParams3D p;
    p.radius = 0.3f;
    p.half_height = 0.6f;
    return p;
}

// Feet (the mesh origin) to capsule centre.
f32 standing_height() {
    const ControllerParams3D p = hero_params();
    return p.half_height + p.radius;
}

f32 wrap_angle(f32 a) {
    while (a > kPi) a -= 2.0f * kPi;
    while (a < -kPi) a += 2.0f * kPi;
    return a;
}

quat yaw_rotation(f32 yaw) { return quat_from_axis_angle({0, 1, 0}, yaw); }

// The first follow camera's yaw (movement is relative to it); 0 when there is none.
f32 camera_yaw(Frame& f) {
    f32 yaw = 0.0f;
    bool found = false;
    f.query<const FollowCamera&>().each([&](const FollowCamera& c) {
        if (found) return;
        found = true;
        yaw = c.yaw;
    });
    return yaw;
}

void room3d_hero(Frame& f) {
    Physics3D* physics = f.world().try_resource<Physics3D>();
    if (!physics) return;
    const input::InputState& in = f.resource<input::InputState>();
    const f32 dt = f.dt;
    const f32 cam_yaw = camera_yaw(f);
    f.query<Hero&, LocalTransform&>().each([&](Entity e, Hero& h, LocalTransform& t) {
        const vec3 feet_offset{0.0f, standing_height(), 0.0f};
        // Physics3D is not saved: after a load (or on the first tick) the character is made again
        // where the hero stands.
        MoveResult3D last;
        if (!physics->world.controller_state(e, last)) {
            if (!physics->world.create_character(e, t.position + feet_offset, hero_params())) return;
            h.created = 1;
        }
        const vec3 fwd{-std::sin(cam_yaw), 0.0f, -std::cos(cam_yaw)};
        const vec3 right{std::cos(cam_yaw), 0.0f, -std::sin(cam_yaw)};
        vec3 dir = fwd * clamp(in.move.y, -1.0f, 1.0f) + right * clamp(in.move.x, -1.0f, 1.0f);
        const f32 len = length(dir);
        if (len > 1.0f) dir = dir * (1.0f / len);

        if (in.jump_pressed && h.grounded) {
            h.vy = h.jump_speed;
            h.grounded = 0;
            ++h.jumps;
        }
        h.vy = std::max(h.vy - kGravity * dt, -30.0f);
        const vec3 move = dir * (h.walk_speed * dt);
        const vec3 before = t.position;
        const MoveResult3D r = physics->world.move_character(e, {move.x, h.vy * dt, move.z}, hero_params());
        t.position = r.position - feet_offset;
        h.grounded = (r.flags & physics::ControllerFlags3D::Ground) ? 1u : 0u;
        if (h.grounded && h.vy < 0.0f) h.vy = -0.5f;
        if (r.hit_ceiling && h.vy > 0.0f) h.vy = 0.0f;
        h.speed = length(vec3{t.position.x - before.x, 0.0f, t.position.z - before.z}) / std::max(dt, 1e-4f);
        if (len > 0.05f) {
            const f32 target = std::atan2(dir.x, dir.z);  // the hero mesh faces +Z
            h.yaw = wrap_angle(h.yaw + wrap_angle(target - h.yaw) * std::min(1.0f, 12.0f * dt));
        }
        t.rotation = yaw_rotation(h.yaw);
        if (t.position.y < kKillY) {  // fell off the meadow: back to the middle of the room
            t.position = {0.0f, 0.5f, 4.0f};
            h.vy = 0.0f;
            physics->world.teleport(e, t.position + feet_offset, quat::identity());
        }

        if (Animator* a = f.world().try_get_mut<Animator>(e)) {
            const bool walking = h.grounded && h.speed > 0.5f;
            a->clip.assign(walking ? StringView("walk") : StringView("idle"));
            a->speed = walking ? std::max(0.6f, h.speed / h.walk_speed) : 1.0f;
        }
    });
}

void room3d_camera(Frame& f) {
    vec3 target;
    bool have_target = false;
    f.query<const Hero&, const LocalTransform&>().each([&](const Hero&, const LocalTransform& t) {
        if (have_target) return;
        have_target = true;
        target = t.position;
    });
    if (!have_target) return;
    const input::InputState& in = f.resource<input::InputState>();
    const Physics3D* physics = f.world().try_resource<Physics3D>();
    f.query<FollowCamera&, LocalTransform&>().each([&](FollowCamera& c, LocalTransform& t) {
        c.yaw = wrap_angle(c.yaw - in.look.x * c.sensitivity);
        c.pitch = clamp(c.pitch + in.look.y * c.sensitivity * 0.7f, -0.4f, 1.2f);
        const vec3 pivot = target + vec3{0.0f, c.height, 0.0f};
        const f32 cp = std::cos(c.pitch);
        const vec3 forward{-std::sin(c.yaw) * cp, -std::sin(c.pitch), -std::cos(c.yaw) * cp};
        vec3 eye = pivot - forward * c.distance;
        if (physics) {  // walls pull the camera in
            if (const auto hit = physics->world.camera_sweep(pivot, eye, 0.2f)) eye = hit->position;
        }
        t.position = eye;
        t.rotation = normalize(yaw_rotation(c.yaw) * quat_from_axis_angle({1, 0, 0}, -c.pitch));
    });
}

// A tiny deterministic generator (the scatter must be the same on every run and platform).
struct Lcg {
    u32 state;
    f32 next() {
        state = state * 1664525u + 1013904223u;
        return static_cast<f32>(state >> 8) * (1.0f / 16777216.0f);
    }
};

void room3d_rocks(Frame& f) {
    kit::StaticInstances3D* statics = f.world().try_resource<kit::StaticInstances3D>();
    if (!statics) return;
    bool placed = false;
    f.query<const RockField&, const LocalTransform&>().each([&](const RockField& field, const LocalTransform& t) {
        const u32 slot = statics->mesh_slot(field.rock.guid);
        if (slot == ~0u) return;
        Lcg rng{field.seed * 2654435761u + 1u};
        const f32 inner = std::max(0.0f, field.inner);
        const f32 outer = std::max(inner + 1.0f, field.outer);
        for (u32 i = 0; i < field.count; ++i) {
            const f32 angle = rng.next() * 2.0f * kPi;
            // Uniform over the ring's area: radius from the square root.
            const f32 r = std::sqrt(inner * inner + rng.next() * (outer * outer - inner * inner));
            const f32 scale = 0.35f + 1.4f * rng.next() * rng.next();
            kit::StaticInstance3D inst;
            inst.position = t.position + vec3{std::cos(angle) * r, -0.12f * scale, std::sin(angle) * r};
            inst.scale = scale;
            inst.rotation = yaw_rotation(rng.next() * 2.0f * kPi);
            inst.mesh = slot;
            inst.cast_shadows = scale > 0.9f ? 1u : 0u;
            inst.max_distance = 170.0f;
            inst.thin_distance = 70.0f;
            statics->instances.push_back(inst);
        }
        placed = true;
    });
    if (placed) statics->build_cells(16.0f, 3.0f);
}

}  // namespace

ResultV install(World& world) {
    if (!world.try_resource<input::InputState>()) input::install(world);
    kit::install_kit3d(world, nullptr);  // the player installs it first (with the assets); a no-op then
    world.register_components<Hero, FollowCamera, RockField>();
    world.add_system(Phase::Startup, "room3d_rocks", &room3d_rocks);
    world.add_system(Phase::FixedUpdate_Pre, "room3d_hero", &room3d_hero);
    world.add_system(Phase::FixedUpdate_Post, "room3d_camera", &room3d_camera);
    return ok();
}

}  // namespace room3d

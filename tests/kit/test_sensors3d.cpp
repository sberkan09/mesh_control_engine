// Raycast3D and Overlap3D (M-GAMEPLAY3D step 6): World3D queries run every tick from an entity,
// their changes raised as TriggerEnter3D / TriggerExit3D for PhysicsEvents and effects.

#include <doctest/doctest.h>

#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/effects.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/particles.h"
#include "mce/kit/sensors3d.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

LocalTransform at(f32 x, f32 y, f32 z) { return LocalTransform{{x, y, z}, math::quat::identity(), {1, 1, 1}}; }

struct Scene {
    World w;
    u64 t = 0;
    u32 enters = 0, exits = 0;
    Entity floor;
    Scene() {
        ecs::install_hierarchy(w);
        install_sensors3d(w, nullptr);
        install_particles(w, nullptr);
        install_effects(w, nullptr);
        BoxCollider3D ground;
        ground.half = {20.0f, 0.5f, 20.0f};
        floor = w.spawn_with(at(0, -0.5f, 0), ground);  // top at y = 0
    }
    // Ticks, counting the sensor events `sensor` raises.
    void run(u32 n, Entity sensor = {}) {
        for (u32 i = 0; i < n; ++i) {
            w.tick(kDt, t++);
            for (const TriggerEnter3D& ev : EventAccess(w).read<TriggerEnter3D>())
                if (ev.trigger == sensor) ++enters;
            for (const TriggerExit3D& ev : EventAccess(w).read<TriggerExit3D>())
                if (ev.trigger == sensor) ++exits;
        }
    }
    Entity wall(f32 x, f32 y, f32 z, math::vec3 half) {
        BoxCollider3D c;
        c.half = half;
        return w.spawn_with(at(x, y, z), c, PhysicsEvents{});
    }
};

}  // namespace

TEST_CASE("sensors3d: a Raycast3D reports the closest hit and raises enter and exit events") {
    Scene s;
    const Entity target = s.wall(0, 1, -5, {1, 1, 0.5f});  // face at z = -4.5
    Raycast3D ray;
    ray.offset = {0, 0, -0.5f};
    const Entity eye = s.w.spawn_with(at(0, 1, 0), ray, PhysicsEvents{});
    s.run(2, eye);
    const Raycast3D* r = s.w.try_get<Raycast3D>(eye);
    CHECK(r->hit == target);
    CHECK(r->hit_distance == doctest::Approx(4.0f).epsilon(1e-3));
    CHECK(math::near(r->point, {0, 1, -4.5f}, 1e-3f));
    CHECK(math::near(r->normal, {0, 0, 1}, 1e-3f));
    CHECK(r->hits == 1);
    CHECK(s.enters == 1);
    CHECK(s.w.try_get<PhysicsEvents>(eye)->last_trigger == target);
    CHECK(s.w.try_get<PhysicsEvents>(eye)->overlapping == 1);
    CHECK(s.w.try_get<PhysicsEvents>(target)->trigger_enters == 1);

    // Turned away (the direction is in entity space): nothing, one exit.
    // Transform edits reach the sensor with WorldTransform, after the tick's propagation.
    s.w.try_get_mut<LocalTransform>(eye)->rotation = math::quat_from_axis_angle({0, 1, 0}, math::kPi);
    s.run(2, eye);
    r = s.w.try_get<Raycast3D>(eye);
    CHECK(r->hit.is_null());
    CHECK(r->hit_distance == 0.0f);
    CHECK(s.exits == 1);
    CHECK(s.w.try_get<PhysicsEvents>(eye)->overlapping == 0);

    // Back again, but too short to reach.
    s.w.try_get_mut<LocalTransform>(eye)->rotation = math::quat::identity();
    s.w.try_get_mut<Raycast3D>(eye)->distance = 3.5f;
    s.run(2, eye);
    CHECK(s.w.try_get<Raycast3D>(eye)->hit.is_null());
    s.w.try_get_mut<Raycast3D>(eye)->distance = 10.0f;
    s.run(1, eye);
    CHECK(s.w.try_get<Raycast3D>(eye)->hits == 2);

    // Switched off: the hit leaves.
    s.w.try_get_mut<Raycast3D>(eye)->enabled = 0;
    s.run(1, eye);
    CHECK(s.w.try_get<Raycast3D>(eye)->hit.is_null());
    CHECK(s.exits == 2);
}

TEST_CASE("sensors3d: a sphere cast hits beside the ray's line; characters only for a thin ray") {
    Scene s;
    const Entity post = s.wall(0.8f, 1, -5, {0.25f, 1, 0.25f});  // beside the line x = 0
    Raycast3D ray;
    const Entity eye = s.w.spawn_with(at(0, 1, 0), ray);
    s.run(1, eye);
    CHECK(s.w.try_get<Raycast3D>(eye)->hit.is_null());
    s.w.try_get_mut<Raycast3D>(eye)->radius = 0.6f;
    s.run(1, eye);
    CHECK(s.w.try_get<Raycast3D>(eye)->hit == post);
    CHECK(s.w.try_get<Raycast3D>(eye)->hit_distance < 5.0f);

    // A character on the line: the ray sees its capsule, unless told not to.
    CharacterController3D c;
    const Entity walker = s.w.spawn_with(at(0, 0.9f, -2), c);
    s.w.try_get_mut<Raycast3D>(eye)->radius = 0.0f;
    s.run(2, eye);
    CHECK(s.w.try_get<Raycast3D>(eye)->hit == walker);
    CHECK(s.w.try_get<Raycast3D>(eye)->hit_distance == doctest::Approx(1.7f).epsilon(0.02));  // radius 0.3
    s.w.try_get_mut<Raycast3D>(eye)->characters = 0;
    s.run(1, eye);
    CHECK(s.w.try_get<Raycast3D>(eye)->hit.is_null());
}

TEST_CASE("sensors3d: a sensor never sees itself or what it hangs under") {
    Scene s;
    const Entity body = s.wall(0, 1, 0, {0.5f, 0.5f, 0.5f});
    Raycast3D ray;
    ray.direction = {0, -1, 0};  // straight down through its own parent to the floor
    const Entity eye = s.w.spawn_with(at(0, 0, 0), ChildOf{body}, ray);
    Overlap3D zone;
    zone.radius = 0.75f;
    const Entity aura = s.w.spawn_with(at(0, 0, 0), ChildOf{body}, zone);
    ecs::propagate_transforms(s.w);  // what a scene load does
    s.run(1, eye);
    CHECK(s.w.try_get<Raycast3D>(eye)->hit == s.floor);
    CHECK(s.w.try_get<Raycast3D>(eye)->hit_distance == doctest::Approx(1.0f).epsilon(1e-3));
    CHECK(s.w.try_get<Overlap3D>(aura)->count == 0);  // the parent box is all it touches
}

TEST_CASE("sensors3d: an Overlap3D counts what touches it, in and out, sphere and box") {
    Scene s;
    Overlap3D zone;
    zone.radius = 1.0f;
    const Entity area = s.w.spawn_with(at(0, 3, 0), zone, PhysicsEvents{});
    s.run(1, area);
    CHECK(s.w.try_get<Overlap3D>(area)->count == 0);

    // A box falls through it and lands on the floor below.
    RigidBody3D box;
    box.half = {0.25f, 0.25f, 0.25f};
    const Entity crate = s.w.spawn_with(at(0, 6, 0), box, PhysicsEvents{});
    u32 most = 0;
    for (u32 i = 0; i < 180; ++i) {
        s.run(1, area);
        most = math::max(most, s.w.try_get<Overlap3D>(area)->count);
    }
    CHECK(most == 1);
    CHECK(s.enters == 1);
    CHECK(s.exits == 1);
    const Overlap3D* o = s.w.try_get<Overlap3D>(area);
    CHECK(o->count == 0);
    CHECK(o->enters == 1);
    CHECK(o->last_other == crate);
    CHECK(s.w.try_get<PhysicsEvents>(crate)->trigger_enters == 1);
    CHECK(s.w.try_get<PhysicsEvents>(crate)->trigger_exits == 1);
    CHECK(s.w.try_get<PhysicsEvents>(area)->overlapping == 0);

    // A flat box over the floor sees the floor, the crate on it and a character, in creation order.
    CharacterController3D c;
    const Entity walker = s.w.spawn_with(at(2, 0.9f, 0), c);
    Overlap3D pad;
    pad.shape = TriggerShape3D::Box;
    pad.half = {3, 0.1f, 1};
    const Entity plate = s.w.spawn_with(at(0, 0.05f, 0), pad);  // sunk into the floor
    s.run(2, plate);
    o = s.w.try_get<Overlap3D>(plate);
    CHECK(o->count == 3);
    CHECK(o->enters == 3);
    s.w.try_get_mut<Overlap3D>(plate)->characters = 0;
    s.run(1, plate);
    CHECK(s.w.try_get<Overlap3D>(plate)->count == 2);
    CHECK(s.w.try_get<Overlap3D>(plate)->last_other == walker);

    // Scaled and turned with its entity: a thin box along X turned 90 degrees no longer reaches the walker.
    s.w.try_get_mut<Overlap3D>(plate)->characters = 1;
    s.w.try_get_mut<LocalTransform>(plate)->rotation = math::quat_from_axis_angle({0, 1, 0}, math::kPi / 2.0f);
    s.run(2, plate);
    CHECK(s.w.try_get<Overlap3D>(plate)->count == 2);  // floor and crate
    s.w.try_get_mut<LocalTransform>(plate)->scale = {1, 1, 3};
    s.run(2, plate);
    CHECK(s.w.try_get<Overlap3D>(plate)->count == 3);

    // Removing the component (or destroying the sensor) lets everything out.
    const u32 exits_before = s.exits;
    s.w.remove<Overlap3D>(plate);
    s.run(1, plate);
    CHECK(s.exits == exits_before + 3);
}

TEST_CASE("sensors3d: an effect fires on a sensor's entry; a replay hashes the same") {
    auto play = [](bool check) {
        Scene s;
        Overlap3D zone;
        zone.radius = 0.5f;
        ParticleBurst burst;
        burst.on = FireOn::TriggerEnter;
        const Entity area = s.w.spawn_with(at(0, 2, 0), zone, burst);
        RigidBody3D ball;
        ball.shape = BodyShape3D::Sphere;
        ball.radius = 0.2f;
        s.w.spawn_with(at(0.1f, 5, 0), ball);
        s.run(120, area);
        if (check) CHECK(s.w.try_get<ParticleBurst>(area)->fired == 1);
        return s.w.state_hash();
    };
    CHECK(play(true) == play(false));
}

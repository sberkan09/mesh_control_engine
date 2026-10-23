// 3D contact events in the kit (M-CONTACTEVENTS 3): RigidBody3D::contact_events raises ContactBegin3D, and
// FireOn::ContactBegin triggers fire on it.

#include <doctest/doctest.h>

#include "mce/audio/components.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/effects.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/particles.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

LocalTransform at(f32 x, f32 y, f32 z) { return LocalTransform{{x, y, z}, math::quat::identity(), {1, 1, 1}}; }

struct Scene {
    World w;
    u64 t = 0;
    u32 begins = 0;
    ContactBegin3D last;
    Scene() {
        w.register_components<LocalTransform>();
        w.register_event<audio::PlaySfx>(Phase::FixedUpdate_Pre);  // what audio::install registers
        install_bodies3d(w, nullptr);
        install_particles(w, nullptr);
        install_effects(w, nullptr);
        BoxCollider3D floor;
        floor.half = {20.0f, 0.5f, 20.0f};
        w.spawn_with(at(0, -0.5f, 0), floor);  // top at y = 0
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) {
            w.tick(kDt, t++);
            for (const ContactBegin3D& c : EventAccess(w).read<ContactBegin3D>()) {
                ++begins;
                last = c;
            }
        }
    }
};

}  // namespace

TEST_CASE("contacts3d: a crate falling on the floor raises one ContactBegin3D, and only when it opted in") {
    Scene s;
    RigidBody3D box;
    box.contact_events = 1;
    const Entity e = s.w.spawn_with(at(0, 3.5f, 0), box);
    RigidBody3D quiet;  // never opted in
    s.w.spawn_with(at(5, 3.5f, 0), quiet);
    s.run(180);
    CHECK(s.begins == 1);
    CHECK((s.last.a == e || s.last.b == e));  // sorted by entity, the floor was spawned first
    CHECK(s.last.approach_speed > 5.0f);
    CHECK(s.last.impulse > 0.0f);
}

TEST_CASE("contacts3d: a threshold above the speed keeps it quiet") {
    Scene s;
    RigidBody3D box;
    box.contact_events = 1;
    box.contact_min_speed = 100.0f;
    s.w.spawn_with(at(0, 3.5f, 0), box);
    s.run(180);
    CHECK(s.begins == 0);
}

TEST_CASE("contacts3d: FireOn::ContactBegin fires a sound once and PhysicsEvents counts the hit") {
    Scene s;
    RigidBody3D box;
    box.contact_events = 1;
    SfxTrigger sfx;
    sfx.clip.guid = asset::Guid{7, 8};
    sfx.on = FireOn::ContactBegin;
    const Entity e = s.w.spawn_with(at(0, 3.5f, 0), box, sfx, PhysicsEvents{});
    s.run(180);
    CHECK(s.w.try_get<SfxTrigger>(e)->fired == 1);
    CHECK(s.w.try_get<PhysicsEvents>(e)->contact_begins == 1);
    CHECK(s.w.try_get<PhysicsEvents>(e)->contact_speed > 5.0f);
}

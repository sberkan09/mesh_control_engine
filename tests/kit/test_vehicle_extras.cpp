// Horn and headlights of the player's car (M-OWHORN): one honk per press and per min_gap_s, only for a seated
// player, and a Lights press cycles Auto / On / Off over the car's spot light.

#include <doctest/doctest.h>

#include "mce/audio/components.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/input/actions.h"
#include "mce/input/ecs.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/seats3d.h"
#include "mce/kit/vehicle_extras.h"
#include "mce/kit/vehicles3d.h"
#include "mce/string_id.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

LocalTransform at(f32 x, f32 y, f32 z) { return LocalTransform{{x, y, z}, math::quat::identity(), {1, 1, 1}}; }

struct Scene {
    World w;
    u64 t = 0;
    Entity car, rider;
    Scene(bool seated = true) {
        w.register_components<LocalTransform>();
        install_seats3d(w, nullptr);
        install_combat3d(w, nullptr);
        install_vehicle_extras(w, nullptr);
        w.register_event<audio::PlaySfx>(Phase::FixedUpdate_Pre);
        BoxCollider3D road;
        road.half = {300.0f, 0.5f, 300.0f};
        w.spawn_with(at(0, -0.5f, 0), road);
        RigidBody3D b;
        b.half = {0.9f, 0.4f, 2.0f};
        b.density = 1200.0f / (8.0f * 0.9f * 0.4f * 2.0f);
        Vehicle3D v;
        v.front_axle = -1.4f;
        v.rear_axle = 1.4f;
        v.track = 0.9f;
        v.wheel_y = -0.3f;
        VehicleHorn3D horn;
        horn.clip.guid = asset::Guid{1, 2};
        SpotLight3D spot;
        spot.intensity = 50.0f;
        spot.enabled = 0;
        car = w.spawn_with(at(0, 1.2f, 0), b, v, VehicleSeat3D{}, horn, VehicleLights3D{}, spot);
        Rider3D r;
        r.use_input = 1;
        CharacterController3D c;
        rider = w.spawn_with(at(seated ? 1.5f : 40.0f, 1.0f, 0.0f), c, r);
        run(2);
        if (seated) {
            run(1, "Interact");
            run(3);
        }
    }
    void run(u32 n, const char* action = nullptr) {
        for (u32 i = 0; i < n; ++i) {
            input::InputState in;
            in.tick = t;
            if (action && i == 0) {
                in.action_count = 1;
                in.action_names[0] = StringId(action);
                in.actions[0].held = in.actions[0].pressed = true;
            }
            input::publish(w, in);
            w.tick(kDt, t++);
        }
    }
    u32 honks() { return w.try_get<VehicleHorn3D>(car)->honks; }
    u64 sounds() { return w.event_queue<audio::PlaySfx>()->total_pushed; }
};

}  // namespace

TEST_CASE("vehicle extras: a seated player honks once per press, no faster than min_gap_s") {
    Scene s;
    REQUIRE(s.w.try_get<Rider3D>(s.rider)->seated == 1);
    s.run(1, "Horn");
    CHECK(s.honks() == 1);
    CHECK(s.sounds() == 1);
    s.run(1, "Horn");  // 1 tick later: inside the gap
    CHECK(s.honks() == 1);
    s.run(30);
    s.run(1, "Horn");
    CHECK(s.honks() == 2);
    CHECK(s.sounds() == 2);
}

TEST_CASE("vehicle extras: nobody honks from outside the car, and `honk` works for scripts") {
    Scene s(false);
    s.run(1, "Horn");
    CHECK(s.honks() == 0);
    s.w.try_get_mut<VehicleHorn3D>(s.car)->honk = 1;
    s.run(1);
    CHECK(s.honks() == 1);
    CHECK(s.w.try_get<VehicleHorn3D>(s.car)->honk == 0);
}

TEST_CASE("vehicle extras: Lights cycles Auto, On, Off over the spot light") {
    Scene s;
    const auto spot = [&]() { return *s.w.try_get<SpotLight3D>(s.car); };
    const auto mode = [&]() { return s.w.try_get<VehicleLights3D>(s.car)->mode; };
    CHECK(mode() == HeadlightMode::Auto);
    CHECK(spot().enabled == 0);  // Auto leaves the light alone
    s.run(1, "Lights");
    CHECK(mode() == HeadlightMode::On);
    CHECK(spot().enabled == 1);
    s.run(5);
    CHECK(spot().enabled == 1);
    s.run(1, "Lights");
    CHECK(mode() == HeadlightMode::Off);
    CHECK(spot().enabled == 0);
    s.run(1, "Lights");
    CHECK(mode() == HeadlightMode::Auto);
}

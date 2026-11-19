// Horn and headlights of the player's car (M-OWHORN): one honk per press and per min_gap_s, only for a seated
// player, and a Lights press cycles Auto / On / Off over the car's spot light.

#include <doctest/doctest.h>

#include "mce/audio/components.h"
#include "mce/ecs/hierarchy.h"
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
#include "mce/kit/world_audio.h"
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

TEST_CASE("vehicle extras: Lights cycles Auto, On, High, Off over the spot light") {
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
    const f32 normal_intensity = spot().intensity, normal_radius = spot().radius;
    s.run(1, "Lights");
    CHECK(mode() == HeadlightMode::High);  // brighter and further
    CHECK(spot().enabled == 1);
    CHECK(spot().intensity == doctest::Approx(normal_intensity * 1.5f));
    CHECK(spot().radius == doctest::Approx(normal_radius * 1.5f));
    s.run(5);
    CHECK(spot().radius == doctest::Approx(normal_radius * 1.5f));  // not compounding
    s.run(1, "Lights");
    CHECK(mode() == HeadlightMode::Off);
    CHECK(spot().radius == doctest::Approx(normal_radius));  // the normal beam is back
    CHECK(spot().enabled == 0);
    s.run(1, "Lights");
    CHECK(mode() == HeadlightMode::Auto);
}

TEST_CASE("vehicle extras: a tail lamp lights while its car brakes and shows `idle` otherwise") {
    Scene s;
    PointLight3D lamp;
    lamp.intensity = 1.0f;
    BrakeLight3D b;
    b.braking = 5.0f;
    b.idle = 0.5f;
    const Entity tail = s.w.spawn_with(at(0, 0.6f, 1.9f), ChildOf{s.car}, lamp, b);
    const auto light = [&]() { return *s.w.try_get<PointLight3D>(tail); };
    s.run(1);
    CHECK(s.w.try_get<BrakeLight3D>(tail)->lit == 0);
    CHECK(light().enabled == 1);
    CHECK(light().intensity == doctest::Approx(0.5f));
    s.w.try_get_mut<Vehicle3D>(s.car)->brake = 0.8f;
    s.run(1);
    CHECK(s.w.try_get<BrakeLight3D>(tail)->lit == 1);
    CHECK(light().intensity == doctest::Approx(5.0f));
    s.w.try_get_mut<Vehicle3D>(s.car)->brake = 0.0f;
    s.run(1);
    CHECK(s.w.try_get<BrakeLight3D>(tail)->lit == 0);
    CHECK(light().intensity == doctest::Approx(0.5f));
    s.w.try_get_mut<BrakeLight3D>(tail)->idle = 0.0f;  // idle 0: the lamp is off
    s.run(1);
    CHECK(light().enabled == 0);
}

TEST_CASE("vehicle extras: a tail lamp shows white in reverse unless the car brakes") {
    Scene s;
    PointLight3D lamp;
    BrakeLight3D b;
    b.braking = 5.0f;
    b.reverse = 2.0f;
    const Entity tail = s.w.spawn_with(at(0, 0.6f, 1.9f), ChildOf{s.car}, lamp, b);
    s.w.try_get_mut<Vehicle3D>(s.car)->gear = -1;
    s.run(1);
    CHECK(s.w.try_get<BrakeLight3D>(tail)->lit == 2);
    CHECK(s.w.try_get<PointLight3D>(tail)->intensity == doctest::Approx(2.0f));
    CHECK(s.w.try_get<PointLight3D>(tail)->color.g > 0.5f);  // white, not red
    s.w.try_get_mut<Vehicle3D>(s.car)->brake = 1.0f;
    s.run(1);
    CHECK(s.w.try_get<BrakeLight3D>(tail)->lit == 1);
    CHECK(s.w.try_get<PointLight3D>(tail)->color.g < 0.2f);
    s.w.try_get_mut<Vehicle3D>(s.car)->brake = 0.0f;
    s.w.try_get_mut<Vehicle3D>(s.car)->gear = 1;
    s.run(1);
    CHECK(s.w.try_get<BrakeLight3D>(tail)->lit == 0);
}

TEST_CASE("vehicle extras: a light flasher alternates its two colours every half period and fades within each half") {
    Scene s;
    LightFlasher3D fl;
    fl.period_s = 1.0f;
    fl.intensity = 10.0f;
    PointLight3D lamp;
    lamp.enabled = 0;
    const Entity e = s.w.spawn_with(at(0, 3, 0), lamp, fl);
    const auto light = [&]() { return *s.w.try_get<PointLight3D>(e); };
    s.run(1);  // t = 1/60: the start of A (red)
    CHECK(light().enabled == 1);
    CHECK(light().color.r > 0.9f);
    CHECK(light().color.b < 0.2f);
    CHECK(light().intensity > 9.0f);
    s.run(20);  // t ~ 0.35: still A, dimmer
    CHECK(light().color.r > 0.9f);
    CHECK(light().intensity < 9.0f);
    s.run(20);  // t ~ 0.68: B (blue), fresh
    CHECK(light().color.b > 0.9f);
    CHECK(light().color.r < 0.3f);
    s.run(40);  // t ~ 1.35: A again
    CHECK(light().color.r > 0.9f);
}

TEST_CASE("vehicle extras: a light flasher that follows the siren is dark until the parent's siren is audible") {
    Scene s;
    const Entity car = s.w.spawn_with(at(0, 0, 0), SirenAudio{});  // current_db starts at -60: silent
    LightFlasher3D fl;
    fl.follow_siren = 1;
    PointLight3D lamp;
    lamp.enabled = 0;
    const Entity bar = s.w.spawn_with(at(0, 1, 0), lamp, fl, ChildOf{car});
    s.run(5);
    CHECK(s.w.try_get<PointLight3D>(bar)->enabled == 0);
    s.w.try_get_mut<SirenAudio>(car)->current_db = -8.0f;
    s.run(2);
    CHECK(s.w.try_get<PointLight3D>(bar)->enabled == 1);
    s.w.try_get_mut<SirenAudio>(car)->current_db = -60.0f;
    s.run(2);
    CHECK(s.w.try_get<PointLight3D>(bar)->enabled == 0);
}

TEST_CASE("vehicle extras: a tail lamp glows at `headlight_idle` while the car's headlights are on") {
    Scene s;
    PointLight3D lamp;
    BrakeLight3D b;
    b.braking = 5.0f;
    b.headlight_idle = 1.0f;
    const Entity tail = s.w.spawn_with(at(0, 0.6f, 1.9f), ChildOf{s.car}, lamp, b);
    const auto light = [&]() { return *s.w.try_get<PointLight3D>(tail); };
    s.run(1);
    CHECK(light().enabled == 0);  // headlights off (Auto in daylight): dark
    s.run(1, "Lights");           // On
    s.run(1);
    CHECK(light().enabled == 1);
    CHECK(light().intensity == doctest::Approx(1.0f));
    s.w.try_get_mut<Vehicle3D>(s.car)->brake = 0.8f;
    s.run(1);
    CHECK(light().intensity == doctest::Approx(5.0f));  // braking still wins
    s.w.try_get_mut<Vehicle3D>(s.car)->brake = 0.0f;
    s.run(1, "Lights");  // High: still lit
    s.run(1, "Lights");  // Off
    s.run(1);
    CHECK(light().enabled == 0);
}

TEST_CASE("vehicle extras: a turn signal blinks only while the car steers toward its side and rolls") {
    Scene s;
    TurnSignal3D ts;
    ts.side = 1;
    ts.period_s = 0.5f;
    PointLight3D lamp;
    lamp.enabled = 0;
    const Entity e = s.w.spawn_with(at(0.8f, 0.6f, 1.9f), ChildOf{s.car}, lamp, ts);
    const auto lit = [&]() { return s.w.try_get<PointLight3D>(e)->enabled; };
    s.run(2);
    CHECK(lit() == 0);  // straight
    s.w.try_get_mut<Vehicle3D>(s.car)->steer = 0.8f;
    s.w.try_get_mut<Vehicle3D>(s.car)->speed = 5.0f;
    s.run(1);
    CHECK(lit() == 1);  // first half of the blink
    s.run(20);          // ~0.35 s: the off half
    s.w.try_get_mut<Vehicle3D>(s.car)->steer = 0.8f;
    s.w.try_get_mut<Vehicle3D>(s.car)->speed = 5.0f;
    CHECK(lit() == 0);
    s.run(15);  // ~0.6 s: on again
    s.w.try_get_mut<Vehicle3D>(s.car)->steer = -0.8f;  // the other way: not this lamp
    s.run(1);
    CHECK(lit() == 0);
}

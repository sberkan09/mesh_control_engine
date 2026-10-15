// World3D vehicles (M-GAMEPLAY3D step 4): refused descriptions, a car resting on its wheels, driving,
// braking, steering, reversing, wheel states, and determinism.

#include <cmath>

#include "helpers.h"

using namespace phys3_test;

namespace {

// A 1.2 t car (1.8 x 0.8 x 4 m body) facing -Z, rear- or four-wheel drive, front-wheel steering.
struct Car {
    WheelDesc3D wheels[4];
    AxleDesc3D axles[2];
    VehicleDesc3D desc;
    Car(Entity body, bool four_wheel_drive = false) {
        const vec3 at[4] = {{-0.9f, -0.3f, -1.4f}, {0.9f, -0.3f, -1.4f}, {-0.9f, -0.3f, 1.4f}, {0.9f, -0.3f, 1.4f}};
        for (int i = 0; i < 4; ++i) {
            wheels[i].position = at[i];
            wheels[i].max_steer = i < 2 ? 0.5f : 0.0f;
            wheels[i].max_handbrake_torque = i < 2 ? 0.0f : 4000.0f;
        }
        axles[0] = AxleDesc3D{2, 3, four_wheel_drive ? 0.5f : 1.0f, 3.42f};
        axles[1] = AxleDesc3D{0, 1, 0.5f, 3.42f};
        desc.body = body;
        desc.wheels = Span<const WheelDesc3D>(wheels, 4);
        desc.axles = Span<const AxleDesc3D>(axles, four_wheel_drive ? 2 : 1);
    }
};

bool add_car_body(World3D& w, Entity e, vec3 at = {0, 1.2f, 0}) {
    BodyDesc3D d;
    d.type = BodyType3D::Dynamic;
    d.shape = Shape3D::box({0.9f, 0.4f, 2.0f});
    d.position = at;
    d.density = 1200.0f / (8.0f * 0.9f * 0.4f * 2.0f);
    return w.create_body(e, d);
}

VehicleState3D state_of(const World3D& w, u32 v, Span<WheelState3D> wheels = {}) {
    VehicleState3D s;
    REQUIRE(w.vehicle_state(v, s, wheels));
    return s;
}

void steps(World3D& w, int n) {
    for (int i = 0; i < n; ++i) w.step(kDt);
}

// A wide flat floor with the grip of dry asphalt (the helper's is 100 m across; a car covers that
// quickly).
void add_big_floor(World3D& w) {
    BodyDesc3D d;
    d.friction = 1.0f;
    d.shape = Shape3D::box({500.0f, 0.5f, 500.0f});
    d.position = {0.0f, -0.5f, 0.0f};
    REQUIRE(w.create_body(ent(1000), d));
}

}  // namespace

TEST_CASE("vehicles: refused descriptions; gone with the body") {
    World3D w;
    add_big_floor(w);
    REQUIRE(add_car_body(w, ent(1)));
    REQUIRE(add_box(w, ent(2), BodyType3D::Static, {10, 1, 0}, {1, 1, 1}));
    REQUIRE(w.create_character(ent(3), {-10, 1, 0}));
    CHECK(w.create_vehicle(Car(ent(9)).desc) == 0);  // no body
    CHECK(w.create_vehicle(Car(ent(2)).desc) == 0);  // static
    CHECK(w.create_vehicle(Car(ent(3)).desc) == 0);  // a character
    {
        Car c(ent(1));
        c.desc.wheels = {};
        CHECK(w.create_vehicle(c.desc) == 0);
    }
    {
        Car c(ent(1));
        c.wheels[2].radius = 0.0f;
        CHECK(w.create_vehicle(c.desc) == 0);
    }
    {
        Car c(ent(1));
        c.wheels[1].suspension_max = c.wheels[1].suspension_min;
        CHECK(w.create_vehicle(c.desc) == 0);
    }
    {
        Car c(ent(1));
        c.axles[0].right = 4;
        CHECK(w.create_vehicle(c.desc) == 0);
    }
    {
        Car c(ent(1));
        c.desc.up = c.desc.forward;
        CHECK(w.create_vehicle(c.desc) == 0);
    }
    const u32 v = w.create_vehicle(Car(ent(1)).desc);
    REQUIRE(v != 0);
    CHECK(w.create_vehicle(Car(ent(1)).desc) == 0);  // already a vehicle
    CHECK(w.stats().vehicles == 1);
    w.destroy_body(ent(1));
    CHECK_FALSE(w.has_vehicle(v));
    CHECK(w.stats().vehicles == 0);
    REQUIRE(add_car_body(w, ent(1)));
    const u32 again = w.create_vehicle(Car(ent(1)).desc);
    CHECK(again != 0);
    CHECK(again != v);
    w.destroy_vehicle(again);
    CHECK(w.has_body(ent(1)));
    CHECK(w.stats().vehicles == 0);
}

TEST_CASE("vehicles: a car rests on its four wheels, drives, brakes, steers and reverses") {
    World3D w;
    add_big_floor(w);
    REQUIRE(add_car_body(w, ent(1)));
    const u32 v = w.create_vehicle(Car(ent(1), true).desc);
    REQUIRE(v != 0);
    steps(w, 120);
    WheelState3D wheels[4];
    VehicleState3D s = state_of(w, v, Span<WheelState3D>(wheels, 4));
    CHECK(s.wheels_on_ground == 4);
    CHECK(std::fabs(s.speed) < 0.05f);
    const f32 rest_y = position_of(w, ent(1)).y;
    CHECK(rest_y > 0.6f);  // on its springs, not on its belly
    CHECK(rest_y < 1.1f);
    for (const WheelState3D& wh : wheels) {
        CHECK(wh.contact);
        CHECK(wh.ground == ent(1000));
        CHECK(wh.position.y == doctest::Approx(0.35f).epsilon(0.05));       // the wheel's centre: one radius up
        CHECK(math::rotate(wh.rotation, vec3{1, 0, 0}).x == doctest::Approx(1.0f).epsilon(0.02));  // the axle points right (+X)
    }
    CHECK(wheels[0].position.z < -1.0f);  // the front wheels lead (forward is -Z)

    // Full throttle for 4 s: forward along -Z, up through the gears.
    w.set_vehicle_input(v, 1.0f, 0.0f, 0.0f, 0.0f);
    steps(w, 240);
    s = state_of(w, v, Span<WheelState3D>(wheels, 4));
    CHECK(s.speed > 8.0f);
    CHECK(s.gear >= 2);
    CHECK(s.rpm > 1000.0f);
    CHECK(position_of(w, ent(1)).z < -15.0f);
    CHECK(std::fabs(position_of(w, ent(1)).x) < 0.5f);  // straight
    CHECK(wheels[2].spin != 0.0f);

    // Brakes: stopped within 3 s.
    w.set_vehicle_input(v, 0.0f, 0.0f, 1.0f, 0.0f);
    steps(w, 180);
    CHECK(std::fabs(state_of(w, v).speed) < 0.3f);

    // Steering right while driving turns the car toward +X.
    const vec3 before = position_of(w, ent(1));
    w.set_vehicle_input(v, 0.3f, 1.0f, 0.0f, 0.0f);
    steps(w, 30);
    CHECK(state_of(w, v, Span<WheelState3D>(wheels, 4)).wheels_on_ground == 4);
    CHECK(wheels[0].steer < -0.3f);  // turned right (negative about up)
    steps(w, 150);
    BodyState3D b;
    REQUIRE(w.body_state(ent(1), b));
    CHECK(b.position.x > before.x + 2.0f);
    CHECK(math::rotate(b.rotation, vec3{0, 0, -1}).x > 0.3f);  // the nose points right of where it did

    CHECK(math::rotate(b.rotation, vec3{0, 1, 0}).y > 0.9f);  // still on its wheels

    // Stop, then reverse.
    w.set_vehicle_input(v, 0.0f, 0.0f, 1.0f, 0.0f);
    steps(w, 240);
    CHECK(state_of(w, v).wheels_on_ground == 4);
    w.set_vehicle_input(v, -1.0f, 0.0f, 0.0f, 0.0f);
    steps(w, 120);
    s = state_of(w, v);
    CHECK(s.speed < -1.0f);
    CHECK(s.gear < 0);
}

TEST_CASE("vehicles: the replay hash holds; a world that never had one keeps its hash") {
    auto run = [] {
        World3D w;
        add_big_floor(w);
        REQUIRE(add_car_body(w, ent(1)));
        REQUIRE(add_car_body(w, ent(2), {4, 1.2f, 0}));
        const u32 a = w.create_vehicle(Car(ent(1)).desc);
        const u32 b = w.create_vehicle(Car(ent(2), true).desc);
        for (int i = 0; i < 300; ++i) {
            w.set_vehicle_input(a, 1.0f, std::sin(f32(i) * 0.05f), 0.0f, 0.0f);
            w.set_vehicle_input(b, 0.7f, -0.3f, 0.0f, i > 200 ? 1.0f : 0.0f);
            w.step(kDt);
        }
        return w.hash_state(0);
    };
    CHECK(run() == run());

    World3D a, b, c;
    add_big_floor(a);
    add_big_floor(b);
    add_big_floor(c);
    REQUIRE(add_car_body(a, ent(1)));
    REQUIRE(add_car_body(b, ent(1)));
    REQUIRE(add_car_body(c, ent(1)));
    b.destroy_vehicle(b.create_vehicle(Car(ent(1)).desc));
    CHECK(a.hash_state(0) == c.hash_state(0));
    CHECK(a.hash_state(0) != b.hash_state(0));
}

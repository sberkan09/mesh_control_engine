#include "mce/kit/vehicles3d.h"

#include <cmath>

#include "kit3d_internal.h"
#include "mce/containers/hash_map.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/input/actions.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/kit3d.h"
#include "mce/log.h"

namespace mce::kit {

// Padding-free: World::state_hash hashes component bytes.
static_assert(sizeof(Vehicle3D) == 184);

namespace {

constexpr f32 kDeg = 0.017453292519943295f;

struct Built {
    u32 id = 0;      // the World3D vehicle
    Vehicle3D shape;  // the settings it was built from
};

struct Vehicles3DState {
    HashMap<Entity, Built> built;
    HashMap<Entity, Vehicle3D> failed;  // warned once; retried when a setting changes
};

// The settings that need a rebuild: everything but the driver, the wheel entities and the
// engine-written fields.
Vehicle3D structural(const Vehicle3D& v) {
    Vehicle3D s = v;
    s.use_input = 0;
    s.throttle = s.steer = s.brake = s.handbrake = 0.0f;
    s.wheel_fl = s.wheel_fr = s.wheel_rl = s.wheel_rr = Entity{};
    s.speed = s.rpm = 0.0f;
    s.gear = 0;
    s.wheels_on_ground = 0;
    s.downforce = s.skid_slip = s.right_after = 0.0f;
    s.self_right = 0;
    s.skidding = 0;
    s.lateral_slip = 0.0f;
    s.flipped_ticks = s.rightings = 0;
    return s;
}

bool same(const Vehicle3D& a, const Vehicle3D& b) {
    const auto* pa = reinterpret_cast<const unsigned char*>(&a);
    const auto* pb = reinterpret_cast<const unsigned char*>(&b);
    for (usize i = 0; i < sizeof(Vehicle3D); ++i)
        if (pa[i] != pb[i]) return false;
    return true;
}

u32 build(Physics3D& phys, Entity e, const Vehicle3D& v) {
    // Wheels: front left, front right, rear left, rear right.
    physics::WheelDesc3D wheels[4];
    const math::vec3 at[4] = {{-v.track, v.wheel_y, v.front_axle},
                              {v.track, v.wheel_y, v.front_axle},
                              {-v.track, v.wheel_y, v.rear_axle},
                              {v.track, v.wheel_y, v.rear_axle}};
    for (u32 i = 0; i < 4; ++i) {
        physics::WheelDesc3D& w = wheels[i];
        w.position = at[i];
        w.radius = v.wheel_radius;
        w.width = v.wheel_width;
        w.suspension_min = v.suspension_min;
        w.suspension_max = v.suspension_max;
        w.suspension_frequency = v.suspension_frequency;
        w.suspension_damping = v.suspension_damping;
        w.max_steer = i < 2 ? v.max_steer * kDeg : 0.0f;
        w.max_brake_torque = v.brake_torque;
        w.max_handbrake_torque = i < 2 ? 0.0f : v.handbrake_torque;
        w.grip = v.grip;
    }
    physics::AxleDesc3D axles[2];
    u32 n = 0;
    if (v.drive != VehicleDrive3D::Rear) axles[n++] = physics::AxleDesc3D{0, 1, 1.0f, 3.42f};
    if (v.drive != VehicleDrive3D::Front) axles[n++] = physics::AxleDesc3D{2, 3, 1.0f, 3.42f};
    for (u32 i = 0; i < n; ++i) axles[i].torque_share = 1.0f / static_cast<f32>(n);
    physics::AntiRollBar3D bars[2] = {{0, 1, v.anti_roll}, {2, 3, v.anti_roll}};
    f32 ratios[10];
    const u32 gears = math::min<u32>(v.gears, 10u);
    for (u32 i = 0; i < gears; ++i)  // geometric from first to top
        ratios[i] = gears == 1 ? v.first_gear : v.first_gear * std::pow(v.top_gear / v.first_gear, static_cast<f32>(i) / static_cast<f32>(gears - 1));
    physics::VehicleDesc3D d;
    if (v.anti_roll > 0.0f) d.anti_roll_bars = Span<const physics::AntiRollBar3D>(bars, 2);
    d.gears = Span<const f32>(ratios, gears);
    d.shift_up_rpm = v.shift_up_rpm;
    d.shift_down_rpm = v.shift_down_rpm;
    d.body = e;
    d.wheels = Span<const physics::WheelDesc3D>(wheels, 4);
    d.axles = Span<const physics::AxleDesc3D>(axles, n);
    d.engine_torque = v.engine_torque;
    d.max_rpm = v.max_rpm;
    d.min_rpm = math::min(1000.0f, v.max_rpm * 0.5f);
    d.max_tilt = math::clamp(v.max_tilt, 0.0f, 180.0f) * kDeg;
    return phys.world.create_vehicle(d);
}

void on_vehicle_removed(World& w, Entity e) {
    Vehicles3DState* st = w.try_resource<Vehicles3DState>();
    if (!st) return;
    if (const Built* b = st->built.find(e)) {
        if (Physics3D* phys = w.try_resource<Physics3D>()) phys->world.destroy_vehicle(b->id);
        st->built.erase(e);
    }
    st->failed.erase(e);
}

// FixedUpdate_Pre, after the bodies: build, rebuild on a settings change, the driver.
void kit3d_vehicles(Frame& f) {
    World& w = f.world();
    Physics3D& phys = f.resource<Physics3D>();
    Vehicles3DState& st = f.resource<Vehicles3DState>();
    const input::InputState* in = w.try_resource<input::InputState>();
    f.query<Vehicle3D>().each([&](Entity e, Vehicle3D& v) {
        const Vehicle3D shape = structural(v);
        Built* b = st.built.find(e);
        if (b && (!same(b->shape, shape) || !phys.world.has_vehicle(b->id))) {  // a setting changed, or the body went
            phys.world.destroy_vehicle(b->id);
            st.built.erase(e);
            b = nullptr;
        }
        if (!b) {
            if (const Vehicle3D* refused = st.failed.find(e)) {
                if (same(*refused, shape)) return;
                st.failed.erase(e);
            }
            if (!phys.world.has_body(e)) return;  // the body comes first
            const u32 id = build(phys, e, v);
            if (id == 0) {
                MCE_LOG_WARN("kit3d: Vehicle3D on entity {} not created (needs a dynamic RigidBody3D and sane wheels)", e.index);
                st.failed.insert_or_assign(e, shape);
                return;
            }
            b = &st.built.insert_or_assign(e, Built{id, shape});
        }
        physics::BodyState3D body;
        if (phys.world.body_state(e, body)) {
            const math::vec3 up = math::rotate(body.rotation, math::vec3{0.0f, 1.0f, 0.0f});
            if (v.downforce > 0.0f) phys.world.apply_impulse(e, up * (-v.downforce * v.speed * v.speed * f.dt));
            // Self-righting: on its side or roof and nearly still for right_after seconds.
            const bool flipped = up.y < 0.6f && math::length_sq(body.linear_velocity) < 1.0f;  // past ~53 degrees
            v.flipped_ticks = flipped && v.self_right ? static_cast<u16>(math::min<u32>(v.flipped_ticks + 1u, 60000u)) : u16{0};
            if (v.self_right && v.flipped_ticks > 0 && static_cast<f32>(v.flipped_ticks) * f.dt >= v.right_after) {
                math::vec3 fwd = math::rotate(body.rotation, math::vec3{0.0f, 0.0f, -1.0f});
                if (fwd.x * fwd.x + fwd.z * fwd.z < 1e-4f) fwd = math::rotate(body.rotation, math::vec3{0.0f, 1.0f, 0.0f});
                const math::quat upright = math::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, std::atan2(-fwd.x, -fwd.z));
                phys.world.teleport(e, body.position + math::vec3{0.0f, 1.0f, 0.0f}, upright);
                phys.world.set_linear_velocity(e, {});
                phys.world.set_angular_velocity(e, {});
                v.flipped_ticks = 0;
                ++v.rightings;
            }
        }
        f32 throttle = v.throttle, steer = v.steer, brake = v.brake, handbrake = v.handbrake;
        if (v.use_input && in) {
            throttle = in->move.y;
            steer = in->move.x;
            brake = math::max(brake, in->trigger_l);
            handbrake = in->jump_held ? 1.0f : handbrake;
        }
        // The throttle against the way the car rolls brakes it first.
        if (throttle * v.speed < 0.0f && std::fabs(v.speed) > 1.0f) {
            brake = math::max(brake, std::fabs(throttle));
            throttle = 0.0f;
        }
        phys.world.set_vehicle_input(b->id, throttle, steer, brake, handbrake);
    });
}

// FixedUpdate_Physics, after the write-back: what the vehicle did; the wheel models.
void kit3d_vehicle_state(Frame& f) {
    World& w = f.world();
    Physics3D& phys = f.resource<Physics3D>();
    Vehicles3DState& st = f.resource<Vehicles3DState>();
    f.query<Vehicle3D>().each([&](Entity e, Vehicle3D& v) {
        const Built* b = st.built.find(e);
        physics::VehicleState3D s;
        physics::WheelState3D wheels[4];
        if (!b || !phys.world.vehicle_state(b->id, s, Span<physics::WheelState3D>(wheels, 4))) {
            v.speed = v.rpm = 0.0f;
            v.gear = 0;
            v.wheels_on_ground = 0;
            return;
        }
        v.speed = s.speed;
        v.rpm = s.rpm;
        v.gear = s.gear;
        v.wheels_on_ground = s.wheels_on_ground;
        u32 skidding = 0;
        f32 widest = 0.0f;
        const bool moving = std::fabs(s.speed) > 3.0f;  // slip ratios are noise at a crawl
        for (u32 i = 0; i < 4; ++i) {
            if (!wheels[i].contact) continue;
            // Sideways sliding and braking lock-up skid; wheelspin under power does not.
            const f32 side = std::fabs(wheels[i].lateral_slip), along = math::max(-wheels[i].longitudinal_slip, 0.0f);
            widest = math::max(widest, side);
            // A skid starts past skid_slip and holds until the slip falls below 70% of it.
            const f32 limit = (v.skidding & (1u << i)) ? 0.7f * v.skid_slip : v.skid_slip;
            if (v.skid_slip <= 0.0f || !moving || (side <= limit && along <= limit)) continue;
            skidding |= 1u << i;
            if ((v.skidding & (1u << i)) == 0) f.events().push(Skid3D{e, i, math::max(side, along), wheels[i].position});
        }
        v.skidding = skidding;
        v.lateral_slip = widest;
        const Entity models[4] = {v.wheel_fl, v.wheel_fr, v.wheel_rl, v.wheel_rr};
        for (u32 i = 0; i < 4; ++i) {
            if (models[i].is_null() || !w.alive(models[i])) continue;
            if (LocalTransform* t = w.try_get_mut<LocalTransform>(models[i])) {
                t->position = wheels[i].position;
                t->rotation = wheels[i].rotation;
            }
        }
    });
}

}  // namespace

void install_vehicles3d(World& world, asset::AssetSystem* assets) {
    install_bodies3d(world, assets);
    if (world.try_resource<Vehicles3DState>()) return;
    world.set_resource(Vehicles3DState{});
    world.register_components<Vehicle3D>();
    world.register_event<Skid3D>(Phase::FixedUpdate_Physics);
    world.set_hooks<Vehicle3D>(nullptr, &on_vehicle_removed);
    world.add_system(Phase::FixedUpdate_Pre, "kit3d_vehicles", &kit3d_vehicles, "kit3d_bodies");
    world.add_system(Phase::FixedUpdate_Physics, "kit3d_vehicle_state", &kit3d_vehicle_state, "kit3d_writeback");
}

}  // namespace mce::kit

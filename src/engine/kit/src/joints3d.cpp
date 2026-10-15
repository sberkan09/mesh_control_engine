#include "mce/kit/joints3d.h"

#include "kit3d_internal.h"
#include "mce/containers/hash_map.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/kit3d.h"
#include "mce/log.h"

namespace mce::kit {

// Padding-free: World::state_hash hashes component bytes.
static_assert(sizeof(Joint3D) == 164);

namespace {

constexpr f32 kDeg = 0.017453292519943295f;

struct Built {
    u32 id = 0;     // the World3D joint
    Joint3D shape;  // the settings it was built from (see structural())
};

// What the joint systems remember: the joint built for each entity, and the settings a creation
// was refused with (warned once; retried when a setting changes).
struct Joints3DState {
    HashMap<Entity, Built> built;
    HashMap<Entity, Joint3D> failed;
};

// The settings that need a rebuild: everything but `enabled`, the motor and the engine-written fields.
Joint3D structural(const Joint3D& j) {
    Joint3D s = j;
    s.motor = JointMotor3D::Off;
    s.enabled = 0;
    s.broken = 0;
    s.motor_target = 0.0f;
    s.motor_max_force = 0.0f;
    s.position = s.force = s.torque = 0.0f;
    return s;
}

// Byte-for-byte (padding-free, see the static_assert): -0 and NaN count as changes, which is what a
// rebuild check wants.
bool same(const Joint3D& a, const Joint3D& b) {
    const auto* pa = reinterpret_cast<const unsigned char*>(&a);
    const auto* pb = reinterpret_cast<const unsigned char*>(&b);
    for (usize i = 0; i < sizeof(Joint3D); ++i)
        if (pa[i] != pb[i]) return false;
    return true;
}

physics::JointMotorMode3D motor_mode(JointMotor3D m) {
    switch (m) {
    case JointMotor3D::Off: return physics::JointMotorMode3D::Off;
    case JointMotor3D::Velocity: return physics::JointMotorMode3D::Velocity;
    case JointMotor3D::Position: return physics::JointMotorMode3D::Position;
    }
    return physics::JointMotorMode3D::Off;
}

// The motor target in World3D units (radians for a hinge).
f32 motor_target(const Joint3D& j) { return j.kind == JointKind3D::Hinge ? j.motor_target * kDeg : j.motor_target; }

math::vec3 to_world(World& w, Entity e, math::vec3 local) {
    const LocalTransform* t = w.try_get<LocalTransform>(e);
    if (!t) return local;
    math::vec3 pos, scale;
    math::quat rot;
    world_pose(w, e, *t, pos, rot, scale);
    return pos + math::rotate(rot, math::vec3{local.x * scale.x, local.y * scale.y, local.z * scale.z});
}

math::vec3 axis_to_world(World& w, Entity e, math::vec3 local) {
    const LocalTransform* t = w.try_get<LocalTransform>(e);
    if (!t) return local;
    math::vec3 pos, scale;
    math::quat rot;
    world_pose(w, e, *t, pos, rot, scale);
    return math::rotate(rot, local);
}

physics::JointDesc3D desc_of(World& w, Entity e, const Joint3D& j) {
    physics::JointDesc3D d;
    switch (j.kind) {
    case JointKind3D::Fixed: d.type = physics::JointType3D::Fixed; break;
    case JointKind3D::Hinge: d.type = physics::JointType3D::Hinge; break;
    case JointKind3D::Slider: d.type = physics::JointType3D::Slider; break;
    case JointKind3D::Distance: d.type = physics::JointType3D::Distance; break;
    case JointKind3D::Cone: d.type = physics::JointType3D::Cone; break;
    case JointKind3D::SixDOF: d.type = physics::JointType3D::SixDOF; break;
    }
    const bool hinge = j.kind == JointKind3D::Hinge;
    d.a = e;
    d.b = j.connected;
    d.anchor = to_world(w, e, j.anchor);
    d.anchor_b = j.connected.is_null() ? j.connected_anchor : to_world(w, j.connected, j.connected_anchor);
    d.axis = axis_to_world(w, e, j.axis);
    d.limits = j.limits != 0;
    d.collide_connected = j.collide_connected != 0;
    if (j.kind == JointKind3D::Distance) {
        d.limit_min = j.min_distance;
        d.limit_max = j.max_distance;
    } else {
        d.limit_min = hinge ? j.limit_min * kDeg : j.limit_min;
        d.limit_max = hinge ? j.limit_max * kDeg : j.limit_max;
    }
    d.cone_half_angle = j.cone_angle * kDeg;
    d.linear_min = j.linear_min;
    d.linear_max = j.linear_max;
    d.angular_min = j.angular_min * kDeg;
    d.angular_max = j.angular_max * kDeg;
    d.spring_frequency = j.spring_frequency;
    d.spring_damping = j.spring_damping;
    d.friction = j.friction;
    d.motor = motor_mode(j.motor);
    d.motor_target = motor_target(j);
    d.motor_max_force = j.motor_max_force;
    d.motor_frequency = j.motor_frequency;
    d.break_force = j.break_force;
    d.break_torque = j.break_torque;
    return d;
}

void on_joint_removed(World& w, Entity e) {
    Joints3DState* st = w.try_resource<Joints3DState>();
    if (!st) return;
    if (const Built* b = st->built.find(e)) {
        if (Physics3D* phys = w.try_resource<Physics3D>()) phys->world.destroy_joint(b->id);
        st->built.erase(e);
    }
    st->failed.erase(e);
}

// FixedUpdate_Pre, after the bodies: build, rebuild on a settings change, switch, drive.
void kit3d_joints(Frame& f) {
    World& w = f.world();
    Physics3D& phys = f.resource<Physics3D>();
    Joints3DState& st = f.resource<Joints3DState>();
    f.query<Joint3D>().each([&](Entity e, Joint3D& j) {
        const Joint3D shape = structural(j);
        Built* b = st.built.find(e);
        if (b && !same(b->shape, shape)) {  // a setting changed: rebuild from where the bodies are now
            phys.world.destroy_joint(b->id);
            st.built.erase(e);
            b = nullptr;
            j.broken = 0;
        }
        if (b && !phys.world.has_joint(b->id)) {  // a body went (and may come back)
            st.built.erase(e);
            b = nullptr;
        }
        if (!b) {
            if (j.broken) return;  // stays broken until a setting changes
            if (const Joint3D* refused = st.failed.find(e)) {
                if (same(*refused, shape)) return;
                st.failed.erase(e);
            }
            if (!phys.world.has_body(e) || (!j.connected.is_null() && !phys.world.has_body(j.connected))) return;
            const u32 id = phys.world.create_joint(desc_of(w, e, j));
            if (id == 0) {
                MCE_LOG_WARN("kit3d: Joint3D on entity {} not created (needs a body, a dynamic one on one side, an axis)", e.index);
                st.failed.insert_or_assign(e, shape);
                return;
            }
            b = &st.built.insert_or_assign(e, Built{id, shape});
        }
        phys.world.set_joint_enabled(b->id, j.enabled != 0);
        phys.world.set_joint_motor(b->id, motor_mode(j.motor), motor_target(j), j.motor_max_force);
    });
}

// FixedUpdate_Physics, after the write-back: what the joints did this step; breaks.
void kit3d_joint_state(Frame& f) {
    Physics3D& phys = f.resource<Physics3D>();
    Joints3DState& st = f.resource<Joints3DState>();
    auto events = f.events();
    f.query<Joint3D>().each([&](Entity e, Joint3D& j) {
        const Built* b = st.built.find(e);
        physics::JointState3D s;
        if (!b || !phys.world.joint_state(b->id, s)) {
            j.position = j.force = j.torque = 0.0f;
            return;
        }
        j.position = j.kind == JointKind3D::Hinge ? s.position / kDeg : s.position;
        j.force = s.force;
        j.torque = s.torque;
        if (s.broken && !j.broken) {
            j.broken = 1;
            events.push(JointBreak3D{e, j.connected});
        }
    });
}

}  // namespace

void install_joints3d(World& world, asset::AssetSystem* assets) {
    install_bodies3d(world, assets);
    if (world.try_resource<Joints3DState>()) return;
    world.set_resource(Joints3DState{});
    world.register_components<Joint3D>();
    world.register_event<JointBreak3D>(Phase::FixedUpdate_Physics);
    world.set_hooks<Joint3D>(nullptr, &on_joint_removed);
    world.add_system(Phase::FixedUpdate_Pre, "kit3d_joints", &kit3d_joints, "kit3d_bodies");
    world.add_system(Phase::FixedUpdate_Physics, "kit3d_joint_state", &kit3d_joint_state, "kit3d_writeback");
}

}  // namespace mce::kit

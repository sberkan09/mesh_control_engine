#include "mce/physics/module.h"

#include <algorithm>
#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/physics/force2d.h"
#include "mce/physics/joint2d.h"
#include "mce/log.h"

namespace mce::physics {

namespace {

// Last component values pushed into the world, per entity index (change detection until the ECS
// grows change ticks at v0.5).
struct SyncEntry {
    Entity entity;
    RigidBody2D body;
    Collider2D collider;
    CharacterController2D controller;
    bool has_controller = false;
    bool failed = false;  // malformed shape: no body until the components change
};

struct SyncState {
    Array<SyncEntry> entries;
};

struct ImportItem {
    Entity entity;
    const RigidBody2D* body;
    const Collider2D* collider;
};

void on_body_removed(World& w, Entity e) {
    if (World2D* pw = w.try_resource<World2D>()) pw->destroy_body(e);
    if (SyncState* s = w.try_resource<SyncState>(); s && e.index < s->entries.size()) s->entries[e.index] = {};
}

void import(Frame& f, World2D& pw, SyncState& sync) {
    World& w = f.world();
    SmallVector<ImportItem, 64> items;
    f.query<const RigidBody2D&, const Collider2D&>().each([&](Entity e, const RigidBody2D& rb, const Collider2D& c) {
        items.push_back({e, &rb, &c});
    });
    std::sort(items.begin(), items.end(),
              [](const ImportItem& a, const ImportItem& b) { return a.entity.index < b.entity.index; });

    for (const ImportItem& it : items) {
        const Entity e = it.entity;
        const CharacterController2D* ctrl = w.try_get<CharacterController2D>(e);
        if (e.index >= sync.entries.size()) sync.entries.resize(e.index + 1);
        SyncEntry& s = sync.entries[e.index];
        const bool known = s.entity == e;

        if (known && pw.has_body(e) && s.has_controller != (ctrl != nullptr)) pw.destroy_body(e);

        if (!pw.has_body(e)) {
            if (known && s.failed && s.body == *it.body && s.collider == *it.collider) continue;
            BodyDesc d;
            d.body = *it.body;
            d.collider = *it.collider;
            d.controller = ctrl;
            if (const LocalTransform* t = w.try_get<LocalTransform>(e)) {
                d.position = t->position.xy();
                d.angle = angle_z(t->rotation);
            }
            s = SyncEntry{};
            s.entity = e;
            s.body = *it.body;
            s.collider = *it.collider;
            s.has_controller = ctrl != nullptr;
            if (ctrl) s.controller = *ctrl;
            s.failed = !pw.create_body(e, d);
            if (s.failed) MCE_LOG_WARN("physics2d: entity {} has a malformed Collider2D; no body created", e.index);
        } else {
            if (s.body != *it.body) {
                pw.set_body_config(e, *it.body);
                s.body = *it.body;
            }
            if (s.collider != *it.collider) {
                if (!pw.set_collider(e, *it.collider))
                    MCE_LOG_WARN("physics2d: entity {} Collider2D edit is malformed; keeping the old shape", e.index);
                s.collider = *it.collider;
            }
            if (ctrl && s.controller != *ctrl) {
                pw.set_controller(e, *ctrl);
                s.controller = *ctrl;
            }
        }
        if (ctrl && !w.has<CharacterGround2D>(e)) f.commands().add<CharacterGround2D>(e);
    }
}

void export_state(Frame& f, const World2D& pw) {
    World& w = f.world();
    pw.each_body([&](const BodyState& b) {
        if (b.type == BodyType::Static || !b.enabled) return;
        if (LocalTransform* t = w.try_get_mut<LocalTransform>(b.entity)) {
            t->position.x = b.position.x;
            t->position.y = b.position.y;
            t->rotation = rotation_z(b.angle);
        }
        if (b.controller) {
            MoveResult2D r;
            if (CharacterGround2D* g = w.try_get_mut<CharacterGround2D>(b.entity); g && pw.controller_state(b.entity, r)) {
                g->flags = r.flags;
                g->ground_normal = r.ground_normal;
                g->platform_velocity = r.platform_velocity;
                g->ground_body = r.ground_body;
            }
        }
    });
}


// ---- Joint2D -------------------------------------------------------------------------------

constexpr f32 kDegToRad = 0.01745329252f;

struct JointEntry {
    Entity entity;
    Joint2D built;  // the settings the live joint was built from (sanitized, readouts zeroed)
    JointId id;
    bool failed = false;  // World2D refused the joint: no retry until the settings or the bodies change
    // What the refusal saw; a different connected entity or body type is a reason to try again.
    Entity seen_connected;
    BodyType seen_type_a = BodyType::Static;
    BodyType seen_type_b = BodyType::Static;
};

struct JointState {
    Array<JointEntry> entries;
};

bool finite(f32 v) { return std::isfinite(v); }

// The settings as the world will see them: readouts dropped, NaN and infinity back to the defaults,
// angles ordered and clamped, no negative spring or torque. `fixed` says something was repaired
// (one warning at build time, not one per tick: the repaired value then compares equal).
Joint2D settings_of(Joint2D j, bool* fixed = nullptr) {
    bool fx = false;
    // Every repair goes through these, so `fixed` is tracked per field (NaN compares unequal to anything).
    auto set = [&](f32& v, f32 to) {
        if (!(v == to)) {
            v = to;
            fx = true;
        }
    };
    j.length_now = 0.0f;
    j.angle_now = 0.0f;
    const Joint2D d;
    if (!finite(j.anchor.x) || !finite(j.anchor.y)) { j.anchor = d.anchor; fx = true; }
    if (!finite(j.connected_anchor.x) || !finite(j.connected_anchor.y)) { j.connected_anchor = d.connected_anchor; fx = true; }
    if (!finite(j.length)) set(j.length, d.length);
    if (!finite(j.min_length)) set(j.min_length, d.min_length);
    if (!finite(j.max_length)) set(j.max_length, d.max_length);
    if (!finite(j.hertz)) set(j.hertz, d.hertz);
    if (!finite(j.damping_ratio)) set(j.damping_ratio, d.damping_ratio);
    if (!finite(j.lower)) set(j.lower, d.lower);
    if (!finite(j.upper)) set(j.upper, d.upper);
    if (!finite(j.motor_speed)) set(j.motor_speed, d.motor_speed);
    if (!finite(j.max_motor_torque)) set(j.max_motor_torque, d.max_motor_torque);
    if (j.hertz < 0.0f) set(j.hertz, 0.0f);
    set(j.damping_ratio, std::clamp(j.damping_ratio, 0.0f, 1.0f));
    if (j.max_motor_torque < 0.0f) set(j.max_motor_torque, 0.0f);
    set(j.lower, std::clamp(j.lower, -180.0f, 180.0f));
    set(j.upper, std::clamp(j.upper, -180.0f, 180.0f));
    if (j.lower > j.upper) { std::swap(j.lower, j.upper); fx = true; }
    if (j.min_length >= 0.0f && j.max_length >= 0.0f && j.min_length > j.max_length) {
        std::swap(j.min_length, j.max_length);
        fx = true;
    }
    if (fixed) *fixed = fx;
    return j;
}

void on_joint_removed(World& w, Entity e) {
    JointState* js = w.try_resource<JointState>();
    if (!js || e.index >= js->entries.size()) return;
    JointEntry& je = js->entries[e.index];
    if (je.entity == e && !je.id.is_null()) {
        if (World2D* pw = w.try_resource<World2D>()) pw->destroy_joint(je.id);
    }
    je = {};
}

JointId build_joint(World2D& pw, Entity e, const Joint2D& j, const BodyState& self) {
    if (j.kind == JointKind2D::Distance) {
        DistanceJointDesc d;
        d.a = e;
        d.b = j.connected;
        d.local_anchor_a = j.anchor;
        d.local_anchor_b = j.connected_anchor;
        d.length = j.length;
        d.min_length = j.min_length;
        d.max_length = j.max_length;
        d.hertz = j.hertz;
        d.damping_ratio = j.damping_ratio;
        d.collide_connected = j.collide_connected != 0;
        return pw.create_distance_joint(d);
    }
    RevoluteJointDesc d;
    d.a = j.connected;  // World2D measures b - a, so the angle is this entity's turn relative to the connected one
    d.b = e;
    // The pivot is placed from the pose World2D holds for this body (what the import wrote), not from
    // LocalTransform, which is not a world pose for an entity with a parent.
    const f32 c = std::cos(self.angle), s = std::sin(self.angle);
    d.world_anchor = {self.position.x + c * j.anchor.x - s * j.anchor.y, self.position.y + s * j.anchor.x + c * j.anchor.y};
    d.enable_limit = j.limits != 0;
    d.lower_angle = j.lower * kDegToRad;
    d.upper_angle = j.upper * kDegToRad;
    d.enable_motor = j.motor != 0;
    d.motor_speed = j.motor_speed * kDegToRad;
    d.max_motor_torque = j.max_motor_torque;
    d.collide_connected = j.collide_connected != 0;
    return pw.create_revolute_joint(d);
}

// Sanitized settings are never NaN, so the defaulted comparison is a safe equality.
bool same_settings(const Joint2D& a, const Joint2D& b) { return a == b; }

// After the bodies are imported and before the step: build, rebuild, update in place and remove.
void sync_joints(Frame& f, World2D& pw, JointState& js) {
    World& w = f.world();
    SmallVector<Entity, 16> owners;
    f.query<const Joint2D&>().each([&](Entity e, const Joint2D&) { owners.push_back(e); });
    std::sort(owners.begin(), owners.end(), [](Entity a, Entity b) { return a.index < b.index; });

    for (const Entity e : owners) {
        Joint2D* comp = w.try_get_mut<Joint2D>(e);
        if (!comp) continue;
        if (e.index >= js.entries.size()) js.entries.resize(e.index + 1);
        JointEntry& je = js.entries[e.index];
        if (je.entity != e) {
            je = {};
            je.entity = e;
        }
        bool fixed = false;
        const Joint2D want = settings_of(*comp, &fixed);
        const bool alive = !je.id.is_null() && pw.joint_alive(je.id);
        BodyState self, other;
        const bool bodies = !comp->connected.is_null() && pw.body_state(e, self) && pw.body_state(comp->connected, other);

        if (!comp->enabled || !bodies) {
            if (alive) pw.destroy_joint(je.id);
            je.id = {};
            je.failed = false;
            continue;
        }
        if (alive && same_settings(je.built, want)) {
            // unchanged
        } else if (alive && want.kind == JointKind2D::Revolute && je.built.kind == JointKind2D::Revolute) {
            Joint2D probe = je.built;  // only the in-place settings differ?
            probe.lower = want.lower;
            probe.upper = want.upper;
            probe.motor_speed = want.motor_speed;
            if (same_settings(probe, want)) {
                if (want.limits) pw.set_joint_limits(je.id, want.lower * kDegToRad, want.upper * kDegToRad);
                pw.set_motor_speed(je.id, want.motor_speed * kDegToRad);
                je.built = want;
            } else {
                pw.destroy_joint(je.id);
                je.id = {};
            }
        } else if (alive) {
            pw.destroy_joint(je.id);
            je.id = {};
        }
        if (je.id.is_null() || !pw.joint_alive(je.id)) {
            // A refused joint is tried again when the settings, the connected entity or a body type change.
            const bool same_world = je.seen_connected == comp->connected && je.seen_type_a == self.type && je.seen_type_b == other.type;
            if (je.failed && same_settings(je.built, want) && same_world) continue;
            je.id = build_joint(pw, e, want, self);
            je.built = want;
            je.failed = je.id.is_null();
            je.seen_connected = comp->connected;
            je.seen_type_a = self.type;
            je.seen_type_b = other.type;
            if (fixed) MCE_LOG_WARN("physics2d: entity {} Joint2D had non-finite or out-of-range settings; using repaired values", e.index);
            if (je.failed) MCE_LOG_WARN("physics2d: entity {} Joint2D was refused (needs two bodies, one dynamic)", e.index);
        }
        if (je.id.is_null()) continue;
        if (want.kind == JointKind2D::Distance) {
            comp->length_now = pw.joint_length(je.id);
        } else {
            comp->angle_now = pw.joint_angle(je.id) / kDegToRad;
        }
    }
}

void publish_events(Frame& f, const World2D& pw) {
    EventAccess ev = f.events();
    for (const TriggerEnter& e : pw.trigger_enters()) ev.push(e);
    for (const TriggerExit& e : pw.trigger_exits()) ev.push(e);
    for (const ContactBegin2D& e : pw.contact_begins()) ev.push(e);
    for (const ContactEnd2D& e : pw.contact_ends()) ev.push(e);
}

// LaunchVelocity2D, Impulse2D and ConstantForce2D (M-PHYSDRIVE2D): pushes from components, applied
// after the import and before the step.
bool finite2(math::vec2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }

void apply_drives(Frame& f, World2D& pw) {
    f.query<LaunchVelocity2D&>().each([&](Entity e, LaunchVelocity2D& l) {
        BodyState bs;
        if (l.applied || !pw.body_state(e, bs) || bs.type == BodyType::Static) return;
        l.applied = 1;
        pw.set_linear_velocity(e, finite2(l.linear) ? l.linear : math::vec2{});
        pw.set_angular_velocity(e, std::isfinite(l.angular) ? l.angular * kDegToRad : 0.0f);
    });
    f.query<Impulse2D&>().each([&](Entity e, Impulse2D& im) {
        if (im.impulse.x == 0.0f && im.impulse.y == 0.0f) return;
        BodyState bs;
        if (!pw.body_state(e, bs) || bs.type != BodyType::Dynamic) return;  // kept until the body can take it
        if (finite2(im.impulse) && finite2(im.point)) {
            const f32 c = std::cos(bs.angle), s = std::sin(bs.angle);
            const math::vec2 at{bs.position.x + c * im.point.x - s * im.point.y, bs.position.y + s * im.point.x + c * im.point.y};
            pw.apply_impulse(e, im.impulse, at);
        }
        im.impulse = {};
    });
    f.query<const ConstantForce2D&>().each([&](Entity e, const ConstantForce2D& cf) {
        BodyState bs;
        if (cf.force.x == 0.0f && cf.force.y == 0.0f) return;  // a zero force must not keep a sleeping body awake
        if (!finite2(cf.force) || !pw.body_state(e, bs) || bs.type != BodyType::Dynamic) return;
        pw.apply_force(e, cf.force);
    });
}

}  // namespace

void physics_step(Frame& f) {
    World2D& pw = f.resource<World2D>();
    SyncState& sync = f.resource<SyncState>();
    import(f, pw, sync);
    sync_joints(f, pw, f.resource<JointState>());
    apply_drives(f, pw);
    pw.step(f.dt);
    export_state(f, pw);
    publish_events(f, pw);
}

void install_physics2d(World& world, const PhysicsConfig& config) {
    world.register_components<RigidBody2D, Collider2D, CharacterController2D, CharacterGround2D, Joint2D, LaunchVelocity2D, Impulse2D, ConstantForce2D, LocalTransform>();
    world.register_event<TriggerEnter>(Phase::FixedUpdate_Physics);
    world.register_event<TriggerExit>(Phase::FixedUpdate_Physics);
    world.register_event<ContactBegin2D>(Phase::FixedUpdate_Physics);
    world.register_event<ContactEnd2D>(Phase::FixedUpdate_Physics);
    world.set_hooks<RigidBody2D>(nullptr, &on_body_removed);
    world.set_hooks<Collider2D>(nullptr, &on_body_removed);
    world.set_resource(World2D(config));
    world.set_hooks<Joint2D>(nullptr, &on_joint_removed);
    world.set_resource(SyncState{});
    world.set_resource(JointState{});
    world.add_system(Phase::FixedUpdate_Physics, "physics_step", &physics_step);
}

}  // namespace mce::physics

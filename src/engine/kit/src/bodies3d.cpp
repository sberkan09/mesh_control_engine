#include "mce/kit/bodies3d.h"

#include <cmath>

#include "kit3d_internal.h"
#include "mce/asset/asset_system.h"
#include "mce/containers/hash_map.h"
#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/input/actions.h"
#include "mce/kit/kit3d.h"
#include "mce/log.h"

namespace mce::kit {

// Padding-free: World::state_hash hashes component bytes.
static_assert(sizeof(RigidBody3D) == 112);
static_assert(sizeof(CharacterController3D) == 116);
static_assert(sizeof(Trigger3D) == 48);

namespace {

constexpr StringId kSprint{"Sprint"};
constexpr StringId kCrouch{"Crouch"};

struct Pose {
    math::vec3 position;
    math::quat rotation;
};

struct Pair {
    Entity trigger;
    Entity other;
};

// What the systems remember between ticks: the pose each body was last put at (an entity found
// elsewhere was moved from outside and its body is teleported), and the trigger overlaps.
struct Bodies3DState {
    HashMap<Entity, Pose> poses;
    HashMap<Entity, u8> failed;  // creation failed (warned once); retried when the component is re-added
    Array<Pair> inside;
};

bool same(const Pose& a, const math::vec3& p, const math::quat& r) {
    return a.position.x == p.x && a.position.y == p.y && a.position.z == p.z && a.rotation.x == r.x && a.rotation.y == r.y &&
           a.rotation.z == r.z && a.rotation.w == r.w;
}

f32 bounding_radius(const RigidBody3D& b, const math::vec3& scale) {
    const f32 s = math::max(std::fabs(scale.x), math::max(std::fabs(scale.y), std::fabs(scale.z)));
    switch (b.shape) {
    case BodyShape3D::Box: return math::length(b.half) * s;
    case BodyShape3D::Sphere: return b.radius * s;
    case BodyShape3D::Capsule: return (b.half_height + b.radius) * s;
    case BodyShape3D::ConvexMesh: return 0.5f * s;
    }
    return 0.5f;
}

bool create_rigid_body(World& w, Physics3D& phys, Entity e, const RigidBody3D& b, const LocalTransform& t) {
    math::vec3 pos, scale;
    math::quat rot;
    world_pose(w, e, t, pos, rot, scale);
    physics::BodyDesc3D d;
    Array<math::vec3> verts;
    Array<u32> indices;
    switch (b.shape) {
    case BodyShape3D::Box: d.shape = physics::Shape3D::box({b.half.x * scale.x, b.half.y * scale.y, b.half.z * scale.z}); break;
    case BodyShape3D::Sphere: d.shape = physics::Shape3D::sphere(b.radius * scale.x); break;
    case BodyShape3D::Capsule: d.shape = physics::Shape3D::capsule(b.half_height * scale.y, b.radius * scale.x); break;
    case BodyShape3D::ConvexMesh: {
        asset::AssetSystem* assets = kit3d_assets(w);
        Result<asset::AssetHandle<asset::Mesh>> h =
            assets ? assets->load_sync<asset::Mesh>(b.hull.guid) : Result<asset::AssetHandle<asset::Mesh>>(Error(Errc::NotFound, "kit3d.hull"));
        if (!h) {
            MCE_LOG_WARN("kit3d: RigidBody3D on entity {}: hull mesh does not load", e.index);
            return false;
        }
        const bool read = mesh_triangles(assets->resolve(h.value()), math::scaling(scale), verts, indices);
        assets->release(asset::AnyHandle(h.value()));
        if (!read || verts.size() < 4) {
            MCE_LOG_WARN("kit3d: RigidBody3D on entity {}: the hull mesh has too few vertices", e.index);
            return false;
        }
        d.shape = physics::Shape3D::convex(verts.span());
        break;
    }
    }
    d.type = b.type == BodyType3DKind::Dynamic ? physics::BodyType3D::Dynamic : physics::BodyType3D::Kinematic;
    d.position = pos;
    d.rotation = rot;
    d.friction = b.friction;
    d.restitution = b.restitution;
    d.density = b.density;
    d.gravity_scale = b.gravity_scale;
    d.linear_damping = b.linear_damping;
    d.layer = static_cast<u8>(b.layer & 15u);
    d.mask = b.mask;
    if (phys.world.create_body(e, d)) return true;
    MCE_LOG_WARN("kit3d: RigidBody3D on entity {} not created", e.index);
    return false;
}

physics::ControllerParams3D params_of(const CharacterController3D& c) {
    physics::ControllerParams3D p;
    p.radius = c.radius;
    p.half_height = c.half_height;
    p.max_slope_deg = c.max_slope_deg;
    p.step_height = c.step_height;
    p.stick_to_floor = c.stick_to_floor;
    p.layer = static_cast<u8>(c.layer & 15u);
    p.mask = c.mask;
    return p;
}

// The primary 3D camera's ground-plane forward and right (world -Z and +X without one).
void camera_axes(World& w, math::vec3& forward, math::vec3& right) {
    forward = {0.0f, 0.0f, -1.0f};
    right = {1.0f, 0.0f, 0.0f};
    bool found = false;
    w.query<const Camera3DComponent, const LocalTransform>().each([&](const Camera3DComponent& c, const LocalTransform& t) {
        if (found || !c.primary) return;
        found = true;
        math::vec3 f = math::rotate(t.rotation, {0.0f, 0.0f, -1.0f});
        f.y = 0.0f;
        const f32 len = math::length(f);
        if (len < 1e-4f) return;
        forward = f * (1.0f / len);
        right = {-forward.z, 0.0f, forward.x};
    });
}

void on_body_removed(World& w, Entity e) {
    if (Physics3D* phys = w.try_resource<Physics3D>())
        if (phys->world.has_body(e)) phys->world.destroy_body(e);
    if (Bodies3DState* s = w.try_resource<Bodies3DState>()) {
        s->poses.erase(e);
        s->failed.erase(e);
    }
}

// FixedUpdate_Pre: create, follow outside moves, apply velocities; walk the characters.
void kit3d_bodies(Frame& f) {
    World& w = f.world();
    Physics3D& phys = f.resource<Physics3D>();
    Bodies3DState& st = f.resource<Bodies3DState>();
    asset::AssetSystem* assets = kit3d_assets(w);

    // Static colliders spawned after Startup, and ones moved since they were built.
    auto sync_static = [&](Entity e, const LocalTransform& t, auto&& create) {
        math::vec3 pos, scale;
        math::quat rot;
        world_pose(w, e, t, pos, rot, scale);
        if (!phys.world.has_body(e)) {
            if (st.failed.find(e)) return;
            if (create()) ++phys.colliders;
            else st.failed.insert_or_assign(e, u8{1});
        } else if (const Pose* p = st.poses.find(e); p && !same(*p, pos, rot)) {
            phys.world.destroy_body(e);  // rebuilt: the box offset and pitch hang off the pose
            create();
        }
        st.poses.insert_or_assign(e, Pose{pos, rot});
    };
    f.query<const BoxCollider3D, const LocalTransform>().each([&](Entity e, const BoxCollider3D& c, const LocalTransform& t) {
        sync_static(e, t, [&] { return create_box_collider(w, phys, e, c, t); });
    });
    f.query<const MeshCollider3D, const LocalTransform>().each([&](Entity e, const MeshCollider3D& c, const LocalTransform& t) {
        sync_static(e, t, [&] { return create_mesh_collider(w, phys, assets, e, c, t); });
    });
    f.query<const TerrainCollider3D, const LocalTransform>().each([&](Entity e, const TerrainCollider3D& c, const LocalTransform& t) {
        sync_static(e, t, [&] { return create_terrain_collider(w, phys, assets, e, c, t); });
    });

    f.query<RigidBody3D, const LocalTransform>().each([&](Entity e, RigidBody3D& b, const LocalTransform& t) {
        if (!phys.world.has_body(e)) {
            if (st.failed.find(e)) return;
            if (!create_rigid_body(w, phys, e, b, t)) {
                st.failed.insert_or_assign(e, u8{1});
                return;
            }
        } else if (const Pose* p = st.poses.find(e); p && !same(*p, t.position, t.rotation)) {
            phys.world.teleport(e, t.position, t.rotation);
        }
        st.poses.insert_or_assign(e, Pose{t.position, t.rotation});
        if (b.type == BodyType3DKind::Kinematic) {
            phys.world.set_linear_velocity(e, b.velocity);
        } else {
            if (b.set_velocity) phys.world.set_linear_velocity(e, b.velocity);
            if (b.stop_spin) phys.world.set_angular_velocity(e, {});
            if (b.impulse.x != 0.0f || b.impulse.y != 0.0f || b.impulse.z != 0.0f) phys.world.apply_impulse(e, b.impulse);
        }
        b.set_velocity = false;
        b.stop_spin = false;
        b.impulse = {};
    });

    const input::InputState* in = w.try_resource<input::InputState>();
    math::vec3 forward, right;
    camera_axes(w, forward, right);
    const math::vec3 gravity = phys.world.config().gravity;
    f.query<CharacterController3D, LocalTransform>().each([&](Entity e, CharacterController3D& c, LocalTransform& t) {
        const physics::ControllerParams3D params = params_of(c);
        if (!phys.world.has_body(e)) {
            if (st.failed.find(e)) return;
            if (!phys.world.create_character(e, t.position, params)) {
                MCE_LOG_WARN("kit3d: CharacterController3D on entity {} not created", e.index);
                st.failed.insert_or_assign(e, u8{1});
                return;
            }
        } else if (const Pose* p = st.poses.find(e); p && !same(*p, t.position, t.rotation)) {
            phys.world.teleport(e, t.position, t.rotation);
            // Only a move is a teleport that stops the character. A turn (NavAgent3D's face_motion, a
            // facing script) keeps the ramped velocity: zeroing it there held a steered character at
            // one tick of acceleration on every turning tick (#377).
            const math::vec3 d = t.position - p->position;
            if (d.x != 0.0f || d.y != 0.0f || d.z != 0.0f) c.velocity = {};
        }
        // Crouching shrinks the capsule at once; standing back up waits for room overhead.
        const bool want_crouch = c.crouch_half_height > 0.0f && c.crouch_half_height < c.half_height &&
                                 (c.crouch || (c.use_input && in && in->has_action(kCrouch) && in->held(kCrouch)));
        if (want_crouch && !c.crouched) c.crouched = phys.world.set_character_height(e, c.crouch_half_height, params, true);
        else if (!want_crouch && c.crouched && phys.world.set_character_height(e, c.half_height, params, false)) c.crouched = false;

        const bool sprinting = !c.crouched && (c.sprint || (c.use_input && in && in->has_action(kSprint) && in->held(kSprint)));
        const f32 speed = c.speed * (c.crouched ? c.crouch_multiplier : sprinting ? c.sprint_multiplier : 1.0f);
        math::vec3 walk{c.move.x, 0.0f, c.move.z};
        bool jump = c.jump;
        bool release = c.release_jump;
        if (c.use_input && in) {
            walk = (right * in->move.x + forward * in->move.y) * speed;
            jump = jump || in->jump_pressed;
            release = release || in->jump_released;
        } else if (c.crouched || sprinting) {
            walk = walk * (speed / math::max(c.speed, 1e-6f));
        }
        c.jump = false;
        c.release_jump = false;

        // Horizontal velocity eases towards the wanted one at the acceleration rates (0 = at once).
        math::vec3 flat{walk.x, 0.0f, walk.z};
        const bool moving = math::length_sq(flat) > 1e-8f;
        f32 rate = moving ? c.acceleration : c.deceleration;
        if (!c.grounded && c.air_acceleration > 0.0f) rate = c.air_acceleration;
        if (rate > 0.0f) {
            const math::vec3 now{c.velocity.x, 0.0f, c.velocity.z};
            const math::vec3 d = flat - now;
            const f32 step = rate * f.dt, len = math::length(d);
            flat = len <= step ? flat : now + d * (step / len);
        }

        // Jumps: a request waits jump_buffer seconds; the ground counts for coyote_time after leaving it.
        const auto ticks = [&](f32 s) { return static_cast<u16>(math::min(std::floor(s / f.dt + 0.5f), 60000.0f)); };
        if (jump) c.buffered_ticks = static_cast<u16>(ticks(c.jump_buffer) + 1);
        const bool on_ground = c.grounded || (!c.jumping && c.airborne_ticks <= ticks(c.coyote_time) && c.airborne_ticks > 0);
        math::vec3 v{flat.x, c.velocity.y, flat.z};
        if (c.grounded && v.y < 0.0f) v.y = 0.0f;
        if (c.buffered_ticks > 0) {
            if (on_ground || c.air_jumps_used < c.air_jumps) {
                if (!on_ground) ++c.air_jumps_used;
                v.y = c.jump_speed;
                c.jumping = 1;
                c.buffered_ticks = 0;
            } else {
                --c.buffered_ticks;
            }
        }
        if (release && c.jumping == 1 && v.y > 0.0f) {
            v.y *= c.jump_cut;
            c.jumping = 2;  // one cut per jump
        }
        v = v + gravity * (c.gravity_scale * f.dt);
        const bool was_grounded = c.grounded;
        const physics::MoveResult3D r = phys.world.move_character(e, v * f.dt, params);
        c.flags = r.flags;
        c.grounded = (r.flags & physics::ControllerFlags3D::Ground) != 0;
        c.velocity = {r.velocity.x, (c.grounded && v.y <= 0.0f) ? 0.0f : r.velocity.y, r.velocity.z};
        if (r.hit_ceiling && c.velocity.y > 0.0f) c.velocity.y = 0.0f;
        if (c.grounded) {
            if (!was_grounded && c.airborne_ticks > 0 && v.y <= 0.0f) f.events().push(Landed3D{e, -v.y});
            c.airborne_ticks = 0;
            c.air_jumps_used = 0;
            c.jumping = 0;
        } else if (c.airborne_ticks < 60000) {
            ++c.airborne_ticks;
        }
        t.position = r.position;
        st.poses.insert_or_assign(e, Pose{t.position, t.rotation});
    });
}

// FixedUpdate_Physics, after the step: body poses into LocalTransform.
void kit3d_writeback(Frame& f) {
    Physics3D& phys = f.resource<Physics3D>();
    Bodies3DState& st = f.resource<Bodies3DState>();
    f.query<RigidBody3D, LocalTransform>().each([&](Entity e, RigidBody3D& b, LocalTransform& t) {
        physics::BodyState3D s;
        if (!phys.world.body_state(e, s)) return;
        t.position = s.position;
        t.rotation = s.rotation;
        b.current_velocity = s.linear_velocity;
        st.poses.insert_or_assign(e, Pose{s.position, s.rotation});
    });
}

bool overlaps(const Trigger3D& tr, const math::vec3& center, const math::vec3& scale, const math::vec3& p, f32 r) {
    if (tr.shape == TriggerShape3D::Sphere) {
        const f32 rr = tr.radius * math::max(std::fabs(scale.x), math::max(std::fabs(scale.y), std::fabs(scale.z))) + r;
        const math::vec3 d = p - center;
        return math::dot(d, d) <= rr * rr;
    }
    const math::vec3 h{std::fabs(tr.half.x * scale.x), std::fabs(tr.half.y * scale.y), std::fabs(tr.half.z * scale.z)};
    const math::vec3 q{math::clamp(p.x, center.x - h.x, center.x + h.x), math::clamp(p.y, center.y - h.y, center.y + h.y),
                       math::clamp(p.z, center.z - h.z, center.z + h.z)};
    const math::vec3 d = p - q;
    return math::dot(d, d) <= r * r;
}

// FixedUpdate_Physics, after the write-back: trigger overlaps and their events.
void kit3d_triggers(Frame& f) {
    World& w = f.world();
    Bodies3DState& st = f.resource<Bodies3DState>();
    struct Candidate {
        Entity e;
        math::vec3 p;
        f32 r;
    };
    Array<Candidate> candidates;
    w.query<const RigidBody3D, const LocalTransform>().each([&](Entity e, const RigidBody3D& b, const LocalTransform& t) {
        candidates.push_back(Candidate{e, t.position, bounding_radius(b, t.scale)});
    });
    w.query<const CharacterController3D, const LocalTransform>().each([&](Entity e, const CharacterController3D& c, const LocalTransform& t) {
        const f32 hh = c.crouched ? c.crouch_half_height : c.half_height;  // crouched: the capsule hangs lower
        candidates.push_back(Candidate{e, t.position - math::vec3{0.0f, c.half_height - hh, 0.0f}, hh + c.radius});
    });

    Array<Pair> now;
    w.query<Trigger3D, const LocalTransform>().each([&](Entity te, Trigger3D& tr, const LocalTransform& t) {
        math::vec3 pos, scale;
        math::quat rot;
        world_pose(w, te, t, pos, rot, scale);
        const math::vec3 center = pos + math::vec3{tr.offset.x * scale.x, tr.offset.y * scale.y, tr.offset.z * scale.z};
        u32 inside = 0;
        for (const Candidate& c : candidates) {
            if (c.e == te || !overlaps(tr, center, scale, c.p, c.r)) continue;
            now.push_back(Pair{te, c.e});
            ++inside;
        }
        tr.inside = inside;
    });

    auto has = [](const Array<Pair>& list, const Pair& p) {
        for (const Pair& q : list)
            if (q.trigger == p.trigger && q.other == p.other) return true;
        return false;
    };
    auto events = f.events();
    for (const Pair& p : st.inside)
        if (!has(now, p)) {
            events.push(TriggerExit3D{p.trigger, p.other});
            if (Trigger3D* tr = w.alive(p.trigger) ? w.try_get_mut<Trigger3D>(p.trigger) : nullptr) tr->last_other = p.other;
        }
    for (const Pair& p : now)
        if (!has(st.inside, p)) {
            events.push(TriggerEnter3D{p.trigger, p.other});
            Trigger3D* tr = w.try_get_mut<Trigger3D>(p.trigger);
            ++tr->enters;
            tr->last_other = p.other;
        }
    st.inside = static_cast<Array<Pair>&&>(now);
}

}  // namespace

void install_bodies3d(World& world, asset::AssetSystem* assets) {
    install_kit3d(world, assets);
    if (world.try_resource<Bodies3DState>()) return;
    world.set_resource(Bodies3DState{});
    world.register_components<RigidBody3D, CharacterController3D, Trigger3D>();
    world.register_event<TriggerEnter3D>(Phase::FixedUpdate_Physics);
    world.register_event<TriggerExit3D>(Phase::FixedUpdate_Physics);
    world.register_event<Landed3D>(Phase::FixedUpdate_Pre);
    world.set_hooks<RigidBody3D>(nullptr, &on_body_removed);
    world.set_hooks<CharacterController3D>(nullptr, &on_body_removed);
    world.set_hooks<BoxCollider3D>(nullptr, &on_body_removed);
    world.set_hooks<MeshCollider3D>(nullptr, &on_body_removed);
    world.set_hooks<TerrainCollider3D>(nullptr, &on_body_removed);
    world.add_system(Phase::FixedUpdate_Pre, "kit3d_bodies", &kit3d_bodies);
    world.add_system(Phase::FixedUpdate_Physics, "kit3d_writeback", &kit3d_writeback, "kit3d_physics");
    world.add_system(Phase::FixedUpdate_Physics, "kit3d_triggers", &kit3d_triggers, "kit3d_writeback");
}

}  // namespace mce::kit

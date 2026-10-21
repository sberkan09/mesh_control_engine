#include "mce/physics/world2d.h"

#include <algorithm>
#include <cmath>

#include "collision.h"
#include "dynamic_tree.h"
#include "mce/assert.h"
#include "mce/hash.h"

namespace mce::physics {

using namespace detail;

namespace {

constexpr u32 kNoSlot = ~0u;
constexpr f32 kOneWayDepenetration = 0.1f;  // metres
constexpr f32 kDynamicCarrySpeed = 0.05f;   // m/s: a resting crate's residual velocity is not carry
constexpr f32 kAngularSlop = 2.0f / 180.0f * math::kPi;          // joint limits (Box2D 2.x)
constexpr f32 kMaxAngularCorrection = 8.0f / 180.0f * math::kPi;

struct Controller {
    f32 max_slope_cos = 0.70710678f;
    f32 snap_down = 0.05f;
    f32 skin = 0.005f;
    MoveResult2D last;
};

struct Body {
    Entity entity;
    bool alive = false;
    bool enabled = true;
    bool fixed_rotation = false;
    bool is_controller = false;
    bool sensor = false;
    bool one_way = false;
    BodyType type = BodyType::Static;
    u8 layer = 0;
    u16 mask = 0xFFFF;
    f32 gravity_scale = 1.0f;
    f32 linear_damping = 0.0f;

    Shape shape;
    f32 density = 1.0f;
    f32 friction = 0.6f;
    f32 restitution = 0.0f;

    f32 mass = 0.0f, inv_mass = 0.0f;
    f32 inertia = 0.0f, inv_i = 0.0f;
    vec2 local_center;

    Xf xf;      // body origin transform
    vec2 c;     // centre of mass, world
    f32 a = 0;  // angle
    vec2 v;
    f32 w = 0;
    vec2 force;
    f32 torque = 0;

    i32 proxy = DynamicTree::kNull;
    Controller ctrl;

    bool awake = true;      // false while the island sleeps (cfg.enable_sleep only)
    f32 sleep_time = 0.0f;  // seconds under the sleep tolerances
};

// Distance + revolute joints in the Box2D 2.x formulation (Catto).
struct Joint {
    enum class Kind : u8 { Distance, Revolute };
    Kind kind = Kind::Distance;
    bool alive = false;
    bool collide_connected = false;
    u32 generation = 1;
    u32 a = 0, b = 0;   // body slots
    u32 island = 0;     // lowest dynamic body slot of the joint's island; kNoSlot = not solved
    vec2 local_a, local_b;  // anchors, body-origin frames

    // distance
    f32 length = 1.0f, min_length = 1.0f, max_length = 1.0f;
    f32 hertz = 0.0f, damping_ratio = 0.0f;
    f32 impulse = 0.0f;
    // revolute
    f32 reference_angle = 0.0f;
    bool enable_limit = false, enable_motor = false;
    f32 lower = 0.0f, upper = 0.0f;
    f32 motor_speed = 0.0f, max_motor_torque = 0.0f;
    vec2 lin_impulse;
    f32 motor_impulse = 0.0f;
    // both: limit impulses (distance: min/max length; revolute: lower/upper angle)
    f32 lower_impulse = 0.0f, upper_impulse = 0.0f;

    // per-step solver data
    vec2 ra, rb, u;
    f32 current_length = 0.0f, mass = 0.0f, soft_mass = 0.0f, gamma = 0.0f, bias = 0.0f;
    f32 axial_mass = 0.0f, k11 = 0.0f, k12 = 0.0f, k22 = 0.0f;
    bool fixed_rotation = false;
};

// 2x2 symmetric solve K x = b (Box2D 2.x Mat22 solve).
vec2 solve22(f32 k11, f32 k12, f32 k22, vec2 b) {
    f32 det = k11 * k22 - k12 * k12;
    if (det != 0.0f) det = 1.0f / det;
    return {det * (k22 * b.x - k12 * b.y), det * (k11 * b.y - k12 * b.x)};
}

struct ConstraintPoint {
    vec2 ra, rb;
    f32 normal_impulse = 0, tangent_impulse = 0;
    f32 normal_mass = 0, tangent_mass = 0;
    f32 velocity_bias = 0;
};

struct Contact {
    u64 key = 0;       // pair_key(lo, hi): contacts are kept sorted by it (warm-start lookup)
    u32 a = 0, b = 0;  // body slots in manifold order (polygon first)
    u32 island = 0;    // lowest dynamic body slot of the contact's island
    Manifold m;
    vec2 normal;
    ConstraintPoint pts[2];
    u32 count = 0;
    f32 friction = 0, restitution = 0;
    f32 approach = 0;
    vec2 point;
    bool block = false;           // two points solved together (Box2D 2.x block solver)
    f32 k11 = 0, k12 = 0, k22 = 0;  // the block's effective-mass matrix
};

constexpr u64 pair_key(u32 a, u32 b) { return (u64(a) << 32) | b; }
constexpr u32 key_hi(u64 k) { return static_cast<u32>(k >> 32); }
constexpr u32 key_lo(u64 k) { return static_cast<u32>(k & 0xFFFFFFFFu); }

bool layers_collide(u8 la, u16 ma, u8 lb, u16 mb) { return ((ma >> lb) & 1u) && ((mb >> la) & 1u); }

math::aabb2 expand(const math::aabb2& b, f32 m) { return {b.min - vec2{m, m}, b.max + vec2{m, m}}; }
bool contains(const math::aabb2& outer, const math::aabb2& inner) {
    return outer.min.x <= inner.min.x && outer.min.y <= inner.min.y && inner.max.x <= outer.max.x &&
           inner.max.y <= outer.max.y;
}

}  // namespace

struct World2D::Impl {
    PhysicsConfig cfg;
    mem::IAllocator* alloc;
    Array<Body> bodies;
    Array<u32> free_slots;
    Array<u32> slot_of;  // entity.index -> slot
    DynamicTree tree;

    Array<u64> pairs;        // scratch
    Array<Contact> contacts;  // rebuilt every step, sorted by key
    Array<Contact> prev_contacts;  // last step's contacts: the warm-start cache
    Array<u32> island_parent;      // union-find over body slots (scratch)
    Array<u32> solve_order;        // contact indices grouped by island (scratch)
    Array<u32> joint_order;        // joint indices grouped by island (scratch)
    struct IslandRange {
        u32 root = 0;
        u32 contact_begin = 0, contact_end = 0;  // into solve_order
        u32 joint_begin = 0, joint_end = 0;      // into joint_order
    };
    Array<IslandRange> island_ranges;  // awake islands with something to solve (scratch)
    Array<u8> island_flags;            // per root: IslandAwake | IslandDriven (scratch)
    Array<f32> island_sleep;           // per root: min sleep time (scratch)
    Array<Joint> joints;
    Array<u32> free_joints;
    Array<u64> no_collide;  // sorted (lo, hi) slot pairs joined with collide_connected = false
    u32 live_joints = 0;
    u32 island_count = 0;    // dynamic-body islands last step (stats)
    u32 solved_islands = 0;  // islands with at least one contact = island_ranges.size() / 2
    Array<u64> touching;      // open contact pairs (lo<<32|hi), sorted
    Array<u64> touching_next;
    Array<u64> sensor_open;   // (sensor<<32|other), sorted
    Array<u64> sensor_next;

    Array<TriggerEnter> trigger_enters;
    Array<TriggerExit> trigger_exits;
    Array<ContactBegin2D> contact_begins;
    Array<ContactEnd2D> contact_ends;
    const QueryFilter* move_filter = nullptr;  // set for the duration of move()
    Array<TriggerExit> pending_trigger_exits;  // from destroy_body, delivered next step
    Array<ContactEnd2D> pending_contact_ends;

    explicit Impl(const PhysicsConfig& c, mem::IAllocator& a)
        : cfg(c), alloc(&a), bodies(a), free_slots(a), slot_of(a), pairs(a), contacts(a), prev_contacts(a),
          island_parent(a), solve_order(a), joint_order(a), island_ranges(a), island_flags(a), island_sleep(a),
          joints(a), free_joints(a), no_collide(a), touching(a),
          touching_next(a), sensor_open(a), sensor_next(a), trigger_enters(a), trigger_exits(a), contact_begins(a),
          contact_ends(a), pending_trigger_exits(a), pending_contact_ends(a) {}

    // ---- lookup -----------------------------------------------------------------------------------
    u32 slot(Entity e) const {
        if (e.is_null() || e.index >= slot_of.size()) return kNoSlot;
        const u32 s = slot_of[e.index];
        if (s == kNoSlot || !(bodies[s].entity == e)) return kNoSlot;
        return s;
    }
    Body* get(Entity e) {
        const u32 s = slot(e);
        return s == kNoSlot ? nullptr : &bodies[s];
    }
    const Body* get(Entity e) const {
        const u32 s = slot(e);
        return s == kNoSlot ? nullptr : &bodies[s];
    }

    // Moves this step: integrated, collided as a querier, solved. Sleeping bodies are not.
    static bool active(const Body& b) { return b.alive && b.enabled && b.type != BodyType::Static && b.awake; }

    static void wake(Body& b) {
        if (b.type != BodyType::Dynamic) return;
        b.awake = true;
        b.sleep_time = 0.0f;
    }

    // Wakes everything touching or jointed to slot `s` (it moved, changed or is going away).
    void wake_touching(u32 s) {
        for (const Contact& c : contacts) {
            if (c.a == s) wake(bodies[c.b]);
            if (c.b == s) wake(bodies[c.a]);
        }
        for (const Joint& j : joints) {
            if (!j.alive) continue;
            if (j.a == s) wake(bodies[j.b]);
            if (j.b == s) wake(bodies[j.a]);
        }
    }

    // ---- body setup --------------------------------------------------------------------------------
    static void sync_xf(Body& b) {
        b.xf.q = Rot::of(b.a);
        b.xf.p = b.c - mul(b.xf.q, b.local_center);
    }

    static void update_mass(Body& b) {
        b.mass = b.inv_mass = b.inertia = b.inv_i = 0.0f;
        b.local_center = {};
        if (b.type == BodyType::Dynamic && !b.sensor) {
            const MassData md = compute_mass(b.shape, b.density);
            b.mass = md.mass > 0.0f ? md.mass : 1.0f;
            b.inv_mass = 1.0f / b.mass;
            b.local_center = md.mass > 0.0f ? md.center : vec2{};
            const f32 i_center = md.inertia - b.mass * dot(b.local_center, b.local_center);
            if (!b.fixed_rotation && i_center > 0.0f && md.mass > 0.0f) {
                b.inertia = i_center;
                b.inv_i = 1.0f / i_center;
            }
        } else if (b.type == BodyType::Dynamic) {
            b.mass = 1.0f;  // a dynamic sensor still falls
            b.inv_mass = 1.0f;
        }
        b.c = mul(b.xf, b.local_center);
    }

    void apply_body_config(Body& b, const RigidBody2D& rb) {
        b.type = b.is_controller ? BodyType::Kinematic : rb.type;
        b.fixed_rotation = rb.fixed_rotation || b.is_controller;
        b.layer = static_cast<u8>(rb.layer & 15u);
        b.mask = rb.mask;
        b.gravity_scale = rb.gravity_scale;
        b.linear_damping = rb.linear_damping;
        if (b.type == BodyType::Static) {
            b.v = {};
            b.w = 0.0f;
        }
        if (b.fixed_rotation) b.w = 0.0f;
        update_mass(b);
    }

    bool apply_collider(Body& b, const Collider2D& c) {
        Shape s;
        if (!build_shape(c.shape, s)) return false;
        b.shape = s;
        b.density = c.density;
        b.friction = c.friction;
        b.restitution = c.restitution;
        b.sensor = c.is_sensor;
        b.one_way = c.one_way;
        return true;
    }

    static void apply_controller(Body& b, const CharacterController2D& c) {
        b.ctrl.max_slope_cos = std::cos(math::radians(math::clamp(c.max_slope_deg, 0.0f, 89.0f)));
        b.ctrl.snap_down = c.snap_down_distance > 0.0f ? c.snap_down_distance : 0.0f;
        b.ctrl.skin = c.skin_width > 0.0f ? c.skin_width : kLinearSlop;
    }

    math::aabb2 fat_box(const Body& b, vec2 displacement) const {
        math::aabb2 box = expand(compute_aabb(b.shape, b.xf), kAabbMargin);
        // Predictive extension along the motion (b2_aabbMultiplier-style).
        const vec2 d = 2.0f * displacement;
        if (d.x < 0.0f) box.min.x += d.x;
        else box.max.x += d.x;
        if (d.y < 0.0f) box.min.y += d.y;
        else box.max.y += d.y;
        return box;
    }

    void refresh_proxy(Body& b, vec2 displacement, bool force) {
        if (b.proxy == DynamicTree::kNull) return;
        const math::aabb2 tight = compute_aabb(b.shape, b.xf);
        if (!force && contains(tree.fat_aabb(b.proxy), tight)) return;
        tree.move_proxy(b.proxy, fat_box(b, displacement));
    }

    // ---- filtering ----------------------------------------------------------------------------------
    bool passes(const Body& b, const QueryFilter& f) const {
        if (!b.alive || !b.enabled) return false;
        if (b.sensor && !f.include_sensors) return false;
        if (!((f.layer_mask >> b.layer) & 1u)) return false;
        for (const Entity& x : f.exclude)
            if (x == b.entity) return false;
        if (f.predicate && !f.predicate(b.entity, f.user)) return false;
        return true;
    }

    // ---- pairs ---------------------------------------------------------------------------------------
    // Every broadphase pair with at least one enabled non-static body, as sorted (lo, hi) slot keys.
    void find_pairs(Array<u64>& out) {
        out.clear();
        for (u32 i = 0; i < bodies.size(); ++i) {
            const Body& bi = bodies[i];
            if (!bi.alive || !bi.enabled || bi.type == BodyType::Static || !bi.awake) continue;
            tree.query(tree.fat_aabb(bi.proxy), [&](u32 j) {
                if (j == i) return true;
                const Body& bj = bodies[j];
                if (!bj.enabled) return true;
                // The lower slot owns the pair — when it queries at all (sleeping bodies do not).
                if (bj.type != BodyType::Static && bj.awake && j < i) return true;
                out.push_back(i < j ? pair_key(i, j) : pair_key(j, i));
                return true;
            });
        }
        std::sort(out.begin(), out.end());
        out.resize(static_cast<usize>(std::unique(out.begin(), out.end()) - out.begin()));
    }

    // ---- stepping -----------------------------------------------------------------------------------
    void step(f32 dt) {
        trigger_enters.clear();
        trigger_exits.clear();
        contact_begins.clear();
        contact_ends.clear();
        for (const TriggerExit& e : pending_trigger_exits) trigger_exits.push_back(e);
        for (const ContactEnd2D& e : pending_contact_ends) contact_ends.push_back(e);
        pending_trigger_exits.clear();
        pending_contact_ends.clear();
        if (!(dt > 0.0f)) return;

        // 1. integrate velocities
        for (Body& b : bodies) {
            if (!b.alive || !b.enabled || b.type != BodyType::Dynamic || !b.awake) continue;
            b.v += dt * (b.gravity_scale * cfg.gravity + b.inv_mass * b.force);
            b.w += dt * b.inv_i * b.torque;
            b.v *= 1.0f / (1.0f + dt * b.linear_damping);
        }

        // 2. collide (start-of-step poses), carrying last step's impulses forward (warm start)
        find_pairs(pairs);
        std::swap(contacts, prev_contacts);
        contacts.clear();
        for (const u64 k : pairs) {
            const u32 ia = key_hi(k), ib = key_lo(k);
            const Body& ba = bodies[ia];
            const Body& bb = bodies[ib];
            if (ba.sensor || bb.sensor) continue;
            if (ba.type != BodyType::Dynamic && bb.type != BodyType::Dynamic) continue;
            if (!layers_collide(ba.layer, ba.mask, bb.layer, bb.mask)) continue;
            if (!no_collide.empty() && std::binary_search(no_collide.begin(), no_collide.end(), k)) continue;
            Contact c;
            c.key = k;
            bool swapped = false;
            collide_any(ba.shape, ba.xf, bb.shape, bb.xf, c.m, &swapped);
            if (c.m.count == 0) continue;
            c.a = swapped ? ib : ia;
            c.b = swapped ? ia : ib;
            c.count = c.m.count;
            const Body& A = bodies[c.a];
            const Body& B = bodies[c.b];
            WorldManifold wm;
            world_manifold(c.m, A.xf, A.shape.radius, B.xf, B.shape.radius, wm);
            c.normal = wm.normal;
            if (A.one_way || B.one_way) {
                // The one-way body must be below: the normal from it to the other body points up,
                // and the other body must not be moving up through it.
                const bool a_is_platform = A.one_way;
                const vec2 up = a_is_platform ? c.normal : -c.normal;
                const Body& other = a_is_platform ? B : A;
                if (up.y < 0.5f || other.v.y > 0.0f) continue;
            }
            c.friction = std::sqrt(A.friction * B.friction);
            c.restitution = math::max(A.restitution, B.restitution);
            c.point = wm.points[0];
            if (c.count == 2) c.point = 0.5f * (wm.points[0] + wm.points[1]);
            const vec2 tangent = cross(c.normal, 1.0f);
            for (u32 j = 0; j < c.count; ++j) {
                ConstraintPoint& cp = c.pts[j];
                cp.ra = wm.points[j] - A.c;
                cp.rb = wm.points[j] - B.c;
                const f32 rna = cross(cp.ra, c.normal);
                const f32 rnb = cross(cp.rb, c.normal);
                const f32 kn = A.inv_mass + B.inv_mass + A.inv_i * rna * rna + B.inv_i * rnb * rnb;
                cp.normal_mass = kn > 0.0f ? 1.0f / kn : 0.0f;
                const f32 rta = cross(cp.ra, tangent);
                const f32 rtb = cross(cp.rb, tangent);
                const f32 kt = A.inv_mass + B.inv_mass + A.inv_i * rta * rta + B.inv_i * rtb * rtb;
                cp.tangent_mass = kt > 0.0f ? 1.0f / kt : 0.0f;
                // Restitution bias from the pre-solve approach speed (before warm starting), only
                // above the threshold so resting contacts stay quiet.
                const f32 vrel =
                    dot(c.normal, B.v + cross(B.w, cp.rb) - A.v - cross(A.w, cp.ra));
                if (vrel < -cfg.restitution_threshold) cp.velocity_bias = -c.restitution * vrel;
                if (-vrel > c.approach) c.approach = -vrel;
            }
            if (cfg.block_solver && c.count == 2) prepare_block(c, A, B);
            if (cfg.warm_starting) match_impulses(c);
            contacts.push_back(c);
        }
        // Sleeping pairs are not re-collided: nothing in them moved, so last step's contact (and
        // its impulses, for the wake-up) carries over unchanged.
        if (cfg.enable_sleep) carry_sleeping_contacts();

        // 3. islands: contact- and joint-connected dynamic bodies (static/kinematic do not link)
        build_islands();

        // 4. velocity solve per island: warm start, then sequential impulse — joints, then
        //    contacts (friction first), Box2D 2.x order
        for (u32 i = 0; i < joint_order.size(); ++i) init_joint(joints[joint_order[i]], dt);
        for (u32 is = 0; is < solved_islands; ++is) {
            const IslandRange& r = island_ranges[is];
            for (u32 i = r.joint_begin; i < r.joint_end; ++i) warm_start_joint(joints[joint_order[i]]);
            for (u32 i = r.contact_begin; i < r.contact_end; ++i) warm_start(contacts[solve_order[i]]);
            for (u32 it = 0; it < cfg.velocity_iterations; ++it) {
                for (u32 i = r.joint_begin; i < r.joint_end; ++i) solve_joint_velocity(joints[joint_order[i]], dt);
                for (u32 i = r.contact_begin; i < r.contact_end; ++i) solve_velocity(contacts[solve_order[i]]);
            }
        }

        // 5. integrate positions
        const f32 max_translation = kMaxLinearSpeed * dt;
        for (Body& b : bodies) {
            if (!b.alive || !b.enabled || b.type == BodyType::Static || b.is_controller || !b.awake) continue;
            vec2 translation = dt * b.v;
            if (dot(translation, translation) > max_translation * max_translation) {
                b.v *= max_translation / math::length(translation);
                translation = dt * b.v;
            }
            f32 rotation = dt * b.w;
            if (rotation * rotation > kMaxRotation * kMaxRotation) {
                b.w *= kMaxRotation / std::fabs(rotation);
                rotation = dt * b.w;
            }
            b.c += translation;
            b.a += rotation;
            sync_xf(b);
        }

        // 5b. position iterations per island (Box2D 2.x non-linear Gauss-Seidel); each island
        //     stops on its own once its deepest penetration is within slop.
        for (u32 is = 0; is < solved_islands; ++is) {
            const IslandRange& r = island_ranges[is];
            for (u32 it = 0; it < cfg.position_iterations; ++it) {
                f32 min_sep = 0.0f;
                for (u32 i = r.contact_begin; i < r.contact_end; ++i)
                    min_sep = math::min(min_sep, solve_position(contacts[solve_order[i]]));
                bool joints_ok = true;
                for (u32 i = r.joint_begin; i < r.joint_end; ++i)
                    joints_ok = solve_joint_position(joints[joint_order[i]]) && joints_ok;
                if (min_sep >= -3.0f * kLinearSlop && joints_ok) break;
            }
        }

        // 5c. islands that stayed still long enough go to sleep
        if (cfg.enable_sleep) update_sleep(dt);

        // 6. clear forces, refresh broadphase
        for (Body& b : bodies) {
            if (!b.alive) continue;
            b.force = {};
            b.torque = 0.0f;
            if (b.enabled && b.type != BodyType::Static && b.awake) refresh_proxy(b, dt * b.v, false);
        }

        update_contact_events();
        update_sensors();
    }

    // ---- solver pieces ----------------------------------------------------------------------------
    // Carries last step's accumulated impulses onto `c` when the same pair touched with the same
    // orientation; points match by feature id (Box2D 2.x contact update).
    void match_impulses(Contact& c) const {
        const Contact* lo = prev_contacts.begin();
        const Contact* hi = prev_contacts.end();
        const Contact* it = std::lower_bound(lo, hi, c.key, [](const Contact& x, u64 k) { return x.key < k; });
        if (it == hi || it->key != c.key || it->a != c.a) return;
        for (u32 j = 0; j < c.count; ++j)
            for (u32 o = 0; o < it->count; ++o)
                if (it->m.points[o].id == c.m.points[j].id) {
                    c.pts[j].normal_impulse = it->pts[o].normal_impulse;
                    c.pts[j].tangent_impulse = it->pts[o].tangent_impulse;
                    break;
                }
    }

    u32 find_root(u32 x) {
        while (island_parent[x] != x) {
            island_parent[x] = island_parent[island_parent[x]];
            x = island_parent[x];
        }
        return x;
    }

    // Union-find over dynamic bodies linked by contacts and joints; the root is always the lowest
    // slot, so islands (and the contacts and joints inside them) come out in body-slot order (D8).
    // With sleeping on, an island is awake when any member is awake or a moving kinematic body
    // touches it; its sleeping members wake, and fully asleep islands are not solved.
    enum : u8 { IslandAwake = 1u << 0, IslandDriven = 1u << 1 };

    void link(u32 a, u32 b) {
        const u32 ra = find_root(a), rb = find_root(b);
        if (ra < rb) island_parent[rb] = ra;
        else if (rb < ra) island_parent[ra] = rb;
    }

    static bool moving_kinematic(const Body& b) {
        return b.type == BodyType::Kinematic && b.enabled && (b.v.x != 0.0f || b.v.y != 0.0f || b.w != 0.0f);
    }

    void build_islands() {
        const u32 n = static_cast<u32>(bodies.size());
        island_parent.resize(n);
        for (u32 i = 0; i < n; ++i) island_parent[i] = i;
        auto dynamic = [&](u32 s) { return bodies[s].type == BodyType::Dynamic; };
        for (const Contact& c : contacts)
            if (dynamic(c.a) && dynamic(c.b)) link(c.a, c.b);
        for (const Joint& j : joints)
            if (j.alive && dynamic(j.a) && dynamic(j.b) && bodies[j.a].enabled && bodies[j.b].enabled) link(j.a, j.b);
        island_count = 0;
        for (u32 i = 0; i < n; ++i) {
            const Body& b = bodies[i];
            if (b.alive && b.enabled && b.type == BodyType::Dynamic && find_root(i) == i) ++island_count;
        }
        for (Contact& c : contacts) c.island = find_root(dynamic(c.a) ? c.a : c.b);
        for (Joint& j : joints) {
            j.island = kNoSlot;
            if (!j.alive || !bodies[j.a].enabled || !bodies[j.b].enabled) continue;
            if (dynamic(j.a)) j.island = find_root(j.a);
            else if (dynamic(j.b)) j.island = find_root(j.b);
        }

        auto awake_island = [&](u32 root) { return !cfg.enable_sleep || (island_flags[root] & IslandAwake) != 0; };
        if (cfg.enable_sleep) {
            island_flags.resize(n);
            for (u32 i = 0; i < n; ++i) island_flags[i] = 0;
            for (u32 i = 0; i < n; ++i) {
                const Body& b = bodies[i];
                if (b.alive && b.enabled && b.type == BodyType::Dynamic && b.awake) island_flags[find_root(i)] |= IslandAwake;
            }
            for (const Contact& c : contacts)
                if (moving_kinematic(bodies[c.a]) || moving_kinematic(bodies[c.b]))
                    island_flags[c.island] |= IslandAwake | IslandDriven;
            for (const Joint& j : joints)
                if (j.island != kNoSlot && (moving_kinematic(bodies[j.a]) || moving_kinematic(bodies[j.b])))
                    island_flags[j.island] |= IslandAwake | IslandDriven;
            for (u32 i = 0; i < n; ++i) {
                Body& b = bodies[i];
                if (b.alive && b.enabled && b.type == BodyType::Dynamic && !b.awake && (island_flags[find_root(i)] & IslandAwake))
                    wake(b);
            }
        }

        solve_order.clear();
        for (u32 i = 0; i < contacts.size(); ++i)
            if (awake_island(contacts[i].island)) solve_order.push_back(i);
        std::stable_sort(solve_order.begin(), solve_order.end(),
                         [&](u32 x, u32 y) { return contacts[x].island < contacts[y].island; });
        joint_order.clear();
        for (u32 i = 0; i < joints.size(); ++i)
            if (joints[i].island != kNoSlot && awake_island(joints[i].island)) joint_order.push_back(i);
        std::stable_sort(joint_order.begin(), joint_order.end(),
                         [&](u32 x, u32 y) { return joints[x].island < joints[y].island; });

        // Merge the two island-sorted lists into one range per island.
        island_ranges.clear();
        u32 ci = 0, ji = 0;
        const u32 cn = static_cast<u32>(solve_order.size()), jn = static_cast<u32>(joint_order.size());
        while (ci < cn || ji < jn) {
            const u32 rc = ci < cn ? contacts[solve_order[ci]].island : kNoSlot;
            const u32 rj = ji < jn ? joints[joint_order[ji]].island : kNoSlot;
            IslandRange r;
            r.root = rc < rj ? rc : rj;
            r.contact_begin = ci;
            while (ci < cn && contacts[solve_order[ci]].island == r.root) ++ci;
            r.contact_end = ci;
            r.joint_begin = ji;
            while (ji < jn && joints[joint_order[ji]].island == r.root) ++ji;
            r.joint_end = ji;
            island_ranges.push_back(r);
        }
        // Contact- and joint-free islands need no solving: only the ranges above are iterated.
        solved_islands = static_cast<u32>(island_ranges.size());
    }

    // Copies last step's contacts between bodies that are both not moving (asleep or static).
    void carry_sleeping_contacts() {
        bool carried = false;
        for (const Contact& c : prev_contacts) {
            const Body& A = bodies[c.a];
            const Body& B = bodies[c.b];
            if (!A.alive || !B.alive || !A.enabled || !B.enabled || active(A) || active(B)) continue;
            contacts.push_back(c);
            carried = true;
        }
        if (carried)
            std::sort(contacts.begin(), contacts.end(), [](const Contact& x, const Contact& y) { return x.key < y.key; });
    }

    void update_sleep(f32 dt) {
        const u32 n = static_cast<u32>(bodies.size());
        island_sleep.resize(n);
        for (u32 i = 0; i < n; ++i) island_sleep[i] = 3.4e38f;
        const f32 lin2 = cfg.linear_sleep_tolerance * cfg.linear_sleep_tolerance;
        const f32 ang2 = cfg.angular_sleep_tolerance * cfg.angular_sleep_tolerance;
        for (u32 i = 0; i < n; ++i) {
            Body& b = bodies[i];
            if (!b.alive || !b.enabled || b.type != BodyType::Dynamic || !b.awake) continue;
            const u32 root = find_root(i);
            if ((island_flags[root] & IslandDriven) || b.w * b.w > ang2 || dot(b.v, b.v) > lin2) b.sleep_time = 0.0f;
            else b.sleep_time += dt;
            island_sleep[root] = math::min(island_sleep[root], b.sleep_time);
        }
        for (u32 i = 0; i < n; ++i) {
            Body& b = bodies[i];
            if (!b.alive || !b.enabled || b.type != BodyType::Dynamic || !b.awake) continue;
            if (island_sleep[find_root(i)] < cfg.time_to_sleep) continue;
            b.awake = false;
            b.v = {};
            b.w = 0.0f;
        }
    }

    // ---- joints ----------------------------------------------------------------------------------
    void init_joint(Joint& j, f32 dt) {
        const Body& A = bodies[j.a];
        const Body& B = bodies[j.b];
        const f32 ma = A.inv_mass, mb = B.inv_mass, ia = A.inv_i, ib = B.inv_i;
        j.ra = mul(Rot::of(A.a), j.local_a - A.local_center);
        j.rb = mul(Rot::of(B.a), j.local_b - B.local_center);
        if (!cfg.warm_starting) {
            j.impulse = j.lower_impulse = j.upper_impulse = j.motor_impulse = 0.0f;
            j.lin_impulse = {};
        }
        if (j.kind == Joint::Kind::Distance) {
            const vec2 d = B.c + j.rb - A.c - j.ra;
            j.current_length = math::length(d);
            if (j.current_length > kLinearSlop) {
                j.u = (1.0f / j.current_length) * d;
            } else {
                j.u = {};
                j.impulse = j.lower_impulse = j.upper_impulse = 0.0f;
            }
            const f32 cra = cross(j.ra, j.u), crb = cross(j.rb, j.u);
            f32 inv = ma + ia * cra * cra + mb + ib * crb * crb;
            j.mass = inv != 0.0f ? 1.0f / inv : 0.0f;
            if (j.min_length < j.max_length && j.hertz > 0.0f) {
                // Box2D 2.x linear stiffness: spring constants from the pair's effective linear mass.
                const f32 m_a = A.type == BodyType::Dynamic ? A.mass : 0.0f;
                const f32 m_b = B.type == BodyType::Dynamic ? B.mass : 0.0f;
                const f32 m = m_a > 0.0f && m_b > 0.0f ? m_a * m_b / (m_a + m_b) : (m_a > 0.0f ? m_a : m_b);
                const f32 omega = 2.0f * math::kPi * j.hertz;
                const f32 k = m * omega * omega;
                const f32 c = 2.0f * m * j.damping_ratio * omega;
                const f32 C = j.current_length - j.length;
                j.gamma = dt * (c + dt * k);
                j.gamma = j.gamma != 0.0f ? 1.0f / j.gamma : 0.0f;
                j.bias = C * dt * k * j.gamma;
                inv += j.gamma;
                j.soft_mass = inv != 0.0f ? 1.0f / inv : 0.0f;
            } else {
                j.gamma = j.bias = 0.0f;
                j.soft_mass = j.mass;
                j.impulse = j.min_length < j.max_length ? 0.0f : j.impulse;  // no spring: no spring impulse
            }
        } else {
            j.k11 = ma + mb + j.ra.y * j.ra.y * ia + j.rb.y * j.rb.y * ib;
            j.k12 = -j.ra.y * j.ra.x * ia - j.rb.y * j.rb.x * ib;
            j.k22 = ma + mb + j.ra.x * j.ra.x * ia + j.rb.x * j.rb.x * ib;
            const f32 axial = ia + ib;
            j.axial_mass = axial > 0.0f ? 1.0f / axial : 0.0f;
            j.fixed_rotation = axial == 0.0f;
            if (!j.enable_motor || j.fixed_rotation) j.motor_impulse = 0.0f;
            if (!j.enable_limit || j.fixed_rotation) j.lower_impulse = j.upper_impulse = 0.0f;
        }
    }

    void warm_start_joint(const Joint& j) {
        Body& A = bodies[j.a];
        Body& B = bodies[j.b];
        if (j.kind == Joint::Kind::Distance) {
            const vec2 p = (j.impulse + j.lower_impulse - j.upper_impulse) * j.u;
            A.v -= A.inv_mass * p;
            A.w -= A.inv_i * cross(j.ra, p);
            B.v += B.inv_mass * p;
            B.w += B.inv_i * cross(j.rb, p);
        } else {
            const vec2 p = j.lin_impulse;
            const f32 axial = j.motor_impulse + j.lower_impulse - j.upper_impulse;
            A.v -= A.inv_mass * p;
            A.w -= A.inv_i * (cross(j.ra, p) + axial);
            B.v += B.inv_mass * p;
            B.w += B.inv_i * (cross(j.rb, p) + axial);
        }
    }

    static void apply_pair(Body& A, Body& B, const Joint& j, vec2 p) {
        A.v -= A.inv_mass * p;
        A.w -= A.inv_i * cross(j.ra, p);
        B.v += B.inv_mass * p;
        B.w += B.inv_i * cross(j.rb, p);
    }

    void solve_joint_velocity(Joint& j, f32 dt) {
        Body& A = bodies[j.a];
        Body& B = bodies[j.b];
        const f32 inv_dt = 1.0f / dt;
        if (j.kind == Joint::Kind::Distance) {
            if (j.min_length < j.max_length) {
                if (j.hertz > 0.0f) {
                    const vec2 dv = B.v + cross(B.w, j.rb) - A.v - cross(A.w, j.ra);
                    const f32 cdot = dot(j.u, dv);
                    const f32 impulse = -j.soft_mass * (cdot + j.bias + j.gamma * j.impulse);
                    j.impulse += impulse;
                    apply_pair(A, B, j, impulse * j.u);
                }
                {  // lower length limit
                    const f32 C = j.current_length - j.min_length;
                    const f32 bias = math::max(0.0f, C) * inv_dt;
                    const vec2 dv = B.v + cross(B.w, j.rb) - A.v - cross(A.w, j.ra);
                    const f32 cdot = dot(j.u, dv);
                    f32 impulse = -j.mass * (cdot + bias);
                    const f32 next = math::max(0.0f, j.lower_impulse + impulse);
                    impulse = next - j.lower_impulse;
                    j.lower_impulse = next;
                    apply_pair(A, B, j, impulse * j.u);
                }
                {  // upper length limit
                    const f32 C = j.max_length - j.current_length;
                    const f32 bias = math::max(0.0f, C) * inv_dt;
                    const vec2 dv = A.v + cross(A.w, j.ra) - B.v - cross(B.w, j.rb);
                    const f32 cdot = dot(j.u, dv);
                    f32 impulse = -j.mass * (cdot + bias);
                    const f32 next = math::max(0.0f, j.upper_impulse + impulse);
                    impulse = next - j.upper_impulse;
                    j.upper_impulse = next;
                    apply_pair(A, B, j, -impulse * j.u);
                }
            } else {  // rigid rod
                const vec2 dv = B.v + cross(B.w, j.rb) - A.v - cross(A.w, j.ra);
                const f32 cdot = dot(j.u, dv);
                const f32 impulse = -j.mass * cdot;
                j.impulse += impulse;
                apply_pair(A, B, j, impulse * j.u);
            }
            return;
        }

        const f32 ia = A.inv_i, ib = B.inv_i;
        if (j.enable_motor && !j.fixed_rotation) {
            const f32 cdot = B.w - A.w - j.motor_speed;
            f32 impulse = -j.axial_mass * cdot;
            const f32 old = j.motor_impulse, max_impulse = dt * j.max_motor_torque;
            j.motor_impulse = math::clamp(old + impulse, -max_impulse, max_impulse);
            impulse = j.motor_impulse - old;
            A.w -= ia * impulse;
            B.w += ib * impulse;
        }
        if (j.enable_limit && !j.fixed_rotation) {
            const f32 angle = B.a - A.a - j.reference_angle;
            {  // lower
                const f32 C = angle - j.lower;
                const f32 cdot = B.w - A.w;
                f32 impulse = -j.axial_mass * (cdot + math::max(C, 0.0f) * inv_dt);
                const f32 next = math::max(j.lower_impulse + impulse, 0.0f);
                impulse = next - j.lower_impulse;
                j.lower_impulse = next;
                A.w -= ia * impulse;
                B.w += ib * impulse;
            }
            {  // upper
                const f32 C = j.upper - angle;
                const f32 cdot = A.w - B.w;
                f32 impulse = -j.axial_mass * (cdot + math::max(C, 0.0f) * inv_dt);
                const f32 next = math::max(j.upper_impulse + impulse, 0.0f);
                impulse = next - j.upper_impulse;
                j.upper_impulse = next;
                A.w += ia * impulse;
                B.w -= ib * impulse;
            }
        }
        const vec2 cdot = B.v + cross(B.w, j.rb) - A.v - cross(A.w, j.ra);
        const vec2 impulse = solve22(j.k11, j.k12, j.k22, -cdot);
        j.lin_impulse += impulse;
        apply_pair(A, B, j, impulse);
    }

    // One NGS pass; true when the joint is within slop.
    bool solve_joint_position(Joint& j) {
        Body& A = bodies[j.a];
        Body& B = bodies[j.b];
        const f32 ma = A.inv_mass, mb = B.inv_mass, ia = A.inv_i, ib = B.inv_i;
        if (j.kind == Joint::Kind::Distance) {
            const vec2 ra = mul(Rot::of(A.a), j.local_a - A.local_center);
            const vec2 rb = mul(Rot::of(B.a), j.local_b - B.local_center);
            const vec2 d = B.c + rb - A.c - ra;
            const f32 len = math::length(d);
            const vec2 u = len > kEpsilon ? (1.0f / len) * d : vec2{};
            f32 C;
            if (j.min_length == j.max_length) C = len - j.min_length;
            else if (len < j.min_length) C = len - j.min_length;
            else if (j.max_length < len) C = len - j.max_length;
            else return true;
            const vec2 p = (-j.mass * C) * u;
            A.c -= ma * p;
            A.a -= ia * cross(ra, p);
            B.c += mb * p;
            B.a += ib * cross(rb, p);
            sync_xf(A);
            sync_xf(B);
            return std::fabs(C) < kLinearSlop;
        }

        f32 angular_error = 0.0f;
        if (j.enable_limit && !j.fixed_rotation) {
            const f32 angle = B.a - A.a - j.reference_angle;
            f32 C = 0.0f;
            if (std::fabs(j.upper - j.lower) < 2.0f * kAngularSlop)
                C = math::clamp(angle - j.lower, -kMaxAngularCorrection, kMaxAngularCorrection);
            else if (angle <= j.lower)
                C = math::clamp(angle - j.lower + kAngularSlop, -kMaxAngularCorrection, 0.0f);
            else if (angle >= j.upper)
                C = math::clamp(angle - j.upper - kAngularSlop, 0.0f, kMaxAngularCorrection);
            const f32 limit_impulse = -j.axial_mass * C;
            A.a -= ia * limit_impulse;
            B.a += ib * limit_impulse;
            angular_error = std::fabs(C);
        }
        const vec2 ra = mul(Rot::of(A.a), j.local_a - A.local_center);
        const vec2 rb = mul(Rot::of(B.a), j.local_b - B.local_center);
        const vec2 C = B.c + rb - A.c - ra;
        const f32 position_error = math::length(C);
        const f32 k11 = ma + mb + ia * ra.y * ra.y + ib * rb.y * rb.y;
        const f32 k12 = -ia * ra.x * ra.y - ib * rb.x * rb.y;
        const f32 k22 = ma + mb + ia * ra.x * ra.x + ib * rb.x * rb.x;
        const vec2 impulse = -solve22(k11, k12, k22, C);
        A.c -= ma * impulse;
        A.a -= ia * cross(ra, impulse);
        B.c += mb * impulse;
        B.a += ib * cross(rb, impulse);
        sync_xf(A);
        sync_xf(B);
        return position_error <= kLinearSlop && angular_error <= kAngularSlop;
    }

    void rebuild_no_collide() {
        no_collide.clear();
        for (const Joint& j : joints)
            if (j.alive && !j.collide_connected) no_collide.push_back(j.a < j.b ? pair_key(j.a, j.b) : pair_key(j.b, j.a));
        std::sort(no_collide.begin(), no_collide.end());
    }

    Joint* joint(JointId id) {
        if (id.index >= joints.size() || !joints[id.index].alive || joints[id.index].generation != id.generation)
            return nullptr;
        return &joints[id.index];
    }
    const Joint* joint(JointId id) const { return const_cast<Impl*>(this)->joint(id); }

    JointId add_joint(Joint j) {
        j.alive = true;
        u32 idx;
        if (!free_joints.empty()) {
            idx = free_joints.back();
            free_joints.pop_back();
            j.generation = joints[idx].generation;
            joints[idx] = j;
        } else {
            idx = static_cast<u32>(joints.size());
            joints.push_back(j);
        }
        ++live_joints;
        wake(bodies[j.a]);
        wake(bodies[j.b]);
        if (!j.collide_connected) {
            rebuild_no_collide();
            // Already-touching bodies stop colliding now: drop their cached contact.
            const u64 k = j.a < j.b ? pair_key(j.a, j.b) : pair_key(j.b, j.a);
            usize w = 0;
            for (usize i = 0; i < contacts.size(); ++i)
                if (contacts[i].key != k) contacts[w++] = contacts[i];
            contacts.resize(w);
        }
        return {idx, joints[idx].generation};
    }

    void remove_joint(u32 idx) {
        Joint& j = joints[idx];
        if (!j.alive) return;
        wake(bodies[j.a]);
        wake(bodies[j.b]);
        const bool had_filter = !j.collide_connected;
        const u32 gen = j.generation + 1;
        j = Joint{};
        j.generation = gen == 0 ? 1 : gen;
        free_joints.push_back(idx);
        --live_joints;
        if (had_filter) rebuild_no_collide();
    }

    void warm_start(Contact& c) {
        Body& A = bodies[c.a];
        Body& B = bodies[c.b];
        const vec2 tangent = cross(c.normal, 1.0f);
        for (u32 j = 0; j < c.count; ++j) {
            const ConstraintPoint& cp = c.pts[j];
            const vec2 p = cp.normal_impulse * c.normal + cp.tangent_impulse * tangent;
            A.v -= A.inv_mass * p;
            A.w -= A.inv_i * cross(cp.ra, p);
            B.v += B.inv_mass * p;
            B.w += B.inv_i * cross(cp.rb, p);
        }
    }

    void solve_velocity(Contact& c) {
        Body& A = bodies[c.a];
        Body& B = bodies[c.b];
        const vec2 n = c.normal;
        const vec2 tangent = cross(n, 1.0f);
        for (u32 j = 0; j < c.count; ++j) {
            ConstraintPoint& cp = c.pts[j];
            const vec2 dv = B.v + cross(B.w, cp.rb) - A.v - cross(A.w, cp.ra);
            const f32 vt = dot(dv, tangent);
            f32 lambda = cp.tangent_mass * (-vt);
            const f32 max_f = c.friction * cp.normal_impulse;
            const f32 next = math::clamp(cp.tangent_impulse + lambda, -max_f, max_f);
            lambda = next - cp.tangent_impulse;
            cp.tangent_impulse = next;
            const vec2 p = lambda * tangent;
            A.v -= A.inv_mass * p;
            A.w -= A.inv_i * cross(cp.ra, p);
            B.v += B.inv_mass * p;
            B.w += B.inv_i * cross(cp.rb, p);
        }
        if (c.block) {
            solve_block(c, A, B);
            return;
        }
        for (u32 j = 0; j < c.count; ++j) {
            ConstraintPoint& cp = c.pts[j];
            const vec2 dv = B.v + cross(B.w, cp.rb) - A.v - cross(A.w, cp.ra);
            const f32 vn = dot(dv, n);
            f32 lambda = -cp.normal_mass * (vn - cp.velocity_bias);
            const f32 next = math::max(cp.normal_impulse + lambda, 0.0f);
            lambda = next - cp.normal_impulse;
            cp.normal_impulse = next;
            const vec2 p = lambda * n;
            A.v -= A.inv_mass * p;
            A.w -= A.inv_i * cross(cp.ra, p);
            B.v += B.inv_mass * p;
            B.w += B.inv_i * cross(cp.rb, p);
        }
    }

    // Box2D 2.x contact solver: a two-point manifold is solved as one 2x2 LCP when its
    // effective-mass matrix is well conditioned; otherwise the second point is dropped (the two
    // points are nearly redundant and solving them one by one would fight).
    void prepare_block(Contact& c, const Body& A, const Body& B) {
        const f32 rn1a = cross(c.pts[0].ra, c.normal), rn1b = cross(c.pts[0].rb, c.normal);
        const f32 rn2a = cross(c.pts[1].ra, c.normal), rn2b = cross(c.pts[1].rb, c.normal);
        const f32 m = A.inv_mass + B.inv_mass;
        const f32 k11 = m + A.inv_i * rn1a * rn1a + B.inv_i * rn1b * rn1b;
        const f32 k22 = m + A.inv_i * rn2a * rn2a + B.inv_i * rn2b * rn2b;
        const f32 k12 = m + A.inv_i * rn1a * rn2a + B.inv_i * rn1b * rn2b;
        constexpr f32 kMaxConditionNumber = 1000.0f;
        if (k11 * k11 < kMaxConditionNumber * (k11 * k22 - k12 * k12)) {
            c.block = true;
            c.k11 = k11;
            c.k12 = k12;
            c.k22 = k22;
        } else {
            c.count = 1;
        }
    }

    // Total enumeration of the 2x2 LCP vn = A x + b, vn >= 0, x >= 0, vn_i x_i = 0 on the
    // accumulated impulses (the Box2D 2.x velocity block solver).
    void solve_block(Contact& c, Body& A, Body& B) {
        ConstraintPoint& c1 = c.pts[0];
        ConstraintPoint& c2 = c.pts[1];
        const vec2 n = c.normal;
        const vec2 a{c1.normal_impulse, c2.normal_impulse};
        const vec2 dv1 = B.v + cross(B.w, c1.rb) - A.v - cross(A.w, c1.ra);
        const vec2 dv2 = B.v + cross(B.w, c2.rb) - A.v - cross(A.w, c2.ra);
        vec2 b{dot(dv1, n) - c1.velocity_bias, dot(dv2, n) - c2.velocity_bias};
        b -= vec2{c.k11 * a.x + c.k12 * a.y, c.k12 * a.x + c.k22 * a.y};
        auto apply = [&](vec2 x) {
            const vec2 d = x - a;
            const vec2 p1 = d.x * n, p2 = d.y * n;
            A.v -= A.inv_mass * (p1 + p2);
            A.w -= A.inv_i * (cross(c1.ra, p1) + cross(c2.ra, p2));
            B.v += B.inv_mass * (p1 + p2);
            B.w += B.inv_i * (cross(c1.rb, p1) + cross(c2.rb, p2));
            c1.normal_impulse = x.x;
            c2.normal_impulse = x.y;
        };
        // Case 1: both points active.
        f32 det = c.k11 * c.k22 - c.k12 * c.k12;
        det = det != 0.0f ? 1.0f / det : 0.0f;
        vec2 x{-det * (c.k22 * b.x - c.k12 * b.y), -det * (c.k11 * b.y - c.k12 * b.x)};
        if (x.x >= 0.0f && x.y >= 0.0f) return apply(x);
        // Case 2: only point 1 active.
        x = {c1.normal_mass * -b.x, 0.0f};
        if (x.x >= 0.0f && c.k12 * x.x + b.y >= 0.0f) return apply(x);
        // Case 3: only point 2 active.
        x = {0.0f, c2.normal_mass * -b.y};
        if (x.y >= 0.0f && c.k12 * x.y + b.x >= 0.0f) return apply(x);
        // Case 4: neither.
        if (b.x >= 0.0f && b.y >= 0.0f) return apply({0.0f, 0.0f});
        // No solution (numerical trouble): keep the impulses as they are.
    }

    // One NGS pass over all manifold points (the velocity solver may have dropped one); returns
    // the deepest separation seen.
    f32 solve_position(const Contact& c) {
        Body& A = bodies[c.a];
        Body& B = bodies[c.b];
        const f32 ma = A.inv_mass, mb = B.inv_mass, ia = A.inv_i, ib = B.inv_i;
        f32 min_sep = 0.0f;
        if (ma == 0.0f && mb == 0.0f) return min_sep;
        for (u32 j = 0; j < c.m.count; ++j) {
            vec2 normal, point;
            f32 separation = 0.0f;
            psm(c, A, B, j, normal, point, separation);
            const vec2 ra = point - A.c;
            const vec2 rb = point - B.c;
            min_sep = math::min(min_sep, separation);
            const f32 C = math::clamp(kBaumgarte * (separation + kLinearSlop), -kMaxLinearCorrection, 0.0f);
            const f32 rna = cross(ra, normal);
            const f32 rnb = cross(rb, normal);
            const f32 k = ma + mb + ia * rna * rna + ib * rnb * rnb;
            const f32 impulse = k > 0.0f ? -C / k : 0.0f;
            const vec2 p = impulse * normal;
            A.c -= ma * p;
            A.a -= ia * cross(ra, p);
            B.c += mb * p;
            B.a += ib * cross(rb, p);
            sync_xf(A);
            sync_xf(B);
        }
        return min_sep;
    }

    // Box2D 2.x position-solver manifold
    void psm(const Contact& c, const Body& A, const Body& B, u32 j, vec2& normal, vec2& point, f32& sep) const {
        const f32 ra = A.shape.radius, rb = B.shape.radius;
        switch (c.m.type) {
            case Manifold::Type::Circles: {
                const vec2 pa = mul(A.xf, c.m.local_point);
                const vec2 pb = mul(B.xf, c.m.points[0].local_point);
                normal = dist_sq(pa, pb) > kEpsilon * kEpsilon ? math::normalize(pb - pa) : vec2{1.0f, 0.0f};
                point = 0.5f * (pa + pb);
                sep = dot(pb - pa, normal) - ra - rb;
                break;
            }
            case Manifold::Type::FaceA: {
                normal = mul(A.xf.q, c.m.local_normal);
                const vec2 plane = mul(A.xf, c.m.local_point);
                const vec2 clip = mul(B.xf, c.m.points[j].local_point);
                sep = dot(clip - plane, normal) - ra - rb;
                point = clip;
                break;
            }
            case Manifold::Type::FaceB: {
                normal = mul(B.xf.q, c.m.local_normal);
                const vec2 plane = mul(B.xf, c.m.local_point);
                const vec2 clip = mul(A.xf, c.m.points[j].local_point);
                sep = dot(clip - plane, normal) - ra - rb;
                point = clip;
                normal = -normal;
                break;
            }
        }
    }

    void update_contact_events() {
        touching_next.clear();
        for (const Contact& c : contacts) {
            const u32 lo = c.a < c.b ? c.a : c.b;
            const u32 hi = c.a < c.b ? c.b : c.a;
            touching_next.push_back(pair_key(lo, hi));
        }
        std::sort(touching_next.begin(), touching_next.end());
        // Begins: in next, not in old. Contacts are in pair order already; look up per key.
        u32 emitted = 0;
        const u32 cap = cfg.max_contact_events_per_tick;
        for (const Contact& c : contacts) {
            const u32 lo = c.a < c.b ? c.a : c.b;
            const u32 hi = c.a < c.b ? c.b : c.a;
            if (std::binary_search(touching.begin(), touching.end(), pair_key(lo, hi))) continue;
            if (emitted++ >= cap) break;
            const bool flip = c.a != lo;
            ContactBegin2D ev;
            ev.a = bodies[lo].entity;
            ev.b = bodies[hi].entity;
            ev.point = c.point;
            ev.normal = flip ? -c.normal : c.normal;
            ev.approach_speed = c.approach;
            contact_begins.push_back(ev);
        }
        emitted = 0;
        for (const u64 k : touching) {
            if (std::binary_search(touching_next.begin(), touching_next.end(), k)) continue;
            if (emitted++ >= cap) break;
            contact_ends.push_back({bodies[key_hi(k)].entity, bodies[key_lo(k)].entity});
        }
        std::swap(touching, touching_next);
    }

    bool shapes_overlap(const Body& a, const Body& b) const {
        Manifold m;
        collide_any(a.shape, a.xf, b.shape, b.xf, m, nullptr);
        return m.count > 0;  // overlapping or exactly touching
    }

    void update_sensors() {
        find_pairs(pairs);
        sensor_next.clear();
        for (const u64 k : pairs) {
            const u32 ia = key_hi(k), ib = key_lo(k);
            const Body& a = bodies[ia];
            const Body& b = bodies[ib];
            if (!a.sensor && !b.sensor) continue;
            if (!layers_collide(a.layer, a.mask, b.layer, b.mask)) continue;
            if (!shapes_overlap(a, b)) continue;
            if (a.sensor) sensor_next.push_back(pair_key(ia, ib));
            if (b.sensor) sensor_next.push_back(pair_key(ib, ia));
        }
        // A sleeping body inside a resting sensor is not re-tested (neither queried): it stays in.
        if (cfg.enable_sleep)
            for (const u64 k : sensor_open) {
                const Body& a = bodies[key_hi(k)];
                const Body& b = bodies[key_lo(k)];
                if (a.alive && b.alive && a.enabled && b.enabled && !active(a) && !active(b)) sensor_next.push_back(k);
            }
        std::sort(sensor_next.begin(), sensor_next.end());
        sensor_next.resize(static_cast<usize>(std::unique(sensor_next.begin(), sensor_next.end()) - sensor_next.begin()));
        for (const u64 k : sensor_next)
            if (!std::binary_search(sensor_open.begin(), sensor_open.end(), k))
                trigger_enters.push_back({bodies[key_hi(k)].entity, bodies[key_lo(k)].entity});
        for (const u64 k : sensor_open)
            if (!std::binary_search(sensor_next.begin(), sensor_next.end(), k))
                trigger_exits.push_back({bodies[key_hi(k)].entity, bodies[key_lo(k)].entity});
        std::swap(sensor_open, sensor_next);
    }

    // Drops the warm-start cache entries of slot `s` (destroyed body, or a shape whose feature ids
    // no longer mean the same thing), so a reused slot never inherits stale impulses.
    void forget_contacts(u32 s) {
        usize w = 0;
        for (usize i = 0; i < contacts.size(); ++i)
            if (contacts[i].a != s && contacts[i].b != s) contacts[w++] = contacts[i];
        contacts.resize(w);
    }

    // Drops every open pair touching `s`, synthesizing exits/ends for the next step's events.
    void close_pairs(u32 s) {
        forget_contacts(s);
        usize w = 0;
        for (usize i = 0; i < sensor_open.size(); ++i) {
            const u64 k = sensor_open[i];
            if (key_hi(k) == s || key_lo(k) == s) {
                pending_trigger_exits.push_back({bodies[key_hi(k)].entity, bodies[key_lo(k)].entity});
            } else {
                sensor_open[w++] = k;
            }
        }
        sensor_open.resize(w);
        w = 0;
        for (usize i = 0; i < touching.size(); ++i) {
            const u64 k = touching[i];
            if (key_hi(k) == s || key_lo(k) == s) {
                pending_contact_ends.push_back({bodies[key_hi(k)].entity, bodies[key_lo(k)].entity});
            } else {
                touching[w++] = k;
            }
        }
        touching.resize(w);
    }

    // ---- controller -------------------------------------------------------------------------------
    struct SweepHit {
        u32 slot = kNoSlot;
        f32 t = 1.0f;
        vec2 normal;
        vec2 point;
    };

    bool solid_for_controller(const Body& self, const Body& o) const {
        if (&self == &o || !o.alive || !o.enabled || o.sensor) return false;
        if (!layers_collide(self.layer, self.mask, o.layer, o.mask)) return false;
        const QueryFilter& f = *move_filter;
        if (!((f.layer_mask >> o.layer) & 1u)) return false;
        for (const Entity& x : f.exclude)
            if (x == o.entity) return false;
        return !f.predicate || f.predicate(o.entity, f.user);
    }

    // Sweeps the controller shape from `pos` by `delta`, stopping `skin` short of the first solid.
    // Dynamic bodies block vertical sweeps (stand on crates) but not horizontal ones: walking
    // into a crate leaves it to the solver, which pushes it with the controller's velocity.
    SweepHit sweep(const Body& self, vec2 pos, vec2 delta, u32 ignore_slot = kNoSlot, bool dynamic_blocks = true) const {
        SweepHit best;
        if (math::length_sq(delta) <= 0.0f) return best;
        const Xf start{pos, self.xf.q};
        const math::aabb2 b0 = compute_aabb(self.shape, start);
        const math::aabb2 b1 = compute_aabb(self.shape, Xf{pos + delta, self.xf.q});
        const math::aabb2 swept = expand(math::merge(b0, b1), self.ctrl.skin + kLinearSlop);
        tree.query(swept, [&](u32 s) {
            const Body& o = bodies[s];
            if (s == ignore_slot || !solid_for_controller(self, o)) return true;
            if (!dynamic_blocks && o.type == BodyType::Dynamic) return true;
            if (o.one_way && !(delta.y < 0.0f)) return true;  // one-way: downward sweeps only
            const CastResult r = detail::shape_cast(self.shape, start, delta, o.shape, o.xf, self.ctrl.skin);
            if (r.kind != CastResult::Kind::Hit) return true;  // miss, or already overlapping
            if (o.one_way && r.normal.y <= 0.5f) return true;   // must land on its top face
            if (r.t < best.t || (r.t == best.t && s < best.slot)) {
                best.slot = s;
                best.t = r.t;
                best.normal = r.normal;
                best.point = r.point;
            }
            return true;
        });
        return best;
    }

    // Pushes the controller out of solids it starts inside (teleports, spawning in a wall).
    vec2 depenetrate(const Body& self, vec2 pos) const {
        for (u32 iter = 0; iter < 4; ++iter) {
            const Xf xf{pos, self.xf.q};
            const math::aabb2 box = compute_aabb(self.shape, xf);
            vec2 push{};
            f32 deepest = 0.0f;
            u32 deepest_slot = kNoSlot;
            tree.query(box, [&](u32 s) {
                const Body& o = bodies[s];
                if (!solid_for_controller(self, o) || o.type == BodyType::Dynamic) return true;
                Manifold m;
                bool swapped = false;
                collide_any(self.shape, xf, o.shape, o.xf, m, &swapped);
                if (m.count == 0) return true;
                WorldManifold wm;
                if (swapped) world_manifold(m, o.xf, o.shape.radius, xf, self.shape.radius, wm);
                else world_manifold(m, xf, self.shape.radius, o.xf, o.shape.radius, wm);
                const vec2 out_dir = swapped ? wm.normal : -wm.normal;  // from other toward self
                f32 sep = wm.separations[0];
                if (m.count == 2) sep = math::min(sep, wm.separations[1]);
                // One-way platforms only push up, and only out of shallow overlaps (a platform that
                // rose into the feet); anything deeper is a body passing through from below.
                if (o.one_way && (out_dir.y < 0.5f || -sep > kOneWayDepenetration)) return true;
                if (sep < deepest || (sep == deepest && s < deepest_slot && sep < 0.0f)) {
                    deepest = sep;
                    deepest_slot = s;
                    push = out_dir * (-sep + self.ctrl.skin);
                }
                return true;
            });
            if (deepest_slot == kNoSlot || deepest >= 0.0f) break;
            pos += push;
        }
        return pos;
    }

    MoveResult2D move(Body& b, vec2 delta) {
        Controller& ctl = b.ctrl;
        const MoveResult2D prev = ctl.last;
        const f32 dt = cfg.fixed_dt;
        MoveResult2D r;

        const vec2 start = b.xf.p;
        vec2 pos = depenetrate(b, start);
        const bool was_grounded = (prev.flags & ControllerFlags::Ground) != 0;

        // 1. horizontal (projected onto a walkable slope we stand on)
        vec2 h{delta.x, 0.0f};
        if (was_grounded && prev.ground_normal.y >= ctl.max_slope_cos && prev.ground_normal.y > 0.0f && h.x != 0.0f)
            h.y = -h.x * prev.ground_normal.x / prev.ground_normal.y;
        for (u32 iter = 0; iter < 3 && math::length_sq(h) > 0.0f; ++iter) {
            const SweepHit hit = sweep(b, pos, h, kNoSlot, false);
            pos += hit.t * h;
            if (hit.slot == kNoSlot) break;
            const vec2 rest = (1.0f - hit.t) * h;
            if (hit.normal.y >= ctl.max_slope_cos) {
                // Walkable slope ahead: continue along it.
                h = {rest.x, hit.normal.y > 0.0f ? -rest.x * hit.normal.x / hit.normal.y : 0.0f};
                continue;
            }
            r.flags |= hit.normal.x > 0.0f ? ControllerFlags::WallLeft : ControllerFlags::WallRight;
            break;
        }

        // 2. vertical
        vec2 v{0.0f, delta.y};
        for (u32 iter = 0; iter < 2 && math::length_sq(v) > 0.0f; ++iter) {
            const SweepHit hit = sweep(b, pos, v);
            pos += hit.t * v;
            if (hit.slot == kNoSlot) break;
            if (hit.normal.y >= ctl.max_slope_cos) {
                set_ground(r, hit);
                break;
            }
            if (hit.normal.y < 0.0f) {
                r.flags |= ControllerFlags::Ceiling;
                r.hit_ceiling = true;
                break;
            }
            // Too steep to stand on: slide down along it.
            const vec2 rest = (1.0f - hit.t) * v;
            v = rest - dot(rest, hit.normal) * hit.normal;
            r.flags |= hit.normal.x > 0.0f ? ControllerFlags::WallLeft : ControllerFlags::WallRight;
        }

        // 3. ground probe / snap-down (stairs, downslopes) when not moving up
        if ((r.flags & ControllerFlags::Ground) == 0 && delta.y <= 0.0f) {
            const f32 probe = was_grounded ? math::max(ctl.snap_down, 2.0f * ctl.skin) : 2.0f * ctl.skin;
            const vec2 down{0.0f, -probe};
            const SweepHit hit = sweep(b, pos, down);
            if (hit.slot != kNoSlot && hit.normal.y >= ctl.max_slope_cos) {
                pos += hit.t * down;
                set_ground(r, hit);
            }
        }

        // 4. moving-platform carry (velocity accumulation, never friction). The intent above was
        //    resolved against the platform's current pose; now ride along by what the platform
        //    will move this step, sweeping without it so a descending platform is followed exactly.
        if ((r.flags & ControllerFlags::OnMovingPlatform) != 0) {
            const u32 gs = slot(r.ground_body);
            const vec2 carry = dt * bodies[gs].v;
            const SweepHit hit = sweep(b, pos, carry, gs);
            pos += hit.t * carry;
        }

        r.position = pos;
        const vec2 moved = pos - start;
        b.v = dt > 0.0f ? moved / dt : vec2{};
        b.c = pos + mul(b.xf.q, b.local_center);
        b.xf.p = pos;
        refresh_proxy(b, moved, false);
        ctl.last = r;
        return r;
    }

    void set_ground(MoveResult2D& r, const SweepHit& hit) const {
        const Body& g = bodies[hit.slot];
        r.flags |= ControllerFlags::Ground;
        r.ground_normal = hit.normal;
        r.ground_body = g.entity;
        // Carry comes from the ground body's solver velocity: a kinematic platform, or a dynamic
        // body (a crate riding a platform) once it moves faster than solver noise.
        const f32 min_speed = g.type == BodyType::Dynamic ? kDynamicCarrySpeed : 0.0f;
        if (g.type != BodyType::Static && !g.is_controller && math::length_sq(g.v) > min_speed * min_speed) {
            r.flags |= ControllerFlags::OnMovingPlatform;
            r.platform_velocity = g.v;
        } else {
            r.platform_velocity = {};
        }
    }
};

// =====================================================================================================

World2D::World2D(const PhysicsConfig& config, mem::IAllocator& alloc)
    : impl_(mem::make_unique<Impl>(alloc, config, alloc)) {}
World2D::~World2D() = default;
World2D::World2D(World2D&&) noexcept = default;
World2D& World2D::operator=(World2D&&) noexcept = default;

const PhysicsConfig& World2D::config() const { return impl_->cfg; }

void World2D::set_gravity(vec2 gravity) { impl_->cfg.gravity = gravity; }

bool World2D::create_body(Entity e, const BodyDesc& d) {
    Impl& w = *impl_;
    if (e.is_null() || w.slot(e) != kNoSlot) return false;
    Body b;
    if (!w.apply_collider(b, d.collider)) return false;
    b.entity = e;
    b.alive = true;
    b.is_controller = d.controller != nullptr;
    if (d.controller) Impl::apply_controller(b, *d.controller);
    b.a = d.angle;
    b.xf = {d.position, Rot::of(d.angle)};
    w.apply_body_config(b, d.body);
    Impl::sync_xf(b);

    u32 s;
    if (!w.free_slots.empty()) {
        s = w.free_slots.back();
        w.free_slots.pop_back();
        w.bodies[s] = b;
    } else {
        s = static_cast<u32>(w.bodies.size());
        w.bodies.push_back(b);
    }
    Body& nb = w.bodies[s];
    nb.proxy = w.tree.create_proxy(w.fat_box(nb, {}), s);
    if (e.index >= w.slot_of.size()) w.slot_of.resize(e.index + 1, kNoSlot);
    w.slot_of[e.index] = s;
    return true;
}

void World2D::destroy_body(Entity e) {
    Impl& w = *impl_;
    const u32 s = w.slot(e);
    if (s == kNoSlot) return;
    w.wake_touching(s);
    for (u32 i = 0; i < w.joints.size(); ++i)
        if (w.joints[i].alive && (w.joints[i].a == s || w.joints[i].b == s)) w.remove_joint(i);
    w.close_pairs(s);
    Body& b = w.bodies[s];
    w.tree.destroy_proxy(b.proxy);
    b = Body{};
    w.slot_of[e.index] = kNoSlot;
    w.free_slots.push_back(s);
}

bool World2D::has_body(Entity e) const { return impl_->slot(e) != kNoSlot; }

bool World2D::set_body_config(Entity e, const RigidBody2D& rb) {
    Body* b = impl_->get(e);
    if (!b) return false;
    impl_->apply_body_config(*b, rb);
    Impl::sync_xf(*b);
    Impl::wake(*b);
    impl_->wake_touching(impl_->slot(e));
    return true;
}

bool World2D::set_collider(Entity e, const Collider2D& c) {
    Body* b = impl_->get(e);
    if (!b) return false;
    const vec2 origin = b->xf.p;
    if (!impl_->apply_collider(*b, c)) return false;
    impl_->forget_contacts(impl_->slot(e));
    b->xf.p = origin;
    impl_->update_mass(*b);  // recomputes the centre of mass from the unchanged origin
    impl_->refresh_proxy(*b, {}, true);
    Impl::wake(*b);
    impl_->wake_touching(impl_->slot(e));
    return true;
}

bool World2D::set_controller(Entity e, const CharacterController2D& c) {
    Body* b = impl_->get(e);
    if (!b || !b->is_controller) return false;
    Impl::apply_controller(*b, c);
    return true;
}

void World2D::step(f32 dt) { impl_->step(dt); }

void World2D::set_linear_velocity(Entity e, vec2 v) {
    if (Body* b = impl_->get(e); b && b->type != BodyType::Static) {
        b->v = v;
        Impl::wake(*b);
    }
}
void World2D::set_angular_velocity(Entity e, f32 w) {
    if (Body* b = impl_->get(e); b && b->type != BodyType::Static && !b->fixed_rotation) {
        b->w = w;
        Impl::wake(*b);
    }
}
void World2D::apply_impulse(Entity e, vec2 impulse, vec2 world_point) {
    Body* b = impl_->get(e);
    if (!b || b->type != BodyType::Dynamic) return;
    Impl::wake(*b);
    b->v += b->inv_mass * impulse;
    b->w += b->inv_i * cross(world_point - b->c, impulse);
}
void World2D::apply_force(Entity e, vec2 force) {
    if (Body* b = impl_->get(e); b && b->type == BodyType::Dynamic) {
        b->force += force;
        Impl::wake(*b);
    }
}
void World2D::teleport(Entity e, vec2 position, f32 angle) {
    Body* b = impl_->get(e);
    if (!b) return;
    b->a = angle;
    b->xf = {position, Rot::of(angle)};
    b->c = mul(b->xf, b->local_center);
    b->ctrl.last.flags = 0;  // controller history no longer applies
    b->ctrl.last.ground_body = Entity::null;
    impl_->refresh_proxy(*b, {}, true);
    Impl::wake(*b);
    impl_->wake_touching(impl_->slot(e));
}
void World2D::set_enabled(Entity e, bool active) {
    if (Body* b = impl_->get(e)) {
        if (b->enabled != active) impl_->wake_touching(impl_->slot(e));
        b->enabled = active;
        Impl::wake(*b);
    }
}

bool World2D::is_awake(Entity e) const {
    const Body* b = impl_->get(e);
    return b && b->awake;
}

void World2D::set_awake(Entity e, bool awake) {
    Impl& w = *impl_;
    Body* b = w.get(e);
    if (!b || b->type != BodyType::Dynamic) return;
    if (awake) {
        Impl::wake(*b);
        return;
    }
    if (!w.cfg.enable_sleep) return;
    // Sleep the whole island (contact/joint-linked dynamic bodies) as of the last step.
    const u32 n = static_cast<u32>(w.bodies.size());
    w.island_parent.resize(n);
    for (u32 i = 0; i < n; ++i) w.island_parent[i] = i;
    auto dynamic = [&](u32 s) { return w.bodies[s].type == BodyType::Dynamic; };
    for (const Contact& c : w.contacts)
        if (dynamic(c.a) && dynamic(c.b)) w.link(c.a, c.b);
    for (const Joint& j : w.joints)
        if (j.alive && dynamic(j.a) && dynamic(j.b)) w.link(j.a, j.b);
    const u32 root = w.find_root(w.slot(e));
    for (u32 i = 0; i < n; ++i) {
        Body& o = w.bodies[i];
        if (!o.alive || o.type != BodyType::Dynamic || w.find_root(i) != root) continue;
        o.awake = false;
        o.sleep_time = w.cfg.time_to_sleep;
        o.v = {};
        o.w = 0.0f;
    }
}

// ---- joints ---------------------------------------------------------------------------------------

namespace {
bool joint_bodies_ok(const Body* a, const Body* b) {
    return a && b && a != b && (a->type == BodyType::Dynamic || b->type == BodyType::Dynamic);
}
}  // namespace

JointId World2D::create_distance_joint(const DistanceJointDesc& d) {
    Impl& w = *impl_;
    const Body* A = w.get(d.a);
    const Body* B = w.get(d.b);
    if (!joint_bodies_ok(A, B)) return {};
    Joint j;
    j.kind = Joint::Kind::Distance;
    j.a = w.slot(d.a);
    j.b = w.slot(d.b);
    j.local_a = d.local_anchor_a;
    j.local_b = d.local_anchor_b;
    j.collide_connected = d.collide_connected;
    const f32 current = math::length(mul(B->xf, d.local_anchor_b) - mul(A->xf, d.local_anchor_a));
    j.length = d.length >= 0.0f ? d.length : current;
    j.length = math::max(j.length, kLinearSlop);
    j.hertz = math::max(d.hertz, 0.0f);
    j.damping_ratio = math::max(d.damping_ratio, 0.0f);
    // A spring with no explicit range is free to stretch: [0, inf) around its rest length.
    const bool spring = j.hertz > 0.0f;
    j.min_length = d.min_length >= 0.0f ? d.min_length : (spring ? 0.0f : j.length);
    j.max_length = d.max_length >= 0.0f ? d.max_length : (spring ? 3.4e38f : j.length);
    j.min_length = math::clamp(j.min_length, kLinearSlop, j.length);
    j.max_length = math::max(j.max_length, j.length);
    return w.add_joint(j);
}

JointId World2D::create_revolute_joint(const RevoluteJointDesc& d) {
    Impl& w = *impl_;
    const Body* A = w.get(d.a);
    const Body* B = w.get(d.b);
    if (!joint_bodies_ok(A, B)) return {};
    Joint j;
    j.kind = Joint::Kind::Revolute;
    j.a = w.slot(d.a);
    j.b = w.slot(d.b);
    j.local_a = mul_t(A->xf, d.world_anchor);
    j.local_b = mul_t(B->xf, d.world_anchor);
    j.reference_angle = B->a - A->a;
    j.collide_connected = d.collide_connected;
    j.enable_limit = d.enable_limit;
    j.lower = math::min(d.lower_angle, d.upper_angle);
    j.upper = math::max(d.lower_angle, d.upper_angle);
    j.enable_motor = d.enable_motor;
    j.motor_speed = d.motor_speed;
    j.max_motor_torque = math::max(d.max_motor_torque, 0.0f);
    return w.add_joint(j);
}

void World2D::destroy_joint(JointId id) {
    if (impl_->joint(id)) impl_->remove_joint(id.index);
}

bool World2D::joint_alive(JointId id) const { return impl_->joint(id) != nullptr; }

f32 World2D::joint_length(JointId id) const {
    const Joint* j = impl_->joint(id);
    if (!j || j->kind != Joint::Kind::Distance) return 0.0f;
    const Body& A = impl_->bodies[j->a];
    const Body& B = impl_->bodies[j->b];
    return math::length(mul(B.xf, j->local_b) - mul(A.xf, j->local_a));
}

f32 World2D::joint_angle(JointId id) const {
    const Joint* j = impl_->joint(id);
    if (!j || j->kind != Joint::Kind::Revolute) return 0.0f;
    return impl_->bodies[j->b].a - impl_->bodies[j->a].a - j->reference_angle;
}

void World2D::set_motor_speed(JointId id, f32 speed) {
    if (Joint* j = impl_->joint(id); j && j->kind == Joint::Kind::Revolute) {
        j->motor_speed = speed;
        Impl::wake(impl_->bodies[j->a]);
        Impl::wake(impl_->bodies[j->b]);
    }
}

void World2D::set_joint_limits(JointId id, f32 lower, f32 upper) {
    Joint* j = impl_->joint(id);
    if (!j) return;
    if (j->kind == Joint::Kind::Revolute) {
        j->lower = math::min(lower, upper);
        j->upper = math::max(lower, upper);
    } else {
        j->min_length = math::clamp(math::min(lower, upper), kLinearSlop, j->length);
        j->max_length = math::max(math::max(lower, upper), j->length);
    }
    Impl::wake(impl_->bodies[j->a]);
    Impl::wake(impl_->bodies[j->b]);
}

bool World2D::body_state(Entity e, BodyState& out) const {
    const u32 s = impl_->slot(e);
    return s != kNoSlot && body_at(s, out);
}
vec2 World2D::linear_velocity(Entity e) const {
    const Body* b = impl_->get(e);
    return b ? b->v : vec2{};
}

u32 World2D::slot_count() const { return static_cast<u32>(impl_->bodies.size()); }
bool World2D::body_at(u32 s, BodyState& out) const {
    const Body& b = impl_->bodies[s];
    if (!b.alive) return false;
    out.entity = b.entity;
    out.type = b.type;
    out.position = b.xf.p;
    out.angle = b.a;
    out.linear_velocity = b.v;
    out.angular_velocity = b.w;
    out.enabled = b.enabled;
    out.controller = b.is_controller;
    out.awake = b.awake;
    return true;
}

MoveResult2D World2D::move_and_collide(Entity e, vec2 delta, const QueryFilter& filter) {
    Body* b = impl_->get(e);
    if (!b) return {};
    if (!b->enabled) {
        MoveResult2D r;
        r.position = b->xf.p;
        return r;
    }
    if (!std::isfinite(delta.x) || !std::isfinite(delta.y)) delta = {};
    impl_->move_filter = &filter;
    const MoveResult2D r = impl_->move(*b, delta);
    impl_->move_filter = nullptr;
    return r;
}

bool World2D::controller_state(Entity e, MoveResult2D& out) const {
    const Body* b = impl_->get(e);
    if (!b || !b->is_controller) return false;
    out = b->ctrl.last;
    return true;
}

// ---- queries --------------------------------------------------------------------------------------

namespace {
bool hit_less(const RayHit2D& a, u32 sa, const RayHit2D& b, u32 sb) { return a.t < b.t || (a.t == b.t && sa < sb); }
}  // namespace

void World2D::raycast_all(vec2 origin, vec2 dir, f32 max_dist, Array<RayHit2D>& out, const QueryFilter& f) const {
    out.clear();
    const Impl& w = *impl_;
    const vec2 d = math::normalize(dir);
    if (!(max_dist > 0.0f) || math::length_sq(d) <= 0.0f) return;
    const vec2 p2 = origin + max_dist * d;
    SmallVector<u32, 32> slots;
    w.tree.raycast(origin, p2, 1.0f, [&](u32 s, f32 max_fraction) {
        const Body& b = w.bodies[s];
        if (!w.passes(b, f)) return max_fraction;
        f32 fraction = 0.0f;
        vec2 normal;
        if (!detail::raycast(b.shape, b.xf, origin, p2, fraction, normal)) return max_fraction;
        RayHit2D h;
        h.entity = b.entity;
        h.t = fraction * max_dist;
        h.point = origin + h.t * d;
        h.normal = normal;
        out.push_back(h);
        slots.push_back(s);
        return max_fraction;
    });
    // Sort by (t, slot) — insertion sort keeps it allocation-free and stable.
    for (usize i = 1; i < out.size(); ++i) {
        const RayHit2D h = out[i];
        const u32 s = slots[i];
        usize j = i;
        while (j > 0 && hit_less(h, s, out[j - 1], slots[j - 1])) {
            out[j] = out[j - 1];
            slots[j] = slots[j - 1];
            --j;
        }
        out[j] = h;
        slots[j] = s;
    }
}

std::optional<RayHit2D> World2D::raycast(vec2 origin, vec2 dir, f32 max_dist, const QueryFilter& f) const {
    const Impl& w = *impl_;
    const vec2 d = math::normalize(dir);
    if (!(max_dist > 0.0f) || math::length_sq(d) <= 0.0f) return std::nullopt;
    const vec2 p2 = origin + max_dist * d;
    RayHit2D best;
    u32 best_slot = kNoSlot;
    w.tree.raycast(origin, p2, 1.0f, [&](u32 s, f32 max_fraction) {
        const Body& b = w.bodies[s];
        if (!w.passes(b, f)) return max_fraction;
        f32 fraction = 0.0f;
        vec2 normal;
        if (!detail::raycast(b.shape, b.xf, origin, p2, fraction, normal)) return max_fraction;
        RayHit2D h;
        h.entity = b.entity;
        h.t = fraction * max_dist;
        h.point = origin + h.t * d;
        h.normal = normal;
        if (best_slot == kNoSlot || hit_less(h, s, best, best_slot)) {
            best = h;
            best_slot = s;
        }
        return max_fraction;
    });
    if (best_slot == kNoSlot) return std::nullopt;
    return best;
}

std::optional<RayHit2D> World2D::shape_cast(const ColliderShape& shape, vec2 from, vec2 to, const QueryFilter& f) const {
    const Impl& w = *impl_;
    Shape s;
    if (!build_shape(shape, s)) return std::nullopt;
    const Xf start{from, Rot{}};
    const vec2 delta = to - from;
    const math::aabb2 swept =
        expand(math::merge(compute_aabb(s, start), compute_aabb(s, Xf{to, Rot{}})), kLinearSlop);
    RayHit2D best;
    u32 best_slot = kNoSlot;
    w.tree.query(swept, [&](u32 slot) {
        const Body& b = w.bodies[slot];
        if (!w.passes(b, f)) return true;
        const CastResult r = detail::shape_cast(s, start, delta, b.shape, b.xf, kLinearSlop);
        if (r.kind == CastResult::Kind::Miss) return true;
        RayHit2D h;
        h.entity = b.entity;
        if (r.kind == CastResult::Kind::Overlap) {
            h.t = 0.0f;
            h.point = from;
            h.normal = -math::normalize(delta);
        } else {
            h.t = r.t;
            h.point = r.point;
            h.normal = r.normal;
        }
        if (best_slot == kNoSlot || hit_less(h, slot, best, best_slot)) {
            best = h;
            best_slot = slot;
        }
        return true;
    });
    if (best_slot == kNoSlot) return std::nullopt;
    return best;
}

bool World2D::overlap_point(vec2 p, const QueryFilter& f) const {
    const Impl& w = *impl_;
    bool hit = false;
    w.tree.query(math::aabb2{p, p}, [&](u32 s) {
        const Body& b = w.bodies[s];
        if (w.passes(b, f) && test_point(b.shape, b.xf, p)) {
            hit = true;
            return false;
        }
        return true;
    });
    return hit;
}

u32 World2D::overlap_aabb(math::aabb2 box, Array<Entity>& out, const QueryFilter& f) const {
    out.clear();
    const Impl& w = *impl_;
    SmallVector<u32, 32> slots;
    w.tree.query(box, [&](u32 s) {
        const Body& b = w.bodies[s];
        if (w.passes(b, f) && compute_aabb(b.shape, b.xf).overlaps(box)) slots.push_back(s);
        return true;
    });
    std::sort(slots.begin(), slots.end());
    for (const u32 s : slots) out.push_back(w.bodies[s].entity);
    return static_cast<u32>(out.size());
}

Span<const TriggerEnter> World2D::trigger_enters() const { return impl_->trigger_enters.span(); }
Span<const TriggerExit> World2D::trigger_exits() const { return impl_->trigger_exits.span(); }
Span<const ContactBegin2D> World2D::contact_begins() const { return impl_->contact_begins.span(); }
Span<const ContactEnd2D> World2D::contact_ends() const { return impl_->contact_ends.span(); }

// ---- tools ------------------------------------------------------------------------------------------

void World2D::debug_draw(DebugDrawSink& sink, u32 flags) const {
    const Impl& w = *impl_;
    const math::Color kStatic{0.5f, 0.9f, 0.5f, 1.0f};
    const math::Color kKinematic{0.5f, 0.5f, 0.9f, 1.0f};
    const math::Color kDynamic{0.9f, 0.7f, 0.7f, 1.0f};
    const math::Color kSensor{0.9f, 0.9f, 0.3f, 1.0f};
    const math::Color kDisabled{0.4f, 0.4f, 0.4f, 1.0f};
    for (const Body& b : w.bodies) {
        if (!b.alive) continue;
        if (flags & DebugDrawFlags::Shapes) {
            math::Color c = b.type == BodyType::Static ? kStatic : b.type == BodyType::Kinematic ? kKinematic : kDynamic;
            if (b.sensor) c = kSensor;
            if (!b.enabled) c = kDisabled;
            if (b.shape.is_circle()) {
                const vec2 center = mul(b.xf, b.shape.v[0]);
                sink.circle(center, b.shape.radius, c);
                sink.line(center, center + b.shape.radius * mul(b.xf.q, vec2{1.0f, 0.0f}), c);
            } else {
                for (u32 i = 0; i < b.shape.count; ++i) {
                    const u32 i2 = i + 1 < b.shape.count ? i + 1 : 0;
                    sink.line(mul(b.xf, b.shape.v[i]), mul(b.xf, b.shape.v[i2]), c);
                }
            }
        }
        if (flags & DebugDrawFlags::Aabbs) {
            const math::aabb2 a = compute_aabb(b.shape, b.xf);
            const math::Color c{0.9f, 0.3f, 0.9f, 1.0f};
            sink.line(a.min, {a.max.x, a.min.y}, c);
            sink.line({a.max.x, a.min.y}, a.max, c);
            sink.line(a.max, {a.min.x, a.max.y}, c);
            sink.line({a.min.x, a.max.y}, a.min, c);
        }
    }
    if (flags & DebugDrawFlags::Broadphase) {
        w.tree.each_node([&](const math::aabb2& a, bool leaf) {
            const math::Color c = leaf ? math::Color{0.3f, 0.6f, 0.9f, 1.0f} : math::Color{0.3f, 0.3f, 0.6f, 1.0f};
            sink.line(a.min, {a.max.x, a.min.y}, c);
            sink.line({a.max.x, a.min.y}, a.max, c);
            sink.line(a.max, {a.min.x, a.max.y}, c);
            sink.line({a.min.x, a.max.y}, a.min, c);
        });
    }
    if (flags & DebugDrawFlags::Contacts) {
        const math::Color c{1.0f, 0.2f, 0.2f, 1.0f};
        for (const Contact& ct : w.contacts) {
            sink.circle(ct.point, 0.05f, c);
            sink.line(ct.point, ct.point + 0.3f * ct.normal, c);
        }
    }
}

u64 World2D::hash_state(u64 seed) const {
    const Impl& w = *impl_;
    Hasher64 h(seed);
    h.u32v(static_cast<u32>(w.bodies.size()));
    for (const Body& b : w.bodies) {
        h.u32v(b.alive ? 1u : 0u);
        if (!b.alive) continue;
        h.u32v(b.entity.index);
        h.u32v(b.entity.generation);
        h.u32v(static_cast<u32>(b.type) | (b.enabled ? 0x100u : 0u) | (b.is_controller ? 0x200u : 0u));
        h.f32v(b.xf.p.x);
        h.f32v(b.xf.p.y);
        h.f32v(b.a);
        h.f32v(b.v.x);
        h.f32v(b.v.y);
        h.f32v(b.w);
        if (b.is_controller) {
            h.u32v(b.ctrl.last.flags);
            h.f32v(b.ctrl.last.position.x);
            h.f32v(b.ctrl.last.position.y);
        }
    }
    h.u32v(static_cast<u32>(w.touching.size()));
    for (const u64 k : w.touching) h.u64v(k);
    h.u32v(static_cast<u32>(w.sensor_open.size()));
    for (const u64 k : w.sensor_open) h.u64v(k);
    // v0.5 state joins the hash only when used, so M3/M3.5 hashes are unchanged.
    if (w.cfg.enable_sleep)
        for (const Body& b : w.bodies) h.u32v(b.alive && b.awake ? 1u : 0u);
    if (w.live_joints > 0) {
        h.u32v(w.live_joints);
        for (const Joint& j : w.joints) {
            if (!j.alive) continue;
            h.u32v(j.a);
            h.u32v(j.b);
            h.u32v(static_cast<u32>(j.kind));
        }
    }
    return h.value();
}

World2D::Stats World2D::stats() const {
    const Impl& w = *impl_;
    Stats s;
    for (const Body& b : w.bodies) s.bodies += b.alive ? 1u : 0u;
    s.contacts = static_cast<u32>(w.contacts.size());
    s.islands = w.island_count;
    s.sensor_pairs = static_cast<u32>(w.sensor_open.size());
    s.proxies = w.tree.proxy_count();
    s.tree_height = w.tree.height();
    s.joints = w.live_joints;
    for (const Body& b : w.bodies) s.sleeping += b.alive && b.type == BodyType::Dynamic && !b.awake ? 1u : 0u;
    return s;
}

bool World2D::validate_broadphase() const { return impl_->tree.validate(); }

}  // namespace mce::physics

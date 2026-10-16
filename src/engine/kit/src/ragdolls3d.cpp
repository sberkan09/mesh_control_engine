#include "mce/kit/ragdolls3d.h"

#include <cmath>

#include "kit3d_internal.h"
#include "mce/asset/formats.h"
#include "mce/containers/array.h"
#include "mce/containers/hash_map.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/kit3d.h"
#include "mce/log.h"
#include "mce/render/mesh.h"

namespace mce::kit {

// Padding-free: World::state_hash hashes component bytes.
static_assert(sizeof(Ragdoll3D) == 116);

namespace {

using math::mat4;
using math::quat;
using math::vec3;

constexpr f32 kDeg = 0.017453292519943295f;
constexpr f32 kPi = 3.14159265358979f;

// A built ragdoll and how its bodies pose the mesh's nodes.
struct Live {
    u32 id = 0;  // the World3D ragdoll
    Array<i32> node_part;  // per node: the part whose body this bone is, -1 when it rides its parent
    Array<mat4> offset;    // per part: its bone's world matrix in the body's frame, at activation
    Array<mat4> local;     // per node: in its parent's space, at activation (the riders)
    Array<mat4> model;     // per node: model space, at activation (roots that ride nothing)
};

struct Ragdolls3DState {
    HashMap<Entity, Live> live;
    HashMap<Entity, u8> failed;  // warned once; retried once `active` is cleared and set again
};

mat4 world_matrix(World& w, Entity e) {
    const LocalTransform* t = w.try_get<LocalTransform>(e);
    if (!t) return mat4::identity();
    vec3 pos, scale;
    quat rot;
    world_pose(w, e, *t, pos, rot, scale);
    return math::trs(pos, rot, scale);
}

vec3 origin(const mat4& m) { return m.col(3).xyz(); }

quat rotation_of(const mat4& m) {
    return quat_from_basis(math::normalize(m.col(0).xyz()), math::normalize(m.col(1).xyz()), math::normalize(m.col(2).xyz()));
}

// The shortest rotation taking unit vector a to unit vector b.
quat from_to(vec3 a, vec3 b) {
    const f32 d = math::dot(a, b);
    if (d < -0.9999f) {
        const vec3 axis = std::fabs(a.x) < 0.9f ? math::cross(a, vec3{1, 0, 0}) : math::cross(a, vec3{0, 1, 0});
        return math::quat_from_axis_angle(math::normalize(axis), kPi);
    }
    const vec3 c = math::cross(a, b);
    return math::normalize(quat{c.x, c.y, c.z, 1.0f + d});
}

vec3 abs3(vec3 v) { return {std::fabs(v.x), std::fabs(v.y), std::fabs(v.z)}; }

f32 volume(const physics::RagdollPart3D& p) {
    switch (p.shape) {
    case physics::ShapeType3D::Sphere: return 4.0f / 3.0f * kPi * p.radius * p.radius * p.radius;
    case physics::ShapeType3D::Capsule:
        return kPi * p.radius * p.radius * (2.0f * p.half_height) + 4.0f / 3.0f * kPi * p.radius * p.radius * p.radius;
    default: return 8.0f * p.half.x * p.half.y * p.half.z;
    }
}

// The rotation that has turned `from` into `to`, as an angular velocity over dt.
vec3 angular_velocity(quat from, quat to, f32 dt) {
    quat d = math::normalize(to * math::conjugate(from));
    if (d.w < 0.0f) d = {-d.x, -d.y, -d.z, -d.w};
    const f32 s = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (s < 1e-6f) return {};
    const f32 angle = 2.0f * std::atan2(s, d.w);
    return vec3{d.x, d.y, d.z} * (angle / (s * dt));
}

// Builds the ragdoll of `e` from skin 0 of `mesh`, posed as it is drawn now. False when the
// skeleton gives no parts or the physics refuses them.
bool build(World& w, Physics3D& phys, Entity e, const Ragdoll3D& r, const render::GpuMesh& mesh, Live& out) {
    if (mesh.skins.empty() || mesh.nodes.empty()) return false;
    const render::GpuSkin& skin = mesh.skins[0];
    const usize nodes = mesh.nodes.size();
    const f32 dt = phys.world.config().fixed_dt;

    // The pose now and one step earlier (for the parts' velocities).
    const Animator* a = w.try_get<Animator>(e);
    Array<mat4> now, before;
    now.resize(nodes);
    before.resize(nodes);
    animated_pose(w, e, mesh, a, now.span());
    if (a) {
        Animator back = *a;
        back.time = math::max(0.0f, back.time - dt * back.speed);
        back.previous_time = math::max(0.0f, back.previous_time - dt * back.speed);
        animated_pose(w, e, mesh, &back, before.span());
    } else {
        before = now;
    }
    const mat4 entity = world_matrix(w, e);
    vec3 carried;  // the entity's own body's velocity
    physics::BodyState3D body;
    if (phys.world.body_state(e, body)) carried = body.linear_velocity;

    Array<i32> joint_of;
    joint_of.resize(nodes, -1);
    for (u32 j = 0; j < skin.joint_count; ++j) {
        const u32 n = mesh.joints[skin.first_joint + j].node;
        if (n < nodes) joint_of[n] = static_cast<i32>(skin.first_joint + j);
    }
    Array<mat4> world;  // node world matrices now
    world.resize(nodes);
    for (usize n = 0; n < nodes; ++n) world[n] = entity * now[n];

    out = Live{};
    out.node_part.resize(nodes, -1);
    out.local.resize(nodes);
    out.model = now;
    for (usize n = 0; n < nodes; ++n) {
        const i32 parent = mesh.nodes[n].parent;
        out.local[n] = parent >= 0 ? math::inverse(now[static_cast<usize>(parent)]) * now[n] : now[n];
    }

    Array<physics::RagdollPart3D> parts;
    Array<f32> weights;
    Array<u32> bone_of;  // per part: its node
    for (usize n = 0; n < nodes; ++n) {
        if (joint_of[n] < 0) continue;
        const render::GpuJoint& joint = mesh.joints[static_cast<usize>(joint_of[n])];
        const u32 flags = joint.ragdoll_flags;
        if (flags & asset::MeshRagdoll::Exclude) continue;
        // The nearest ancestor with a part is the parent; a second tree rides along unsimulated.
        i32 parent_part = -1;
        for (i32 p = mesh.nodes[n].parent; p >= 0; p = mesh.nodes[static_cast<usize>(p)].parent)
            if (out.node_part[static_cast<usize>(p)] >= 0) {
                parent_part = out.node_part[static_cast<usize>(p)];
                break;
            }
        if (parent_part < 0 && !parts.empty()) continue;

        const vec3 o = origin(world[n]);
        const quat rot = rotation_of(world[n]);
        // The child bones' origins, in the bone's frame (a joint that moves no vertex, a socket,
        // is not a child bone unless the sidecar includes it).
        vec3 child_sum;
        u32 children = 0;
        for (usize c = n + 1; c < nodes; ++c) {
            if (mesh.nodes[c].parent != static_cast<i32>(n) || joint_of[c] < 0) continue;
            const render::GpuJoint& cj = mesh.joints[static_cast<usize>(joint_of[c])];
            if (cj.influences || (cj.ragdoll_flags & asset::MeshRagdoll::Include)) {
                child_sum = child_sum + math::rotate(math::conjugate(rot), origin(world[c]) - o);
                ++children;
            }
        }
        // The vertices the bone moves, as a box in its frame.
        vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
        if (joint.influences) {
            const mat4 to_world = world[n] * joint.inverse_bind;
            for (u32 k = 0; k < 8; ++k) {
                const vec3 b{(k & 1) ? joint.bounds.max.x : joint.bounds.min.x, (k & 2) ? joint.bounds.max.y : joint.bounds.min.y,
                             (k & 4) ? joint.bounds.max.z : joint.bounds.min.z};
                const vec3 l = math::rotate(math::conjugate(rot), math::transform_point(to_world, b) - o);
                lo = math::min(lo, l);
                hi = math::max(hi, l);
            }
        }
        const vec3 half = joint.influences ? (hi - lo) * 0.5f : vec3{};
        const vec3 mid = joint.influences ? (hi + lo) * 0.5f : vec3{};

        physics::RagdollPart3D part;
        part.parent = parent_part;
        vec3 centre;  // bone frame
        quat turn = quat::identity();
        vec3 twist_dir;  // bone frame
        f32 length = 0.0f;
        const bool radius_set = (flags & asset::MeshRagdoll::Radius) != 0;
        if (children == 1 || (children > 1 && !joint.influences)) {
            // A capsule from the bone to its child (or the middle of its children).
            const vec3 d = child_sum * (1.0f / static_cast<f32>(children));
            length = math::length(d);
            const vec3 dir = length > 1e-5f ? d * (1.0f / length) : vec3{0, 1, 0};
            const vec3 across = vec3{1, 1, 1} - abs3(dir);
            f32 radius = joint.influences ? math::dot(half, across) * 0.5f : 0.2f * length;
            radius = radius_set ? joint.ragdoll_radius : radius * r.radius_scale;
            part.shape = physics::ShapeType3D::Capsule;
            part.radius = math::max(radius, 0.01f);
            part.half_height = math::max(0.5f * length - part.radius, 0.01f);
            centre = d * 0.5f;
            turn = from_to(vec3{0, 1, 0}, dir);
            twist_dir = dir;
        } else if (children > 1) {
            // A box over the vertices it moves (a pelvis, a chest), or a ball when a radius is set.
            centre = mid;
            length = 2.0f * math::max(half.x, math::max(half.y, half.z));
            if (radius_set) {
                part.shape = physics::ShapeType3D::Sphere;
                part.radius = math::max(joint.ragdoll_radius, 0.01f);
            } else {
                part.shape = physics::ShapeType3D::Box;
                part.half = math::max(half * r.radius_scale, vec3{0.01f, 0.01f, 0.01f});
            }
        } else if (joint.influences) {
            // A leaf (a hand, the head): a capsule along the longest side of its vertices.
            const u32 k = half.x >= half.y && half.x >= half.z ? 0u : (half.y >= half.z ? 1u : 2u);
            const f32 h[3] = {half.x, half.y, half.z};
            const f32 longest = h[k];
            length = 2.0f * longest;
            const f32 radius = radius_set ? joint.ragdoll_radius : (h[(k + 1) % 3] + h[(k + 2) % 3]) * 0.5f * r.radius_scale;
            centre = mid;
            vec3 axis;
            (k == 0 ? axis.x : k == 1 ? axis.y : axis.z) = 1.0f;
            if (longest - radius > 0.01f) {
                part.shape = physics::ShapeType3D::Capsule;
                part.radius = math::max(radius, 0.01f);
                part.half_height = longest - part.radius;
                turn = from_to(vec3{0, 1, 0}, axis);
            } else {
                part.shape = physics::ShapeType3D::Sphere;
                part.radius = math::max(math::max(radius, longest), 0.01f);
            }
        } else {
            part.shape = physics::ShapeType3D::Sphere;
            part.radius = radius_set ? math::max(joint.ragdoll_radius, 0.01f) : 0.05f;
        }
        const bool include = (flags & asset::MeshRagdoll::Include) != 0;
        if (!include && (!joint.influences || length < r.min_bone_length)) continue;

        part.position = o + math::rotate(rot, centre);
        part.rotation = math::normalize(rot * turn);
        part.pivot = o;
        if (parent_part >= 0 && math::length_sq(twist_dir) == 0.0f) {
            const vec3 from_parent = o - origin(world[bone_of[static_cast<usize>(parent_part)]]);
            twist_dir = math::length_sq(from_parent) > 1e-10f ? math::rotate(math::conjugate(rot), math::normalize(from_parent))
                                                               : vec3{0, 1, 0};
        }
        part.twist_axis = math::rotate(rot, math::length_sq(twist_dir) > 0.0f ? twist_dir : vec3{0, 1, 0});
        part.swing = ((flags & asset::MeshRagdoll::Swing) ? joint.ragdoll_swing : r.swing) * kDeg;
        part.twist = ((flags & asset::MeshRagdoll::Twist) ? joint.ragdoll_twist : r.twist) * kDeg;
        // Moving as the animation moved it over the last step, plus the entity's own velocity.
        const mat4 step = entity * before[n] * math::inverse(world[n]);
        const vec3 was = math::transform_point(step, part.position);
        part.linear_velocity = (part.position - was) * (1.0f / dt) + carried;
        part.angular_velocity = angular_velocity(rotation_of(step) * part.rotation, part.rotation, dt);
        weights.push_back((flags & asset::MeshRagdoll::Mass) ? joint.ragdoll_mass : 1.0f);
        out.node_part[n] = static_cast<i32>(parts.size());
        bone_of.push_back(static_cast<u32>(n));
        parts.push_back(part);
    }
    if (parts.empty()) return false;

    // The mass, shared by size and the sidecar's weights.
    f32 total = 0.0f;
    for (usize i = 0; i < parts.size(); ++i) total += volume(parts[i]) * weights[i];
    for (usize i = 0; i < parts.size(); ++i)
        parts[i].mass = total > 0.0f ? math::max(r.mass * volume(parts[i]) * weights[i] / total, 0.01f) : r.mass / static_cast<f32>(parts.size());

    physics::RagdollDesc3D desc;
    desc.owner = e;
    desc.layer = r.layer;
    desc.mask = r.mask;
    desc.friction = r.friction;
    desc.angular_damping = r.damping;
    desc.joint_friction = r.joint_friction;
    out.id = phys.world.create_ragdoll(desc, parts.span());
    if (out.id == 0) return false;
    out.offset.resize(parts.size());
    for (usize i = 0; i < parts.size(); ++i)
        out.offset[i] = math::inverse(math::trs(parts[i].position, parts[i].rotation, {1, 1, 1})) * world[bone_of[i]];
    return true;
}

// The mesh's nodes in model space from the bodies' poses now.
void pose(World& w, Physics3D& phys, Entity e, const Live& live, Array<mat4>& nodes, const render::GpuMesh& mesh, vec3& root) {
    Array<physics::BodyState3D> states;
    states.resize(live.offset.size());
    phys.world.ragdoll_state(live.id, states.span());
    const mat4 to_model = math::inverse(world_matrix(w, e));
    nodes.resize(live.node_part.size());
    for (usize n = 0; n < nodes.size(); ++n) {
        const i32 p = live.node_part[n];
        const i32 parent = mesh.nodes[n].parent;
        if (p >= 0) {
            const physics::BodyState3D& s = states[static_cast<usize>(p)];
            const mat4 bone = math::trs(s.position, s.rotation, {1, 1, 1}) * live.offset[static_cast<usize>(p)];
            if (p == 0) root = origin(bone);
            nodes[n] = to_model * bone;
        } else {
            nodes[n] = parent >= 0 ? nodes[static_cast<usize>(parent)] * live.local[n] : live.model[n];
        }
    }
}

void drop(World& w, Entity e) {
    Ragdolls3DState* st = w.try_resource<Ragdolls3DState>();
    if (!st) return;
    if (const Live* l = st->live.find(e)) {
        if (Physics3D* phys = w.try_resource<Physics3D>()) phys->world.destroy_ragdoll(l->id);
        st->live.erase(e);
    }
}

void on_ragdoll_removed(World& w, Entity e) {
    drop(w, e);
    if (Ragdolls3DState* st = w.try_resource<Ragdolls3DState>()) st->failed.erase(e);
    if (Ragdoll3DPoses* rp = w.try_resource<Ragdoll3DPoses>()) rp->poses.erase(e);
}

// FixedUpdate_Pre, after the bodies: build, impulses, remove, blend back.
void kit3d_ragdolls(Frame& f) {
    World& w = f.world();
    Physics3D& phys = f.resource<Physics3D>();
    Ragdolls3DState& st = f.resource<Ragdolls3DState>();
    Ragdoll3DPoses& rp = f.resource<Ragdoll3DPoses>();
    const f32 dt = phys.world.config().fixed_dt;
    f.query<Ragdoll3D>().each([&](Entity e, Ragdoll3D& r) {
        Live* live = st.live.find(e);
        if (live && !phys.world.has_ragdoll(live->id)) {
            st.live.erase(e);
            live = nullptr;
        }
        if (!r.active) {
            st.failed.erase(e);
            if (live) {  // the last physics pose stays, fading out
                drop(w, e);
                r.parts = 0;
                r.settled = 0;
            } else if (RagdollPose* p = rp.poses.find(e)) {
                p->weight = r.blend_time > 0.0f ? p->weight - dt / r.blend_time : 0.0f;
                if (p->weight <= 0.0f) rp.poses.erase(e);
            }
            const RagdollPose* p = rp.poses.find(e);
            r.blend = p ? p->weight : 0.0f;
            return;
        }
        if (!live) {
            if (st.failed.find(e)) return;
            const MeshRef* ref = w.try_get<MeshRef>(e);
            const render::GpuMesh* mesh = ref ? anim3d_mesh(w, ref->mesh.guid) : nullptr;
            if (!mesh) return;  // no mesh (yet)
            Live fresh;
            if (!build(w, phys, e, r, *mesh, fresh)) {
                MCE_LOG_WARN("kit3d: Ragdoll3D on entity {} not built (needs a skinned mesh whose bones move vertices)", e.index);
                st.failed.insert_or_assign(e, u8{1});
                return;
            }
            r.parts = phys.world.ragdoll_part_count(fresh.id);
            r.settled = 0;
            live = &st.live.insert_or_assign(e, static_cast<Live&&>(fresh));
        }
        if (math::length_sq(r.impulse) > 0.0f) {
            i32 part = -1;
            if (!r.impulse_bone.empty()) {
                const MeshRef* ref = w.try_get<MeshRef>(e);
                const render::GpuMesh* mesh = ref ? anim3d_mesh(w, ref->mesh.guid) : nullptr;
                u32 n = mesh ? mesh->find_node(r.impulse_bone.view()) : ~0u;
                for (; mesh && n < live->node_part.size(); n = static_cast<u32>(mesh->nodes[n].parent)) {
                    if (live->node_part[n] >= 0) {
                        part = live->node_part[n];
                        break;
                    }
                    if (mesh->nodes[n].parent < 0) break;
                }
                if (part < 0) part = 0;
            }
            phys.world.ragdoll_impulse(live->id, part, r.impulse);
            r.impulse = {};
        }
    });
}

// FixedUpdate_Physics, after the write-back: the mesh's pose from the bodies.
void kit3d_ragdoll_pose(Frame& f) {
    World& w = f.world();
    Physics3D& phys = f.resource<Physics3D>();
    Ragdolls3DState& st = f.resource<Ragdolls3DState>();
    Ragdoll3DPoses& rp = f.resource<Ragdoll3DPoses>();
    f.query<Ragdoll3D>().each([&](Entity e, Ragdoll3D& r) {
        const Live* live = st.live.find(e);
        if (!live || !phys.world.has_ragdoll(live->id)) return;
        const MeshRef* ref = w.try_get<MeshRef>(e);
        const render::GpuMesh* mesh = ref ? anim3d_mesh(w, ref->mesh.guid) : nullptr;
        if (!mesh || mesh->nodes.size() != live->node_part.size()) {  // the mesh changed under it
            drop(w, e);
            rp.poses.erase(e);
            r.parts = 0;
            r.blend = 0.0f;
            return;
        }
        RagdollPose* p = rp.poses.find(e);
        if (!p) p = &rp.poses.insert_or_assign(e, RagdollPose{});
        p->weight = 1.0f;
        pose(w, phys, e, *live, p->nodes, *mesh, r.root_position);
        r.blend = 1.0f;
        r.settled = phys.world.ragdoll_active(live->id) ? 0u : 1u;
    });
}

}  // namespace

bool apply_ragdoll_pose(World& w, Entity e, Span<mat4> pose) {
    const Ragdoll3DPoses* rp = w.try_resource<Ragdoll3DPoses>();
    const RagdollPose* p = rp ? rp->poses.find(e) : nullptr;
    if (!p || p->nodes.size() != pose.size()) return false;
    const f32 t = math::clamp(p->weight, 0.0f, 1.0f);
    for (usize n = 0; n < pose.size(); ++n) {
        if (t >= 1.0f) {
            pose[n] = p->nodes[n];
            continue;
        }
        const mat4& a = pose[n];
        const mat4& b = p->nodes[n];
        const vec3 sa{math::length(a.col(0).xyz()), math::length(a.col(1).xyz()), math::length(a.col(2).xyz())};
        const vec3 sb{math::length(b.col(0).xyz()), math::length(b.col(1).xyz()), math::length(b.col(2).xyz())};
        pose[n] = math::trs(origin(a) + (origin(b) - origin(a)) * t, math::slerp(rotation_of(a), rotation_of(b), t),
                            sa + (sb - sa) * t);
    }
    return true;
}

void install_ragdolls3d(World& world, asset::AssetSystem* assets) {
    install_bodies3d(world, assets);
    if (world.try_resource<Ragdolls3DState>()) return;
    world.set_resource(Ragdolls3DState{});
    world.set_resource(Ragdoll3DPoses{});
    world.register_components<Ragdoll3D>();
    world.set_hooks<Ragdoll3D>(nullptr, &on_ragdoll_removed);
    world.add_system(Phase::FixedUpdate_Pre, "kit3d_ragdolls", &kit3d_ragdolls, "kit3d_bodies");
    world.add_system(Phase::FixedUpdate_Physics, "kit3d_ragdoll_pose", &kit3d_ragdoll_pose, "kit3d_writeback");
}

}  // namespace mce::kit

// World3D over Jolt Physics 5 (ADR-0003). The only translation unit family that may include Jolt.

#include "mce/physics3d/world3d.h"

// Jolt.h must come first.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/GroupFilter.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/ConeConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>
#include <Jolt/RegisterTypes.h>

#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "mce/containers/hash_map.h"
#include "mce/hash.h"
#include "mce/log.h"
#include "mce/memory/allocator.h"

namespace mce::physics {

namespace {

// ---- Jolt global state (ADR-0003 §5): reference-counted by live worlds, main thread only --------

u32 g_jolt_refs = 0;

void* jolt_allocate(size_t size) { return mem::heap().allocate(size ? size : 1, mem::kDefaultAlign).ptr; }
void* jolt_aligned_allocate(size_t size, size_t align) {
    return mem::heap().allocate(size ? size : 1, align > mem::kDefaultAlign ? align : mem::kDefaultAlign).ptr;
}
void jolt_free(void* p) {
    if (p) mem::heap().free(p);
}
void* jolt_reallocate(void* block, size_t old_size, size_t new_size) {
    void* p = jolt_allocate(new_size);
    if (block && p) std::memcpy(p, block, old_size < new_size ? old_size : new_size);
    jolt_free(block);
    return p;
}
void jolt_trace(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    MCE_LOG_WARN("jolt: {}", buf);
}

void jolt_acquire() {
    if (g_jolt_refs++ > 0) return;
    JPH::Allocate = jolt_allocate;
    JPH::Reallocate = jolt_reallocate;
    JPH::Free = jolt_free;
    JPH::AlignedAllocate = jolt_aligned_allocate;
    JPH::AlignedFree = jolt_free;
    JPH::Trace = jolt_trace;
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
}

void jolt_release() {
    MCE_ASSERT(g_jolt_refs > 0);
    if (--g_jolt_refs > 0) return;
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}

// ---- layers (ADR-0003 §4) ------------------------------------------------------------------------
// ObjectLayer (32 bits) = layer index (bits 0-3) | moving bit (bit 4) | collision mask (bits 16-31).
// Two broad-phase layers: static and moving. Statics never test against statics.

constexpr u32 kMovingBit = 1u << 4;
constexpr JPH::BroadPhaseLayer kBpStatic(0);
constexpr JPH::BroadPhaseLayer kBpMoving(1);

JPH::ObjectLayer object_layer(u8 layer, u16 mask, bool moving) {
    return static_cast<JPH::ObjectLayer>((layer & 15u) | (moving ? kMovingBit : 0u) | (static_cast<u32>(mask) << 16));
}
u32 layer_bit(JPH::ObjectLayer l) { return 1u << (l & 15u); }
u32 layer_mask(JPH::ObjectLayer l) { return l >> 16; }
bool is_moving(JPH::ObjectLayer l) { return (l & kMovingBit) != 0; }

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l) const override {
        return is_moving(l) ? kBpMoving : kBpStatic;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer l) const override {
        return l == kBpMoving ? "moving" : "static";
    }
#endif
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer l, JPH::BroadPhaseLayer bp) const override {
        return is_moving(l) || bp == kBpMoving;
    }
};

class ObjectPairs final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        if (!is_moving(a) && !is_moving(b)) return false;
        return (layer_bit(a) & layer_mask(b)) != 0 && (layer_bit(b) & layer_mask(a)) != 0;
    }
};

// ---- conversions ---------------------------------------------------------------------------------

JPH::Vec3 to_jph(vec3 v) { return {v.x, v.y, v.z}; }
JPH::Quat to_jph(quat q) { return {q.x, q.y, q.z, q.w}; }
vec3 to_mce(JPH::Vec3Arg v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
quat to_mce(JPH::QuatArg q) { return {q.GetX(), q.GetY(), q.GetZ(), q.GetW()}; }

u64 pack(Entity e) { return (static_cast<u64>(e.generation) << 32) | e.index; }
Entity unpack(u64 v) { return Entity{static_cast<u32>(v), static_cast<u32>(v >> 32)}; }

JPH::EMotionType motion_type(BodyType3D t) {
    switch (t) {
        case BodyType3D::Static: return JPH::EMotionType::Static;
        case BodyType3D::Kinematic: return JPH::EMotionType::Kinematic;
        case BodyType3D::Dynamic: return JPH::EMotionType::Dynamic;
    }
    return JPH::EMotionType::Static;
}

bool positive(f32 v) { return v > 0.0f && v < 1e6f; }

// Builds the Jolt shape for `s`; null when malformed.
JPH::RefConst<JPH::Shape> build_shape(const Shape3D& s, f32 density) {
    JPH::Ref<JPH::ShapeSettings> settings;
    switch (s.type) {
        case ShapeType3D::Box: {
            if (!positive(s.half.x) || !positive(s.half.y) || !positive(s.half.z)) return nullptr;
            const f32 smallest = math::min(s.half.x, math::min(s.half.y, s.half.z));
            auto* b = new JPH::BoxShapeSettings(to_jph(s.half), math::min(JPH::cDefaultConvexRadius, smallest * 0.5f));
            b->SetDensity(density);
            settings = b;
            break;
        }
        case ShapeType3D::Sphere: {
            if (!positive(s.radius)) return nullptr;
            auto* b = new JPH::SphereShapeSettings(s.radius);
            b->SetDensity(density);
            settings = b;
            break;
        }
        case ShapeType3D::Capsule: {
            if (!positive(s.radius) || !positive(s.half_height)) return nullptr;
            auto* b = new JPH::CapsuleShapeSettings(s.half_height, s.radius);
            b->SetDensity(density);
            settings = b;
            break;
        }
        case ShapeType3D::Convex: {
            if (s.points.size() < 4) return nullptr;
            JPH::Array<JPH::Vec3> pts;
            pts.reserve(s.points.size());
            for (const vec3& p : s.points) pts.push_back(to_jph(p));
            auto* b = new JPH::ConvexHullShapeSettings(pts);
            b->SetDensity(density);
            settings = b;
            break;
        }
        case ShapeType3D::Mesh: {
            if (s.points.empty() || s.indices.empty() || s.indices.size() % 3 != 0) return nullptr;
            JPH::VertexList verts;
            verts.reserve(s.points.size());
            for (const vec3& p : s.points) verts.push_back(JPH::Float3(p.x, p.y, p.z));
            JPH::IndexedTriangleList tris;
            tris.reserve(s.indices.size() / 3);
            for (usize i = 0; i < s.indices.size(); i += 3) {
                if (s.indices[i] >= s.points.size() || s.indices[i + 1] >= s.points.size() ||
                    s.indices[i + 2] >= s.points.size())
                    return nullptr;
                tris.push_back(JPH::IndexedTriangle(s.indices[i], s.indices[i + 1], s.indices[i + 2]));
            }
            settings = new JPH::MeshShapeSettings(std::move(verts), std::move(tris));
            break;
        }
        case ShapeType3D::HeightField: {
            static_assert(kHeightFieldHole == JPH::HeightFieldShapeConstants::cNoCollisionValue);
            if (s.samples < 4 || s.samples > 8193 || s.heights.size() != usize(s.samples) * s.samples) return nullptr;
            if (!positive(s.spacing.x) || !positive(s.spacing.z) || !positive(std::fabs(s.spacing.y))) return nullptr;
            settings = new JPH::HeightFieldShapeSettings(s.heights.data(), to_jph(s.origin), to_jph(s.spacing), s.samples);
            break;
        }
    }
    if (!settings) return nullptr;
    JPH::ShapeSettings::ShapeResult r = settings->Create();
    if (r.HasError()) return nullptr;
    return r.Get();
}

// Body pairs joined by a joint with collide_connected off never collide. Such bodies carry this
// filter with their slot index as the sub-group; `pairs` counts the joints per pair.
class JointPairFilter final : public JPH::GroupFilter {
public:
    static u64 key(u32 a, u32 b) { return a < b ? (static_cast<u64>(a) << 32) | b : (static_cast<u64>(b) << 32) | a; }
    bool CanCollide(const JPH::CollisionGroup& g1, const JPH::CollisionGroup& g2) const override {
        if (g1.GetGroupFilter() != this || g2.GetGroupFilter() != this) return true;
        return pairs.find(key(g1.GetSubGroupID(), g2.GetSubGroupID())) == nullptr;
    }
    HashMap<u64, u32> pairs;
};

}  // namespace

// ---- the world -------------------------------------------------------------------------------------

struct World3D::Impl {
    struct Slot {
        Entity entity;
        BodyType3D type = BodyType3D::Static;
        bool alive = false;
        JPH::BodyID body;                       // rigid bodies
        JPH::Ref<JPH::CharacterVirtual> character;  // characters
        JPH::ObjectLayer character_layer = 0;
        MoveResult3D last;
    };

    PhysicsConfig3D config;
    // Declaration order is destruction order in reverse: Jolt objects go before the filters they
    // point at, and characters before the system.
    BroadPhaseLayers bp_layers;
    ObjectVsBroadPhase object_vs_bp;
    ObjectPairs object_pairs;
    JPH::TempAllocatorImpl temp;
    JPH::JobSystemSingleThreaded jobs{JPH::cMaxPhysicsJobs};
    JPH::PhysicsSystem system;
    JPH::CharacterVsCharacterCollisionSimple char_vs_char;
    Array<Slot> slots;
    Array<u32> free_slots;
    Array<u32> slot_of;  // entity index -> slot + 1 (0 = none)
    u32 characters = 0;

    struct Joint {
        JPH::Ref<JPH::TwoBodyConstraint> constraint;
        JointDesc3D desc;
        u32 slot_a = 0;
        u32 slot_b = 0xFFFFFFFFu;  // the world
        u32 generation = 0;
        JPH::Vec3 local_a = JPH::Vec3::sZero();  // Distance: the anchors in a's and b's (or the world's) frame
        JPH::Vec3 local_b = JPH::Vec3::sZero();
        bool alive = false;
        bool enabled = false;
        bool broken = false;
        bool filtered = false;  // counted in pair_filter
        f32 force = 0.0f;
        f32 torque = 0.0f;
    };
    Array<Joint> joints;
    Array<u32> free_joints;
    u32 live_joints = 0;
    bool had_joints = false;  // hash_state folds joints only once a world has had one
    JPH::Ref<JointPairFilter> pair_filter = new JointPairFilter();

    struct RagdollSlot {
        JPH::Ref<JPH::Ragdoll> ragdoll;
        Entity owner;
        u32 generation = 0;
        bool alive = false;
    };
    Array<RagdollSlot> ragdolls;
    Array<u32> free_ragdolls;
    u32 live_ragdolls = 0;
    u32 ragdoll_groups = 0;     // Jolt collision group per ragdoll (unique in the system)
    bool had_ragdolls = false;  // hash_state folds ragdolls only once a world has had one

    // id = generation << 20 | (index + 1), as joints.
    RagdollSlot* find_ragdoll(u32 id) {
        const u32 i = (id & 0xFFFFFu) - 1u;
        if ((id & 0xFFFFFu) == 0 || i >= ragdolls.size()) return nullptr;
        RagdollSlot& r = ragdolls[i];
        return r.alive && r.generation == (id >> 20) ? &r : nullptr;
    }
    const RagdollSlot* find_ragdoll(u32 id) const { return const_cast<Impl*>(this)->find_ragdoll(id); }

    struct Vehicle {
        JPH::Ref<JPH::VehicleConstraint> constraint;
        JPH::Ref<JPH::VehicleCollisionTester> tester;
        u32 slot = 0;  // the body
        u32 generation = 0;
        bool alive = false;
    };
    Array<Vehicle> vehicles;
    Array<u32> free_vehicles;
    u32 live_vehicles = 0;
    bool had_vehicles = false;  // hash_state folds vehicles only once a world has had one

    Vehicle* find_vehicle(u32 id) {
        const u32 i = (id & 0xFFFFFu) - 1u;
        if ((id & 0xFFFFFu) == 0 || i >= vehicles.size()) return nullptr;
        Vehicle& v = vehicles[i];
        return v.alive && v.generation == (id >> 20) ? &v : nullptr;
    }
    const Vehicle* find_vehicle(u32 id) const { return const_cast<Impl*>(this)->find_vehicle(id); }

    void remove_vehicle(Vehicle& v) {
        system.RemoveStepListener(v.constraint);
        system.RemoveConstraint(v.constraint);
        const u32 gen = v.generation;
        v = Vehicle{};
        v.generation = (gen + 1) & 0xFFFu;
        free_vehicles.push_back(static_cast<u32>(&v - vehicles.data()));
        --live_vehicles;
    }

    explicit Impl(const PhysicsConfig3D& c) : config(c), temp(static_cast<JPH::uint>(c.temp_allocator_bytes)) {
        system.Init(c.max_bodies, 0, c.max_body_pairs, c.max_contact_constraints, bp_layers, object_vs_bp,
                    object_pairs);
        system.SetGravity(to_jph(c.gravity));
    }

    ~Impl() {
        for (Joint& j : joints)
            if (j.alive) system.RemoveConstraint(j.constraint);
        joints.clear();
        for (Vehicle& v : vehicles)
            if (v.alive) {
                system.RemoveStepListener(v.constraint);
                system.RemoveConstraint(v.constraint);
            }
        vehicles.clear();
        for (RagdollSlot& r : ragdolls)
            if (r.alive) r.ragdoll->RemoveFromPhysicsSystem();
        ragdolls.clear();  // destroys the parts' bodies
        JPH::BodyInterface& bi = system.GetBodyInterfaceNoLock();
        for (Slot& s : slots) {
            if (!s.alive) continue;
            if (s.character) {
                char_vs_char.Remove(s.character);
                s.character = nullptr;
            } else {
                bi.RemoveBody(s.body);
                bi.DestroyBody(s.body);
            }
        }
    }

    Slot* find(Entity e) {
        if (e.is_null() || e.index >= slot_of.size() || slot_of[e.index] == 0) return nullptr;
        Slot& s = slots[slot_of[e.index] - 1];
        return s.alive && s.entity == e ? &s : nullptr;
    }
    const Slot* find(Entity e) const { return const_cast<Impl*>(this)->find(e); }
    u32 slot_index(Entity e) const {
        const Slot* s = find(e);
        return s ? static_cast<u32>(s - slots.data()) : 0xFFFFFFFFu;
    }

    Slot& claim(Entity e) {
        u32 i;
        if (!free_slots.empty()) {
            i = free_slots.back();
            free_slots.pop_back();
        } else {
            i = static_cast<u32>(slots.size());
            slots.emplace_back();
        }
        if (e.index >= slot_of.size()) slot_of.resize(e.index + 1, 0u);
        slot_of[e.index] = i + 1;
        Slot& s = slots[i];
        s = Slot{};
        s.entity = e;
        s.alive = true;
        return s;
    }

    // id = generation << 20 | (index + 1); 0 is never a joint.
    Joint* find_joint(u32 id) {
        const u32 i = (id & 0xFFFFFu) - 1u;
        if ((id & 0xFFFFFu) == 0 || i >= joints.size()) return nullptr;
        Joint& j = joints[i];
        return j.alive && j.generation == (id >> 20) ? &j : nullptr;
    }
    const Joint* find_joint(u32 id) const { return const_cast<Impl*>(this)->find_joint(id); }

    void remove_joint(Joint& j) {
        system.RemoveConstraint(j.constraint);
        if (j.filtered) {
            const u64 k = JointPairFilter::key(j.slot_a, j.slot_b);
            if (u32* n = pair_filter->pairs.find(k); n && --*n == 0) pair_filter->pairs.erase(k);
        }
        const u32 gen = j.generation;
        j = Joint{};
        j.generation = (gen + 1) & 0xFFFu;
        free_joints.push_back(static_cast<u32>(&j - joints.data()));
        --live_joints;
    }

    // After a step: the force and torque each joint held with, and the breaks (in creation order).
    void check_joints() {
        const f32 inv_dt = 1.0f / config.fixed_dt;
        for (Joint& j : joints) {
            if (!j.alive) continue;
            if (!j.enabled) {
                j.force = j.torque = 0.0f;
                continue;
            }
            f32 lin = 0.0f, ang = 0.0f;
            const auto len2 = [](const JPH::Vector<2>& v) { return std::sqrt(v[0] * v[0] + v[1] * v[1]); };
            switch (j.desc.type) {
            case JointType3D::Fixed: {
                const auto* c = static_cast<const JPH::FixedConstraint*>(j.constraint.GetPtr());
                lin = c->GetTotalLambdaPosition().Length();
                ang = c->GetTotalLambdaRotation().Length();
                break;
            }
            case JointType3D::Hinge: {
                const auto* c = static_cast<const JPH::HingeConstraint*>(j.constraint.GetPtr());
                lin = c->GetTotalLambdaPosition().Length();
                const f32 r = len2(c->GetTotalLambdaRotation()), l = c->GetTotalLambdaRotationLimits();
                ang = std::sqrt(r * r + l * l);
                break;
            }
            case JointType3D::Slider: {
                const auto* c = static_cast<const JPH::SliderConstraint*>(j.constraint.GetPtr());
                const f32 p = len2(c->GetTotalLambdaPosition()), l = c->GetTotalLambdaPositionLimits();
                lin = std::sqrt(p * p + l * l);
                ang = c->GetTotalLambdaRotation().Length();
                break;
            }
            case JointType3D::Distance:
                lin = std::fabs(static_cast<const JPH::DistanceConstraint*>(j.constraint.GetPtr())->GetTotalLambdaPosition());
                break;
            case JointType3D::Cone: {
                const auto* c = static_cast<const JPH::ConeConstraint*>(j.constraint.GetPtr());
                lin = c->GetTotalLambdaPosition().Length();
                ang = std::fabs(c->GetTotalLambdaRotation());
                break;
            }
            case JointType3D::SixDOF: {
                const auto* c = static_cast<const JPH::SixDOFConstraint*>(j.constraint.GetPtr());
                lin = c->GetTotalLambdaPosition().Length();
                ang = c->GetTotalLambdaRotation().Length();
                break;
            }
            }
            j.force = lin * inv_dt;
            j.torque = ang * inv_dt;
            const bool snap = (j.desc.break_force > 0.0f && j.force > j.desc.break_force) ||
                              (j.desc.break_torque > 0.0f && j.torque > j.desc.break_torque);
            if (!snap) continue;
            j.constraint->SetEnabled(false);
            j.enabled = false;
            j.broken = true;
            system.GetBodyInterfaceNoLock().ActivateConstraint(j.constraint);
        }
    }

    void release(Slot& s) {
        const u32 i = static_cast<u32>(&s - slots.data());
        slot_of[s.entity.index] = 0;
        s = Slot{};
        free_slots.push_back(i);
    }
};

namespace {

// Query filters shared by raycast / shape_cast / camera_sweep.
class LayerFilter final : public JPH::ObjectLayerFilter {
public:
    explicit LayerFilter(u16 mask) : mask_(mask) {}
    bool ShouldCollide(JPH::ObjectLayer l) const override { return (layer_bit(l) & mask_) != 0; }

private:
    u32 mask_;
};

class EntityFilter final : public JPH::BodyFilter {
public:
    explicit EntityFilter(const QueryFilter3D& f) : f_(f) {}
    bool ShouldCollideLocked(const JPH::Body& b) const override {
        if (!f_.include_dynamic && b.IsDynamic()) return false;
        const Entity e = unpack(b.GetUserData());
        for (const Entity x : f_.exclude)
            if (x == e) return false;
        return true;
    }

private:
    const QueryFilter3D& f_;
};

}  // namespace

World3D::World3D(const PhysicsConfig3D& config) {
    jolt_acquire();
    impl_ = mem::UniquePtr<Impl>(mem::heap(), mem::make<Impl>(mem::heap(), config));
}

World3D::~World3D() {
    if (!impl_) return;
    impl_.reset();
    jolt_release();
}

World3D::World3D(World3D&& o) noexcept = default;
World3D& World3D::operator=(World3D&& o) noexcept {
    if (this != &o) {
        if (impl_) {
            impl_.reset();
            jolt_release();
        }
        impl_ = std::move(o.impl_);
    }
    return *this;
}

const PhysicsConfig3D& World3D::config() const { return impl_->config; }

void World3D::set_gravity(vec3 gravity) {
    impl_->config.gravity = gravity;
    impl_->system.SetGravity(to_jph(gravity));
}

bool World3D::create_body(Entity e, const BodyDesc3D& desc) {
    Impl& w = *impl_;
    if (e.is_null() || has_body(e)) return false;
    if ((desc.shape.type == ShapeType3D::Mesh || desc.shape.type == ShapeType3D::HeightField) && desc.type == BodyType3D::Dynamic)
        return false;  // Jolt: no dynamic meshes or height fields
    JPH::RefConst<JPH::Shape> shape = build_shape(desc.shape, desc.density);
    if (!shape) return false;

    const bool moving = desc.type != BodyType3D::Static;
    JPH::BodyCreationSettings bcs(shape, to_jph(desc.position), to_jph(math::normalize(desc.rotation)),
                                  motion_type(desc.type), object_layer(desc.layer, desc.mask, moving));
    bcs.mFriction = desc.friction;
    bcs.mRestitution = desc.restitution;
    bcs.mGravityFactor = desc.gravity_scale;
    bcs.mLinearDamping = desc.linear_damping;
    bcs.mUserData = pack(e);
    JPH::BodyInterface& bi = w.system.GetBodyInterfaceNoLock();
    const JPH::BodyID id = bi.CreateAndAddBody(bcs, moving ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    if (id.IsInvalid()) return false;  // max_bodies reached

    Impl::Slot& s = w.claim(e);
    s.type = desc.type;
    s.body = id;
    return true;
}

bool World3D::create_character(Entity e, vec3 position, const ControllerParams3D& p) {
    Impl& w = *impl_;
    if (e.is_null() || has_body(e) || !positive(p.radius) || !positive(p.half_height)) return false;

    JPH::Ref<JPH::CharacterVirtualSettings> cs = new JPH::CharacterVirtualSettings();
    cs->mShape = new JPH::CapsuleShape(p.half_height, p.radius);
    // Contacts below the bottom of the cylinder support the character (position = capsule centre).
    cs->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), p.half_height);
    cs->mMaxSlopeAngle = JPH::DegreesToRadians(p.max_slope_deg);
    cs->mMass = p.mass;
    cs->mMaxStrength = p.max_push_force;
    cs->mCharacterPadding = p.padding;
    cs->mEnhancedInternalEdgeRemoval = true;
    JPH::Ref<JPH::CharacterVirtual> ch =
        new JPH::CharacterVirtual(cs, to_jph(position), JPH::Quat::sIdentity(), pack(e), &w.system);
    ch->SetCharacterVsCharacterCollision(&w.char_vs_char);
    w.char_vs_char.Add(ch);

    Impl::Slot& s = w.claim(e);
    s.type = BodyType3D::Kinematic;
    s.character = ch;
    s.character_layer = object_layer(p.layer, p.mask, true);
    s.last.position = position;
    ++w.characters;
    return true;
}

void World3D::destroy_body(Entity e) {
    Impl& w = *impl_;
    Impl::Slot* s = w.find(e);
    if (!s) return;
    const u32 slot = static_cast<u32>(s - w.slots.data());
    for (Impl::Joint& j : w.joints)
        if (j.alive && (j.slot_a == slot || j.slot_b == slot)) w.remove_joint(j);
    for (Impl::Vehicle& v : w.vehicles)
        if (v.alive && v.slot == slot) w.remove_vehicle(v);
    if (s->character) {
        w.char_vs_char.Remove(s->character);
        s->character = nullptr;
        --w.characters;
    } else {
        JPH::BodyInterface& bi = w.system.GetBodyInterfaceNoLock();
        bi.RemoveBody(s->body);
        bi.DestroyBody(s->body);
    }
    w.release(*s);
}

bool World3D::has_body(Entity e) const { return impl_->find(e) != nullptr; }

void World3D::step(f32 dt) {
    Impl& w = *impl_;
    MCE_ASSERT_MSG(dt == w.config.fixed_dt, "World3D::step: dt must equal the fixed step (physics.md D7)");
    w.system.Update(w.config.fixed_dt, 1, &w.temp, &w.jobs);
    w.check_joints();
}

void World3D::set_linear_velocity(Entity e, vec3 v) {
    Impl& w = *impl_;
    const Impl::Slot* s = w.find(e);
    if (!s || s->character || s->type == BodyType3D::Static) return;
    JPH::BodyInterface& bi = w.system.GetBodyInterfaceNoLock();
    bi.SetLinearVelocity(s->body, to_jph(v));
    if (math::length_sq(v) > 0.0f) bi.ActivateBody(s->body);
}

void World3D::set_angular_velocity(Entity e, vec3 v) {
    Impl& w = *impl_;
    const Impl::Slot* s = w.find(e);
    if (!s || s->character || s->type == BodyType3D::Static) return;
    JPH::BodyInterface& bi = w.system.GetBodyInterfaceNoLock();
    bi.SetAngularVelocity(s->body, to_jph(v));
    if (math::length_sq(v) > 0.0f) bi.ActivateBody(s->body);
}

void World3D::apply_impulse(Entity e, vec3 impulse) {
    Impl& w = *impl_;
    const Impl::Slot* s = w.find(e);
    if (!s || s->character || s->type != BodyType3D::Dynamic) return;
    w.system.GetBodyInterfaceNoLock().AddImpulse(s->body, to_jph(impulse));
}

void World3D::teleport(Entity e, vec3 position, quat rotation) {
    Impl& w = *impl_;
    Impl::Slot* s = w.find(e);
    if (!s) return;
    if (s->character) {
        s->character->SetPosition(to_jph(position));
        s->character->SetRotation(to_jph(math::normalize(rotation)));
        s->character->SetLinearVelocity(JPH::Vec3::sZero());
        s->last.position = position;
        return;
    }
    w.system.GetBodyInterfaceNoLock().SetPositionAndRotation(s->body, to_jph(position), to_jph(math::normalize(rotation)),
                                                             JPH::EActivation::Activate);
}

bool World3D::body_state(Entity e, BodyState3D& out) const {
    const Impl& w = *impl_;
    const Impl::Slot* s = w.find(e);
    if (!s) return false;
    out = BodyState3D{};
    out.entity = e;
    out.type = s->type;
    if (s->character) {
        out.position = to_mce(s->character->GetPosition());
        out.rotation = to_mce(s->character->GetRotation());
        out.linear_velocity = to_mce(s->character->GetLinearVelocity());
        return true;
    }
    const JPH::BodyInterface& bi = w.system.GetBodyInterfaceNoLock();
    JPH::RVec3 p;
    JPH::Quat q;
    bi.GetPositionAndRotation(s->body, p, q);
    out.position = to_mce(p);
    out.rotation = to_mce(q);
    out.linear_velocity = to_mce(bi.GetLinearVelocity(s->body));
    out.angular_velocity = to_mce(bi.GetAngularVelocity(s->body));
    return true;
}

MoveResult3D World3D::move_character(Entity e, vec3 delta, const ControllerParams3D& p) {
    Impl& w = *impl_;
    Impl::Slot* s = w.find(e);
    if (!s || !s->character) return {};
    JPH::CharacterVirtual& ch = *s->character;
    const f32 dt = w.config.fixed_dt;

    ch.SetMaxSlopeAngle(JPH::DegreesToRadians(p.max_slope_deg));
    ch.SetMass(p.mass);
    ch.SetMaxStrength(p.max_push_force);
    ch.SetLinearVelocity(to_jph(delta / dt));

    JPH::CharacterVirtual::ExtendedUpdateSettings us;
    us.mStickToFloorStepDown = JPH::Vec3(0.0f, -p.stick_to_floor, 0.0f);
    us.mWalkStairsStepUp = JPH::Vec3(0.0f, p.step_height, 0.0f);
    const JPH::DefaultBroadPhaseLayerFilter bp_filter(w.object_vs_bp, s->character_layer);
    const JPH::DefaultObjectLayerFilter layer_filter(w.object_pairs, s->character_layer);
    const JPH::BodyFilter body_filter;
    const JPH::ShapeFilter shape_filter;
    ch.ExtendedUpdate(dt, to_jph(w.config.gravity), us, bp_filter, layer_filter, body_filter, shape_filter, w.temp);

    MoveResult3D r;
    r.position = to_mce(ch.GetPosition());
    r.velocity = to_mce(ch.GetLinearVelocity());
    switch (ch.GetGroundState()) {
        case JPH::CharacterBase::EGroundState::OnGround: r.flags |= ControllerFlags3D::Ground; break;
        case JPH::CharacterBase::EGroundState::OnSteepGround: r.flags |= ControllerFlags3D::SteepGround; break;
        default: break;
    }
    if (ch.IsSupported()) {
        r.ground_normal = to_mce(ch.GetGroundNormal());
        r.ground_velocity = to_mce(ch.GetGroundVelocity());
        const JPH::BodyID ground = ch.GetGroundBodyID();
        if (!ground.IsInvalid()) r.ground_body = unpack(w.system.GetBodyInterfaceNoLock().GetUserData(ground));
        if (math::length_sq(r.ground_velocity) > 1e-8f) r.flags |= ControllerFlags3D::OnMovingBody;
    }
    for (const auto& c : ch.GetActiveContacts()) {
        if (!c.mHadCollision || c.mWasDiscarded) continue;
        const f32 ny = c.mSurfaceNormal.GetY();
        if (ny < -0.5f) {
            r.flags |= ControllerFlags3D::Ceiling;
            r.hit_ceiling = true;
        } else if (ny > -0.3f && ny < 0.3f) {
            r.flags |= ControllerFlags3D::Wall;
        }
    }
    s->last = r;
    return r;
}

bool World3D::controller_state(Entity e, MoveResult3D& out) const {
    const Impl::Slot* s = impl_->find(e);
    if (!s || !s->character) return false;
    out = s->last;
    return true;
}

// ---- joints ------------------------------------------------------------------------------------------

namespace {

JPH::SpringSettings spring_of(f32 frequency, f32 damping) {
    JPH::SpringSettings s;
    s.mMode = JPH::ESpringMode::FrequencyAndDamping;
    s.mFrequency = frequency > 0.0f ? frequency : 0.0f;
    s.mDamping = damping > 0.0f ? damping : 0.0f;
    return s;
}

JPH::MotorSettings motor_of(const JointDesc3D& d) {
    JPH::MotorSettings m(d.motor_frequency > 0.0f ? d.motor_frequency : 2.0f, d.motor_damping > 0.0f ? d.motor_damping : 0.0f);
    const f32 limit = d.motor_max_force > 0.0f ? d.motor_max_force : FLT_MAX;
    m.SetForceLimit(limit);
    m.SetTorqueLimit(limit);
    return m;
}

JPH::EMotorState motor_state(JointMotorMode3D m) {
    switch (m) {
    case JointMotorMode3D::Off: return JPH::EMotorState::Off;
    case JointMotorMode3D::Velocity: return JPH::EMotorState::Velocity;
    case JointMotorMode3D::Position: return JPH::EMotorState::Position;
    }
    return JPH::EMotorState::Off;
}

void apply_motor(JPH::TwoBodyConstraint* c, const JointDesc3D& d) {
    const f32 limit = d.motor_max_force > 0.0f ? d.motor_max_force : FLT_MAX;
    if (d.type == JointType3D::Hinge) {
        auto* h = static_cast<JPH::HingeConstraint*>(c);
        h->GetMotorSettings().SetTorqueLimit(limit);
        if (d.motor == JointMotorMode3D::Velocity) h->SetTargetAngularVelocity(d.motor_target);
        if (d.motor == JointMotorMode3D::Position) h->SetTargetAngle(d.motor_target);
        h->SetMotorState(motor_state(d.motor));
    } else if (d.type == JointType3D::Slider) {
        auto* sl = static_cast<JPH::SliderConstraint*>(c);
        sl->GetMotorSettings().SetForceLimit(limit);
        if (d.motor == JointMotorMode3D::Velocity) sl->SetTargetVelocity(d.motor_target);
        if (d.motor == JointMotorMode3D::Position) sl->SetTargetPosition(d.motor_target);
        sl->SetMotorState(motor_state(d.motor));
    }
}

// Six-DOF axis rule: min > max free, min == max locked, else limited.
void six_dof_axis(JPH::SixDOFConstraintSettings& s, JPH::SixDOFConstraintSettings::EAxis axis, f32 lo, f32 hi, bool angular) {
    if (lo > hi) {
        s.MakeFreeAxis(axis);
    } else if (lo == hi) {
        s.MakeFixedAxis(axis);
    } else {
        if (angular) {
            lo = math::clamp(lo, -JPH::JPH_PI, JPH::JPH_PI);
            hi = math::clamp(hi, -JPH::JPH_PI, JPH::JPH_PI);
        }
        s.SetLimitedAxis(axis, lo, hi);
    }
}

}  // namespace

u32 World3D::create_joint(const JointDesc3D& d) {
    Impl& w = *impl_;
    const Impl::Slot* sa = w.find(d.a);
    const Impl::Slot* sb = d.b.is_null() ? nullptr : w.find(d.b);
    if (!sa || sa->character || d.a == d.b) return 0;
    if (!d.b.is_null() && (!sb || sb->character)) return 0;
    if (sa->type == BodyType3D::Static && (!sb || sb->type == BodyType3D::Static)) return 0;

    const vec3 axis = math::normalize(d.axis);
    if (d.type != JointType3D::Fixed && d.type != JointType3D::Distance && !(math::length_sq(axis) > 0.5f)) return 0;
    vec3 normal = d.normal - axis * math::dot(axis, d.normal);
    if (math::length_sq(normal) < 1e-8f) {  // any perpendicular, picked the same way every time
        const vec3 other = std::fabs(axis.x) < 0.9f ? vec3{1.0f, 0.0f, 0.0f} : vec3{0.0f, 1.0f, 0.0f};
        normal = other - axis * math::dot(axis, other);
    }
    normal = math::normalize(normal);
    const JPH::RVec3 anchor = to_jph(d.anchor);

    JPH::Ref<JPH::TwoBodyConstraintSettings> settings;
    switch (d.type) {
    case JointType3D::Fixed: {
        auto* f = new JPH::FixedConstraintSettings();
        f->mPoint1 = f->mPoint2 = anchor;
        settings = f;
        break;
    }
    case JointType3D::Hinge: {
        auto* h = new JPH::HingeConstraintSettings();
        h->mPoint1 = h->mPoint2 = anchor;
        h->mHingeAxis1 = h->mHingeAxis2 = to_jph(axis);
        h->mNormalAxis1 = h->mNormalAxis2 = to_jph(normal);
        if (d.limits) {
            h->mLimitsMin = math::clamp(d.limit_min, -JPH::JPH_PI, 0.0f);
            h->mLimitsMax = math::clamp(d.limit_max, 0.0f, JPH::JPH_PI);
        }
        if (d.spring_frequency > 0.0f) h->mLimitsSpringSettings = spring_of(d.spring_frequency, d.spring_damping);
        h->mMaxFrictionTorque = d.friction > 0.0f ? d.friction : 0.0f;
        h->mMotorSettings = motor_of(d);
        settings = h;
        break;
    }
    case JointType3D::Slider: {
        auto* sl = new JPH::SliderConstraintSettings();
        sl->mPoint1 = sl->mPoint2 = anchor;
        sl->mSliderAxis1 = sl->mSliderAxis2 = to_jph(axis);
        sl->mNormalAxis1 = sl->mNormalAxis2 = to_jph(normal);
        if (d.limits) {
            sl->mLimitsMin = d.limit_min < 0.0f ? d.limit_min : 0.0f;
            sl->mLimitsMax = d.limit_max > 0.0f ? d.limit_max : 0.0f;
        }
        if (d.spring_frequency > 0.0f) sl->mLimitsSpringSettings = spring_of(d.spring_frequency, d.spring_damping);
        sl->mMaxFrictionForce = d.friction > 0.0f ? d.friction : 0.0f;
        sl->mMotorSettings = motor_of(d);
        settings = sl;
        break;
    }
    case JointType3D::Distance: {
        auto* ds = new JPH::DistanceConstraintSettings();
        ds->mPoint1 = to_jph(d.anchor_b);
        ds->mPoint2 = anchor;
        const f32 rest = math::length(d.anchor_b - d.anchor);
        const f32 lo = d.limit_min < 0.0f ? rest : d.limit_min;
        const f32 hi = d.limit_max < 0.0f ? rest : d.limit_max;
        ds->mMinDistance = math::min(lo, hi);
        ds->mMaxDistance = math::max(lo, hi);
        if (d.spring_frequency > 0.0f) ds->mLimitsSpringSettings = spring_of(d.spring_frequency, d.spring_damping);
        settings = ds;
        break;
    }
    case JointType3D::Cone: {
        auto* c = new JPH::ConeConstraintSettings();
        c->mPoint1 = c->mPoint2 = anchor;
        c->mTwistAxis1 = c->mTwistAxis2 = to_jph(axis);
        c->mHalfConeAngle = math::clamp(d.cone_half_angle, 0.0f, JPH::JPH_PI);
        settings = c;
        break;
    }
    case JointType3D::SixDOF: {
        using Axis = JPH::SixDOFConstraintSettings::EAxis;
        auto* x = new JPH::SixDOFConstraintSettings();
        x->mPosition1 = x->mPosition2 = anchor;
        x->mAxisX1 = x->mAxisX2 = to_jph(axis);
        x->mAxisY1 = x->mAxisY2 = to_jph(normal);
        x->mSwingType = JPH::ESwingType::Pyramid;
        six_dof_axis(*x, Axis::TranslationX, d.linear_min.x, d.linear_max.x, false);
        six_dof_axis(*x, Axis::TranslationY, d.linear_min.y, d.linear_max.y, false);
        six_dof_axis(*x, Axis::TranslationZ, d.linear_min.z, d.linear_max.z, false);
        six_dof_axis(*x, Axis::RotationX, d.angular_min.x, d.angular_max.x, true);
        six_dof_axis(*x, Axis::RotationY, d.angular_min.y, d.angular_max.y, true);
        six_dof_axis(*x, Axis::RotationZ, d.angular_min.z, d.angular_max.z, true);
        settings = x;
        break;
    }
    }
    if (!settings) return 0;

    JPH::BodyInterface& bi = w.system.GetBodyInterfaceNoLock();
    // Jolt measures body 2 against body 1, so `b` (or the world) is body 1: angles and positions are a's.
    JPH::TwoBodyConstraint* c = bi.CreateConstraint(settings, sb ? sb->body : JPH::BodyID(), sa->body);
    if (!c) return 0;
    w.system.AddConstraint(c);

    u32 index;
    if (!w.free_joints.empty()) {
        index = w.free_joints.back();
        w.free_joints.pop_back();
    } else {
        index = static_cast<u32>(w.joints.size());
        if (index >= 0xFFFFFu) {
            w.system.RemoveConstraint(c);
            return 0;
        }
        w.joints.emplace_back();
    }
    Impl::Joint& j = w.joints[index];
    j.constraint = c;
    j.desc = d;
    j.slot_a = static_cast<u32>(sa - w.slots.data());
    j.slot_b = sb ? static_cast<u32>(sb - w.slots.data()) : 0xFFFFFFFFu;
    j.alive = j.enabled = true;
    j.broken = false;
    if (d.type == JointType3D::Distance) {
        j.local_a = bi.GetWorldTransform(sa->body).Inversed() * to_jph(d.anchor);
        j.local_b = sb ? JPH::Vec3(bi.GetWorldTransform(sb->body).Inversed() * to_jph(d.anchor_b)) : to_jph(d.anchor_b);
    }
    if (!d.collide_connected && sb) {
        bi.SetCollisionGroup(sa->body, JPH::CollisionGroup(w.pair_filter, 0, j.slot_a));
        bi.SetCollisionGroup(sb->body, JPH::CollisionGroup(w.pair_filter, 0, j.slot_b));
        ++w.pair_filter->pairs[JointPairFilter::key(j.slot_a, j.slot_b)];
        j.filtered = true;
    }
    apply_motor(c, d);
    bi.ActivateConstraint(c);
    ++w.live_joints;
    w.had_joints = true;
    return (j.generation << 20) | (index + 1);
}

void World3D::destroy_joint(u32 joint) {
    if (Impl::Joint* j = impl_->find_joint(joint)) impl_->remove_joint(*j);
}

bool World3D::has_joint(u32 joint) const { return impl_->find_joint(joint) != nullptr; }

void World3D::set_joint_motor(u32 joint, JointMotorMode3D mode, f32 target, f32 max_force) {
    Impl::Joint* j = impl_->find_joint(joint);
    if (!j || (j->desc.type != JointType3D::Hinge && j->desc.type != JointType3D::Slider)) return;
    if (j->desc.motor == mode && j->desc.motor_target == target && j->desc.motor_max_force == max_force) return;
    j->desc.motor = mode;
    j->desc.motor_target = target;
    j->desc.motor_max_force = max_force;
    apply_motor(j->constraint, j->desc);
    impl_->system.GetBodyInterfaceNoLock().ActivateConstraint(j->constraint);
}

void World3D::set_joint_enabled(u32 joint, bool enabled) {
    Impl::Joint* j = impl_->find_joint(joint);
    if (!j || j->broken || j->enabled == enabled) return;
    j->enabled = enabled;
    j->constraint->SetEnabled(enabled);
    impl_->system.GetBodyInterfaceNoLock().ActivateConstraint(j->constraint);
}

bool World3D::joint_state(u32 joint, JointState3D& out) const {
    const Impl& w = *impl_;
    const Impl::Joint* j = w.find_joint(joint);
    if (!j) return false;
    out = JointState3D{};
    out.enabled = j->enabled;
    out.broken = j->broken;
    out.force = j->force;
    out.torque = j->torque;
    switch (j->desc.type) {
    case JointType3D::Hinge: out.position = static_cast<const JPH::HingeConstraint*>(j->constraint.GetPtr())->GetCurrentAngle(); break;
    case JointType3D::Slider: out.position = static_cast<const JPH::SliderConstraint*>(j->constraint.GetPtr())->GetCurrentPosition(); break;
    case JointType3D::Distance: {
        const JPH::BodyInterface& bi = w.system.GetBodyInterfaceNoLock();
        const JPH::Vec3 pa = bi.GetWorldTransform(w.slots[j->slot_a].body) * j->local_a;
        const JPH::Vec3 pb = j->slot_b != 0xFFFFFFFFu ? JPH::Vec3(bi.GetWorldTransform(w.slots[j->slot_b].body) * j->local_b) : j->local_b;
        out.position = (pb - pa).Length();
        break;
    }
    default: break;
    }
    return true;
}

// ---- ragdolls ------------------------------------------------------------------------------------------

namespace {

// A unit vector perpendicular to `v` (a unit vector).
JPH::Vec3 perpendicular(JPH::Vec3Arg v) {
    const JPH::Vec3 other = std::fabs(v.GetX()) < 0.9f ? JPH::Vec3::sAxisX() : JPH::Vec3::sAxisY();
    return v.Cross(other).Normalized();
}

}  // namespace

u32 World3D::create_ragdoll(const RagdollDesc3D& d, Span<const RagdollPart3D> parts) {
    Impl& w = *impl_;
    if (parts.empty() || parts.size() > 0xFFFFu) return 0;
    JPH::Ref<JPH::RagdollSettings> rs = new JPH::RagdollSettings();
    rs->mSkeleton = new JPH::Skeleton();
    rs->mParts.resize(parts.size());
    JPH::Array<JPH::Mat44> poses;
    poses.reserve(parts.size());
    for (usize i = 0; i < parts.size(); ++i) {
        const RagdollPart3D& p = parts[i];
        if ((i == 0) != (p.parent < 0) || p.parent >= static_cast<i32>(i)) return 0;
        if (p.shape != ShapeType3D::Capsule && p.shape != ShapeType3D::Sphere && p.shape != ShapeType3D::Box) return 0;
        if (!positive(p.mass)) return 0;
        Shape3D shape;
        shape.type = p.shape;
        shape.radius = p.radius;
        shape.half_height = p.half_height;
        shape.half = p.half;
        JPH::RefConst<JPH::Shape> js = build_shape(shape, 1000.0f);
        if (!js) return 0;
        rs->mSkeleton->AddJoint("", p.parent);
        const JPH::Quat rot = to_jph(math::normalize(p.rotation));
        JPH::RagdollSettings::Part& part = rs->mParts[i];
        part.SetShape(js);
        part.mPosition = to_jph(p.position);
        part.mRotation = rot;
        part.mMotionType = JPH::EMotionType::Dynamic;
        part.mObjectLayer = object_layer(d.layer, d.mask, true);
        part.mFriction = d.friction;
        part.mLinearDamping = d.linear_damping;
        part.mAngularDamping = d.angular_damping;
        part.mLinearVelocity = to_jph(p.linear_velocity);
        part.mAngularVelocity = to_jph(p.angular_velocity);
        part.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        part.mMassPropertiesOverride.mMass = p.mass;
        poses.push_back(JPH::Mat44::sRotationTranslation(rot, to_jph(p.position)));
        if (p.parent < 0) continue;
        const JPH::Vec3 twist = to_jph(p.twist_axis);
        if (twist.LengthSq() < 1e-8f) return 0;
        auto* c = new JPH::SwingTwistConstraintSettings();
        c->mSpace = JPH::EConstraintSpace::WorldSpace;
        c->mPosition1 = c->mPosition2 = to_jph(p.pivot);
        c->mTwistAxis1 = c->mTwistAxis2 = twist.Normalized();
        c->mPlaneAxis1 = c->mPlaneAxis2 = perpendicular(c->mTwistAxis1);
        const f32 swing = math::clamp(p.swing, 0.0f, 3.1f), tw = math::clamp(p.twist, 0.0f, 3.1f);
        c->mNormalHalfConeAngle = c->mPlaneHalfConeAngle = swing;
        c->mTwistMinAngle = -tw;
        c->mTwistMaxAngle = tw;
        c->mMaxFrictionTorque = d.joint_friction;
        part.mToParent = c;
    }
    rs->Stabilize();
    rs->DisableParentChildCollisions(poses.data(), 0.0f);
    JPH::Ref<JPH::Ragdoll> r =
        rs->CreateRagdoll(static_cast<JPH::CollisionGroup::GroupID>(++w.ragdoll_groups), pack(d.owner), &w.system);
    if (!r) return 0;  // out of bodies
    r->AddToPhysicsSystem(JPH::EActivation::Activate);

    u32 i;
    if (!w.free_ragdolls.empty()) {
        i = w.free_ragdolls.back();
        w.free_ragdolls.pop_back();
    } else {
        i = static_cast<u32>(w.ragdolls.size());
        w.ragdolls.emplace_back();
    }
    Impl::RagdollSlot& slot = w.ragdolls[i];
    slot.ragdoll = r;
    slot.owner = d.owner;
    slot.alive = true;
    ++w.live_ragdolls;
    w.had_ragdolls = true;
    return (slot.generation << 20) | (i + 1);
}

void World3D::destroy_ragdoll(u32 ragdoll) {
    Impl& w = *impl_;
    Impl::RagdollSlot* r = w.find_ragdoll(ragdoll);
    if (!r) return;
    r->ragdoll->RemoveFromPhysicsSystem();
    const u32 gen = r->generation;
    *r = Impl::RagdollSlot{};  // releases the ragdoll: its bodies go
    r->generation = (gen + 1) & 0xFFFu;
    w.free_ragdolls.push_back(static_cast<u32>(r - w.ragdolls.data()));
    --w.live_ragdolls;
}

bool World3D::has_ragdoll(u32 ragdoll) const { return impl_->find_ragdoll(ragdoll) != nullptr; }

u32 World3D::ragdoll_part_count(u32 ragdoll) const {
    const Impl::RagdollSlot* r = impl_->find_ragdoll(ragdoll);
    return r ? static_cast<u32>(r->ragdoll->GetBodyCount()) : 0u;
}

bool World3D::ragdoll_state(u32 ragdoll, Span<BodyState3D> out) const {
    const Impl& w = *impl_;
    const Impl::RagdollSlot* r = w.find_ragdoll(ragdoll);
    if (!r || out.size() < r->ragdoll->GetBodyCount()) return false;
    const JPH::BodyInterface& bi = w.system.GetBodyInterfaceNoLock();
    for (usize i = 0; i < r->ragdoll->GetBodyCount(); ++i) {
        const JPH::BodyID id = r->ragdoll->GetBodyID(static_cast<int>(i));
        JPH::RVec3 p;
        JPH::Quat q;
        bi.GetPositionAndRotation(id, p, q);
        BodyState3D& s = out[i];
        s = BodyState3D{};
        s.entity = r->owner;
        s.type = BodyType3D::Dynamic;
        s.position = to_mce(p);
        s.rotation = to_mce(q);
        s.linear_velocity = to_mce(bi.GetLinearVelocity(id));
        s.angular_velocity = to_mce(bi.GetAngularVelocity(id));
    }
    return true;
}

void World3D::ragdoll_impulse(u32 ragdoll, i32 part, vec3 impulse) {
    Impl& w = *impl_;
    Impl::RagdollSlot* r = w.find_ragdoll(ragdoll);
    if (!r) return;
    JPH::BodyInterface& bi = w.system.GetBodyInterfaceNoLock();
    const int n = static_cast<int>(r->ragdoll->GetBodyCount());
    if (part >= 0) {
        if (part < n) bi.AddImpulse(r->ragdoll->GetBodyID(part), to_jph(impulse));
        return;
    }
    // Spread by mass: every part gets the same change of velocity.
    f32 total = 0.0f;
    for (int i = 0; i < n; ++i) {
        JPH::BodyLockRead lock(w.system.GetBodyLockInterfaceNoLock(), r->ragdoll->GetBodyID(i));
        if (lock.Succeeded()) total += 1.0f / lock.GetBody().GetMotionProperties()->GetInverseMass();
    }
    if (total <= 0.0f) return;
    const JPH::Vec3 dv = to_jph(impulse) / total;
    for (int i = 0; i < n; ++i) bi.AddLinearVelocity(r->ragdoll->GetBodyID(i), dv);
}

bool World3D::ragdoll_active(u32 ragdoll) const {
    const Impl::RagdollSlot* r = impl_->find_ragdoll(ragdoll);
    return r && r->ragdoll->IsActive(false);
}

// ---- vehicles ----------------------------------------------------------------------------------------

u32 World3D::create_vehicle(const VehicleDesc3D& d) {
    Impl& w = *impl_;
    Impl::Slot* s = w.find(d.body);
    if (!s || s->character || s->type != BodyType3D::Dynamic || d.wheels.empty()) return 0;
    const u32 slot = static_cast<u32>(s - w.slots.data());
    for (const Impl::Vehicle& v : w.vehicles)
        if (v.alive && v.slot == slot) return 0;
    const JPH::Vec3 up = to_jph(d.up), forward = to_jph(d.forward);
    if (up.LengthSq() < 1e-8f || forward.LengthSq() < 1e-8f || std::fabs(up.Normalized().Dot(forward.Normalized())) > 0.01f) return 0;
    if (!positive(d.engine_torque) || !positive(d.max_rpm) || d.min_rpm < 0.0f || d.min_rpm >= d.max_rpm) return 0;

    JPH::VehicleConstraintSettings vs;
    vs.mUp = up.Normalized();
    vs.mForward = forward.Normalized();
    vs.mMaxPitchRollAngle = math::clamp(d.max_tilt, 0.0f, JPH::JPH_PI);
    for (const WheelDesc3D& wd : d.wheels) {
        if (!positive(wd.radius) || !positive(wd.width) || wd.suspension_min < 0.0f || wd.suspension_max <= wd.suspension_min ||
            !positive(wd.suspension_frequency) || wd.grip < 0.0f)
            return 0;
        auto* ws = new JPH::WheelSettingsWV();
        ws->mPosition = to_jph(wd.position);
        ws->mSuspensionDirection = -vs.mUp;
        ws->mSteeringAxis = vs.mUp;
        ws->mWheelUp = vs.mUp;
        ws->mWheelForward = vs.mForward;
        ws->mSuspensionMinLength = wd.suspension_min;
        ws->mSuspensionMaxLength = wd.suspension_max;
        ws->mSuspensionSpring.mFrequency = wd.suspension_frequency;
        ws->mSuspensionSpring.mDamping = math::clamp(wd.suspension_damping, 0.0f, 1.0f);
        ws->mRadius = wd.radius;
        ws->mWidth = wd.width;
        ws->mMaxSteerAngle = math::clamp(wd.max_steer, 0.0f, 1.5f);
        ws->mMaxBrakeTorque = math::max(wd.max_brake_torque, 0.0f);
        ws->mMaxHandBrakeTorque = math::max(wd.max_handbrake_torque, 0.0f);
        for (JPH::LinearCurve::Point& p : ws->mLongitudinalFriction.mPoints) p.mY *= wd.grip;
        for (JPH::LinearCurve::Point& p : ws->mLateralFriction.mPoints) p.mY *= wd.grip;
        vs.mWheels.push_back(ws);
    }
    auto* cs = new JPH::WheeledVehicleControllerSettings();
    cs->mEngine.mMaxTorque = d.engine_torque;
    cs->mEngine.mMinRPM = d.min_rpm;
    cs->mEngine.mMaxRPM = d.max_rpm;
    if (!d.gears.empty()) {
        cs->mTransmission.mGearRatios.clear();
        for (const f32 g : d.gears) {
            if (!positive(g)) return 0;
            cs->mTransmission.mGearRatios.push_back(g);
        }
    }
    if (!(d.reverse_gear < 0.0f)) return 0;
    cs->mTransmission.mReverseGearRatios = {d.reverse_gear};
    const i32 wheel_count = static_cast<i32>(d.wheels.size());
    for (const AxleDesc3D& a : d.axles) {
        if (a.left < -1 || a.left >= wheel_count || a.right < -1 || a.right >= wheel_count || (a.left < 0 && a.right < 0) ||
            !positive(a.ratio) || a.torque_share < 0.0f)
            return 0;
        JPH::VehicleDifferentialSettings ds;
        ds.mLeftWheel = a.left;
        ds.mRightWheel = a.right;
        ds.mDifferentialRatio = a.ratio;
        ds.mEngineTorqueRatio = a.torque_share;
        cs->mDifferentials.push_back(ds);
    }
    vs.mController = cs;

    JPH::Ref<JPH::VehicleConstraint> c;
    JPH::ObjectLayer layer = 0;
    {
        JPH::BodyLockWrite lock(w.system.GetBodyLockInterfaceNoLock(), s->body);
        if (!lock.Succeeded()) return 0;
        c = new JPH::VehicleConstraint(lock.GetBody(), vs);
        layer = lock.GetBody().GetObjectLayer();
    }
    JPH::Ref<JPH::VehicleCollisionTester> tester = new JPH::VehicleCollisionTesterCastCylinder(layer);
    c->SetVehicleCollisionTester(tester);
    w.system.AddConstraint(c);
    w.system.AddStepListener(c);

    u32 i;
    if (!w.free_vehicles.empty()) {
        i = w.free_vehicles.back();
        w.free_vehicles.pop_back();
    } else {
        i = static_cast<u32>(w.vehicles.size());
        w.vehicles.emplace_back();
    }
    Impl::Vehicle& v = w.vehicles[i];
    v.constraint = c;
    v.tester = tester;
    v.slot = slot;
    v.alive = true;
    ++w.live_vehicles;
    w.had_vehicles = true;
    return (v.generation << 20) | (i + 1);
}

void World3D::destroy_vehicle(u32 vehicle) {
    Impl& w = *impl_;
    if (Impl::Vehicle* v = w.find_vehicle(vehicle)) w.remove_vehicle(*v);
}

bool World3D::has_vehicle(u32 vehicle) const { return impl_->find_vehicle(vehicle) != nullptr; }

void World3D::set_vehicle_input(u32 vehicle, f32 forward, f32 right, f32 brake, f32 handbrake) {
    Impl& w = *impl_;
    Impl::Vehicle* v = w.find_vehicle(vehicle);
    if (!v) return;
    auto* ctl = static_cast<JPH::WheeledVehicleController*>(v->constraint->GetController());
    forward = math::clamp(forward, -1.0f, 1.0f);
    right = math::clamp(right, -1.0f, 1.0f);
    brake = math::clamp(brake, 0.0f, 1.0f);
    handbrake = math::clamp(handbrake, 0.0f, 1.0f);
    ctl->SetDriverInput(forward, right, brake, handbrake);
    if (forward != 0.0f || right != 0.0f || brake != 0.0f || handbrake != 0.0f)
        w.system.GetBodyInterfaceNoLock().ActivateBody(w.slots[v->slot].body);
}

bool World3D::vehicle_state(u32 vehicle, VehicleState3D& out, Span<WheelState3D> wheels) const {
    const Impl& w = *impl_;
    const Impl::Vehicle* v = w.find_vehicle(vehicle);
    if (!v) return false;
    const JPH::VehicleConstraint& c = *v->constraint;
    const auto* ctl = static_cast<const JPH::WheeledVehicleController*>(c.GetController());
    const JPH::Body& body = *c.GetVehicleBody();
    out = VehicleState3D{};
    out.speed = body.GetLinearVelocity().Dot(body.GetRotation() * c.GetLocalForward());
    out.rpm = ctl->GetEngine().GetCurrentRPM();
    out.gear = ctl->GetTransmission().GetCurrentGear();
    const usize n = c.GetWheels().size();
    for (usize i = 0; i < n; ++i) {
        const JPH::Wheel* wheel = c.GetWheel(static_cast<JPH::uint>(i));
        if (wheel->HasContact()) ++out.wheels_on_ground;
        if (i >= wheels.size()) continue;
        WheelState3D& ws = wheels[i];
        ws = WheelState3D{};
        ws.contact = wheel->HasContact();
        if (ws.contact) {
            JPH::BodyLockRead lock(w.system.GetBodyLockInterfaceNoLock(), wheel->GetContactBodyID());
            if (lock.Succeeded() && lock.GetBody().GetUserData() != 0) ws.ground = unpack(lock.GetBody().GetUserData());
        }
        ws.suspension = wheel->GetSuspensionLength();
        ws.spin = wheel->GetAngularVelocity();
        ws.steer = wheel->GetSteerAngle();
        const JPH::RMat44 m = c.GetWheelWorldTransform(static_cast<JPH::uint>(i), JPH::Vec3::sAxisX(), JPH::Vec3::sAxisY());
        ws.position = to_mce(m.GetTranslation());
        ws.rotation = to_mce(m.GetQuaternion());
    }
    return true;
}

std::optional<ShapeHit3D> World3D::camera_sweep(vec3 from, vec3 to, f32 sphere_radius) const {
    QueryFilter3D f;
    f.include_dynamic = false;
    return shape_cast(sphere_radius, from, to, f);
}

std::optional<RayHit3D> World3D::raycast(vec3 origin, vec3 dir, f32 max_dist, const QueryFilter3D& f) const {
    const Impl& w = *impl_;
    const vec3 d = math::normalize(dir);
    if (!(max_dist > 0.0f) || math::length_sq(d) == 0.0f) return std::nullopt;

    const JPH::RRayCast ray(to_jph(origin), to_jph(d * max_dist));
    JPH::RayCastSettings settings;
    JPH::AllHitCollisionCollector<JPH::CastRayCollector> hits;
    const LayerFilter layers(f.layer_mask);
    const EntityFilter bodies(f);
    w.system.GetNarrowPhaseQueryNoLock().CastRay(ray, settings, hits, {}, layers, bodies);

    // Closest hit; equal fractions resolve by creation slot so the answer never depends on Jolt's
    // broad-phase traversal order.
    const JPH::RayCastResult* best = nullptr;
    u32 best_slot = 0;
    for (const JPH::RayCastResult& h : hits.mHits) {
        const u32 slot = w.slot_index(unpack(w.system.GetBodyInterfaceNoLock().GetUserData(h.mBodyID)));
        if (!best || h.mFraction < best->mFraction || (h.mFraction == best->mFraction && slot < best_slot)) {
            best = &h;
            best_slot = slot;
        }
    }
    if (!best) return std::nullopt;

    RayHit3D r;
    const JPH::RVec3 point = ray.GetPointOnRay(best->mFraction);
    JPH::BodyLockRead lock(w.system.GetBodyLockInterfaceNoLock(), best->mBodyID);
    if (!lock.Succeeded()) return std::nullopt;
    r.entity = unpack(lock.GetBody().GetUserData());
    r.point = to_mce(point);
    r.normal = to_mce(lock.GetBody().GetWorldSpaceSurfaceNormal(best->mSubShapeID2, point));
    r.t = best->mFraction * max_dist;
    return r;
}

std::optional<ShapeHit3D> World3D::shape_cast(f32 sphere_radius, vec3 from, vec3 to, const QueryFilter3D& f) const {
    const Impl& w = *impl_;
    if (!positive(sphere_radius)) return std::nullopt;
    const vec3 motion = to - from;

    const JPH::SphereShape sphere(sphere_radius);
    const JPH::RShapeCast cast =
        JPH::RShapeCast::sFromWorldTransform(&sphere, JPH::Vec3::sOne(), JPH::RMat44::sTranslation(to_jph(from)), to_jph(motion));
    JPH::ShapeCastSettings settings;
    JPH::AllHitCollisionCollector<JPH::CastShapeCollector> hits;
    const LayerFilter layers(f.layer_mask);
    const EntityFilter bodies(f);
    w.system.GetNarrowPhaseQueryNoLock().CastShape(cast, settings, JPH::RVec3::sZero(), hits, {}, layers, bodies);

    const JPH::ShapeCastResult* best = nullptr;
    u32 best_slot = 0;
    for (const JPH::ShapeCastResult& h : hits.mHits) {
        const u32 slot = w.slot_index(unpack(w.system.GetBodyInterfaceNoLock().GetUserData(h.mBodyID2)));
        if (!best || h.mFraction < best->mFraction || (h.mFraction == best->mFraction && slot < best_slot)) {
            best = &h;
            best_slot = slot;
        }
    }
    if (!best) return std::nullopt;

    ShapeHit3D r;
    r.entity = unpack(w.system.GetBodyInterfaceNoLock().GetUserData(best->mBodyID2));
    r.point = to_mce(best->mContactPointOn2);
    r.normal = math::normalize(-to_mce(best->mPenetrationAxis));
    r.t = best->mFraction;
    r.position = from + motion * best->mFraction;
    return r;
}

u64 World3D::hash_state(u64 seed) const {
    const Impl& w = *impl_;
    const JPH::BodyInterface& bi = w.system.GetBodyInterfaceNoLock();
    Hasher64 h(seed);
    const auto v3 = [&h](JPH::Vec3Arg v) {
        h.f32v(v.GetX());
        h.f32v(v.GetY());
        h.f32v(v.GetZ());
    };
    h.u32v(static_cast<u32>(w.slots.size()));
    for (const Impl::Slot& s : w.slots) {
        h.u32v(s.alive ? 1u : 0u);
        if (!s.alive) continue;
        h.u32v(s.entity.index);
        h.u32v(s.entity.generation);
        h.u32v(static_cast<u32>(s.type) | (s.character ? 0x100u : 0u));
        if (s.character) {
            v3(s.character->GetPosition());
            v3(s.character->GetLinearVelocity());
            h.u32v(static_cast<u32>(s.character->GetGroundState()));
            h.u32v(s.last.flags);
            continue;
        }
        JPH::RVec3 p;
        JPH::Quat q;
        bi.GetPositionAndRotation(s.body, p, q);
        v3(p);
        h.f32v(q.GetX());
        h.f32v(q.GetY());
        h.f32v(q.GetZ());
        h.f32v(q.GetW());
        v3(bi.GetLinearVelocity(s.body));
        v3(bi.GetAngularVelocity(s.body));
    }
    if (w.had_joints) {  // a world that never had a joint keeps its pre-joint hashes
        h.u32v(static_cast<u32>(w.joints.size()));
        for (const Impl::Joint& j : w.joints)
            h.u32v((j.alive ? 1u : 0u) | (j.enabled ? 2u : 0u) | (j.broken ? 4u : 0u) | (j.slot_a << 3));
    }
    if (w.had_vehicles) {  // likewise for vehicles: the drivetrain and each wheel
        h.u32v(static_cast<u32>(w.vehicles.size()));
        for (const Impl::Vehicle& v : w.vehicles) {
            h.u32v(v.alive ? 1u : 0u);
            if (!v.alive) continue;
            h.u32v(v.slot);
            const auto* ctl = static_cast<const JPH::WheeledVehicleController*>(v.constraint->GetController());
            h.f32v(ctl->GetEngine().GetCurrentRPM());
            h.u32v(static_cast<u32>(ctl->GetTransmission().GetCurrentGear()));
            h.f32v(ctl->GetTransmission().GetClutchFriction());
            for (const JPH::Wheel* wheel : v.constraint->GetWheels()) {
                h.f32v(wheel->GetAngularVelocity());
                h.f32v(wheel->GetRotationAngle());
                h.f32v(wheel->GetSteerAngle());
                h.f32v(wheel->GetSuspensionLength());
            }
        }
    }
    if (w.had_ragdolls) {  // likewise for ragdolls
        h.u32v(static_cast<u32>(w.ragdolls.size()));
        for (const Impl::RagdollSlot& r : w.ragdolls) {
            h.u32v(r.alive ? 1u : 0u);
            if (!r.alive) continue;
            for (const JPH::BodyID id : r.ragdoll->GetBodyIDs()) {
                JPH::RVec3 p;
                JPH::Quat q;
                bi.GetPositionAndRotation(id, p, q);
                v3(p);
                h.f32v(q.GetX());
                h.f32v(q.GetY());
                h.f32v(q.GetZ());
                h.f32v(q.GetW());
                v3(bi.GetLinearVelocity(id));
                v3(bi.GetAngularVelocity(id));
            }
        }
    }
    return h.value();
}

World3D::Stats World3D::stats() const {
    const Impl& w = *impl_;
    Stats st;
    st.bodies = w.system.GetNumBodies();
    st.active_bodies = w.system.GetNumActiveBodies(JPH::EBodyType::RigidBody);
    st.characters = w.characters;
    st.joints = w.live_joints;
    st.ragdolls = w.live_ragdolls;
    st.vehicles = w.live_vehicles;
    return st;
}

}  // namespace mce::physics

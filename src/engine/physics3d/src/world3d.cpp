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
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

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
    }
    if (!settings) return nullptr;
    JPH::ShapeSettings::ShapeResult r = settings->Create();
    if (r.HasError()) return nullptr;
    return r.Get();
}

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

    explicit Impl(const PhysicsConfig3D& c) : config(c), temp(static_cast<JPH::uint>(c.temp_allocator_bytes)) {
        system.Init(c.max_bodies, 0, c.max_body_pairs, c.max_contact_constraints, bp_layers, object_vs_bp,
                    object_pairs);
        system.SetGravity(to_jph(c.gravity));
    }

    ~Impl() {
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
    if (desc.shape.type == ShapeType3D::Mesh && desc.type == BodyType3D::Dynamic) return false;  // Jolt: no dynamic meshes
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
    return h.value();
}

World3D::Stats World3D::stats() const {
    const Impl& w = *impl_;
    Stats st;
    st.bodies = w.system.GetNumBodies();
    st.active_bodies = w.system.GetNumActiveBodies(JPH::EBodyType::RigidBody);
    st.characters = w.characters;
    return st;
}

}  // namespace mce::physics

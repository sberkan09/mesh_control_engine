#include "mce/kit/skeleton2d.h"

#include <cmath>
#include <cstring>

#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/log.h"

namespace mce::kit {

// World::state_hash hashes component bytes: no padding.
static_assert(sizeof(Bone2DPose) == 20);
static_assert(sizeof(Bone2DTransform) == 24);
static_assert(sizeof(Skeleton2D) == 2872);
static_assert(sizeof(Bone2DFollow) == 64);

// ------------------------------------------------------------------ pose math

Bone2DTransform Bone2DTransform::trs(math::vec2 t, f32 radians, math::vec2 s) {
    const f32 cs = std::cos(radians), sn = std::sin(radians);
    Bone2DTransform m;
    m.a = cs * s.x;
    m.b = sn * s.x;
    m.c = -sn * s.y;
    m.d = cs * s.y;
    m.x = t.x;
    m.y = t.y;
    return m;
}

Bone2DTransform operator*(const Bone2DTransform& p, const Bone2DTransform& q) {
    Bone2DTransform m;
    m.a = p.a * q.a + p.c * q.b;
    m.b = p.b * q.a + p.d * q.b;
    m.c = p.a * q.c + p.c * q.d;
    m.d = p.b * q.c + p.d * q.d;
    m.x = p.a * q.x + p.c * q.y + p.x;
    m.y = p.b * q.x + p.d * q.y + p.y;
    return m;
}

Bone2DTransform bone2d_local(const Bone2DPose& p) {
    return Bone2DTransform::trs({p.x, p.y}, p.rotation, {p.scale_x, p.scale_y});
}

Bone2DPose bone2d_offset(const asset::Skeleton2DBone& rest, const Bone2DPose& o) {
    Bone2DPose p;
    p.x = rest.x + o.x;
    p.y = rest.y + o.y;
    p.rotation = rest.rotation + o.rotation;
    p.scale_x = rest.scale_x * o.scale_x;
    p.scale_y = rest.scale_y * o.scale_y;
    return p;
}

void skeleton2d_world_pose(Span<const asset::Skeleton2DBone> bones, Span<const Bone2DPose> local,
                           Span<Bone2DTransform> world) {
    const usize n = bones.size() < local.size() ? (bones.size() < world.size() ? bones.size() : world.size())
                                                : (local.size() < world.size() ? local.size() : world.size());
    for (usize i = 0; i < n; ++i) {
        const Bone2DTransform l = bone2d_local(local[i]);
        const u16 parent = bones[i].parent;
        world[i] = parent != asset::kSkeleton2DNoParent && parent < i ? world[parent] * l : l;
    }
}

void skeleton2d_pose_offsets(Span<const asset::Skeleton2DBone> bones, Span<const Bone2DPose> offsets,
                             Span<Bone2DTransform> world) {
    Bone2DPose local[kSkeleton2DBones];
    const usize n = bones.size() < kSkeleton2DBones ? bones.size() : kSkeleton2DBones;
    for (usize i = 0; i < n; ++i) local[i] = bone2d_offset(bones[i], i < offsets.size() ? offsets[i] : Bone2DPose{});
    skeleton2d_world_pose(bones.subspan(0, n), Span<const Bone2DPose>(local, n), world);
}

Bone2DTransform transform_2d(const math::vec3& position, const math::quat& rotation, const math::vec3& scale) {
    return Bone2DTransform::trs({position.x, position.y}, angle_z(rotation), {scale.x, scale.y});
}

void skeleton2d_quads(const Skeleton2DRegistry::Def& def, const Skeleton2D& sk, const Bone2DTransform& entity,
                      Array<Skeleton2DQuad>& out) {
    if (!sk.visible) return;
    Bone2DTransform world[kSkeleton2DBones];
    skeleton2d_pose_offsets(def.bones.span(), Span<const Bone2DPose>(sk.pose, kSkeleton2DBones),
                            Span<Bone2DTransform>(world, kSkeleton2DBones));
    // Flips mirror the skeleton about the entity's origin.
    const Bone2DTransform placed =
        entity * Bone2DTransform::trs({}, 0.0f, {sk.flip_x ? -1.0f : 1.0f, sk.flip_y ? -1.0f : 1.0f});
    const usize bone_count = def.bones.size() < kSkeleton2DBones ? def.bones.size() : kSkeleton2DBones;
    for (u32 i = 0; i < def.slots.size(); ++i) {
        const asset::Skeleton2DSlot& s = def.slots[i];
        if (s.bone >= bone_count) continue;
        const Bone2DTransform m =
            placed * world[s.bone] * Bone2DTransform::trs({s.offset[0], s.offset[1]}, s.rotation, {1.0f, 1.0f});
        // Decompose into rotation and axis lengths (a sprite has no shear): the +x axis gives the
        // angle, the determinant the signed +y length.
        const f32 sx = std::sqrt(m.a * m.a + m.b * m.b);
        const f32 det = m.a * m.d - m.b * m.c;
        Skeleton2DQuad q;
        q.texture = s.texture_guid();
        q.rect = {s.rect[0], s.rect[1], s.rect[2], s.rect[3]};
        q.position = {m.x, m.y};
        q.rotation = sx > 0.0f ? std::atan2(m.b, m.a) : 0.0f;
        const f32 sy = sx > 0.0f ? det / sx : std::sqrt(m.c * m.c + m.d * m.d);
        q.scale = {s.size[0] * sx, s.size[1] * sy};
        q.pivot = {s.pivot[0], s.pivot[1]};
        q.tint = {s.tint[0] * sk.tint.r, s.tint[1] * sk.tint.g, s.tint[2] * sk.tint.b, s.tint[3] * sk.tint.a};
        q.slot = i;
        out.push_back(q);
    }
}

// ------------------------------------------------------------------ registry

StringView Skeleton2DRegistry::Def::string(u32 offset) const {
    if (offset >= strings.size()) return {};
    usize n = 0;
    while (offset + n < strings.size() && strings[offset + n] != '\0') ++n;
    return StringView(strings.data() + offset, n);
}

i32 Skeleton2DRegistry::Def::find_bone(StringView n) const {
    for (usize i = 0; i < bones.size(); ++i)
        if (string(bones[i].name) == n) return static_cast<i32>(i);
    return -1;
}

namespace {

Result<void> copy_def(Skeleton2DRegistry::Def& d, asset::Guid guid, Span<const byte> payload) {
    Result<asset::Skeleton2DView> view = asset::Skeleton2DView::read(payload);
    if (!view) return view.error();
    const asset::Skeleton2DView& v = view.value();
    d.guid = guid;
    d.name.assign(v.name());
    d.bones.clear();
    d.slots.clear();
    d.strings.clear();
    for (const asset::Skeleton2DBone& b : v.bones) d.bones.push_back(b);
    for (const asset::Skeleton2DSlot& s : v.slots) d.slots.push_back(s);
    for (char c : v.strings) d.strings.push_back(c);
    return {};
}

}  // namespace

Result<void> Skeleton2DRegistry::define(asset::Guid guid, Span<const byte> payload) {
    Def d;
    if (Result<void> r = copy_def(d, guid, payload); !r) return r;
    for (Def& existing : defs_)
        if (existing.guid == guid) {
            existing = static_cast<Def&&>(d);
            return {};
        }
    defs_.push_back(static_cast<Def&&>(d));
    return {};
}

const Skeleton2DRegistry::Def* Skeleton2DRegistry::resolve(asset::Guid guid) {
    if (guid == asset::Guid{}) return nullptr;
    Def* cached = nullptr;
    for (Def& d : defs_)
        if (d.guid == guid) cached = &d;
    if (cached &&
        (!assets_ || cached->key == asset::kInvalidAssetKey || assets_->generation(cached->key) == cached->generation))
        return cached;
    if (!assets_) return nullptr;
    const bool fresh = cached == nullptr;
    if (fresh) {
        defs_.push_back(Def{});
        cached = &defs_.back();
    }
    const asset::AssetKey key = assets_->find(guid);
    bool loaded = false;
    if (key != asset::kInvalidAssetKey) {
        if (Result<asset::AssetHandle<asset::Skeleton2D>> h = assets_->load_sync<asset::Skeleton2D>(key)) {
            loaded = static_cast<bool>(copy_def(*cached, guid, assets_->resolve(h.value())));
            cached->key = key;
            cached->generation = h.value().generation;
            assets_->release(asset::AnyHandle(h.value()));
        }
    }
    if (loaded) return cached;
    if (fresh) defs_.pop_back();
    bool warned = false;
    for (const asset::Guid& g : warned_) warned |= g == guid;
    if (!warned) {
        MCE_LOG_WARN("kit: skeleton {} cannot be loaded; its Skeleton2D components stay empty",
                     asset::to_string(guid).view());
        warned_.push_back(guid);
    }
    return nullptr;
}

// ------------------------------------------------------------------ system

namespace {

// Where an entity stands: its WorldTransform when the hierarchy wrote one, else its LocalTransform.
bool placement_2d(World& w, Entity e, Bone2DTransform& out) {
    if (const WorldTransform* t = w.try_get<WorldTransform>(e)) {
        out = transform_2d(t->position, t->rotation, t->scale);
        return true;
    }
    if (const LocalTransform* t = w.try_get<LocalTransform>(e)) {
        out = transform_2d(t->position, t->rotation, t->scale);
        return true;
    }
    return false;
}

}  // namespace

void kit_skeleton2d(Frame& f) {
    Skeleton2DRegistry* reg = f.try_resource<Skeleton2DRegistry>();
    if (!reg) return;
    f.query<Skeleton2D>().each([&](Entity, Skeleton2D& sk) {
        const Skeleton2DRegistry::Def* def = reg->resolve(sk.skeleton.guid);
        const u32 n =
            def ? static_cast<u32>(def->bones.size() < kSkeleton2DBones ? def->bones.size() : kSkeleton2DBones) : 0u;
        Bone2DTransform world[kSkeleton2DBones];
        if (def)
            skeleton2d_pose_offsets(def->bones.span(), Span<const Bone2DPose>(sk.pose, kSkeleton2DBones),
                                    Span<Bone2DTransform>(world, kSkeleton2DBones));
        for (u32 i = n; i < kSkeleton2DBones; ++i) world[i] = Bone2DTransform{};
        sk.bone_count = n;
        std::memcpy(sk.world, world, sizeof(world));
    });

    // Followers, after every skeleton is posed (ECS order).
    struct Follow {
        Entity e;
        Bone2DFollow f;
    };
    Array<Follow> follows;
    f.query<const Bone2DFollow, const LocalTransform>().each(
        [&](Entity e, const Bone2DFollow& b, const LocalTransform&) { follows.push_back(Follow{e, b}); });
    World& w = f.world();
    for (const Follow& fl : follows) {
        Bone2DTransform bone;
        if (!w.alive(fl.f.skeleton) || !skeleton2d_bone(w, fl.f.skeleton, fl.f.bone.view(), bone)) continue;
        const Skeleton2D* sk = w.try_get<Skeleton2D>(fl.f.skeleton);
        Bone2DTransform m = Bone2DTransform::trs({}, 0.0f, {sk->flip_x ? -1.0f : 1.0f, sk->flip_y ? -1.0f : 1.0f}) *
                            bone * Bone2DTransform::trs({fl.f.offset.x, fl.f.offset.y}, fl.f.rotation, {1.0f, 1.0f});
        const ChildOf* parent = w.try_get<ChildOf>(fl.e);
        if (!parent || !(parent->parent == fl.f.skeleton)) {
            Bone2DTransform at;
            if (placement_2d(w, fl.f.skeleton, at)) m = at * m;
        }
        const f32 sx = std::sqrt(m.a * m.a + m.b * m.b);
        const f32 sy = sx > 0.0f ? (m.a * m.d - m.b * m.c) / sx : std::sqrt(m.c * m.c + m.d * m.d);
        LocalTransform* t = w.try_get_mut<LocalTransform>(fl.e);
        if (!t) continue;
        t->position.x = m.x;
        t->position.y = m.y;
        if (fl.f.follow_rotation) t->rotation = rotation_z(sx > 0.0f ? std::atan2(m.b, m.a) : 0.0f);
        if (fl.f.follow_scale) {
            t->scale.x = sx;
            t->scale.y = sy;
        }
    }
}

bool skeleton2d_bone(World& world, Entity e, StringView bone, Bone2DTransform& out) {
    const Skeleton2D* sk = world.try_get<Skeleton2D>(e);
    Skeleton2DRegistry* reg = world.try_resource<Skeleton2DRegistry>();
    if (!sk || !reg) return false;
    const Skeleton2DRegistry::Def* def = reg->resolve(sk->skeleton.guid);
    if (!def) return false;
    const i32 i = def->find_bone(bone);
    if (i < 0 || static_cast<u32>(i) >= sk->bone_count) return false;
    out = sk->world[i];
    return true;
}

void install_skeleton2d(World& world, asset::AssetSystem* assets) {
    if (Skeleton2DRegistry* r = world.try_resource<Skeleton2DRegistry>()) {
        r->set_assets(assets);
        return;
    }
    world.register_components<Skeleton2D, Bone2DFollow>();
    world.set_resource(Skeleton2DRegistry(assets));
    world.add_system(Phase::FixedUpdate_Post, "kit_skeleton2d", &kit_skeleton2d);
    world.run_during_cutscenes(Phase::FixedUpdate_Post, "kit_skeleton2d");
}

}  // namespace mce::kit

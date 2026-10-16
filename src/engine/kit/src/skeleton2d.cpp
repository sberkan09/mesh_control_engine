#include "mce/kit/skeleton2d.h"

#include <cmath>
#include <cstring>

#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/kit/anim3d.h"
#include "mce/log.h"
#include "mce/string_id.h"

namespace mce::kit {

// World::state_hash hashes component bytes: no padding.
static_assert(sizeof(Bone2DPose) == 20);
static_assert(sizeof(Bone2DTransform) == 24);
static_assert(sizeof(Skeleton2D) == 2872);
static_assert(sizeof(Bone2DFollow) == 64);
static_assert(sizeof(Skeleton2DSlots) == 4 + kSkeleton2DSlotCount + kSkeleton2DSlotCount * sizeof(math::Color));

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

namespace {

// A quad attachment as a sprite: placed at m * (offset, rotation), decomposed into rotation and
// axis lengths (a sprite has no shear): the +x axis gives the angle, the determinant the signed
// +y length.
Skeleton2DQuad quad_of(const asset::Skeleton2DAttachment& s, const Bone2DTransform& bone) {
    const Bone2DTransform m = bone * Bone2DTransform::trs({s.offset[0], s.offset[1]}, s.rotation, {1.0f, 1.0f});
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
    return q;
}

}  // namespace

void skeleton2d_slots_setup(const Skeleton2DRegistry::Def& def, Skeleton2DSlots& slots) {
    const u32 n = def.slots.size() < kSkeleton2DSlotCount ? static_cast<u32>(def.slots.size()) : kSkeleton2DSlotCount;
    for (u32 i = 0; i < kSkeleton2DSlotCount; ++i) {
        if (i < n) {
            const asset::Skeleton2DSlot& s = def.slots[i];
            slots.attachment[i] = s.attachment;
            slots.color[i] = {s.tint[0], s.tint[1], s.tint[2], s.tint[3]};
        } else {
            slots.attachment[i] = 0;
            slots.color[i] = math::Color{};
        }
    }
    slots.slot_count = n;
}

void skeleton2d_draws(const Skeleton2DRegistry::Def& def, const Skeleton2D& sk, const Skeleton2DSlots* slots,
                      const Skeleton2DDeform* deform, const Bone2DTransform& entity, Skeleton2DDraws& out) {
    if (!sk.visible) return;
    Bone2DTransform world[kSkeleton2DBones];
    skeleton2d_pose_offsets(def.bones.span(), Span<const Bone2DPose>(sk.pose, kSkeleton2DBones),
                            Span<Bone2DTransform>(world, kSkeleton2DBones));
    // Flips mirror the skeleton about the entity's origin.
    const Bone2DTransform placed =
        entity * Bone2DTransform::trs({}, 0.0f, {sk.flip_x ? -1.0f : 1.0f, sk.flip_y ? -1.0f : 1.0f});
    const usize bone_count = def.bones.size() < kSkeleton2DBones ? def.bones.size() : kSkeleton2DBones;
    const bool live = slots && slots->slot_count != 0 && slots->slot_count == def.slots.size();
    for (u32 i = 0; i < def.slots.size(); ++i) {
        const asset::Skeleton2DSlot& s = def.slots[i];
        if (s.bone >= bone_count) continue;
        const u32 shown = live ? slots->attachment[i] : s.attachment;
        if (shown >= s.attachment_count) continue;  // none (255) or out of the slot's list
        const math::Color c = live ? slots->color[i] : math::Color{s.tint[0], s.tint[1], s.tint[2], s.tint[3]};
        const math::Color tint{c.r * sk.tint.r, c.g * sk.tint.g, c.b * sk.tint.b, c.a * sk.tint.a};
        if (!(tint.a > 0.0f)) continue;  // alpha 0 hides the slot
        const u32 ai = s.first_attachment + shown;
        if (ai >= def.attachments.size()) continue;
        const asset::Skeleton2DAttachment& a = def.attachments[ai];
        const Bone2DTransform bone = placed * world[s.bone];
        if (a.kind == asset::Skeleton2DAttachmentKind::Quad) {
            Skeleton2DQuad q = quad_of(a, bone);
            q.tint = tint;
            q.slot = i;
            out.quads.push_back(q);
            continue;
        }
        // A mesh: each vertex in the slot bone's space, or the weighted sum of its bones' world
        // transforms of its weight positions, in the order listed. The deform offset adds to the
        // local position(s) first, so it turns with the bones.
        if (u64(a.first_vertex) + a.vertex_count > def.vertices.size() || u64(a.first_index) + a.index_count > def.indices.size())
            continue;
        const f32* d = deform ? deform->of(ai) : nullptr;
        Skeleton2DMesh m;
        m.texture = a.texture_guid();
        m.tint = tint;
        m.rotation = std::atan2(bone.b, bone.a);
        m.mirrored = bone.a * bone.d - bone.b * bone.c < 0.0f;
        m.slot = i;
        m.first_vertex = static_cast<u32>(out.vertices.size());
        m.vertex_count = a.vertex_count;
        m.first_index = static_cast<u32>(out.indices.size());
        m.index_count = a.index_count;
        for (u32 v = 0; v < a.vertex_count; ++v) {
            const asset::Skeleton2DVertex& vx = def.vertices[a.first_vertex + v];
            const f32 dx = d ? d[2 * v] : 0.0f;
            const f32 dy = d ? d[2 * v + 1] : 0.0f;
            math::vec2 p;
            if (vx.weight_count == 0) {
                p = bone.apply({vx.x + dx, vx.y + dy});
            } else {
                f32 px = 0.0f, py = 0.0f;
                for (u32 k = 0; k < vx.weight_count && vx.first_weight + k < def.weights.size(); ++k) {
                    const asset::Skeleton2DWeight& w = def.weights[vx.first_weight + k];
                    const math::vec2 q =
                        w.bone < bone_count ? world[w.bone].apply({w.x + dx, w.y + dy}) : math::vec2{w.x + dx, w.y + dy};
                    px = px + w.weight * q.x;
                    py = py + w.weight * q.y;
                }
                p = placed.apply({px, py});
            }
            out.vertices.push_back(Skeleton2DMeshVertex{p, {vx.u, vx.v}});
        }
        for (u32 k = 0; k < a.index_count; ++k) out.indices.push_back(def.indices[a.first_index + k]);
        out.meshes.push_back(m);
    }
}

void skeleton2d_quads(const Skeleton2DRegistry::Def& def, const Skeleton2D& sk, const Bone2DTransform& entity,
                      Array<Skeleton2DQuad>& out) {
    Skeleton2DDraws d;
    skeleton2d_draws(def, sk, nullptr, nullptr, entity, d);
    for (const Skeleton2DQuad& q : d.quads) out.push_back(q);
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

i32 Skeleton2DRegistry::Def::find_slot(StringView n) const {
    for (usize i = 0; i < slots.size(); ++i)
        if (string(slots[i].name) == n) return static_cast<i32>(i);
    return -1;
}

i32 Skeleton2DRegistry::Def::find_attachment(u32 slot, StringView n) const {
    if (slot >= slots.size()) return -1;
    const asset::Skeleton2DSlot& s = slots[slot];
    for (u32 i = 0; i < s.attachment_count; ++i)
        if (s.first_attachment + i < attachments.size() && string(attachments[s.first_attachment + i].name) == n)
            return static_cast<i32>(i);
    return -1;
}

i32 Skeleton2DRegistry::Def::find_animation(u64 id) const {
    if (id == 0) return -1;
    for (usize i = 0; i < animation_ids.size(); ++i)
        if (animation_ids[i] == id) return static_cast<i32>(i);
    return -1;
}

f32 Skeleton2DRegistry::Def::duration(u64 id) const {
    const i32 i = find_animation(id);
    return i < 0 ? 0.0f : animations[static_cast<usize>(i)].duration;
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
    d.animations.clear();
    d.timelines.clear();
    d.keys.clear();
    d.events.clear();
    d.animation_ids.clear();
    d.strings.clear();
    d.attachments.clear();
    d.vertices.clear();
    d.weights.clear();
    d.indices.clear();
    d.slot_keys.clear();
    d.values.clear();
    d.slot_timelines = false;
    for (const asset::Skeleton2DBone& b : v.bones) d.bones.push_back(b);
    for (const asset::Skeleton2DSlot& s : v.slots) d.slots.push_back(s);
    for (const asset::Skeleton2DAnimation& a : v.animations) {
        d.animations.push_back(a);
        d.animation_ids.push_back(StringId(v.string(a.name)).value);
    }
    for (const asset::Skeleton2DTimeline& t : v.timelines) {
        d.timelines.push_back(t);
        d.slot_timelines |= t.channel == asset::Skeleton2DChannel::Color || t.channel == asset::Skeleton2DChannel::Attachment;
    }
    for (const asset::Skeleton2DKey& k : v.keys) d.keys.push_back(k);
    for (const asset::Skeleton2DEvent& e : v.events) d.events.push_back(e);
    for (const asset::Skeleton2DAttachment& a : v.attachments) d.attachments.push_back(a);
    for (const asset::Skeleton2DVertex& x : v.vertices) d.vertices.push_back(x);
    for (const asset::Skeleton2DWeight& w : v.weights) d.weights.push_back(w);
    for (u32 i : v.indices) d.indices.push_back(i);
    for (const asset::Skeleton2DSlotKey& k : v.slot_keys) d.slot_keys.push_back(k);
    for (f32 x : v.values) d.values.push_back(x);
    for (char c : v.strings) d.strings.push_back(c);
    // A slot without listed attachments shows its own quad: append it as the slot's one
    // attachment, named after the slot (after the chunk's, so timeline indices keep pointing at
    // the same entries).
    for (u32 i = 0; i < d.slots.size(); ++i) {
        asset::Skeleton2DSlot& s = d.slots[i];
        if (s.attachment_count != 0) continue;
        asset::Skeleton2DAttachment a{};
        a.name = s.name;
        a.slot = static_cast<u16>(i);
        a.kind = asset::Skeleton2DAttachmentKind::Quad;
        a.texture[0] = s.texture[0];
        a.texture[1] = s.texture[1];
        for (u32 k = 0; k < 4; ++k) a.rect[k] = s.rect[k];
        for (u32 k = 0; k < 2; ++k) {
            a.size[k] = s.size[k];
            a.pivot[k] = s.pivot[k];
            a.offset[k] = s.offset[k];
        }
        a.rotation = s.rotation;
        s.first_attachment = static_cast<u32>(d.attachments.size());
        s.attachment_count = 1;
        d.attachments.push_back(a);
    }
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
    // Slot state set up from the skeleton when fresh or when the skeleton's slot count changed.
    f.query<const Skeleton2D, Skeleton2DSlots>().each([&](Entity, const Skeleton2D& sk, Skeleton2DSlots& slots) {
        const Skeleton2DRegistry::Def* def = reg->resolve(sk.skeleton.guid);
        if (def && (slots.slot_count == 0 || slots.slot_count != def->slots.size())) skeleton2d_slots_setup(*def, slots);
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
    world.register_components<Skeleton2D, Bone2DFollow, Skeleton2DAnimator, Skeleton2DSlots>();
    world.register_event<AnimEvent3D>(Phase::FixedUpdate_Post);  // shared with kit3d's Animator (idempotent)
    world.set_resource(Skeleton2DRegistry(assets));
    // The animators write `pose` before the skeletons turn it into world transforms.
    world.add_system(Phase::FixedUpdate_Post, "kit_skeleton2d_animators", &kit_skeleton2d_animators);
    world.add_system(Phase::FixedUpdate_Post, "kit_skeleton2d", &kit_skeleton2d, "kit_skeleton2d_animators");
    world.run_during_cutscenes(Phase::FixedUpdate_Post, "kit_skeleton2d_animators");
    world.run_during_cutscenes(Phase::FixedUpdate_Post, "kit_skeleton2d");
}

}  // namespace mce::kit

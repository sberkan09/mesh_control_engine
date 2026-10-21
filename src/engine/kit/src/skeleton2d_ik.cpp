// 2D IK (M-ANIM2D 4): the .mcskel2d's two-bone and CCD constraints, solved in the skeleton's space
// after the pose and written back as local rotations (skeleton2d.h). skeleton2d_solve_pose is the
// one entry point: "kit_skeleton2d" and the sprite extract both pose through it. Everything is
// plain float math in a fixed order (no tolerance exits), so a solve gives the same bits on every
// run.

#include <cmath>

#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/kit/skeleton2d.h"

namespace mce::kit {

// World::state_hash hashes component bytes: no padding.
static_assert(sizeof(Skeleton2DIK) == 4 + 2 * kSkeleton2DIKCount * sizeof(f32));
static_assert(sizeof(IKTarget2D) == 60);

namespace {

constexpr f32 kPi = 3.14159265358979323846f;
constexpr f32 kTwoPi = 2.0f * kPi;

// An angle taken to [-pi, pi]: the shortest turn.
f32 wrap_angle(f32 a) { return a - kTwoPi * std::round(a / kTwoPi); }

f32 clamp01(f32 x) { return x > 0.0f ? (x < 1.0f ? x : 1.0f) : 0.0f; }

math::vec2 origin(const Bone2DTransform& m) { return {m.x, m.y}; }

// The bones of one skeleton being solved: local poses and their world transforms.
struct Chain {
    const Skeleton2DRegistry::Def& def;
    u32 n;
    Bone2DPose* local;
    Bone2DTransform* world;

    // World transforms of bones from..n-1 (bones are parent-first, so the rest are unchanged).
    void recompute(u32 from) {
        for (u32 i = from; i < n; ++i) {
            const Bone2DTransform l = bone2d_local(local[i]);
            const u16 parent = def.bones[i].parent;
            world[i] = parent != asset::kSkeleton2DNoParent && parent < i ? world[parent] * l : l;
        }
    }

    // Turns bone j so that `effector` (a world point riding the bone) lies on the ray from the
    // bone's origin through `target`. Worked in the parent's space, where the bone's local pose is
    // a plain rotation about its origin, so a parent's scale or mirror needs no special case.
    void aim(u32 j, math::vec2 effector, math::vec2 target) {
        const u16 parent = def.bones[j].parent;
        const Bone2DTransform inv =
            parent != asset::kSkeleton2DNoParent && parent < j ? bone2d_inverse(world[parent]) : Bone2DTransform{};
        const math::vec2 tp = inv.apply(target);
        const math::vec2 ep = inv.apply(effector);
        const f32 tx = tp.x - local[j].x, ty = tp.y - local[j].y;
        const f32 ex = ep.x - local[j].x, ey = ep.y - local[j].y;
        if ((tx == 0.0f && ty == 0.0f) || (ex == 0.0f && ey == 0.0f)) return;
        local[j].rotation += wrap_angle(std::atan2(ty, tx) - std::atan2(ey, ex));
        recompute(j);
    }

    // The tip of bone b: (length, 0) in its space. A bone of length 0 aims its +x axis, so its
    // "tip" for its own turn is (1, 0); for the bones above it the tip is its origin.
    math::vec2 tip(u32 b, bool own) const {
        const f32 len = def.bones[b].length;
        return world[b].apply({len > 0.0f ? len : (own ? 1.0f : 0.0f), 0.0f});
    }

    void two_bone(u32 upper, u32 lower, math::vec2 t, bool negative) {
        const math::vec2 o = origin(world[upper]);
        const math::vec2 e0 = origin(world[lower]);
        const math::vec2 end = tip(lower, false);
        const f32 l1 = std::sqrt((e0.x - o.x) * (e0.x - o.x) + (e0.y - o.y) * (e0.y - o.y));
        const f32 l2 = std::sqrt((end.x - e0.x) * (end.x - e0.x) + (end.y - e0.y) * (end.y - e0.y));
        const f32 dx = t.x - o.x, dy = t.y - o.y;
        const f32 d = std::sqrt(dx * dx + dy * dy);
        if (l1 > 0.0f && d > 0.0f) {
            // Law of cosines: the angle at the upper bone between the target and the elbow. Out of
            // reach (or too close) the clamp gives a straight (or folded) chain.
            f32 bend = 0.0f;
            if (d < l1 + l2) {
                const f32 c = (l1 * l1 + d * d - l2 * l2) / (2.0f * l1 * d);
                bend = std::acos(c < -1.0f ? -1.0f : (c > 1.0f ? 1.0f : c));
            }
            // Positive bends the elbow counter-clockwise: the upper bone turns clockwise of the target.
            const f32 a = std::atan2(dy, dx) + (negative ? bend : -bend);
            aim(upper, e0, {o.x + l1 * std::cos(a), o.y + l1 * std::sin(a)});
        }
        aim(lower, tip(lower, true), t);
    }

    void ccd(Span<const u32> bones, math::vec2 t, u32 iterations) {
        const u32 last = bones[bones.size() - 1];
        for (u32 it = 0; it < iterations; ++it)
            for (usize k = bones.size(); k-- > 0;) aim(bones[k], tip(last, bones[k] == last), t);
    }
};

}  // namespace

Bone2DTransform bone2d_inverse(const Bone2DTransform& m) {
    const f32 det = m.a * m.d - m.b * m.c;
    if (det == 0.0f || !std::isfinite(det)) return Bone2DTransform{};
    Bone2DTransform r;
    r.a = m.d / det;
    r.b = -m.b / det;
    r.c = -m.c / det;
    r.d = m.a / det;
    r.x = -(r.a * m.x + r.c * m.y);
    r.y = -(r.b * m.x + r.d * m.y);
    return r;
}

i32 Skeleton2DRegistry::Def::find_ik(StringView n) const {
    for (usize i = 0; i < ik.size(); ++i)
        if (string(ik[i].name) == n) return static_cast<i32>(i);
    return -1;
}

void skeleton2d_ik_setup(const Skeleton2DRegistry::Def& def, Skeleton2DIK& ik) {
    const u32 n = def.ik.size() < kSkeleton2DIKCount ? static_cast<u32>(def.ik.size()) : kSkeleton2DIKCount;
    for (u32 i = 0; i < kSkeleton2DIKCount; ++i) ik.mix[i] = i < n ? def.ik[i].mix : 0.0f;
    ik.constraint_count = n;
}

void skeleton2d_ik_mix(const Skeleton2DRegistry::Def& def, const Skeleton2DIK* ik, f32 out[kSkeleton2DIKCount]) {
    const u32 n = def.ik.size() < kSkeleton2DIKCount ? static_cast<u32>(def.ik.size()) : kSkeleton2DIKCount;
    const bool live = ik && ik->constraint_count != 0 && ik->constraint_count == n;
    for (u32 i = 0; i < kSkeleton2DIKCount; ++i) {
        f32 m = i < n ? (live ? ik->mix[i] : def.ik[i].mix) : 0.0f;
        if (ik && ik->override_mix[i] >= 0.0f) m = ik->override_mix[i];
        out[i] = clamp01(m);
    }
}

void skeleton2d_solve_pose(const Skeleton2DRegistry::Def& def, Span<const Bone2DPose> offsets, const Skeleton2DIK* ik,
                           const Skeleton2DIKTargets* targets, Span<Bone2DTransform> world) {
    u32 n = def.bones.size() < kSkeleton2DBones ? static_cast<u32>(def.bones.size()) : kSkeleton2DBones;
    if (world.size() < n) n = static_cast<u32>(world.size());
    Bone2DPose local[kSkeleton2DBones];
    Bone2DTransform posed[kSkeleton2DBones];
    for (u32 i = 0; i < n; ++i) local[i] = bone2d_offset(def.bones[i], i < offsets.size() ? offsets[i] : Bone2DPose{});
    Chain c{def, n, local, posed};
    c.recompute(0);
    if (!def.ik.empty()) {
        f32 mix[kSkeleton2DIKCount];
        skeleton2d_ik_mix(def, ik, mix);
        const u32 count = def.ik.size() < kSkeleton2DIKCount ? static_cast<u32>(def.ik.size()) : kSkeleton2DIKCount;
        for (u32 ci = 0; ci < count; ++ci) {
            const asset::Skeleton2DIKConstraint& k = def.ik[ci];
            f32 m = mix[ci];
            math::vec2 t;
            if (targets && (targets->set & (1u << ci))) {
                t = targets->position[ci];
                m = clamp01(m * targets->mix[ci]);
            } else if (k.target < n) {
                t = origin(posed[k.target]);
            } else {
                continue;  // no target bone and no entity target: the chain keeps its pose
            }
            if (!(m > 0.0f) || k.bone >= n || k.bone_count == 0 || k.bone_count > asset::kSkeleton2DMaxIKChain)
                continue;
            // The chain, root-most first (the reader checked the parents exist).
            u32 bones[asset::kSkeleton2DMaxIKChain];
            u32 b = k.bone;
            bool ok = true;
            for (u32 i = k.bone_count; i-- > 0;) {
                bones[i] = b;
                if (i > 0) {
                    ok = ok && def.bones[b].parent < b;
                    if (!ok) break;
                    b = def.bones[b].parent;
                }
            }
            if (!ok) continue;
            f32 before[asset::kSkeleton2DMaxIKChain];
            for (u32 i = 0; i < k.bone_count; ++i) before[i] = local[bones[i]].rotation;
            if (k.kind == asset::Skeleton2DIKKind::TwoBone && k.bone_count == 2)
                c.two_bone(bones[0], bones[1], t, (k.flags & asset::Skeleton2DIKFlags::BendNegative) != 0);
            else
                c.ccd(Span<const u32>(bones, k.bone_count), t, k.iterations);
            // Mix: each rotation moves that fraction of the shortest way from its pose to its solve.
            if (m < 1.0f) {
                for (u32 i = 0; i < k.bone_count; ++i)
                    local[bones[i]].rotation = before[i] + wrap_angle(local[bones[i]].rotation - before[i]) * m;
                c.recompute(bones[0]);
            }
        }
    }
    for (u32 i = 0; i < n; ++i) world[i] = posed[i];
}

namespace {

// Where target entity `te` stands in the skeleton's space (skeleton entity `e`): its
// LocalTransform when it is a ChildOf `e` (already in the entity's space), else its world
// placement taken back through the skeleton entity's. Flips mirror the skeleton's space.
bool target_in_skeleton(World& w, Entity e, const Skeleton2D& sk, Entity te, math::vec2& out) {
    math::vec2 p;
    const ChildOf* parent = w.try_get<ChildOf>(te);
    if (parent && parent->parent == e) {
        const LocalTransform* t = w.try_get<LocalTransform>(te);
        if (!t) return false;
        p = {t->position.x, t->position.y};
    } else {
        if (const WorldTransform* t = w.try_get<WorldTransform>(te))
            p = {t->position.x, t->position.y};
        else if (const LocalTransform* l = w.try_get<LocalTransform>(te))
            p = {l->position.x, l->position.y};
        else
            return false;
        if (const WorldTransform* sw = w.try_get<WorldTransform>(e))
            p = bone2d_inverse(transform_2d(sw->position, sw->rotation, sw->scale)).apply(p);
        else if (const LocalTransform* sl = w.try_get<LocalTransform>(e))
            p = bone2d_inverse(transform_2d(sl->position, sl->rotation, sl->scale)).apply(p);
    }
    out = {sk.flip_x ? -p.x : p.x, sk.flip_y ? -p.y : p.y};
    return true;
}

}  // namespace

const Skeleton2DIK* skeleton2d_ik_state(World& world, Entity e, const Skeleton2DRegistry::Def& def,
                                        Skeleton2DRegistry& reg, Skeleton2DIK& scratch) {
    if (def.ik.empty()) return nullptr;
    if (const Skeleton2DIK* ik = world.try_get<Skeleton2DIK>(e)) return ik;
    const Skeleton2DAnimator* a = def.ik_timelines ? world.try_get<Skeleton2DAnimator>(e) : nullptr;
    if (!a) return nullptr;
    asset::AnimGraphView g;
    const bool graphed = a->graph.guid != asset::Guid{} && reg.graph(a->graph.guid, g);
    Skeleton2DBlend blend;
    skeleton2d_animator_sample(def, *a, graphed ? &g : nullptr, blend);
    skeleton2d_ik_setup(def, scratch);
    blend.write_ik(def, scratch);
    return &scratch;
}

void skeleton2d_ik_targets(World& world, Entity e, const Skeleton2DRegistry::Def& def, const Skeleton2D& sk,
                           Skeleton2DIKTargets& out) {
    out = Skeleton2DIKTargets{};
    if (def.ik.empty()) return;
    world.query<const IKTarget2D>().each([&](Entity te, const IKTarget2D& t) {
        if (!t.enabled || !(t.skeleton == e)) return;
        const i32 ci = def.find_ik(t.constraint.view());
        if (ci < 0 || static_cast<u32>(ci) >= kSkeleton2DIKCount) return;
        math::vec2 p;
        if (!target_in_skeleton(world, e, sk, te, p)) return;
        out.position[ci] = p;
        out.mix[ci] = clamp01(t.mix);
        out.set |= 1u << static_cast<u32>(ci);
    });
}

}  // namespace mce::kit

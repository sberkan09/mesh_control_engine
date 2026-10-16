// M-ANIM2D 4: 2D IK as data. The two-bone solve (the tip on a reachable target, the bend sides
// mirrored, a straight chain at an unreachable one), mix interpolation, CCD with a fixed pass count
// and the same bits run to run, entity targets through a moved and flipped skeleton, keyed mixes
// and their cross-fade, Bone2DFollow and the extract posing through the solve, the cook of "ik"
// and its errors, the reader's rejects, and determinism with a save in the middle of a fade.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <cstring>
#include <string>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"
#include "mce/ecs/hierarchy.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/kit/skeleton2d.h"
#include "mce/scene/scene.h"

using namespace mce;
using namespace cook_test;
using kit::Bone2DPose;
using kit::Bone2DTransform;
using kit::IKTarget2D;
using kit::Skeleton2D;
using kit::Skeleton2DAnimator;
using kit::Skeleton2DIK;

namespace {

constexpr f32 kDt = 0.125f;  // a power of two: playheads add up exactly
constexpr const char* kRigGuid = "5c2d0000-0000-4000-8000-000000000301";
constexpr const char* kScene = "5c2d0000-0000-4000-8000-000000000302";
// Bones.
constexpr u32 kUpper = 1, kLower = 2, kHand = 3, kTarget = 4, kC1 = 5, kC3 = 7, kGoal = 8, kAim = 9;
// Constraints, as the cook sorts them by "order": look (-1), then arm and tail (0) as listed.
constexpr u32 kLook = 0, kArm = 1, kTail = 2;
// Entities.
constexpr u32 kRig = 0, kCursor = 1, kFollower = 2, kAnim = 3;

std::string diag_text(const cook::CookReport& r) {
    std::string s;
    for (const cook::Diagnostic& d : r.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        s += std::string(line.view().data(), line.view().size()) + "\n";
    }
    return s;
}

// The rig: an arm (upper, lower: 1 m each, hand at the lower's tip) reaching for bone "target" at
// (1.2, 0.8); a tail of three 1 m bones from (0, 3) reaching for "goal" at (1.5, 4.5) by CCD; a
// pointer bone "aim" at (0, -3) that an IKTarget2D entity aims ("look" has no target bone).
//   reach (once, 1 s): the arm's mix keyed 0 -> 1.   hold (1 s): the arm's mix keyed 0.25.
const std::string& project() {
    static const std::string root = [] {
        const std::string r = fresh_dir("skeleton2d_ik_project");
        write_file(r + "/assets/rig.mcskel2d", std::string(R"({ "format": "mce-skeleton2d", "version": 1,
            "guid": ")") + kRigGuid + R"(", "name": "rig",
            "bones": [ { "name": "root" },
                       { "name": "upper", "parent": "root", "length": 1 },
                       { "name": "lower", "parent": "upper", "x": 1, "length": 1 },
                       { "name": "hand", "parent": "lower", "x": 1 },
                       { "name": "target", "parent": "root", "x": 1.2, "y": 0.8 },
                       { "name": "c1", "parent": "root", "y": 3, "length": 1 },
                       { "name": "c2", "parent": "c1", "x": 1, "length": 1 },
                       { "name": "c3", "parent": "c2", "x": 1, "length": 1 },
                       { "name": "goal", "parent": "root", "x": 1.5, "y": 4.5 },
                       { "name": "aim", "parent": "root", "y": -3, "length": 0.5 } ],
            "slots": [ { "name": "fore", "bone": "lower", "size": [1, 0.2], "offset": [0.5, 0] },
                       { "name": "pointer", "bone": "aim", "size": [0.5, 0.1], "offset": [0.25, 0] } ],
            "ik": [ { "name": "arm", "bones": ["upper", "lower"], "target": "target", "bend": "positive" },
                    { "name": "tail", "bones": ["c1", "c2", "c3"], "target": "goal", "iterations": 10 },
                    { "name": "look", "bones": ["aim"], "order": -1 } ],
            "animations": [
              { "name": "reach", "loop": false, "ik": { "arm": [ { "t": 0, "mix": 0 }, { "t": 1, "mix": 1 } ] } },
              { "name": "hold", "duration": 1, "ik": { "arm": [ { "t": 0, "mix": 0.25 } ] } } ] })");
        write_file(r + "/assets/stage.mcscene", std::string(R"({ "format": "mce-scene", "version": 1,
            "guid": ")") + kScene + R"(", "name": "stage", "entities": [
            { "name": "Rig", "components": {
                "SaveKey": { "key": 1 },
                "LocalTransform": { "position": [10, 2, 0] },
                "Skeleton2D": { "skeleton": "guid:)" +
                                                    kRigGuid + R"(" } } },
            { "name": "Cursor", "components": {
                "SaveKey": { "key": 2 },
                "LocalTransform": { "position": [12, 5, 0] },
                "IKTarget2D": { "skeleton": "$0", "constraint": "look" } } },
            { "name": "Follower", "components": {
                "LocalTransform": { "position": [0, 0, 0] },
                "Bone2DFollow": { "skeleton": "$0", "bone": "hand" } } },
            { "name": "Anim", "components": {
                "SaveKey": { "key": 3 },
                "LocalTransform": { "position": [0, -10, 0] },
                "Skeleton2D": { "skeleton": "guid:)" +
                                                    kRigGuid + R"(" },
                "Skeleton2DAnimator": { "clip": "reach", "fade": 0.5, "loop": false } } } ] })");
        cook::CookService svc;
        const cook::CookReport rep = svc.cook(StringView(r.c_str()), cook::CookOptions{});
        INFO(diag_text(rep));
        REQUIRE(rep.ok());
        return r;
    }();
    return root;
}

struct Stage {
    asset::AssetSystem assets;
    World world;
    Array<Entity> e;
    u64 ticks = 0;

    Stage() {
        const std::string cooked = project() + "/cooked";
        const StringView roots[] = {StringView(cooked.c_str())};
        REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
        world.register_components<LocalTransform, SaveKey>();
        kit::install_skeleton2d(world, &assets);
        REQUIRE(scene::load(world, assets, asset::parse_guid(kScene).value(), &e));
        REQUIRE(e.size() == 4);
    }
    Skeleton2D& sk(u32 i = kRig) { return *world.try_get_mut<Skeleton2D>(e[i]); }
    Skeleton2DAnimator& anim() { return *world.try_get_mut<Skeleton2DAnimator>(e[kAnim]); }
    LocalTransform& at(u32 i) { return *world.try_get_mut<LocalTransform>(e[i]); }
    const kit::Skeleton2DRegistry::Def& def() {
        const kit::Skeleton2DRegistry::Def* d =
            world.try_resource<kit::Skeleton2DRegistry>()->resolve(sk().skeleton.guid);
        REQUIRE(d);
        return *d;
    }
    // Skeleton entity * flips: skeleton space to world.
    Bone2DTransform placed(u32 i = kRig) {
        const LocalTransform& t = at(i);
        return kit::transform_2d(t.position, t.rotation, t.scale) *
               Bone2DTransform::trs({}, 0.0f, {sk(i).flip_x ? -1.0f : 1.0f, sk(i).flip_y ? -1.0f : 1.0f});
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) world.tick(kDt, ticks++);
    }
};

// The posed bones of `def` from rest (offsets `pose`, may be null) with `ik` and `targets`.
struct Posed {
    Bone2DTransform w[kit::kSkeleton2DBones];
};
Posed solve(const kit::Skeleton2DRegistry::Def& def, const Skeleton2DIK* ik = nullptr,
            const kit::Skeleton2DIKTargets* targets = nullptr, const Bone2DPose* pose = nullptr) {
    Bone2DPose rest[kit::kSkeleton2DBones];
    Posed p;
    kit::skeleton2d_solve_pose(def, Span<const Bone2DPose>(pose ? pose : rest, kit::kSkeleton2DBones), ik, targets,
                               Span<Bone2DTransform>(p.w, kit::kSkeleton2DBones));
    return p;
}

math::vec2 origin(const Bone2DTransform& m) { return {m.x, m.y}; }
f32 angle(const Bone2DTransform& m) { return std::atan2(m.b, m.a); }
f32 cross2(math::vec2 a, math::vec2 b) { return a.x * b.y - a.y * b.x; }

// The cooked rig's payload, copied so tests can change it.
Array<byte> rig_payload() {
    Stage s;
    const asset::AssetKey k = s.assets.find(asset::parse_guid(kRigGuid).value());
    REQUIRE(k != asset::kInvalidAssetKey);
    auto h = s.assets.load_sync<asset::Skeleton2D>(k);
    REQUIRE(h);
    Array<byte> out;
    for (byte b : s.assets.resolve(h.value())) out.push_back(b);
    s.assets.release(asset::AnyHandle(h.value()));
    return out;
}

}  // namespace

TEST_CASE("skeleton2d ik: constraints cook into CHNK_SKEL2D minor 3, sorted by order") {
    const Array<byte> bytes = rig_payload();
    auto view = asset::Skeleton2DView::read(bytes.span());
    REQUIRE(view);
    const asset::Skeleton2DView& v = view.value();
    CHECK(v.header->flags == asset::Skeleton2DHeaderFlags::IK);  // no attachment parts: slots keep their own quads
    REQUIRE(v.ik.size() == 3);
    CHECK(v.string(v.ik[kLook].name) == StringView("look"));
    CHECK(v.string(v.ik[kArm].name) == StringView("arm"));
    CHECK(v.string(v.ik[kTail].name) == StringView("tail"));
    const asset::Skeleton2DIKConstraint& arm = v.ik[kArm];
    CHECK(arm.kind == asset::Skeleton2DIKKind::TwoBone);
    CHECK(arm.bone == kLower);
    CHECK(arm.bone_count == 2);
    CHECK(arm.target == kTarget);
    CHECK(arm.flags == 0);
    CHECK(arm.mix == 1.0f);
    CHECK(arm.iterations == 0);
    const asset::Skeleton2DIKConstraint& tail = v.ik[kTail];
    CHECK(tail.kind == asset::Skeleton2DIKKind::CCD);  // three bones: CCD by default
    CHECK(tail.bone == kC3);
    CHECK(tail.bone_count == 3);
    CHECK(tail.iterations == 10);
    const asset::Skeleton2DIKConstraint& look = v.ik[kLook];
    CHECK(look.kind == asset::Skeleton2DIKKind::CCD);  // one bone: an aim
    CHECK(look.target == asset::kSkeleton2DNoTarget);
    CHECK(look.order == -1);
    CHECK(look.iterations == 10);
    // The mix keys: IKMix timelines of bone keys on the constraint's sorted index.
    REQUIRE(v.animations.size() == 2);
    const asset::Skeleton2DTimeline& tl = v.timelines[v.animations[0].first_timeline];
    CHECK(tl.channel == asset::Skeleton2DChannel::IKMix);
    CHECK(tl.bone == kArm);
    REQUIRE(tl.key_count == 2);
    CHECK(v.keys[tl.first_key].value[0] == 0.0f);
    CHECK(v.keys[tl.first_key + 1].value[0] == 1.0f);
    CHECK(v.animations[0].duration == 1.0f);
}

TEST_CASE(
    "skeleton2d ik: two-bone reaches a reachable target, the bend sides mirror, an unreachable one gets a straight "
    "chain") {
    Stage s;
    const kit::Skeleton2DRegistry::Def& def = s.def();
    const Posed p = solve(def);
    // The tip of the lower bone lands on the target bone (1.2, 0.8).
    const math::vec2 tip = p.w[kLower].apply({1.0f, 0.0f});
    CHECK(std::fabs(tip.x - 1.2f) <= 1e-5f);
    CHECK(std::fabs(tip.y - 0.8f) <= 1e-5f);
    CHECK(origin(p.w[kTarget]) == math::vec2{1.2f, 0.8f});
    // Law of cosines with l1 = l2 = 1, d = |(1.2, 0.8)|: the elbow sits clockwise of the target line.
    const f64 d = std::sqrt(1.2 * 1.2 + 0.8 * 0.8);
    const f64 a = std::atan2(0.8, 1.2) - std::acos(d * d / (2.0 * d));
    const math::vec2 elbow = origin(p.w[kLower]);
    CHECK(std::fabs(elbow.x - static_cast<f32>(std::cos(a))) <= 1e-5f);
    CHECK(std::fabs(elbow.y - static_cast<f32>(std::sin(a))) <= 1e-5f);
    CHECK(cross2({1.2f, 0.8f}, elbow) < 0.0f);
    // The hand rides the lower bone's tip.
    CHECK(std::fabs(p.w[kHand].x - 1.2f) <= 1e-5f);
    CHECK(std::fabs(p.w[kHand].y - 0.8f) <= 1e-5f);

    // "negative": the same chain with the bend flag set; the elbow mirrors across the target line.
    Array<byte> bytes = rig_payload();
    auto view = asset::Skeleton2DView::read(bytes.span());
    REQUIRE(view);
    const_cast<asset::Skeleton2DIKConstraint&>(view.value().ik[kArm]).flags = asset::Skeleton2DIKFlags::BendNegative;
    kit::Skeleton2DRegistry reg;
    REQUIRE(reg.define(asset::Guid{7, 7}, bytes.span()));
    const Posed n = solve(*reg.resolve(asset::Guid{7, 7}));
    const math::vec2 ntip = n.w[kLower].apply({1.0f, 0.0f});
    CHECK(std::fabs(ntip.x - 1.2f) <= 1e-5f);
    CHECK(std::fabs(ntip.y - 0.8f) <= 1e-5f);
    // The reflection of the positive elbow across the line through the origin and (1.2, 0.8).
    const f32 ux = static_cast<f32>(1.2 / d), uy = static_cast<f32>(0.8 / d);
    const f32 along = elbow.x * ux + elbow.y * uy;
    const math::vec2 mirrored{2.0f * along * ux - elbow.x, 2.0f * along * uy - elbow.y};
    const math::vec2 nelbow = origin(n.w[kLower]);
    CHECK(std::fabs(nelbow.x - mirrored.x) <= 1e-5f);
    CHECK(std::fabs(nelbow.y - mirrored.y) <= 1e-5f);
    CHECK(cross2({1.2f, 0.8f}, nelbow) > 0.0f);

    // Out of reach: the target bone moved to (6.2, 0.8); both bones point straight at it.
    Bone2DPose pose[kit::kSkeleton2DBones];
    pose[kTarget].x = 5.0f;
    const Posed far = solve(def, nullptr, nullptr, pose);
    const math::vec2 to{6.2f, 0.8f};
    const f32 len = std::sqrt(to.x * to.x + to.y * to.y);
    const math::vec2 e = origin(far.w[kLower]);
    const math::vec2 t = far.w[kLower].apply({1.0f, 0.0f});
    CHECK(std::fabs(e.x - to.x / len) <= 1e-5f);
    CHECK(std::fabs(e.y - to.y / len) <= 1e-5f);
    CHECK(std::fabs(t.x - 2.0f * to.x / len) <= 1e-5f);
    CHECK(std::fabs(t.y - 2.0f * to.y / len) <= 1e-5f);
    CHECK(std::fabs(angle(far.w[kUpper]) - angle(far.w[kLower])) <= 1e-5f);  // no bend
}

TEST_CASE("skeleton2d ik: mix 0 keeps the pose, mix 0.5 moves each rotation half way") {
    Stage s;
    const kit::Skeleton2DRegistry::Def& def = s.def();
    const Posed full = solve(def);
    Skeleton2DIK ik;
    ik.override_mix[kArm] = 0.0f;
    const Posed none = solve(def, &ik);
    CHECK(none.w[kUpper] == Bone2DTransform{});  // the rest pose: upper on +x at the origin
    CHECK(origin(none.w[kLower]) == math::vec2{1.0f, 0.0f});
    ik.override_mix[kArm] = 0.5f;
    const Posed half = solve(def, &ik);
    const f32 up = angle(full.w[kUpper]), low = angle(full.w[kLower]) - up;  // the solved local rotations
    CHECK(angle(half.w[kUpper]) == doctest::Approx(0.5f * up).epsilon(1e-6));
    CHECK(angle(half.w[kLower]) - angle(half.w[kUpper]) == doctest::Approx(0.5f * low).epsilon(1e-6));
    // The mix in use: the override, else a set-up Skeleton2DIK's mix, else the asset's.
    f32 mix[kit::kSkeleton2DIKCount];
    kit::skeleton2d_ik_mix(def, &ik, mix);
    CHECK(mix[kArm] == 0.5f);
    CHECK(mix[kTail] == 1.0f);
    Skeleton2DIK set;
    kit::skeleton2d_ik_setup(def, set);
    CHECK(set.constraint_count == 3);
    set.mix[kTail] = 0.25f;
    kit::skeleton2d_ik_mix(def, &set, mix);
    CHECK(mix[kTail] == 0.25f);
    set.override_mix[kTail] = 2.0f;  // clamped to 1
    kit::skeleton2d_ik_mix(def, &set, mix);
    CHECK(mix[kTail] == 1.0f);
}

TEST_CASE("skeleton2d ik: CCD gives a fixed result after its passes, the same bits every run") {
    Stage s;
    const kit::Skeleton2DRegistry::Def& def = s.def();
    const Posed a = solve(def), b = solve(def);
    for (u32 i = 0; i < 10; ++i) {
        CHECK(std::bit_cast<u32>(a.w[i].a) == std::bit_cast<u32>(b.w[i].a));
        CHECK(std::bit_cast<u32>(a.w[i].b) == std::bit_cast<u32>(b.w[i].b));
        CHECK(std::bit_cast<u32>(a.w[i].x) == std::bit_cast<u32>(b.w[i].x));
        CHECK(std::bit_cast<u32>(a.w[i].y) == std::bit_cast<u32>(b.w[i].y));
    }
    // After 10 passes the tail's tip is on the goal (1.5, 4.5) to within 1e-4.
    const math::vec2 tip = a.w[kC3].apply({1.0f, 0.0f});
    CHECK(origin(a.w[kGoal]) == math::vec2{1.5f, 4.5f});
    CHECK(std::fabs(tip.x - 1.5f) <= 1e-4f);
    CHECK(std::fabs(tip.y - 4.5f) <= 1e-4f);
    // The chain's angles after exactly 10 passes (the count is fixed: no tolerance exit).
    CHECK(angle(a.w[kC1]) == doctest::Approx(0.12046659).epsilon(1e-5));
    CHECK(angle(a.w[kC1 + 1]) == doctest::Approx(0.47346306).epsilon(1e-5));
    CHECK(angle(a.w[kC3]) == doctest::Approx(1.96356523).epsilon(1e-5));
    // One pass leaves the chain elsewhere: the pass count is what fixes the result.
    Array<byte> bytes = rig_payload();
    auto view = asset::Skeleton2DView::read(bytes.span());
    REQUIRE(view);
    const_cast<asset::Skeleton2DIKConstraint&>(view.value().ik[kTail]).iterations = 1;
    kit::Skeleton2DRegistry reg;
    REQUIRE(reg.define(asset::Guid{7, 8}, bytes.span()));
    const Posed one = solve(*reg.resolve(asset::Guid{7, 8}));
    CHECK(std::fabs(angle(one.w[kC1]) - angle(a.w[kC1])) > 1e-3f);
    // The aim bone (no target bone, no entity target) keeps its rest pose here.
    CHECK(a.w[kAim] == Bone2DTransform::trs({0.0f, -3.0f}, 0.0f, {1.0f, 1.0f}));
}

TEST_CASE("skeleton2d ik: an entity target through a moved, turned and flipped skeleton entity") {
    Stage s;
    s.at(kRig).rotation = rotation_z(0.5f);
    s.at(kRig).scale = {2.0f, 2.0f, 1.0f};
    s.sk().flip_x = true;
    s.run(1);
    const IKTarget2D& cursor = *s.world.try_get<IKTarget2D>(s.e[kCursor]);
    CHECK(cursor.found == 1u);
    // The aim bone's +x axis, placed in the world, points at the cursor (12, 5).
    const Bone2DTransform at = s.placed() * s.sk().world[kAim];
    const math::vec2 o = origin(at), axis{at.a, at.b};
    const math::vec2 to{12.0f - o.x, 5.0f - o.y};
    const f32 n = std::sqrt(axis.x * axis.x + axis.y * axis.y) * std::sqrt(to.x * to.x + to.y * to.y);
    CHECK(std::fabs(cross2(axis, to)) / n <= 1e-5f);
    CHECK(axis.x * to.x + axis.y * to.y > 0.0f);

    // The target's mix scales the turn; disabled, the bone rests (no target bone).
    s.world.try_get_mut<IKTarget2D>(s.e[kCursor])->enabled = false;
    s.run(1);
    CHECK(s.sk().world[kAim] == Bone2DTransform::trs({0.0f, -3.0f}, 0.0f, {1.0f, 1.0f}));
    IKTarget2D& t = *s.world.try_get_mut<IKTarget2D>(s.e[kCursor]);
    t.enabled = true;
    t.mix = 0.5f;
    s.run(1);
    const f32 half = angle(s.sk().world[kAim]);
    t.mix = 1.0f;
    s.run(1);
    CHECK(half == doctest::Approx(0.5f * angle(s.sk().world[kAim])).epsilon(1e-6));
    // A constraint name the skeleton lacks: not found.
    t.constraint = "nope";
    s.run(1);
    CHECK(s.world.try_get<IKTarget2D>(s.e[kCursor])->found == 0u);

    // A target that is a ChildOf the skeleton entity is already in its space (flips still apply).
    Stage c;
    ecs::install_hierarchy(c.world);
    REQUIRE(ecs::set_parent(c.world, c.e[kCursor], c.e[kRig]));
    c.at(kCursor).position = {3.0f, -1.0f, 0.0f};  // skeleton entity space
    c.sk().flip_x = true;
    c.run(1);
    const Bone2DTransform& aim = c.sk().world[kAim];
    // Skeleton space: the flip mirrors x, so the target is (-3, -1); the aim bone sits at (0, -3).
    CHECK(angle(aim) == doctest::Approx(std::atan2(2.0f, -3.0f)).epsilon(1e-6));
}

TEST_CASE("skeleton2d ik: Bone2DFollow sees the solved pose") {
    Stage s;
    s.sk().flip_x = true;
    s.run(1);
    // The hand rides the lower bone's tip, which IK put on the target bone (1.2, 0.8).
    const math::vec2 want = s.placed().apply({1.2f, 0.8f});
    const LocalTransform& f = s.at(kFollower);
    CHECK(f.position.x == doctest::Approx(want.x).epsilon(1e-5));
    CHECK(f.position.y == doctest::Approx(want.y).epsilon(1e-5));
    CHECK(f.position.x == doctest::Approx(10.0f - 1.2f).epsilon(1e-5));  // the rest pose would put it at 10 - 2
}

TEST_CASE("skeleton2d ik: the extract poses through the same solve as the tick") {
    Stage s;
    s.at(kRig).rotation = rotation_z(0.25f);
    s.sk().flip_y = true;
    // Before any tick: the bone-target constraints already pose what skeleton2d_quads draws.
    Array<kit::Skeleton2DQuad> quads;
    {
        const LocalTransform& t = s.at(kRig);
        kit::skeleton2d_quads(s.def(), s.sk(), kit::transform_2d(t.position, t.rotation, t.scale), quads);
    }
    REQUIRE(quads.size() == 2);
    s.run(1);
    auto expect = [&](u32 bone, f32 offset) { return (s.placed() * s.sk().world[bone]).apply({offset, 0.0f}); };
    CHECK(quads[0].position == expect(kLower, 0.5f));

    // With the entity target and a mix override, as SpriteExtract calls it.
    Skeleton2DIK ik;
    kit::skeleton2d_ik_setup(s.def(), ik);
    ik.override_mix[kLook] = 0.75f;
    s.world.add(s.e[kRig], ik);
    s.at(kCursor).position = {4.0f, 7.0f, 0.0f};
    s.run(1);
    kit::Skeleton2DIKTargets targets;
    kit::skeleton2d_ik_targets(s.world, s.e[kRig], s.def(), s.sk(), targets);
    CHECK(targets.set == (1u << kLook));
    kit::Skeleton2DDraws draws;
    const LocalTransform& t = s.at(kRig);
    kit::skeleton2d_draws(s.def(), s.sk(), nullptr, nullptr, s.world.try_get<Skeleton2DIK>(s.e[kRig]), &targets,
                          kit::transform_2d(t.position, t.rotation, t.scale), draws);
    REQUIRE(draws.quads.size() == 2);
    CHECK(draws.quads[0].position == expect(kLower, 0.5f));
    CHECK(draws.quads[1].position == expect(kAim, 0.25f));
    CHECK(draws.quads[1].rotation == doctest::Approx(angle(s.placed() * s.sk().world[kAim])).epsilon(1e-6));
}

TEST_CASE("skeleton2d ik: keyed mix plays through the animator and blends in a cross-fade") {
    Stage s;
    CHECK(s.world.try_get<Skeleton2DIK>(s.e[kAnim]) == nullptr);
    s.run(1);  // the switching tick: reach at 0, the component added
    const Skeleton2DIK* ik = s.world.try_get<Skeleton2DIK>(s.e[kAnim]);
    REQUIRE(ik);
    CHECK(ik->constraint_count == 3);
    CHECK(ik->mix[kArm] == 0.0f);
    CHECK(ik->mix[kTail] == 1.0f);                               // not keyed: the asset's
    CHECK(s.world.try_get<Skeleton2DIK>(s.e[kRig]) == nullptr);  // no animator: none added
    // At mix 0 the arm keeps its rest pose.
    CHECK(origin(s.sk(kAnim).world[kLower]) == math::vec2{1.0f, 0.0f});
    s.run(4);  // reach at 0.5
    CHECK(s.anim().time == 0.5f);
    CHECK(s.world.try_get<Skeleton2DIK>(s.e[kAnim])->mix[kArm] == 0.5f);
    const Posed full = solve(s.def());
    CHECK(angle(s.sk(kAnim).world[kUpper]) == doctest::Approx(0.5f * angle(full.w[kUpper])).epsilon(1e-6));

    // Cross-fade to hold (0.25) over 0.5 s: reach keeps playing underneath.
    s.anim().clip = "hold";
    s.run(1);  // the switch: both at their start, blend 0
    s.run(2);  // blend 0.5: hold 0.25, reach at previous_time
    REQUIRE(s.anim().blend == 0.5f);
    const f32 prev = s.anim().previous_time;
    CHECK(s.world.try_get<Skeleton2DIK>(s.e[kAnim])->mix[kArm] ==
          doctest::Approx(0.5f * 0.25f + 0.5f * prev).epsilon(1e-6));
    s.run(2);  // the fade is over
    CHECK(s.world.try_get<Skeleton2DIK>(s.e[kAnim])->mix[kArm] == 0.25f);
    // Gameplay's override wins over the keys.
    s.world.try_get_mut<Skeleton2DIK>(s.e[kAnim])->override_mix[kArm] = 1.0f;
    s.run(1);
    CHECK(s.world.try_get<Skeleton2DIK>(s.e[kAnim])->mix[kArm] == 0.25f);
    CHECK(s.sk(kAnim).world[kUpper] == full.w[kUpper]);
}

TEST_CASE("skeleton2d ik: two runs hash the same, and a save mid-fade resumes identically") {
    auto script = [](Stage& s, u64 tick) {
        s.at(kCursor).position.x = 12.0f - 0.5f * static_cast<f32>(tick);
        if (tick == 3) s.anim().clip = "hold";
    };
    Stage a, b;
    for (u32 i = 0; i < 12; ++i) {
        script(a, a.ticks);
        script(b, b.ticks);
        a.run(1);
        b.run(1);
        REQUIRE(a.world.state_hash() == b.world.state_hash());
    }

    Stage c;
    for (u32 i = 0; i < 5; ++i) {
        script(c, c.ticks);
        c.run(1);
    }
    REQUIRE(c.anim().blend == 0.25f);  // mid-fade
    Array<byte> bytes;
    save::write_save(c.world, save::SaveInfo{}, bytes);
    Stage d;  // the scene as authored: no Skeleton2DIK yet
    d.ticks = c.ticks;
    REQUIRE(save::read_save(d.world, bytes.span()));
    const Skeleton2DIK* ik = d.world.try_get<Skeleton2DIK>(d.e[kAnim]);
    REQUIRE(ik);
    CHECK(std::bit_cast<u32>(ik->mix[kArm]) ==
          std::bit_cast<u32>(c.world.try_get<Skeleton2DIK>(c.e[kAnim])->mix[kArm]));
    for (u32 i = 0; i < 3; ++i) {
        script(c, c.ticks);
        script(d, d.ticks);
        c.run(1);
        d.run(1);
        CHECK(d.world.state_hash() == c.world.state_hash());
        for (u32 k = 0; k < 10; ++k) CHECK(d.sk(kAnim).world[k] == c.sk(kAnim).world[k]);
    }
}

TEST_CASE("skeleton2d ik: the reader rejects bad constraints and mix keys") {
    const Array<byte> good = rig_payload();
    REQUIRE(asset::Skeleton2DView::read(good.span()));
    CHECK_FALSE(asset::Skeleton2DView::read(good.span().subspan(0, good.size() - 1)));
    auto rejects = [&](auto&& mutate) {
        Array<byte> c;
        for (byte b : good) c.push_back(b);
        auto v = asset::Skeleton2DView::read(c.span());
        REQUIRE(v);
        mutate(v.value());
        return !asset::Skeleton2DView::read(c.span());
    };
    using V = asset::Skeleton2DView;
    auto ik = [](const V& v, u32 i) -> asset::Skeleton2DIKConstraint& {
        return const_cast<asset::Skeleton2DIKConstraint&>(v.ik[i]);
    };
    auto header = [](const V& v) -> asset::Skeleton2DHeader& {
        return const_cast<asset::Skeleton2DHeader&>(*v.header);
    };
    auto ik_header = [](const V& v) -> asset::Skeleton2DIKHeader& {
        // Right after the header (the rig has no attachment header).
        auto* p = reinterpret_cast<const byte*>(v.header) + sizeof(asset::Skeleton2DHeader);
        return *const_cast<asset::Skeleton2DIKHeader*>(reinterpret_cast<const asset::Skeleton2DIKHeader*>(p));
    };
    auto key = [](const V& v, u32 i) -> asset::Skeleton2DKey& {
        return const_cast<asset::Skeleton2DKey&>(v.keys[v.timelines[v.animations[0].first_timeline].first_key + i]);
    };
    CHECK(rejects([&](const V& v) { header(v).flags |= 4u; }));  // an unknown flag
    CHECK(rejects(
        [&](const V& v) { header(v).flags &= ~u32(asset::Skeleton2DHeaderFlags::IK); }));  // the IK parts unannounced
    CHECK(rejects([&](const V& v) { ik_header(v).constraint_count = 0; }));
    CHECK(rejects([&](const V& v) { ik_header(v).constraint_count = 17; }));
    CHECK(rejects([&](const V& v) { ik(v, kArm).bone_count = 3; }));  // two-bone takes 2
    CHECK(rejects([&](const V& v) { ik(v, kArm).iterations = 1; }));  // only CCD iterates
    CHECK(rejects([&](const V& v) { ik(v, kArm).flags = 2; }));       // an unknown flag
    CHECK(rejects([&](const V& v) { ik(v, kArm).kind = static_cast<asset::Skeleton2DIKKind>(2); }));
    CHECK(rejects([&](const V& v) { ik(v, kArm).bone = 10; }));        // no such bone
    CHECK(rejects([&](const V& v) { ik(v, kArm).target = kUpper; }));  // in the chain
    CHECK(rejects([&](const V& v) { ik(v, kArm).target = kHand; }));   // under the chain
    CHECK(rejects([&](const V& v) { ik(v, kArm).target = 10; }));      // no such bone
    CHECK(rejects([&](const V& v) { ik(v, kArm).mix = 1.5f; }));
    CHECK(rejects([&](const V& v) { ik(v, kArm).mix = std::nanf(""); }));
    CHECK(rejects([&](const V& v) { ik(v, kArm).name = 9999; }));
    CHECK(rejects([&](const V& v) { ik(v, kArm).order = -2; }));  // after look's -1: unsorted
    CHECK(rejects([&](const V& v) { ik(v, kTail).iterations = 0; }));
    CHECK(rejects([&](const V& v) { ik(v, kTail).iterations = 65; }));
    CHECK(rejects([&](const V& v) { ik(v, kTail).bone_count = 0; }));
    CHECK(rejects([&](const V& v) { ik(v, kTail).bone_count = 5; }));  // c3, c2, c1, root, then none
    CHECK(rejects([&](const V& v) { ik(v, kTail).bone_count = 33; }));
    CHECK(rejects([&](const V& v) {
        auto& tl = const_cast<asset::Skeleton2DTimeline&>(v.timelines[v.animations[0].first_timeline]);
        tl.bone = 3;  // no such constraint
    }));
    CHECK(rejects([&](const V& v) { key(v, 1).value[0] = 1.5f; }));          // a mix out of 0..1
    CHECK(rejects([&](const V& v) { key(v, 1).value[1] = 1.0f; }));          // the second value is unused
    CHECK(rejects([&](const V& v) { ik(v, kTail).bone_count = 4; }));        // up to the root: the goal is under it
    CHECK_FALSE(rejects([&](const V& v) { ik(v, kTail).bone_count = 2; }));  // c2, c3: reads
}

TEST_CASE("skeleton2d ik: cook errors name the constraint and the bone") {
    struct Case {
        const char* ik;
        const char* anims;
        const char* expect;
    };
    const Case cases[] = {
        {R"([ { "name": "arm", "bones": ["upper", "elbow"] } ])", "[]",
         "ik constraint 'arm': 'bones' must name bones of the skeleton"},
        {R"([ { "name": "arm", "bones": ["upper", "other"] } ])", "[]",
         "ik constraint 'arm': bone 'other' is not a child of 'upper'"},
        {R"([ { "name": "arm", "bones": ["upper", "lower"], "target": "upper" } ])", "[]",
         "ik constraint 'arm': target bone 'upper' is in the chain or under its first bone"},
        {R"([ { "name": "arm", "bones": ["upper", "lower"], "target": "tip" } ])", "[]",
         "ik constraint 'arm': target bone 'tip' is in the chain or under its first bone"},
        {R"([ { "name": "arm", "bones": ["upper", "lower"], "target": "nope" } ])", "[]",
         "ik constraint 'arm': 'target' must name a bone of the skeleton, or be null"},
        {R"([ { "name": "arm", "bones": ["upper", "lower", "tip"], "type": "two_bone" } ])", "[]",
         "ik constraint 'arm': a two-bone constraint lists exactly 2 bones"},
        {R"([ { "name": "arm", "bones": ["upper", "lower", "tip"], "bend": "negative" } ])", "[]",
         "ik constraint 'arm': 'bend' belongs to a two-bone constraint"},
        {R"([ { "name": "arm", "bones": ["upper", "lower"], "bend": "left" } ])", "[]",
         "ik constraint 'arm': 'bend' must be \"positive\" or \"negative\""},
        {R"([ { "name": "arm", "bones": ["upper", "lower"], "iterations": 4 } ])", "[]",
         "ik constraint 'arm': 'iterations' belongs to a CCD constraint"},
        {R"([ { "name": "arm", "bones": ["upper"], "iterations": 0 } ])", "[]",
         "ik constraint 'arm': 'iterations' must be a whole number from 1 to 64"},
        {R"([ { "name": "arm", "bones": ["upper"], "mix": 2 } ])", "[]", "ik constraint 'arm': 'mix' must be in 0..1"},
        {R"([ { "name": "arm", "bones": ["upper"], "type": "fabrik" } ])", "[]",
         "ik constraint 'arm': 'type' must be \"two_bone\" or \"ccd\""},
        {R"([ { "name": "arm", "bones": [] } ])", "[]",
         "ik constraint 'arm': 'bones' must be an array of 1 to 32 bone names"},
        {R"([ { "name": "arm", "bones": ["upper"] }, { "name": "arm", "bones": ["lower"] } ])", "[]",
         "ik constraint 'arm': two IK constraints share this name"},
        {R"([ { "name": "arm", "bones": ["upper"], "pole": 1 } ])", "[]", "pole"},
        {R"([ { "name": "arm", "bones": ["upper"] } ])", R"([ { "name": "walk", "ik": { "leg": [ { "t": 0 } ] } } ])",
         "animation 'walk', ik 'leg': no IK constraint has that name"},
        {R"([ { "name": "arm", "bones": ["upper"] } ])",
         R"([ { "name": "walk", "ik": { "arm": [ { "t": 0, "mix": 1.5 } ] } } ])",
         "animation 'walk', ik 'arm': 'mix' must be in 0..1"},
        {R"([ { "name": "arm", "bones": ["upper"] } ])",
         R"([ { "name": "walk", "ik": { "arm": [ { "t": 1 }, { "t": 0.5 } ] } } ])",
         "animation 'walk', ik 'arm': mix keys must be in increasing time order"},
        {R"([ { "name": "arm", "bones": ["upper"] } ])", R"([ { "name": "walk", "ik": { "arm": [] } } ])",
         "animation 'walk', ik 'arm': keys must be an array of at least one key"},
    };
    u32 i = 0;
    for (const Case& c : cases) {
        const std::string root = fresh_dir("skeleton2d_ik_errors_" + std::to_string(i++));
        write_file(
            root + "/assets/bad.mcskel2d",
            std::string(R"({ "format": "mce-skeleton2d", "version": 1, "guid": "5c2d0000-0000-4000-8000-0000000003f0",
    "bones": [ { "name": "root" }, { "name": "upper", "parent": "root" }, { "name": "lower", "parent": "upper" },
               { "name": "tip", "parent": "lower" }, { "name": "other", "parent": "root" } ], "ik": )") +
                c.ik + R"(, "animations": )" + c.anims + " }");
        cook::CookService svc;
        const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
        CHECK_FALSE(r.ok());
        const std::string text = diag_text(r);
        INFO(text);
        CHECK(text.find(c.expect) != std::string::npos);
        CHECK(text.find("bad.mcskel2d:") != std::string::npos);
    }
    // At most 16 constraints.
    std::string many = "[";
    for (u32 k = 0; k < 17; ++k)
        many += (k ? ", " : "") + std::string(R"({ "name": "c)") + std::to_string(k) + R"(", "bones": ["upper"] })";
    many += "]";
    const std::string root = fresh_dir("skeleton2d_ik_errors_many");
    write_file(
        root + "/assets/bad.mcskel2d",
        std::string(R"({ "format": "mce-skeleton2d", "version": 1, "guid": "5c2d0000-0000-4000-8000-0000000003f1",
    "bones": [ { "name": "root" }, { "name": "upper", "parent": "root" } ], "ik": )") +
            many + " }");
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    CHECK_FALSE(r.ok());
    CHECK(diag_text(r).find("'ik' must be an array of at most 16 constraints") != std::string::npos);
}

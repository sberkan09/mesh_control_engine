// M-ANIM2D 6: samples/skel2d_demo, the courier. The sample's own assets are cooked and played with the
// stock systems, as mce_player does: a timeline drives the animator's parameters, the .mcanim graph
// picks idle, walk and jump, IK keeps the feet on two IKTarget2D entities while it idles and lands
// them again after the jump, the face attachment swaps in the air, a lantern follows a hand bone,
// and two runs hash the same.

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <string>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"
#include "mce/ecs/transform.h"
#include "mce/kit/skeleton2d.h"
#include "mce/kit/stock.h"
#include "mce/scene/scene.h"

using namespace mce;
using namespace cook_test;
using kit::Bone2DTransform;
using kit::Skeleton2D;
using kit::Skeleton2DAnimator;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
constexpr const char* kScene = "5d1e0000-0000-4000-8000-000000000005";
// Entities in the scene's order.
constexpr u32 kCourier = 1, kFootNear = 2, kFootFar = 3, kLantern = 4;
constexpr u32 kHeadSlot = 10;  // the head is the 11th slot; attachment 1 is "wow"

const std::string& project() {
    static const std::string root = [] {
        const std::string r = fresh_dir("skeleton2d_demo_project");
        std::filesystem::copy(std::string(MCE_SOURCE_DIR) + "/samples/skel2d_demo/assets", r + "/assets",
                              std::filesystem::copy_options::recursive);
        cook::CookService svc;
        const cook::CookReport rep = svc.cook(StringView(r.c_str()), cook::CookOptions{});
        std::string diags;
        for (const cook::Diagnostic& d : rep.diags.all()) {
            cook::String line;
            cook::format_diagnostic(d, line);
            diags += std::string(line.view().data(), line.view().size()) + "\n";
        }
        INFO(diags);
        REQUIRE(rep.ok());
        return r;
    }();
    return root;
}

struct Demo {
    asset::AssetSystem assets;
    World world;
    Array<Entity> e;
    u64 ticks = 0;

    Demo() {
        const std::string cooked = project() + "/cooked";
        const StringView roots[] = {StringView(cooked.c_str())};
        REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
        kit::install_stock(world, &assets);
        REQUIRE(scene::load(world, assets, asset::parse_guid(kScene).value(), &e));
        REQUIRE(e.size() == 15);
    }
    void run_to(u64 tick) {
        while (ticks < tick) world.tick(kDt, ticks++);
    }
    Skeleton2D& sk() { return *world.try_get_mut<Skeleton2D>(e[kCourier]); }
    Skeleton2DAnimator& anim() { return *world.try_get_mut<Skeleton2DAnimator>(e[kCourier]); }
    math::vec3 at(u32 i) { return world.try_get<LocalTransform>(e[i])->position; }
    Bone2DTransform bone(const char* name) {
        Bone2DTransform t;
        REQUIRE(kit::skeleton2d_bone(world, e[kCourier], StringView(name), t));
        return t;
    }
    // A foot bone's distance from its target, both in the skeleton's space.
    f32 foot_gap(const char* bone_name, u32 target) {
        const Bone2DTransform b = bone(bone_name);
        const math::vec3 t = at(target);
        return std::hypot(b.x - t.x, b.y - t.y);
    }
};

}  // namespace

TEST_CASE("skel2d_demo: the sample's assets cook") { CHECK(!project().empty()); }

TEST_CASE("skel2d_demo: idle plants the feet on the IK targets, walking frees them, the jump swaps the face") {
    Demo d;
    // 1 s in: the graph's start state, the timeline holds the courier at x = -2.
    d.run_to(60);
    CHECK(d.anim().state.view() == StringView("idle"));
    CHECK(d.at(kCourier).x == doctest::Approx(-2.0f));
    CHECK(d.foot_gap("foot_near", kFootNear) < 2e-3f);
    CHECK(d.foot_gap("foot_far", kFootFar) < 2e-3f);
    // The IK is doing the work: the idle clip bobs the hip, the foot would leave the ground without it.
    CHECK(d.sk().pose[1].y < -0.02f);

    // The lantern is on the near forearm's tip: world = courier + bone * (0.3, -0.04).
    const math::vec2 hand = d.bone("arm_near_low").apply({0.3f, -0.04f});
    CHECK(d.at(kLantern).x == doctest::Approx(d.at(kCourier).x + hand.x).epsilon(1e-3));
    CHECK(d.at(kLantern).y == doctest::Approx(d.at(kCourier).y + hand.y).epsilon(1e-3));

    // 3 s in (the timeline walks it from 1.5 s): the walk state, the legs' IK mix keyed to 0 and
    // a courier on its way.
    d.run_to(180);
    CHECK(d.anim().state.view() == StringView("walk"));
    const kit::Skeleton2DIK* ik = d.world.try_get<kit::Skeleton2DIK>(d.e[kCourier]);
    REQUIRE(ik);
    CHECK(ik->constraint_count == 2);
    CHECK(ik->mix[0] == 0.0f);
    CHECK(ik->mix[1] == 0.0f);
    CHECK(d.at(kCourier).x > -2.0f);
    CHECK(d.at(kCourier).x < 2.0f);
    CHECK(d.sk().pose[9].rotation != 0.0f);  // thigh_near swings

    // 5.9 s: the jump (the parameter pulses at 5.5 s), the root is up, the face is the surprised one.
    d.run_to(354);
    CHECK(d.anim().state.view() == StringView("jump"));
    CHECK(d.sk().pose[0].y > 0.4f);
    const kit::Skeleton2DSlots* slots = d.world.try_get<kit::Skeleton2DSlots>(d.e[kCourier]);
    REQUIRE(slots);
    CHECK(slots->attachment[kHeadSlot] == 1);

    // 6.7 s: back in idle after the one-shot, calm face, the feet back on their targets.
    d.run_to(400);
    CHECK(d.anim().state.view() == StringView("idle"));
    CHECK(slots->attachment[kHeadSlot] == 0);
    CHECK(d.sk().pose[0].y == doctest::Approx(0.0f).epsilon(1e-3));
    CHECK(d.foot_gap("foot_near", kFootNear) < 2e-3f);
    CHECK(d.foot_gap("foot_far", kFootFar) < 2e-3f);

    // 7.4 s: walking back, mirrored.
    d.run_to(444);
    CHECK(d.sk().flip_x);
    d.run_to(540);
    CHECK(d.at(kCourier).x < 2.0f);
}

TEST_CASE("skel2d_demo: two runs hash the same, a longer run differs") {
    Demo a, b, c;
    a.run_to(900);  // 1.5 laps of the 12 s timeline
    b.run_to(900);
    c.run_to(899);
    CHECK(a.world.state_hash() == b.world.state_hash());
    CHECK(a.world.state_hash() != c.world.state_hash());
}

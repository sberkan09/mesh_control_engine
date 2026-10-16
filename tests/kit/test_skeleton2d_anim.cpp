// M-ANIM2D 2: 2D bone animation. Key sampling with exact numbers (linear, stepped, bezier), the
// .mcskel2d "animations" cook and its errors, the CHNK_SKEL2D reader's animation rejects, and the
// Skeleton2DAnimator: looping and one-shot clips, cross-fade weights, events fired once per
// crossing, an AnimGraph driving 2D clips, untouched channels, determinism and save/load mid-fade.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/kit/anim3d.h"
#include "mce/kit/skeleton2d.h"
#include "mce/scene/scene.h"
#include "mce/string_id.h"

using namespace mce;
using namespace cook_test;
using asset::Skeleton2DKey;
using kit::Skeleton2D;
using kit::Skeleton2DAnimator;

namespace {

constexpr f32 kPi = 3.14159265358979f;
constexpr f32 kDt = 0.125f;  // a power of two: playheads add up exactly
constexpr const char* kHero = "5c2d0000-0000-4000-8000-000000000101";
constexpr const char* kScene = "5c2d0000-0000-4000-8000-000000000102";
constexpr const char* kGraph = "5c2d0000-0000-4000-8000-000000000103";
constexpr u32 kArm = 1;  // the arm bone's index

std::string diag_text(const cook::CookReport& r) {
    std::string s;
    for (const cook::Diagnostic& d : r.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        s += std::string(line.view().data(), line.view().size()) + "\n";
    }
    return s;
}

Skeleton2DKey key(f32 t, f32 v0, f32 v1 = 0.0f, asset::Skeleton2DCurve curve = asset::Skeleton2DCurve::Linear) {
    Skeleton2DKey k{};
    k.t = t;
    k.value[0] = v0;
    k.value[1] = v1;
    k.curve = curve;
    return k;
}

// The cooked project: a two-bone skeleton with three animations, a graph over them, and a scene
// with the hero playing "swing" (fade 0.5).
//   swing (loops, 1 s): arm rotates 0 -> 90 degrees linearly; arm scale 1, stepping to 2 at 0.5;
//                       events "start" at 0 and "tick" at 0.5.
//   raise (once, 0.5 s): arm translates y 0 -> 1 on the bezier [1/3, 1, 2/3, 1]; event "top" at 0.5.
//   still (loops, 2 s by "duration"): no keys.
const std::string& project() {
    static const std::string root = [] {
        const std::string r = fresh_dir("skeleton2d_anim_project");
        write_file(r + "/assets/hero.mcskel2d", std::string(R"({ "format": "mce-skeleton2d", "version": 1,
            "guid": ")") + kHero + R"(", "name": "hero",
            "bones": [ { "name": "root" }, { "name": "arm", "parent": "root", "x": 0.5 } ],
            "animations": [
              { "name": "swing",
                "bones": { "arm": {
                  "rotate": [ { "t": 0, "value": 0 }, { "t": 1, "value": 90, "curve": "linear" } ],
                  "scale": [ { "t": 0, "x": 1, "y": 1, "curve": "stepped" }, { "t": 0.5, "x": 2, "y": 2 } ] } },
                "events": [ { "t": 0, "name": "start" }, { "t": 0.5, "name": "tick" } ] },
              { "name": "raise", "loop": false,
                "bones": { "arm": { "translate": [ { "t": 0, "curve": [0.3333333, 1, 0.6666667, 1] },
                                                   { "t": 0.5, "y": 1 } ] } },
                "events": [ { "t": 0.5, "name": "top" } ] },
              { "name": "still", "duration": 2 } ] })");
        write_file(r + "/assets/hero.mcanim", std::string(R"({ "format": "mce-animgraph", "version": 1,
            "guid": ")") + kGraph + R"(", "name": "hero", "params": ["go"],
            "states": [ { "name": "idle", "clip": "swing" }, { "name": "jump", "clip": "raise", "loop": false } ],
            "transitions": [
              { "from": "idle", "to": "jump", "when": [ { "param": "go", "op": ">", "value": 0.5 } ], "fade": 0.25 },
              { "from": "jump", "to": "idle", "on_finish": true, "fade": 0 } ] })");
        write_file(r + "/assets/stage.mcscene", std::string(R"({ "format": "mce-scene", "version": 1,
            "guid": ")") + kScene + R"(", "name": "stage", "entities": [
            { "name": "Hero", "components": {
                "SaveKey": { "key": 1 },
                "LocalTransform": { "position": [0, 0, 0] },
                "Skeleton2D": { "skeleton": "guid:)" +
                                                    kHero + R"(" },
                "Skeleton2DAnimator": { "clip": "swing", "fade": 0.5 } } } ] })");
        cook::CookService svc;
        const cook::CookReport rep = svc.cook(StringView(r.c_str()), cook::CookOptions{});
        INFO(diag_text(rep));
        REQUIRE(rep.ok());
        return r;
    }();
    return root;
}

std::vector<u64>& seen_events() {
    static std::vector<u64> v;
    return v;
}

void collect_events(Frame& f) {
    for (const kit::AnimEvent3D& e : f.events().read<kit::AnimEvent3D>()) seen_events().push_back(e.id);
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
        world.add_system(Phase::FixedUpdate_Pre, "test_collect_events", &collect_events);
        REQUIRE(scene::load(world, assets, asset::parse_guid(kScene).value(), &e));
        REQUIRE(e.size() == 1);
    }
    Skeleton2D& hero() { return *world.try_get_mut<Skeleton2D>(e[0]); }
    Skeleton2DAnimator& anim(Entity x = {}) { return *world.try_get_mut<Skeleton2DAnimator>(x == Entity{} ? e[0] : x); }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) world.tick(kDt, ticks++);
    }
};

u64 id(const char* name) { return StringId(name).value; }

f32 rad(f32 deg) { return deg * kPi / 180.0f; }

}  // namespace

TEST_CASE("skeleton2d anim: linear, stepped and bezier keys sample exactly") {
    using C = asset::Skeleton2DCurve;
    const Skeleton2DKey lin[] = {key(0.0f, 0.0f, 10.0f), key(2.0f, 4.0f, 20.0f)};
    f32 v[2];
    asset::skeleton2d_sample(lin, 0.5f, v);
    CHECK(v[0] == 1.0f);
    CHECK(v[1] == 12.5f);
    asset::skeleton2d_sample(lin, -1.0f, v);  // before the first key: its value
    CHECK(v[0] == 0.0f);
    asset::skeleton2d_sample(lin, 5.0f, v);  // after the last: its value
    CHECK(v[0] == 4.0f);

    const Skeleton2DKey step[] = {key(0.0f, 1.0f, 0.0f, C::Stepped), key(1.0f, 3.0f, 0.0f, C::Stepped),
                                  key(2.0f, 7.0f)};
    asset::skeleton2d_sample(step, 0.99f, v);
    CHECK(v[0] == 1.0f);
    asset::skeleton2d_sample(step, 1.0f, v);
    CHECK(v[0] == 3.0f);
    asset::skeleton2d_sample(step, 1.5f, v);
    CHECK(v[0] == 3.0f);

    // x(s) = s for cx = 1/3, 2/3, so y(0.5) = 3 * 0.25 * 0.5 * (1 + 1) * ... = 0.375 + 0.375 + 0.125.
    const f32 flat[4] = {1.0f / 3.0f, 1.0f, 2.0f / 3.0f, 1.0f};
    CHECK(asset::skeleton2d_bezier(flat, 0.5f) == doctest::Approx(0.875f).epsilon(1e-5));
    // CSS "ease" (0.25, 0.1, 0.25, 1) at x = 0.5: 0.8024033877 (the reference solution).
    const f32 ease[4] = {0.25f, 0.1f, 0.25f, 1.0f};
    CHECK(asset::skeleton2d_bezier(ease, 0.5f) == doctest::Approx(0.8024034f).epsilon(1e-5));
    CHECK(asset::skeleton2d_bezier(ease, 0.0f) == 0.0f);
    CHECK(asset::skeleton2d_bezier(ease, 1.0f) == 1.0f);
    Skeleton2DKey bz[] = {key(0.0f, 0.0f, 0.0f, C::Bezier), key(1.0f, 2.0f, 4.0f)};
    std::memcpy(bz[0].bezier, ease, sizeof ease);
    asset::skeleton2d_sample(bz, 0.5f, v);
    CHECK(v[0] == doctest::Approx(2.0f * 0.8024034f).epsilon(1e-5));
    CHECK(v[1] == doctest::Approx(4.0f * 0.8024034f).epsilon(1e-5));
    // Deterministic: the same input gives the same bits.
    f32 w[2];
    asset::skeleton2d_sample(bz, 0.5f, w);
    CHECK(std::bit_cast<u32>(v[0]) == std::bit_cast<u32>(w[0]));
    CHECK(std::bit_cast<u32>(v[1]) == std::bit_cast<u32>(w[1]));
}

TEST_CASE("skeleton2d anim: animations cook into CHNK_SKEL2D minor 1") {
    Stage s;
    const asset::AssetKey k = s.assets.find(asset::parse_guid(kHero).value());
    REQUIRE(k != asset::kInvalidAssetKey);
    auto h = s.assets.load_sync<asset::Skeleton2D>(k);
    REQUIRE(h);
    auto view = asset::Skeleton2DView::read(s.assets.resolve(h.value()));
    REQUIRE(view);
    const asset::Skeleton2DView& v = view.value();
    REQUIRE(v.animations.size() == 3);
    CHECK(v.find_animation("raise") == 1);
    CHECK(v.find_animation("walk") == -1);
    const asset::Skeleton2DAnimation& swing = v.animations[0];
    CHECK(v.string(swing.name) == StringView("swing"));
    CHECK(swing.duration == 1.0f);  // the last key
    CHECK(swing.flags == asset::Skeleton2DAnimationFlags::Loop);
    REQUIRE(swing.timeline_count == 2);
    const asset::Skeleton2DTimeline& rot = v.timelines[swing.first_timeline];
    CHECK(rot.bone == kArm);
    CHECK(rot.channel == asset::Skeleton2DChannel::Rotate);
    REQUIRE(rot.key_count == 2);
    CHECK(v.keys[rot.first_key + 1].value[0] == doctest::Approx(kPi / 2));  // degrees in the text
    const asset::Skeleton2DTimeline& sc = v.timelines[swing.first_timeline + 1];
    CHECK(sc.channel == asset::Skeleton2DChannel::Scale);
    CHECK(v.keys[sc.first_key].curve == asset::Skeleton2DCurve::Stepped);
    REQUIRE(swing.event_count == 2);
    CHECK(v.string(v.events[swing.first_event + 1].name) == StringView("tick"));
    CHECK(v.events[swing.first_event + 1].id == id("tick"));
    const asset::Skeleton2DAnimation& raise = v.animations[1];
    CHECK(raise.flags == 0u);
    CHECK(raise.duration == 0.5f);
    const asset::Skeleton2DKey& b = v.keys[v.timelines[raise.first_timeline].first_key];
    CHECK(b.curve == asset::Skeleton2DCurve::Bezier);
    CHECK(b.bezier[1] == 1.0f);
    CHECK(v.keys[v.timelines[raise.first_timeline].first_key + 1].value[0] == 0.0f);  // translate x defaults to 0
    CHECK(v.animations[2].duration == 2.0f);
    CHECK(v.animations[2].timeline_count == 0);
    s.assets.release(asset::AnyHandle(h.value()));
}

TEST_CASE("skeleton2d anim: the reader rejects bad animation data and reads a minor-0 payload") {
    // A minimal payload: two bones, one animation with one rotate timeline of two keys, one event.
    struct Parts {
        asset::Skeleton2DHeader h{};
        asset::Skeleton2DBone bones[2]{};
        asset::Skeleton2DAnimation anim{};
        asset::Skeleton2DTimeline tl{};
        Skeleton2DKey keys[2]{};
        asset::Skeleton2DEvent ev{};
        char pool[16] = "root\0arm\0hit";
    };
    Parts base;
    base.h.bone_count = 2;
    base.h.string_bytes = sizeof base.pool;
    base.h.animation_count = 1;
    base.h.timeline_count = 1;
    base.h.key_count = 2;
    base.h.event_count = 1;
    base.bones[0].parent = asset::kSkeleton2DNoParent;
    base.bones[1].name = 5;
    base.bones[1].parent = 0;
    for (asset::Skeleton2DBone& b : base.bones) b.scale_x = b.scale_y = 1.0f;
    base.anim.name = 9;
    base.anim.duration = 1.0f;
    base.anim.timeline_count = 1;
    base.anim.event_count = 1;
    base.tl.bone = 1;
    base.tl.key_count = 2;
    base.keys[0] = key(0.0f, 0.0f);
    base.keys[1] = key(1.0f, 1.0f);
    base.ev.t = 0.5f;
    base.ev.name = 9;
    auto bytes = [](const Parts& p, bool animations = true) {
        Array<byte> out;
        auto append = [&](const void* d, usize n) {
            const usize at = out.size();
            out.resize(at + n, byte{0});
            std::memcpy(out.data() + at, d, n);
        };
        asset::Skeleton2DHeader h = p.h;
        if (!animations) h.animation_count = h.timeline_count = h.key_count = h.event_count = 0;
        append(&h, sizeof h);
        append(p.bones, sizeof p.bones);
        if (animations) {
            append(&p.anim, sizeof p.anim);
            append(&p.tl, sizeof p.tl);
            append(p.keys, sizeof p.keys);
            append(&p.ev, sizeof p.ev);
        }
        append(p.pool, sizeof p.pool);
        return out;
    };
    {
        const Array<byte> ok = bytes(base);
        auto v = asset::Skeleton2DView::read(ok.span());
        REQUIRE(v);
        CHECK(v.value().find_animation("hit") == 0);
        CHECK_FALSE(asset::Skeleton2DView::read(ok.span().subspan(0, ok.size() - 1)));
        const Array<byte> plain = bytes(base, false);  // a step-1 (minor 0) skeleton
        auto p = asset::Skeleton2DView::read(plain.span());
        REQUIRE(p);
        CHECK(p.value().animations.empty());
    }
    auto rejects = [&](auto&& mutate) {
        Parts c = base;
        mutate(c);
        const Array<byte> b = bytes(c);
        return !asset::Skeleton2DView::read(b.span());
    };
    CHECK(rejects([](Parts& c) { c.keys[1].t = 0.0f; }));   // keys not strictly increasing
    CHECK(rejects([](Parts& c) { c.keys[0].t = 2.0f; }));   // unsorted
    CHECK(rejects([](Parts& c) { c.keys[0].t = -1.0f; }));  // negative time
    CHECK(rejects([](Parts& c) { c.tl.bone = 2; }));        // no such bone
    CHECK(rejects([](Parts& c) { c.keys[1].value[0] = std::nanf(""); }));
    CHECK(rejects([](Parts& c) { c.anim.duration = INFINITY; }));
    CHECK(rejects([](Parts& c) { c.tl.channel = static_cast<asset::Skeleton2DChannel>(3); }));
    CHECK(rejects([](Parts& c) { c.keys[0].curve = static_cast<asset::Skeleton2DCurve>(3); }));
    CHECK(rejects([](Parts& c) {
        c.keys[0].curve = asset::Skeleton2DCurve::Bezier;
        c.keys[0].bezier[0] = 1.5f;  // cx out of 0..1: x(s) would not be monotonic
    }));
    CHECK(rejects([](Parts& c) { c.tl.key_count = 0; }));
    CHECK(rejects([](Parts& c) { c.tl.key_count = 3; }));         // past the keys
    CHECK(rejects([](Parts& c) { c.anim.timeline_count = 2; }));  // past the timelines
    CHECK(rejects([](Parts& c) { c.anim.event_count = 2; }));     // past the events
    CHECK(rejects([](Parts& c) { c.anim.name = 99; }));           // string out of range
    CHECK(rejects([](Parts& c) { c.ev.t = std::nanf(""); }));
    CHECK(rejects([](Parts& c) { c.anim.flags = 2; }));
    CHECK(rejects([](Parts& c) { c.h.animation_count = asset::kSkeleton2DMaxAnimations + 1; }));
}

TEST_CASE("skeleton2d anim: cook errors name the animation and the bone") {
    struct Case {
        const char* anims;
        const char* expect;
    };
    const Case cases[] = {
        {R"([ { "name": "walk", "bones": { "arm": { "rotate": [ { "t": 0.5 }, { "t": 0.25 } ] } } } ])",
         "animation 'walk', bone 'arm': rotate keys must be in increasing time order"},
        {R"([ { "name": "walk", "bones": { "leg": { "rotate": [ { "t": 0 } ] } } } ])",
         "animation 'walk', bone 'leg': no bone has that name"},
        {R"([ { "name": "walk", "bones": { "arm": { "scale": [ { "t": 0, "curve": "smooth" } ] } } } ])",
         "animation 'walk', bone 'arm': a key's 'curve' must be"},
        {R"([ { "name": "walk", "bones": { "arm": { "translate": [ { "t": 0, "curve": [1.5, 0, 0.5, 1] } ] } } } ])",
         "animation 'walk', bone 'arm': a bezier curve's cx1 and cx2 must be in 0..1"},
        {R"([ { "name": "walk", "bones": { "arm": { "rotate": [ { "value": 3 } ] } } } ])",
         "animation 'walk', bone 'arm': a key needs a time 't'"},
        {R"([ { "name": "walk", "bones": { "arm": { "rotate": [] } } } ])",
         "'rotate' must be an array of at least one key"},
        {R"([ { "name": "walk" }, { "name": "walk" } ])", "two animations share this name"},
        {R"([ { "name": "walk", "events": [ { "t": 1, "name": "a" }, { "t": 0.5, "name": "b" } ] } ])",
         "animation 'walk': events must be in time order"},
        {R"([ { "name": "walk", "events": [ { "t": 1, "name": "a_very_long_event_name_here" } ] } ])",
         "an event's 'name' must be a string of 1 to 23 characters"},
        {R"([ { "name": "walk", "duration": -1 } ])", "animation 'walk': 'duration' must be 0 or more"},
        {R"([ { "name": "walk", "speed": 2 } ])", "speed"},
    };
    u32 i = 0;
    for (const Case& c : cases) {
        const std::string root = fresh_dir("skeleton2d_anim_errors_" + std::to_string(i++));
        write_file(
            root + "/assets/bad.mcskel2d",
            std::string(R"({ "format": "mce-skeleton2d", "version": 1, "guid": "5c2d0000-0000-4000-8000-0000000001f0",
    "bones": [ { "name": "root" }, { "name": "arm", "parent": "root" } ], "animations": )") +
                c.anims + " }");
        cook::CookService svc;
        const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
        CHECK_FALSE(r.ok());
        const std::string text = diag_text(r);
        INFO(text);
        CHECK(text.find(c.expect) != std::string::npos);
        CHECK(text.find("bad.mcskel2d:") != std::string::npos);
    }
}

TEST_CASE("skeleton2d anim: a looping clip wraps and fires each event once per crossing") {
    seen_events().clear();
    Stage s;
    s.run(1);  // the starting tick: time 0, the rest of the clip at 0
    CHECK(s.anim().playing == id("swing"));
    CHECK(s.anim().time == 0.0f);
    CHECK(s.hero().pose[kArm].rotation == 0.0f);
    s.run(1);  // [0, 0.125): "start"
    CHECK(s.anim().time == 0.125f);
    CHECK(s.hero().pose[kArm].rotation == doctest::Approx(rad(90.0f) * 0.125f));
    CHECK(s.hero().pose[kArm].scale_x == 1.0f);  // stepped: 1 until 0.5
    CHECK(s.anim().events_fired == 1);
    CHECK(s.anim().last_event.view() == StringView("start"));
    s.run(3);  // time 0.5: the scale steps to 2
    CHECK(s.anim().time == 0.5f);
    CHECK(s.hero().pose[kArm].scale_x == 2.0f);
    CHECK(s.hero().pose[kArm].scale_y == 2.0f);
    CHECK(s.anim().events_fired == 1);  // "tick" at 0.5 is crossed by the next tick's [0.5, 0.625)
    s.run(1);
    CHECK(s.anim().events_fired == 2);
    CHECK(s.anim().last_event.view() == StringView("tick"));
    s.run(3);  // 0.875 -> 1.0 wraps to 0
    CHECK(s.anim().time == 0.0f);
    CHECK(s.anim().events_fired == 2);
    CHECK_FALSE(s.anim().finished);
    // Faster: one tick goes 0 -> 0.375 -> 0.75 -> 1.125, wrapping to 0.125 and crossing "start" again.
    s.anim().speed = 3.0f;
    s.run(3);
    CHECK(s.anim().time == 0.125f);
    CHECK(s.anim().events_fired == 5);  // start, tick, start
    s.run(1);                           // the collector reads last tick's events in FixedUpdate_Pre
    const std::vector<u64> want = {id("start"), id("tick"), id("start"), id("tick"), id("start")};
    CHECK(seen_events() == want);
    // The root bone is keyed by no clip: never written.
    CHECK(s.hero().pose[0] == kit::Bone2DPose{});
}

TEST_CASE("skeleton2d anim: a cross-fade blends by weight and a one-shot holds its end") {
    Stage s;
    // Gameplay owns channels no clip keys: the arm's translation (until "raise" keys it) and the root.
    s.hero().pose[0].x = 3.0f;
    s.hero().pose[kArm].x = 0.25f;
    s.run(4);  // swing at 0.375
    CHECK(s.anim().time == 0.375f);
    CHECK(s.hero().pose[kArm].x == 0.25f);
    s.anim().clip = "raise";  // fade 0.5 (the scene's)
    s.run(1);                 // the switching tick: raise at 0, swing held at 0.375 with full weight
    CHECK(s.anim().playing == id("raise"));
    CHECK(s.anim().previous == id("swing"));
    CHECK(s.anim().blend == 0.0f);
    CHECK(s.hero().pose[kArm].rotation == doctest::Approx(rad(90.0f) * 0.375f));
    CHECK(s.hero().pose[kArm].y == 0.0f);  // raise keys it (weight 0): written
    CHECK(s.hero().pose[kArm].x == 0.0f);  // the translate channel now belongs to the clips
    s.run(2);                              // halfway through the fade
    CHECK(s.anim().time == 0.25f);
    CHECK(s.anim().previous_time == 0.625f);
    CHECK(s.anim().blend == 0.5f);
    const Skeleton2D& h = s.hero();
    CHECK(h.pose[kArm].rotation ==
          doctest::Approx(0.5f * rad(90.0f) * 0.625f));                     // swing at half weight, raise adds rest (0)
    CHECK(h.pose[kArm].y == doctest::Approx(0.5f * 0.875f).epsilon(1e-5));  // raise's bezier at u = 0.5, half weight
    CHECK(h.pose[kArm].scale_x == 1.5f);  // swing's 2 at half weight + one (rest) at the other half
    CHECK(h.pose[0].x == 3.0f);
    s.run(2);  // raise reaches 0.5: "top" fires, the one-shot holds just under its end
    CHECK(s.anim().finished);
    CHECK(s.anim().time == std::nextafter(0.5f, 0.0f));
    CHECK(s.anim().blend == 1.0f);
    CHECK(s.anim().previous == 0);
    CHECK(s.anim().last_event.view() == StringView("top"));
    const u32 fired = s.anim().events_fired;
    CHECK(s.hero().pose[kArm].y == doctest::Approx(1.0f).epsilon(1e-4));
    s.run(4);
    CHECK(s.anim().events_fired == fired);  // held: nothing more
    CHECK(s.anim().finished);
    s.anim().restart = true;
    s.run(1);
    CHECK_FALSE(s.anim().finished);
    CHECK(s.anim().time == 0.0f);
    CHECK(s.anim().blend == 1.0f);  // restarting the same clip is a cut
    CHECK_FALSE(s.anim().restart);
    // Off: the animator's loop plays even a looping clip once.
    s.anim().clip = "swing";
    s.anim().fade = 0.0f;
    s.anim().loop = false;
    s.run(9);
    CHECK(s.anim().finished);
    CHECK(s.anim().time == std::nextafter(1.0f, 0.0f));
}

TEST_CASE("skeleton2d anim: an AnimGraph drives 2D clips on a parameter and on a one-shot's end") {
    Stage s;
    Skeleton2D sk;
    sk.skeleton.guid = asset::parse_guid(kHero).value();
    Skeleton2DAnimator a;
    a.graph.guid = asset::parse_guid(kGraph).value();
    const Entity e = s.world.spawn_with(LocalTransform{}, sk, a);
    s.run(1);
    CHECK(s.anim(e).state.view() == StringView("idle"));
    CHECK(s.anim(e).clip.view() == StringView("swing"));
    CHECK(s.anim(e).blend == 1.0f);
    s.run(1);
    CHECK(s.anim(e).time == 0.125f);
    s.anim(e).params.x = 1.0f;  // go
    s.run(1);                   // idle advances to 0.25, then the transition enters jump (fade 0.25)
    CHECK(s.anim(e).state.view() == StringView("jump"));
    CHECK(s.anim(e).playing == id("raise"));
    CHECK(s.anim(e).previous == id("swing"));
    CHECK(s.anim(e).previous_time == 0.25f);
    CHECK(s.anim(e).blend == 0.0f);
    s.anim(e).params.x = 0.0f;
    s.run(1);
    CHECK(s.anim(e).blend == 0.5f);  // halfway through the transition's fade
    CHECK(s.world.try_get<Skeleton2D>(e)->pose[kArm].rotation == doctest::Approx(0.5f * rad(90.0f) * 0.375f));
    s.run(2);
    CHECK(s.anim(e).state.view() == StringView("jump"));
    CHECK(s.anim(e).time == 0.375f);
    s.run(1);  // raise ends: "top" fires, the one-shot finishes and on_finish returns to idle (a cut)
    CHECK(s.anim(e).state.view() == StringView("idle"));
    CHECK(s.anim(e).playing == id("swing"));
    CHECK(s.anim(e).time == 0.0f);
    CHECK(s.anim(e).blend == 1.0f);
    CHECK(s.anim(e).last_event.view() == StringView("top"));
    s.anim(e).restart = true;
    s.run(1);
    CHECK(s.anim(e).state.view() == StringView("idle"));
    CHECK_FALSE(s.anim(e).restart);
}

TEST_CASE("skeleton2d anim: two runs hash the same, and a save mid-fade resumes identically") {
    auto script = [](Stage& s, u64 tick) {
        if (tick == 4) s.anim().clip = "raise";
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
    for (u32 i = 0; i < 7; ++i) {
        script(c, c.ticks);
        c.run(1);
    }
    REQUIRE(c.anim().blend == 0.5f);  // mid-fade
    Array<byte> bytes;
    save::write_save(c.world, save::SaveInfo{}, bytes);
    Stage d;  // the scene as authored
    d.ticks = c.ticks;
    REQUIRE(save::read_save(d.world, bytes.span()));
    CHECK(d.anim().playing == c.anim().playing);
    CHECK(d.anim().previous == c.anim().previous);
    CHECK(d.anim().blend == 0.5f);
    CHECK(d.anim().previous_time == c.anim().previous_time);
    CHECK(d.hero().pose[kArm] == c.hero().pose[kArm]);
    c.run(1);
    d.run(1);
    CHECK(d.anim().blend == c.anim().blend);
    CHECK(d.hero().pose[kArm] == c.hero().pose[kArm]);
    for (u32 i = 0; i < 2; ++i) CHECK(d.hero().world[i] == c.hero().world[i]);
    CHECK(d.world.state_hash() == c.world.state_hash());
}

TEST_CASE("skeleton2d anim: the animators run before the skeletons in FixedUpdate_Post") {
    Stage s;
    s.run(2);
    // The world pose of this tick already has this tick's animated rotation.
    const f32 r = s.hero().pose[kArm].rotation;
    CHECK(r > 0.0f);
    CHECK(s.hero().world[kArm].b == doctest::Approx(std::sin(r)));
}

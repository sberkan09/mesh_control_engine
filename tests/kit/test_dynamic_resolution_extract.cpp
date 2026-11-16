// Extract feed of dynamic resolution (M-DYNRES 46d): Kit3DExtract copies the first enabled DynamicResolution3D (by entity
// index) into Scene3D post.dynres through apply_quality. The fields arrive unchanged, out-of-range and NaN values arrive
// clamped or as the defaults; no component and enabled 0 leave the disabled default (the fixed render scale, bit for
// bit); r.dynres 0 turns it off so the renderer's controller resolves to nothing; a cvar target or an injected schedule
// does not touch the feed (the renderer reads them). Nothing is written back to the world: over a run of the weather and
// the day clock with an injected overload, a world with the component ends in the state of the same world without it.

#include <doctest/doctest.h>

#include <bit>
#include <limits>

#include "mce/convar.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/day_cycle.h"
#include "mce/kit/dynamic_resolution.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/weather.h"
#include "mce/render/dynres.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }

struct Rig {
    render::MeshStore store;
    World world;
    Kit3DExtract extract;
    render::Scene3D scene;
    render::Camera3D cam;

    explicit Rig(bool weather = false) : scene(store, mem::heap()) {
        install_kit3d(world, nullptr);
        if (weather) install_weather(world, nullptr);
        extract.init(store, nullptr);
        extract.set_frame_dt(0.25f);
        world.spawn_with(LocalTransform{}, Camera3DComponent{});
        REQUIRE(extract.camera(world, 1.0f, cam));
    }
    render::DynamicResolution frame() {
        scene.begin(cam, {320, 180});
        extract.extract(world, scene, 1.0f);
        return scene.post().dynres;
    }
};

struct CvarScope {  // sets a convar for the scope, resets it after
    ConVar* cv;
    CvarScope(const char* name, const char* value) : cv(ConVar::find(StringId(name))) {
        REQUIRE(cv != nullptr);
        REQUIRE(cv->set_string(value));
    }
    ~CvarScope() { cv->reset(); }
    CvarScope(const CvarScope&) = delete;
    CvarScope& operator=(const CvarScope&) = delete;
};

bool is_default(const render::DynamicResolution& d) {
    const render::DynamicResolution r;
    return d.enabled == r.enabled && same_bits(d.target_ms, r.target_ms) && same_bits(d.min_scale, r.min_scale) &&
           same_bits(d.max_scale, r.max_scale) && same_bits(d.headroom, r.headroom) && d.pad_[0] == 0 && d.pad_[1] == 0 &&
           d.pad_[2] == 0;
}

}  // namespace

TEST_CASE("dynamic resolution extract: no component keeps the disabled default") {
    Rig r;
    for (int i = 0; i < 3; ++i) CHECK(is_default(r.frame()));
}

TEST_CASE("dynamic resolution extract: enabled 0 keeps the disabled default") {
    Rig r;
    DynamicResolution3D c;
    c.enabled = 0;
    c.target_ms = 8.0f;
    r.world.spawn_with(c);
    CHECK(is_default(r.frame()));
}

TEST_CASE("dynamic resolution extract: an enabled component reaches PostFx::dynres unchanged") {
    Rig r;
    DynamicResolution3D c;
    c.target_ms = 9.5f;
    c.min_scale = 0.7f;
    c.max_scale = 0.9f;
    c.headroom = 0.2f;
    r.world.spawn_with(c);
    const render::DynamicResolution d = r.frame();
    CHECK(d.enabled);
    CHECK(same_bits(d.target_ms, 9.5f));
    CHECK(same_bits(d.min_scale, 0.7f));
    CHECK(same_bits(d.max_scale, 0.9f));
    CHECK(same_bits(d.headroom, 0.2f));
    CHECK(d.pad_[0] == 0u);
    // The default component gives the render defaults, switched on.
    r.world.query<DynamicResolution3D&>().each([](DynamicResolution3D& k) { k = DynamicResolution3D{}; });
    render::DynamicResolution want;
    want.enabled = true;
    const render::DynamicResolution e = r.frame();
    CHECK(e.enabled);
    CHECK(same_bits(e.target_ms, want.target_ms));
    CHECK(same_bits(e.min_scale, want.min_scale));
    CHECK(same_bits(e.max_scale, want.max_scale));
    CHECK(same_bits(e.headroom, want.headroom));
}

TEST_CASE("dynamic resolution extract: out-of-range and NaN values arrive clamped or as the defaults") {
    Rig r;
    DynamicResolution3D c;
    c.target_ms = 500.0f;
    c.min_scale = 0.1f;
    c.max_scale = 3.0f;
    c.headroom = 0.0f;
    const Entity e = r.world.spawn_with(c);
    render::DynamicResolution d = r.frame();
    CHECK(d.enabled);
    CHECK(same_bits(d.target_ms, 100.0f));
    CHECK(same_bits(d.min_scale, 0.5f));
    CHECK(same_bits(d.max_scale, 1.0f));
    CHECK(same_bits(d.headroom, 0.05f));
    c.target_ms = 1.0f;
    c.min_scale = 0.9f;
    c.max_scale = 0.7f;  // under min_scale: raised to it
    c.headroom = 2.0f;
    r.world.set(e, c);
    d = r.frame();
    CHECK(same_bits(d.target_ms, 4.0f));
    CHECK(same_bits(d.min_scale, 0.9f));
    CHECK(same_bits(d.max_scale, 0.9f));
    CHECK(same_bits(d.headroom, 0.5f));
    c.target_ms = c.min_scale = c.max_scale = c.headroom = kNaN;
    r.world.set(e, c);
    d = r.frame();
    CHECK(d.enabled);
    CHECK(same_bits(d.target_ms, 14.0f));
    CHECK(same_bits(d.min_scale, 0.6f));
    CHECK(same_bits(d.max_scale, 1.0f));
    CHECK(same_bits(d.headroom, 0.15f));
}

TEST_CASE("dynamic resolution extract: the first enabled component by entity index wins; removing it clears the setting") {
    Rig r;
    DynamicResolution3D off;
    off.enabled = 0;
    off.target_ms = 5.0f;
    const Entity e0 = r.world.spawn_with(off);
    DynamicResolution3D a;
    a.target_ms = 8.0f;
    const Entity e1 = r.world.spawn_with(a);
    DynamicResolution3D b;
    b.target_ms = 20.0f;
    const Entity e2 = r.world.spawn_with(b, LocalTransform{});  // another table: the pick is by index, not table order
    REQUIRE(e0.index < e1.index);
    REQUIRE(e1.index < e2.index);
    CHECK(same_bits(r.frame().target_ms, 8.0f));
    r.world.destroy(e1);
    CHECK(same_bits(r.frame().target_ms, 20.0f));
    r.world.destroy(e2);
    CHECK(is_default(r.frame()));
}

TEST_CASE("dynamic resolution extract: r.dynres 0 gives no controller; the cvar target and the injection leave the feed") {
    Rig r;
    r.world.spawn_with(DynamicResolution3D{});
    render::DynResParams p;
    {
        CvarScope off("r.dynres", "0");
        const render::DynamicResolution d = r.frame();
        CHECK_FALSE(d.enabled);
        CHECK_FALSE(render::dynres_resolve(d, false, 0.0f, 0.5f, 1.0f, p));
        CHECK_FALSE(render::dynres_resolve(d, true, 0.0f, 0.5f, 1.0f, p));  // the feed itself is off
    }
    const render::DynamicResolution on = r.frame();
    CHECK(on.enabled);
    CHECK(render::dynres_resolve(on, true, 0.0f, 0.5f, 1.0f, p));
    CHECK(same_bits(p.target_ms, 14.0f));
    {
        CvarScope target("r.dynres.target_ms", "8");
        CvarScope inject("r.dynres.inject", "30@0");
        const render::DynamicResolution d = r.frame();
        CHECK(d.enabled);
        CHECK(same_bits(d.target_ms, 14.0f));  // the renderer applies the cvar, the feed carries the authored value
    }
    // Every tier allows it: the tier's render_scale is the ceiling (the renderer's), the feed stays on.
    for (const render::QualityTier t : {render::QualityTier::Low, render::QualityTier::Medium, render::QualityTier::High}) {
        r.extract.set_quality(render::quality_settings(t));
        CHECK(r.frame().enabled);
    }
}

TEST_CASE("dynamic resolution extract: nothing reaches the simulation; a run with the component ends where one without it "
          "does") {
    // Two copies of a world that moves (the weather, the day clock), one with the component on an extra entity and an
    // injected overload, the other with the same entity bare; the extract runs every tick. At the end the component is
    // taken off: the two worlds then hash the same, so nothing the component or the feed did reached any other state.
    CvarScope inject("r.dynres.inject", "30@0,6@60");
    auto run = [](bool with_component) {
        Rig r(true);
        Weather w;
        w.target = WeatherKind::Rain;
        w.transition_seconds = 2.0f;
        r.world.spawn_with(w);
        r.world.spawn_with(DayCycle{});
        DynamicResolution3D c;
        c.target_ms = 10.0f;
        const Entity e = with_component ? r.world.spawn_with(c) : r.world.spawn();
        u64 tick = 0;
        u64 mid = 0;
        const u64 start = r.world.state_hash();
        for (int i = 0; i < 120; ++i) {
            r.world.tick(1.0 / 60.0, tick++);
            const render::DynamicResolution d = r.frame();
            CHECK(d.enabled == with_component);
            if (i == 60) mid = r.world.state_hash();
        }
        if (with_component) {
            r.world.query<const DynamicResolution3D&>().each([](const DynamicResolution3D& k) {
                CHECK(same_bits(k.target_ms, 10.0f));  // the component itself is never written
                CHECK(k.enabled == 1u);
            });
            r.world.remove<DynamicResolution3D>(e);
        }
        CHECK(r.world.state_hash() != start);  // the world moved over the run
        return std::make_pair(r.world.state_hash(), mid);
    };
    const auto a = run(true);
    const auto b = run(false);
    CHECK(a.first == b.first);
    CHECK(a.second != b.second);  // the worlds did differ while one carried the component (the check above is not vacuous)
}

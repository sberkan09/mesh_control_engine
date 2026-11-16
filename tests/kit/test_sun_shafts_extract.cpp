// Extract feed of the sun light shafts (M-SUNSHAFTS 45d): Kit3DExtract copies the first enabled SunShafts3D (by entity
// index) into Scene3D post.shafts through apply_quality, with `visibility` from sun_shafts_visibility (the post-quality
// cloud cover). No component or enabled 0 leaves the disabled default (bit-identical to a scene without the feature);
// the Low tier and r.sun_shafts 0 turn it off, so the pass gets no frame; the sun the pass reads is the scene's light
// (Environment3D and its day clock), so a sunset moves the projected sun and fades the shafts with no component edit;
// nothing is written back to the world and the state hash over a run does not move. The pass works in view space, so no
// rebase case is needed.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>

#include "mce/convar.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/cloud_layer.h"
#include "mce/kit/day_cycle.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/sun_shafts.h"
#include "mce/kit/weather.h"
#include "mce/render/atmosphere.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"
#include "mce/render/sun_shafts.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::kit;

namespace {

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }

struct Rig {
    render::MeshStore store;
    World world;
    Kit3DExtract extract;
    render::Scene3D scene;
    render::Camera3D cam;

    Rig() : scene(store, mem::heap()) {
        install_kit3d(world, nullptr);
        extract.init(store, nullptr);
        extract.set_frame_dt(0.25f);
        world.spawn_with(LocalTransform{}, Camera3DComponent{});
        REQUIRE(extract.camera(world, 1.0f, cam));
    }
    render::SunShafts frame() {
        scene.begin(cam, {320, 180});
        extract.extract(world, scene, 1.0f);
        return scene.post().shafts;
    }
    Environment3D& env() { return world.resource<Environment3D>(); }
};

ConVar& shafts_cvar() {
    ConVar* cv = ConVar::find(StringId("r.sun_shafts"));
    REQUIRE(cv != nullptr);
    return *cv;
}

struct ResetShaftsCvar {
    ~ResetShaftsCvar() { shafts_cvar().reset(); }
};

bool is_default(const render::SunShafts& s) {
    const render::SunShafts d;
    return s.enabled == d.enabled && same_bits(s.intensity, d.intensity) && same_bits(s.length, d.length) &&
           same_bits(s.decay, d.decay) && same_bits(s.threshold, d.threshold) && same_bits(s.cap, d.cap) &&
           same_bits(s.visibility, d.visibility) && same_bits(s.scatter_m, d.scatter_m) && s.pad_[0] == 0 &&
           s.pad_[1] == 0 && s.pad_[2] == 0;
}

// A camera at the origin looking level toward the horizontal heading of `toward` (unit, toward the sun).
render::View3D view_toward(math::vec3 toward) {
    const f32 yaw = std::atan2(-toward.x, -toward.z);  // orbit_camera: yaw 0 looks down -Z, R_y(yaw) * (0, 0, -1)
    const render::Camera3D c = render::orbit_camera({0.0f, 0.0f, 0.0f}, yaw, 0.0f, 0.001f);
    return render::compute_view_3d(c, {320, 180});
}

// The frame the pass would draw with this extract's inputs (High tier detail).
bool pass_frame(const Rig& r, const render::View3D& view, render::SunShaftsFrame& out) {
    return render::sun_shafts_frame(r.scene.post().shafts, r.scene.light(), r.scene.fog(), r.scene.volumetrics(), false,
                                    view.view_proj, 320.0f / 180.0f, 2, 0.0f, out);
}

}  // namespace

TEST_CASE("sun shafts extract: no component keeps the disabled default") {
    Rig r;
    for (int i = 0; i < 3; ++i) CHECK(is_default(r.frame()));
}

TEST_CASE("sun shafts extract: enabled 0 keeps the disabled default") {
    Rig r;
    SunShafts3D c;
    c.enabled = 0;
    c.intensity = 3.0f;
    r.world.spawn_with(c);
    CHECK(is_default(r.frame()));
}

TEST_CASE("sun shafts extract: an enabled component reaches PostFx::shafts unchanged") {
    Rig r;
    SunShafts3D c;
    c.intensity = 2.5f;
    c.length = 0.6f;
    c.decay = 0.9f;
    c.threshold = 2.0f;
    c.cap = 3.0f;
    c.scatter_m = 120.0f;
    r.world.spawn_with(c);
    const render::SunShafts s = r.frame();
    CHECK(s.enabled);
    CHECK(same_bits(s.intensity, 2.5f));
    CHECK(same_bits(s.length, 0.6f));
    CHECK(same_bits(s.decay, 0.9f));
    CHECK(same_bits(s.threshold, 2.0f));
    CHECK(same_bits(s.cap, 3.0f));
    CHECK(same_bits(s.scatter_m, 120.0f));
    CHECK(same_bits(s.visibility, 1.0f));  // a clear sky
}

TEST_CASE("sun shafts extract: out-of-range component values arrive clamped") {
    Rig r;
    SunShafts3D c;
    c.intensity = 100.0f;
    c.length = 0.0f;
    c.decay = 2.0f;
    c.threshold = 0.0f;
    c.cap = 50.0f;
    c.scatter_m = 1.0f;
    r.world.spawn_with(c);
    const render::SunShafts s = r.frame();
    CHECK(s.enabled);
    CHECK(same_bits(s.intensity, 4.0f));
    CHECK(same_bits(s.length, 0.1f));
    CHECK(same_bits(s.decay, 1.0f));
    CHECK(same_bits(s.threshold, 0.05f));
    CHECK(same_bits(s.cap, 8.0f));
    CHECK(same_bits(s.scatter_m, 5.0f));
}

TEST_CASE("sun shafts extract: the first enabled component by entity index wins; removing it clears the shafts") {
    Rig r;
    SunShafts3D off;
    off.enabled = 0;
    off.intensity = 4.0f;
    const Entity e0 = r.world.spawn_with(off);
    SunShafts3D a;
    a.intensity = 2.0f;
    const Entity e1 = r.world.spawn_with(a);
    SunShafts3D b;
    b.intensity = 3.0f;
    const Entity e2 = r.world.spawn_with(b, LocalTransform{});  // another table: the pick is by index, not table order
    REQUIRE(e0.index < e1.index);
    REQUIRE(e1.index < e2.index);
    CHECK(same_bits(r.frame().intensity, 2.0f));
    r.world.destroy(e1);
    CHECK(same_bits(r.frame().intensity, 3.0f));
    r.world.destroy(e2);
    CHECK(is_default(r.frame()));
}

TEST_CASE("sun shafts extract: cloud cover dims the visibility by cloud_follow") {
    Rig r;
    r.world.spawn_with(SunShafts3D{});
    Weather w;
    w.target = w.current = WeatherKind::Clear;
    r.world.spawn_with(w);
    CloudLayer layer;
    layer.fair_coverage = 0.5f;
    layer.overcast_coverage = 0.5f;
    layer.strength = 1.0f;
    r.world.spawn_with(layer);
    CHECK(r.frame().visibility == doctest::Approx(0.75f));  // 1 - 0.5 * 0.5
    r.world.query<SunShafts3D&>().each([](SunShafts3D& c) { c.cloud_follow = 0.0f; });
    CHECK(same_bits(r.frame().visibility, 1.0f));
}

TEST_CASE("sun shafts extract: the Low tier and r.sun_shafts 0 turn the shafts off, so the pass gets no frame") {
    ResetShaftsCvar reset;
    Rig r;
    r.world.spawn_with(SunShafts3D{});
    r.env().time_of_day = 17.0f;  // a low sun
    const math::vec3 toward = -math::normalize(render::sun_direction_at(17.0f));
    const render::View3D view = view_toward(toward);
    render::SunShaftsFrame fr;
    r.frame();
    REQUIRE(r.scene.post().shafts.enabled);
    CHECK(pass_frame(r, view, fr));
    shafts_cvar().set_string("0");
    CHECK_FALSE(r.frame().enabled);
    CHECK_FALSE(pass_frame(r, view, fr));
    shafts_cvar().set_string("1");
    CHECK(r.frame().enabled);
    r.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    CHECK_FALSE(r.frame().enabled);
    CHECK_FALSE(pass_frame(r, view, fr));
    r.extract.set_quality(render::quality_settings(render::QualityTier::Medium));
    CHECK(r.frame().enabled);
}

TEST_CASE(
    "sun shafts extract: the sun is the scene's light, so a sunset moves the projected sun and fades the shafts") {
    // The day clock (Environment3D::time_of_day, what a WorldClock writes and a DayCycle reads) moves the sun; the
    // component is never touched. A fixed camera looks toward the sun's heading at 17:30; from 16:30 to 19:30 the sun
    // sinks toward that view, its projected uv moves every step, the elevation fade drops to 0 and the pass stops
    // getting a frame.
    Rig r;
    r.world.spawn_with(SunShafts3D{});
    r.world.spawn_with(DayCycle{});
    const math::vec3 toward_ref = -math::normalize(render::sun_direction_at(17.5f));
    const render::View3D view = view_toward(toward_ref);
    u32 drawn = 0, after_sunset = 0;
    bool was_drawn = false, ended = false;
    math::vec2 prev_uv{0.0f, 0.0f};
    f32 prev_fade = 2.0f;
    for (f32 h = 16.5f; h <= 19.5f; h += 0.125f) {
        r.env().time_of_day = h;
        r.frame();
        const math::vec3 toward = -math::normalize(r.scene.light().direction);
        const f32 fade = render::sun_shafts_elevation_fade(toward, render::kSunShaftsHighFloor);
        render::SunShaftsFrame fr;
        const bool on = pass_frame(r, view, fr);
        if (on) {
            ++drawn;
            if (was_drawn) CHECK((fr.sun_uv.x != prev_uv.x || fr.sun_uv.y != prev_uv.y));
            prev_uv = fr.sun_uv;
        }
        if (toward.y < -0.02f) {
            ++after_sunset;
            CHECK_FALSE(on);
            CHECK(fade == 0.0f);
        }
        CHECK(fade <= prev_fade + 1e-6f);  // the sun only sinks over the evening
        prev_fade = fade;
        ended = !on;
        was_drawn = on;
    }
    MESSAGE("frames with shafts " << drawn << ", after sunset " << after_sunset);
    CHECK(drawn > 4);
    CHECK(after_sunset > 2);
    CHECK(ended);
    r.world.query<const SunShafts3D&>().each([](const SunShafts3D& c) {
        CHECK(same_bits(c.intensity, 1.0f));
        CHECK(c.enabled == 1u);
    });
}

TEST_CASE("sun shafts extract: nothing is written back to the world; the state hash over a run does not move") {
    auto run = [](bool with_component) {
        Rig r;
        if (with_component) {
            SunShafts3D c;
            c.intensity = 2.0f;
            r.world.spawn_with(c);
        }
        r.env().time_of_day = 17.0f;
        r.frame();
        const u64 before = r.world.state_hash();
        for (int i = 0; i < 40; ++i) r.frame();
        CHECK(r.world.state_hash() == before);
        if (with_component)
            r.world.query<const SunShafts3D&>().each([](const SunShafts3D& c) {
                CHECK(same_bits(c.intensity, 2.0f));
                CHECK(same_bits(c.cloud_follow, 0.5f));
                CHECK(c.enabled == 1u);
            });
        return r.scene.light().direction;
    };
    const math::vec3 a = run(true);
    const math::vec3 b = run(false);
    CHECK((same_bits(a.x, b.x) && same_bits(a.y, b.y) && same_bits(a.z, b.z)));  // the component does not move the sun
}

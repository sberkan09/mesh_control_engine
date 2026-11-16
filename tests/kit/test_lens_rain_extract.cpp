// Extract feed of the rain on the lens (M-LENSRAIN 42d): Kit3DExtract copies the first LensRain3D into Scene3D post.rain with
// amount = wet_q / 2^24, through apply_quality (tier, r.lens_rain, the Range clamps). No component or enabled 0 leaves the
// disabled default (bit-identical to a scene without the feature); nothing is written back to the world.

#include <doctest/doctest.h>

#include <bit>

#include "mce/convar.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/lens_rain.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"
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
    render::LensRain frame() {
        scene.begin(cam, {320, 180});
        extract.extract(world, scene, 1.0f);
        return scene.post().rain;
    }
};

ConVar& rain_cvar() {
    ConVar* cv = ConVar::find(StringId("r.lens_rain"));
    REQUIRE(cv != nullptr);
    return *cv;
}

struct ResetRainCvar {
    ~ResetRainCvar() { rain_cvar().reset(); }
};

bool is_default(const render::LensRain& r) {
    const render::LensRain d;
    return r.enabled == d.enabled && same_bits(r.amount, d.amount) && same_bits(r.density, d.density) &&
           same_bits(r.size, d.size) && same_bits(r.refraction, d.refraction) && same_bits(r.blur, d.blur) &&
           same_bits(r.trail, d.trail) && same_bits(r.fall_speed, d.fall_speed) && r.seed == d.seed;
}

LensRain3D wet(u32 q) {
    LensRain3D c;
    c.wet_q = q;
    return c;
}

}  // namespace

TEST_CASE("lens rain extract: no component keeps the disabled default") {
    Rig r;
    for (int i = 0; i < 3; ++i) CHECK(is_default(r.frame()));
    CHECK_FALSE(r.frame().enabled);
}

TEST_CASE("lens rain extract: enabled 0 keeps the disabled default") {
    Rig r;
    LensRain3D c = wet(1u << 23);
    c.enabled = 0;
    c.density = 3.0f;
    r.world.spawn_with(c);
    CHECK(is_default(r.frame()));
}

TEST_CASE("lens rain extract: an enabled component copies its fields, amount is wet_q / 2^24") {
    Rig r;
    LensRain3D c = wet(1u << 22);
    c.density = 2.5f;
    c.size = 1.5f;
    c.refraction = 0.75f;
    c.blur = 0.5f;
    c.trail = 0.25f;
    c.fall_speed = 1.0f;
    c.seed = 77;
    r.world.spawn_with(c);
    const render::LensRain f = r.frame();
    CHECK(f.enabled);
    CHECK(same_bits(f.amount, 0.25f));
    CHECK(same_bits(f.density, 2.5f));
    CHECK(same_bits(f.size, 1.5f));
    CHECK(same_bits(f.refraction, 0.75f));
    CHECK(same_bits(f.blur, 0.5f));
    CHECK(same_bits(f.trail, 0.25f));
    CHECK(same_bits(f.fall_speed, 1.0f));
    CHECK(f.seed == 77u);
}

TEST_CASE("lens rain extract: wet_q 0 and 2^24 give amounts 0 and 1, above 2^24 clamps") {
    Rig r;
    const Entity e = r.world.spawn_with(wet(0));
    CHECK(same_bits(r.frame().amount, 0.0f));
    r.world.try_get_mut<LensRain3D>(e)->wet_q = 1u << 24;
    CHECK(same_bits(r.frame().amount, 1.0f));
    r.world.try_get_mut<LensRain3D>(e)->wet_q = 0xFFFFFFFFu;
    CHECK(same_bits(r.frame().amount, 1.0f));
}

TEST_CASE("lens rain extract: the Range clamps apply") {
    Rig r;
    LensRain3D c = wet(1u << 23);
    c.density = 100.0f;
    c.size = 0.0f;
    c.refraction = 9.0f;
    c.blur = -1.0f;
    c.trail = 5.0f;
    c.fall_speed = 50.0f;
    r.world.spawn_with(c);
    const render::LensRain f = r.frame();
    CHECK(same_bits(f.density, 4.0f));
    CHECK(same_bits(f.size, 0.25f));
    CHECK(same_bits(f.refraction, 2.0f));
    CHECK(same_bits(f.blur, 0.0f));
    CHECK(same_bits(f.trail, 1.0f));
    CHECK(same_bits(f.fall_speed, 2.0f));
}

TEST_CASE("lens rain extract: the first component wins and removing it clears the rain") {
    Rig r;
    const Entity first = r.world.spawn_with(wet(1u << 22));
    const Entity second = r.world.spawn_with(wet(1u << 23));
    CHECK(same_bits(r.frame().amount, 0.25f));
    r.world.destroy(first);
    CHECK(same_bits(r.frame().amount, 0.5f));
    r.world.destroy(second);
    CHECK(is_default(r.frame()));
}

TEST_CASE("lens rain extract: nothing is written back to the world") {
    Rig r;
    const Entity e = r.world.spawn_with(wet(12345));
    r.frame();
    r.frame();
    CHECK(r.world.try_get<LensRain3D>(e)->wet_q == 12345u);
}

TEST_CASE("lens rain extract: the Low tier and r.lens_rain 0 disable it") {
    ResetRainCvar reset;
    Rig r;
    r.world.spawn_with(wet(1u << 23));
    CHECK(r.frame().enabled);

    rain_cvar().set_string("0");
    CHECK_FALSE(r.frame().enabled);
    rain_cvar().reset();
    CHECK(r.frame().enabled);

    const render::QualitySettings low = render::quality_settings(render::QualityTier::Low);
    r.extract.set_quality(low);
    CHECK_FALSE(r.frame().enabled);
    r.extract.set_quality(render::quality_settings(render::QualityTier::Medium));
    CHECK(r.frame().enabled);
    r.extract.set_quality(render::quality_settings(render::QualityTier::High));
    CHECK(r.frame().enabled);
}
